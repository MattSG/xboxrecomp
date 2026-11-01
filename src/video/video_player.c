/**
 * Shared FMV player: FFmpeg decode onto a D3D8 texture.
 *
 * Titles whose video is an ordinary container (WMV, MP4, ...) do not need
 * their in-game decoder emulated to be watchable. The recompiled title still
 * drives everything -- it opens the file when its own logic says to, and the
 * runtime plays that file -- so the video appears where the game puts it
 * rather than being replayed on a timer.
 *
 * Lifted from burnout3's player, with its boot-sequence state machine and menu
 * dependencies removed: those are title logic, and this is the decoder.
 *
 * SDL3 has no decoder, so libavformat/libavcodec decode and libswscale
 * converts to BGRA on every host. The pump in xbox_VideoPlayFile() owns its
 * D3D8 device and its decoder on one thread, so nothing here touches the
 * guest's execution or its memory.
 */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "video_player.h"
#include "d3d/d3d8_xbox.h"
#include "d3d/d3d8_internal.h"

/* ================================================================
 * Decoder state
 * ================================================================ */

static struct {
    int                  initialized;
    AVFormatContext     *fmt;
    AVCodecContext      *codec;
    AVFrame             *frame;
    AVPacket            *packet;
    struct SwsContext   *sws;
    int                  stream;        /* video stream index */
    int                  draining;      /* demuxer is at EOF; flushing the decoder */
    int                  width;
    int                  height;
    float                duration;      /* seconds */
    float                elapsed;       /* playback position */
    float                frame_time;    /* time per frame (1/fps) */
    float                next_frame_at; /* when to decode next frame */
    int                  finished;
    int                  has_frame;     /* current frame valid? */

    /* Frame buffer (BGRA, top-down, uploaded to the D3D texture) */
    uint8_t             *frame_buf;
    uint32_t             frame_buf_size;
    uint32_t             stride;

    /* D3D8 texture for rendering */
    IDirect3DTexture8   *texture;
    uint32_t             tex_width;
    uint32_t             tex_height;
} g_vp;

static uint32_t next_pot(uint32_t v)
{
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4;
    v |= v >> 8; v |= v >> 16;
    return v + 1;
}

/* ================================================================
 * Public API
 * ================================================================ */

int video_init(void)
{
    if (g_vp.initialized) return 0;
    av_log_set_level(AV_LOG_ERROR);
    g_vp.initialized = 1;
    fprintf(stderr, "  [VIDEO] FFmpeg initialized\n");
    return 0;
}

void video_shutdown(void)
{
    video_close();
    g_vp.initialized = 0;
}

int video_open(const char *path)
{
    const AVCodec *decoder = NULL;
    AVStream *st;
    AVRational fps;
    HRESULT hr;
    int idx;

    if (!g_vp.initialized) {
        if (video_init() != 0) return -1;
    }

    /* Close any existing video */
    video_close();

    if (avformat_open_input(&g_vp.fmt, path, NULL, NULL) < 0) {
        fprintf(stderr, "  [VIDEO] File not found or unreadable: %s\n", path);
        g_vp.fmt = NULL;
        return -1;
    }
    if (avformat_find_stream_info(g_vp.fmt, NULL) < 0) goto fail;

    idx = av_find_best_stream(g_vp.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (idx < 0 || !decoder) {
        fprintf(stderr, "  [VIDEO] No video stream found in %s\n", path);
        goto fail;
    }
    st = g_vp.fmt->streams[idx];
    g_vp.stream = idx;

    g_vp.codec = avcodec_alloc_context3(decoder);
    if (!g_vp.codec) goto fail;
    if (avcodec_parameters_to_context(g_vp.codec, st->codecpar) < 0) goto fail;
    if (avcodec_open2(g_vp.codec, decoder, NULL) < 0) {
        fprintf(stderr, "  [VIDEO] Cannot open decoder %s\n", decoder->name);
        goto fail;
    }

    g_vp.width = g_vp.codec->width;
    g_vp.height = g_vp.codec->height;
    if (g_vp.width <= 0 || g_vp.height <= 0) goto fail;

    fps = av_guess_frame_rate(g_vp.fmt, st, NULL);
    g_vp.frame_time = (fps.num > 0 && fps.den > 0) ? (float)fps.den / (float)fps.num
                                                    : 1.0f / 30.0f;
    g_vp.duration = g_vp.fmt->duration > 0 ? (float)((double)g_vp.fmt->duration / AV_TIME_BASE)
                                           : 60.0f;

    g_vp.frame = av_frame_alloc();
    g_vp.packet = av_packet_alloc();
    if (!g_vp.frame || !g_vp.packet) goto fail;

    /* Allocate frame buffer */
    g_vp.stride = (uint32_t)g_vp.width * 4;
    g_vp.frame_buf_size = g_vp.stride * (uint32_t)g_vp.height;
    g_vp.frame_buf = (uint8_t *)calloc(1, g_vp.frame_buf_size);
    if (!g_vp.frame_buf) goto fail;

    /* Create D3D8 texture (power-of-2 for compatibility) */
    g_vp.tex_width  = next_pot((uint32_t)g_vp.width);
    g_vp.tex_height = next_pot((uint32_t)g_vp.height);

    /* LIN_A8R8G8B8, not A8R8G8B8.
     *
     * On Xbox the plain format is SWIZZLED -- only the LIN_ variants are
     * linear (see d3d8_format_is_swizzled). We write plain linear BGRA rows
     * here, so asking for A8R8G8B8 made the d3d8 layer faithfully unswizzle
     * data that was never swizzled, which scrambled every frame into a grid of
     * blocks. */
    hr = d3d8_CreateTextureImpl(g_vp.tex_width, g_vp.tex_height, 1, 0,
                                D3DFMT_LIN_A8R8G8B8, &g_vp.texture);
    if (FAILED(hr)) {
        fprintf(stderr, "  [VIDEO] Failed to create texture %ux%u (hr=0x%08lX)\n",
                g_vp.tex_width, g_vp.tex_height, (unsigned long)hr);
        goto fail;
    }

    g_vp.elapsed = 0.0f;
    g_vp.next_frame_at = 0.0f;
    g_vp.finished = 0;
    g_vp.has_frame = 0;
    g_vp.draining = 0;

    fprintf(stderr, "  [VIDEO] Opened: %s (%dx%d, %.1fs, %.1f fps, %s)\n",
            path, g_vp.width, g_vp.height, g_vp.duration,
            1.0f / g_vp.frame_time, decoder->name);
    return 0;

fail:
    video_close();
    return -1;
}

/* Convert the decoded frame to BGRA (top-down) in frame_buf. */
static int convert_frame(const AVFrame *f)
{
    uint8_t *dst[4] = { g_vp.frame_buf, NULL, NULL, NULL };
    int dst_stride[4] = { (int)g_vp.stride, 0, 0, 0 };

    g_vp.sws = sws_getCachedContext(g_vp.sws, f->width, f->height, (enum AVPixelFormat)f->format,
                                    g_vp.width, g_vp.height, AV_PIX_FMT_BGRA,
                                    SWS_BILINEAR, NULL, NULL, NULL);
    if (!g_vp.sws) return -1;
    sws_scale(g_vp.sws, (const uint8_t * const *)f->data, f->linesize, 0, f->height,
              dst, dst_stride);
    g_vp.has_frame = 1;
    return 0;
}

/* Decode one frame into frame_buf (BGRA). Returns 1 on a new frame, 0 if
 * nothing yet (try again), -1 at end of stream or on error. */
static int decode_one_frame(void)
{
    if (!g_vp.fmt) return -1;

    for (;;) {
        int r = avcodec_receive_frame(g_vp.codec, g_vp.frame);
        if (r == 0) {
            int ok = convert_frame(g_vp.frame);
            av_frame_unref(g_vp.frame);
            if (ok < 0) { g_vp.finished = 1; return -1; }
            return 1;
        }
        if (r == AVERROR_EOF) {
            g_vp.finished = 1;
            return -1;
        }
        if (r != AVERROR(EAGAIN)) {
            g_vp.finished = 1;
            return -1;
        }

        /* The decoder wants input. */
        if (g_vp.draining) {
            g_vp.finished = 1;
            return -1;
        }
        r = av_read_frame(g_vp.fmt, g_vp.packet);
        if (r < 0) {
            avcodec_send_packet(g_vp.codec, NULL);      /* flush */
            g_vp.draining = 1;
            continue;
        }
        if (g_vp.packet->stream_index == g_vp.stream)
            avcodec_send_packet(g_vp.codec, g_vp.packet);
        av_packet_unref(g_vp.packet);
    }
}

/* Upload frame_buf to the D3D texture */
static void upload_frame(void)
{
    D3DLOCKED_RECT lr;
    HRESULT hr;
    int y;

    if (!g_vp.texture || !g_vp.frame_buf || !g_vp.has_frame) return;

    hr = g_vp.texture->lpVtbl->LockRect(g_vp.texture, 0, &lr, NULL, 0);
    if (SUCCEEDED(hr)) {
        /* Copy frame data, accounting for texture being POT-padded */
        uint8_t *dst = (uint8_t *)lr.pBits;
        for (y = 0; y < g_vp.height; y++) {
            memcpy(dst + (size_t)y * lr.Pitch,
                   g_vp.frame_buf + (size_t)y * g_vp.stride,
                   g_vp.stride);
        }
        g_vp.texture->lpVtbl->UnlockRect(g_vp.texture, 0);
    }
}

int video_update(float dt)
{
    if (!g_vp.fmt || g_vp.finished) return -1;

    g_vp.elapsed += dt;

    /* Decode frames until we catch up to elapsed time */
    while (g_vp.elapsed >= g_vp.next_frame_at && !g_vp.finished) {
        int r = decode_one_frame();
        if (r < 0) {
            g_vp.finished = 1;
            return -1;
        }
        g_vp.next_frame_at += g_vp.frame_time;
    }

    if (g_vp.has_frame) {
        upload_frame();
        return 1;
    }
    return 0;
}

void video_render(void)
{
    IDirect3DDevice8 *dev;
    float u_max, v_max, sw, sh;

    if (!g_vp.texture || !g_vp.has_frame) return;

    dev = d3d8_GetDevice();
    if (!dev) return;

    /* UV coords account for POT texture padding */
    u_max = (float)g_vp.width  / (float)g_vp.tex_width;
    v_max = (float)g_vp.height / (float)g_vp.tex_height;
    sw = (float)d3d8_GetBackbufferWidth();
    sh = (float)d3d8_GetBackbufferHeight();

    /* Transformed + textured vertex (XYZRHW + UV) */
    struct { float x, y, z, rhw; float u, v; } quad[4] = {
        {  -0.5f,      -0.5f, 0.0f, 1.0f, 0.0f,  0.0f  },
        { sw - 0.5f,   -0.5f, 0.0f, 1.0f, u_max, 0.0f  },
        {  -0.5f,  sh - 0.5f, 0.0f, 1.0f, 0.0f,  v_max },
        { sw - 0.5f, sh-0.5f, 0.0f, 1.0f, u_max, v_max },
    };

    /* Set render state for fullscreen quad */
    dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture8 *)g_vp.texture);
    dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    /* Use the enum names, not hand-written numbers. COLOROP was passed a
     * literal 3 labelled SELECTARG1, but D3DTOP_SELECTARG1 is 2 and 3 is
     * D3DTOP_SELECTARG2. The stage therefore selected ARG2, which defaults to
     * D3DTA_CURRENT, and on stage 0 with no diffuse in the vertex format that
     * is opaque white. The video decoded, converted and uploaded perfectly,
     * and every frame then drew a white rectangle over it. */
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->lpVtbl->SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
    dev->lpVtbl->SetTexture(dev, 0, NULL);
}

int video_is_finished(void)
{
    return g_vp.finished;
}

void video_close(void)
{
    if (g_vp.sws) { sws_freeContext(g_vp.sws); g_vp.sws = NULL; }
    if (g_vp.frame) av_frame_free(&g_vp.frame);
    if (g_vp.packet) av_packet_free(&g_vp.packet);
    if (g_vp.codec) avcodec_free_context(&g_vp.codec);
    if (g_vp.fmt) avformat_close_input(&g_vp.fmt);
    if (g_vp.texture) {
        g_vp.texture->lpVtbl->Release(g_vp.texture);
        g_vp.texture = NULL;
    }
    if (g_vp.frame_buf) {
        free(g_vp.frame_buf);
        g_vp.frame_buf = NULL;
    }
    g_vp.has_frame = 0;
    g_vp.finished = 0;
    g_vp.draining = 0;
    g_vp.elapsed = 0.0f;
    g_vp.width = 0;
    g_vp.height = 0;
}

/* Write the frame currently decoded to a 24-bit BMP.
 *
 * The point is evidence rather than debugging: "the video played" is otherwise
 * only a log line, and a log line cannot distinguish a decoded frame that got
 * drawn from a black screen that ran for the right number of seconds. This
 * writes what was uploaded to the texture, so a non-black file means real
 * decoded pixels reached the renderer. */
int video_dump_frame_bmp(const char *path)
{
    FILE *f;
    uint32_t w = (uint32_t)g_vp.width, h = (uint32_t)g_vp.height, y, x;
    uint32_t row = ((w * 3u) + 3u) & ~3u;
    uint32_t img = row * h, total = 54u + img;
    uint8_t hdr[54];
    uint8_t *line;

    if (!g_vp.has_frame || !g_vp.frame_buf || !w || !h)
        return -1;
    f = fopen(path, "wb");
    if (!f) return -1;

    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < h; y++) {
        const uint8_t *src = g_vp.frame_buf + (size_t)(h - 1 - y) * g_vp.stride;
        for (x = 0; x < w; x++) {          /* BGRA -> BGR */
            line[x * 3 + 0] = src[x * 4 + 0];
            line[x * 3 + 1] = src[x * 4 + 1];
            line[x * 3 + 2] = src[x * 4 + 2];
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [VIDEO] wrote %s (%ux%u)\n", path, w, h);
    return 0;
}
