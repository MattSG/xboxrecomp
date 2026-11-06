/* FFmpeg decoding: a WMA file decodes to interleaved 16-bit PCM, and a video
 * plays onto a D3D8 texture with the right pixels. The test makes its own media
 * with the ffmpeg binary that was downloaded (XBOX_FFMPEG_BIN), or $FFMPEG. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio/wma_decoder.h"
#include "video/video_player.h"
#include "d3d/d3d8_xbox.h"

void *recomp_lookup(unsigned long a) { (void)a; abort(); }
void *recomp_lookup_manual(unsigned long a) { (void)a; abort(); }

#define FAIL(msg) do { fprintf(stderr, "FAIL: %s\n", msg); return 1; } while (0)

static int run(const char *fmt, const char *ffmpeg)
{
    char cmd[1024];
    snprintf(cmd, sizeof cmd, fmt, ffmpeg);
    return system(cmd);
}

int main(void)
{
    const char *ffmpeg = getenv("FFMPEG");
    XboxWmaPcm pcm;
    IDirect3D8 *d3d;
    IDirect3DDevice8 *dev;
    D3DPRESENT_PARAMETERS pp;
    FILE *f;
    unsigned char bmp[54 + 64 * 3 * 48 + 64];
    size_t n;
    int i, frames = 0;

#ifdef XBOX_FFMPEG_BIN
    if (!ffmpeg) ffmpeg = XBOX_FFMPEG_BIN;
#endif
    if (!ffmpeg) { puts("SKIP: no ffmpeg binary to make test media"); return 0; }

    /* 1 s of 440 Hz stereo as WMA, 1 s of a flat colour as MPEG-4 video. */
    if (run("\"%s\" -v error -y -f lavfi -i sine=frequency=440:sample_rate=44100:duration=1 -ac 2 -c:a wmav2 media_test.wma", ffmpeg))
        FAIL("could not make test WMA");
    if (run("\"%s\" -v error -y -f lavfi -i color=c=0x3366CC:size=64x48:rate=10:duration=1 -c:v mpeg4 -q:v 2 media_test.avi", ffmpeg))
        FAIL("could not make test video");

    if (xbox_wma_decode_file("media_test.wma", &pcm) != 0) FAIL("WMA decode");
    printf("wma: %u Hz, %u ch, %u bit, %zu bytes\n", pcm.sample_rate, pcm.channels, pcm.bits_per_sample, pcm.size);
    if (pcm.sample_rate != 44100 || pcm.channels != 2 || pcm.bits_per_sample != 16) FAIL("WMA format");
    if (pcm.size < 44100 * 4 * 9 / 10 || pcm.size > 44100 * 4 * 12 / 10) FAIL("WMA length is not about one second");
    {   /* a 440 Hz sine is not silence */
        const short *s = (const short *)pcm.data;
        int peak = 0;
        for (i = 0; i < 4410; i++) { int v = abs(s[i * 2 + 20000]); if (v > peak) peak = v; }
        if (peak < 1000) FAIL("decoded audio is silent");
    }
    xbox_wma_pcm_free(&pcm);
    if (xbox_wma_decode_file("does_not_exist.wma", &pcm) == 0) FAIL("missing file decoded");

    SDL_setenv_unsafe("RECOMP_HIDE_WINDOW", "1", 1);
    d3d = xbox_Direct3DCreate8(220);
    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = 64; pp.BackBufferHeight = 48; pp.Windowed = TRUE;
    if (!d3d || FAILED(d3d->lpVtbl->CreateDevice(d3d, 0, 0, NULL, 0, &pp, &dev))) FAIL("CreateDevice");

    if (video_open("media_test.avi") != 0) FAIL("video_open");
    for (i = 0; i < 20; i++) {
        int r = video_update(0.1f);
        if (r > 0) frames++;
        if (r < 0) break;
    }
    printf("video: %d frames uploaded, finished=%d\n", frames, video_is_finished());
    if (frames < 5) FAIL("too few video frames");
    if (video_dump_frame_bmp("media_test.bmp") != 0) FAIL("frame dump");
    video_close();

    f = fopen("media_test.bmp", "rb");
    if (!f) FAIL("no bmp");
    n = fread(bmp, 1, sizeof bmp, f);
    fclose(f);
    if (n < 54 + 64 * 3 * 48) FAIL("bmp too short");
    {   /* centre pixel, BGR; a flat 0x3366CC through YUV 4:2:0 lands within a few counts */
        const unsigned char *p = bmp + 54 + (size_t)24 * 192 + 32 * 3;
        printf("centre pixel BGR: %u %u %u\n", p[0], p[1], p[2]);
        if (abs(p[2] - 0x33) > 14 || abs(p[1] - 0x66) > 14 || abs(p[0] - 0xCC) > 14) FAIL("video colour is wrong");
    }

    puts("PASS: FFmpeg decodes WMA to PCM and plays video onto a D3D8 texture");
    return 0;
}
