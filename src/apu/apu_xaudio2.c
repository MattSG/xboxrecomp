/**
 * Audio output backend (SDL3)
 *
 * Low-latency stereo output through an SDL3 audio stream on the default
 * playback device, on every host. Called from the APU monitor frame to submit
 * mixed samples. The xa2_* names are kept from the XAudio2 backend this
 * replaced so the callers did not change.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "apu_xaudio2.h"

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024   /* ~21ms per submission */
#define XA2_NUM_BUFS      3

static SDL_AudioStream *g_stream = NULL;
static int              g_initialized = 0;
static int              g_frames_written = 0;

int xa2_init(void)
{
    SDL_AudioSpec spec;

    if (g_initialized) return 1;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "[AUDIO] SDL audio init failed: %s\n", SDL_GetError());
        return 0;
    }

    spec.format   = SDL_AUDIO_S16;
    spec.channels = XA2_CHANNELS;
    spec.freq     = XA2_SAMPLE_RATE;
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                         &spec, NULL, NULL);
    if (!g_stream) {
        fprintf(stderr, "[AUDIO] SDL_OpenAudioDeviceStream failed: %s\n",
                SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return 0;
    }
    /* Streams from SDL_OpenAudioDeviceStream start paused. */
    SDL_ResumeAudioStreamDevice(g_stream);

    g_initialized = 1;
    g_frames_written = 0;
    fprintf(stderr, "[AUDIO] SDL3 audio initialized (%d Hz stereo 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return 1;
}

void xa2_shutdown(void)
{
    if (g_stream) {
        SDL_DestroyAudioStream(g_stream);   /* also closes the device */
        g_stream = NULL;
    }
    if (g_initialized) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        fprintf(stderr, "[AUDIO] Shut down (%d frames written)\n", g_frames_written);
    }
    g_initialized = 0;
}

int xa2_is_active(void)
{
    return g_initialized;
}

/* Submit a buffer of mixed samples. Called from the APU frame thread.
 * Returns 1 if the buffer was queued, 0 if the queue is already full. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    int copy_samples;

    if (!g_initialized || !g_stream) return 0;

    if (SDL_GetAudioStreamQueued(g_stream) >=
        XA2_NUM_BUFS * XA2_BUF_SAMPLES * XA2_CHANNELS * (int)sizeof(int16_t))
        return 0;

    copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
    if (!SDL_PutAudioStreamData(g_stream, samples,
                                copy_samples * XA2_CHANNELS * (int)sizeof(int16_t)))
        return 0;

    g_frames_written++;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}
