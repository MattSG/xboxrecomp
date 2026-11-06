#include "wma_decoder.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <stdlib.h>
#include <string.h>

/* Grow out->data to hold `extra` more bytes (amortised doubling). */
static int wma_append(XboxWmaPcm *out, size_t *capacity, const uint8_t *bytes, size_t extra)
{
    size_t need = out->size + extra;

    if (need < out->size) return -1;
    if (need > *capacity) {
        size_t cap = *capacity ? *capacity : 65536;
        uint8_t *grown;
        while (cap < need) {
            if (cap > ((size_t)-1) / 2) return -1;
            cap *= 2;
        }
        grown = (uint8_t *)realloc(out->data, cap);
        if (!grown) return -1;
        out->data = grown;
        *capacity = cap;
    }
    memcpy(out->data + out->size, bytes, extra);
    out->size = need;
    return 0;
}

/* Convert one decoded frame to interleaved S16 at the stream's own rate and
 * channel layout, appending to out. frame == NULL flushes the resampler. */
static int wma_emit(SwrContext *swr, const AVFrame *frame, int channels, int rate,
                    XboxWmaPcm *out, size_t *capacity)
{
    uint8_t *buf = NULL;
    int linesize = 0;
    int in_samples = frame ? frame->nb_samples : 0;
    int max_out = swr_get_out_samples(swr, in_samples) + 256;
    int got;
    int rc = -1;

    (void)rate;
    if (max_out <= 0) return 0;
    if (av_samples_alloc(&buf, &linesize, channels, max_out, AV_SAMPLE_FMT_S16, 0) < 0)
        return -1;
    got = swr_convert(swr, &buf, max_out,
                      frame ? (const uint8_t * const *)frame->extended_data : NULL, in_samples);
    if (got < 0) goto done;
    if (got > 0 && wma_append(out, capacity, buf, (size_t)got * channels * 2) != 0) goto done;
    rc = 0;
done:
    av_freep(&buf);
    return rc;
}

int xbox_wma_decode_file(const char *path, XboxWmaPcm *out)
{
    AVFormatContext *fmt = NULL;
    AVCodecContext *codec = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    SwrContext *swr = NULL;
    const AVCodec *decoder = NULL;
    AVChannelLayout layout;
    size_t capacity = 0;
    int stream, channels, rate, result = -1;

    /* The public contract says every failure leaves a non-NULL output cleared,
     * including argument validation failures. */
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!path) return -1;
    memset(&layout, 0, sizeof(layout));

    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) { fmt = NULL; goto done; }
    if (avformat_find_stream_info(fmt, NULL) < 0) goto done;

    /* Decode only the first audio stream. */
    stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (stream < 0 || !decoder) goto done;

    codec = avcodec_alloc_context3(decoder);
    if (!codec) goto done;
    if (avcodec_parameters_to_context(codec, fmt->streams[stream]->codecpar) < 0) goto done;
    if (avcodec_open2(codec, decoder, NULL) < 0) goto done;

    channels = codec->ch_layout.nb_channels;
    rate = codec->sample_rate;
    if (channels <= 0 || channels > 8 || rate <= 0) goto done;
    if (av_channel_layout_copy(&layout, &codec->ch_layout) < 0) goto done;

    /* Ordinary interleaved 16-bit PCM at the source's rate and layout. */
    if (swr_alloc_set_opts2(&swr, &layout, AV_SAMPLE_FMT_S16, rate,
                            &codec->ch_layout, codec->sample_fmt, rate, 0, NULL) < 0 || !swr)
        goto done;
    if (swr_init(swr) < 0) goto done;

    out->sample_rate = (uint32_t)rate;
    out->channels = (uint16_t)channels;
    out->bits_per_sample = 16;

    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) goto done;

    for (;;) {
        int r = av_read_frame(fmt, pkt);
        if (r < 0) {
            avcodec_send_packet(codec, NULL);       /* flush the decoder */
        } else if (pkt->stream_index != stream) {
            av_packet_unref(pkt);
            continue;
        } else {
            r = avcodec_send_packet(codec, pkt);
            av_packet_unref(pkt);
            if (r < 0) goto done;
        }
        for (;;) {
            int rr = avcodec_receive_frame(codec, frame);
            if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF) break;
            if (rr < 0) goto done;
            if (wma_emit(swr, frame, channels, rate, out, &capacity) != 0) goto done;
            av_frame_unref(frame);
        }
        if (r < 0) break;                           /* demuxer is done and the decoder is flushed */
    }
    if (wma_emit(swr, NULL, channels, rate, out, &capacity) != 0) goto done;   /* resampler tail */

    if (out->size == 0) goto done;
    result = 0;

done:
    if (swr) swr_free(&swr);
    av_channel_layout_uninit(&layout);
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    if (codec) avcodec_free_context(&codec);
    if (fmt) avformat_close_input(&fmt);
    if (result != 0) xbox_wma_pcm_free(out);
    return result;
}

void xbox_wma_pcm_free(XboxWmaPcm *pcm)
{
    if (!pcm) return;
    free(pcm->data);
    memset(pcm, 0, sizeof(*pcm));
}
