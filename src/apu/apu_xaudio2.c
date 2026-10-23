/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apu_xaudio2.h"

/* XAudio2 on Windows; elsewhere the same contract over an SDL3 audio
 * stream (one queue, no callback). */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024   /* ~21ms per submission */
/* The APU submits 256 samples (5.33 ms), not a full 1024-sample slot.
 * Three slots only covered 16 ms and dropped buffers during scheduler bursts.
 * Sixty-four slots cover 341 ms: enough for the APU's bounded 250 ms
 * catch-up plus scheduler jitter. Capacity does not force a full queue;
 * steady-state submission remains one 256-sample buffer every 5.33 ms. */
#define XA2_NUM_BUFS      64
#define XA2_CUSHION       8      /* silent buffers queued after running dry */

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][2];
static int                     g_xa2_next_buf = 0;
static float                   g_xa2_gain = 1.0f;  /* 0 under RECOMP_AUDIO_MUTE */
static int                     g_xa2_muted;        /* pause state last applied */
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;
static FILE                   *g_xa2_pcm_dump = NULL;
static volatile LONG g_xa2_error;
static int g_xa2_error_reported;

/* Endpoint loss otherwise looks like a permanently full queue. Keep the
 * callback small; destruction/recovery must happen outside the audio thread. */
static void STDMETHODCALLTYPE xa2_processing_start(IXAudio2EngineCallback *self) { (void)self; }
static void STDMETHODCALLTYPE xa2_processing_end(IXAudio2EngineCallback *self) { (void)self; }
static void STDMETHODCALLTYPE xa2_critical_error(IXAudio2EngineCallback *self, HRESULT error)
{
    (void)self;
    InterlockedExchange(&g_xa2_error, (LONG)error);
}
static struct IXAudio2EngineCallbackVtbl g_xa2_callback_vtable = {
    xa2_processing_start, xa2_processing_end, xa2_critical_error
};
static IXAudio2EngineCallback g_xa2_callback = { &g_xa2_callback_vtable };

int xa2_init(void)
{
    HRESULT hr;
    int com_initialized;
    WAVEFORMATEX wfx = { 0 };

    if (g_xa2_initialized) return 1;

    InterlockedExchange(&g_xa2_error, 0);
    g_xa2_error_reported = 0;
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2_RegisterForCallbacks(g_xa2, &g_xa2_callback);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] RegisterForCallbacks failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }
    {   /* RECOMP_AUDIO_MUTE=1: the device runs and paces as usual, at volume 0
         * (scripted runs). */
        const char *mute = getenv("RECOMP_AUDIO_MUTE");
        g_xa2_gain = (mute && *mute && *mute != '0') ? 0.0f : 1.0f;
        g_xa2_muted = 0;
        if (g_xa2_gain == 0.0f)
            IXAudio2MasteringVoice_SetVolume(g_xa2_master, 0.0f, XAUDIO2_COMMIT_NOW);
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] Start failed: 0x%08lX\n", hr);
        goto fail;
    }

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;
    {
        const char *path = getenv("RECOMP_XA2_PCM_DUMP");
        if (path && *path) {
            g_xa2_pcm_dump = fopen(path, "wb");
            fprintf(stderr, "[XA2] PCM capture %s: %s (s16le stereo 48000 Hz)\n",
                    g_xa2_pcm_dump ? "opened" : "failed", path);
        }
    }

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return 1;

fail:
    xa2_shutdown();
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

void xa2_shutdown(void)
{
    if (g_xa2_pcm_dump) {
        fclose(g_xa2_pcm_dump);
        g_xa2_pcm_dump = NULL;
    }
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. 1=accepted, 0=queue full, -1=failure. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;
    HRESULT hr;

    if (!samples || num_samples <= 0 || num_samples > XA2_BUF_SAMPLES) return -1;
    if (!g_xa2_initialized || !g_xa2_source) return -1;
    LONG engine_error = InterlockedCompareExchange(&g_xa2_error, 0, 0);
    if (engine_error) {
        if (!g_xa2_error_reported) {
            fprintf(stderr, "[XA2] Critical engine error: 0x%08lX\n", engine_error);
            g_xa2_error_reported = 1;
        }
        return -1;
    }

    {   /* Paused in the background (fb_present.c): quiet, not a held note. */
        extern int xbox_FramebufferPaused(void);
        int paused = xbox_FramebufferPaused();
        if (paused != g_xa2_muted && g_xa2_master) {
            /* Back to the configured gain, not to 1: a muted run stays muted. */
            g_xa2_master->lpVtbl->SetVolume(g_xa2_master, paused ? 0.0f : g_xa2_gain,
                                            XAUDIO2_COMMIT_NOW);
            g_xa2_muted = paused;
        }
    }
    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if ((int)state.BuffersQueued >= XA2_NUM_BUFS) return 0;
    /* The APU produces in real time, so the queue holds only what the last
     * stall's catch-up left: usually next to nothing, and every hitch after
     * that ran it dry -- the device played silence, a click each time. Run
     * dry (or never filled), put XA2_CUSHION buffers of silence in front:
     * one ~43 ms gap, then slack for the following hitches. */
    if (state.BuffersQueued == 0) {
        static const int16_t silence[XA2_BUF_SAMPLES][2];
        if (g_xa2_frames_written)
            fprintf(stderr, "[XA2] underrun after %d buffers\n", g_xa2_frames_written);
        memset(&xbuf, 0, sizeof(xbuf));
        xbuf.AudioBytes = num_samples * XA2_CHANNELS * sizeof(int16_t);
        xbuf.pAudioData = (const BYTE *)silence;
        for (idx = 0; idx < XA2_CUSHION; idx++)
            if (FAILED(IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL)))
                break;
    }

    idx = g_xa2_next_buf;
    copy_samples = num_samples;
    memcpy(g_xa2_bufs[idx], samples, copy_samples * XA2_CHANNELS * sizeof(int16_t));

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * XA2_CHANNELS * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
    if (FAILED(hr)) return -1;
    if (g_xa2_pcm_dump) {
        fwrite(g_xa2_bufs[idx], 1, xbuf.AudioBytes, g_xa2_pcm_dump);
        fflush(g_xa2_pcm_dump);
    }

    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    g_xa2_frames_written++;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

#else /* !_WIN32 -- SDL3 audio stream */

#include <SDL3/SDL.h>

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024
/* Same depth as the XAudio2 queue: 64 submissions, ~341 ms at 256 samples. */
#define XA2_NUM_BUFS      64
#define XA2_CUSHION       8

static SDL_AudioStream *g_stream;
static float g_gain = 1.0f;
static int g_frames_written;
static FILE *g_pcm_dump;
static int g_muted;   /* last pause state applied to the stream gain */

int xa2_init(void)
{
    const SDL_AudioSpec spec = { SDL_AUDIO_S16LE, XA2_CHANNELS, XA2_SAMPLE_RATE };
    const char *mute = getenv("RECOMP_AUDIO_MUTE");
    const char *path = getenv("RECOMP_XA2_PCM_DUMP");

    if (g_stream) return 1;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "[XA2] SDL audio init failed: %s\n", SDL_GetError());
        return 0;
    }
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!g_stream) {
        fprintf(stderr, "[XA2] SDL audio stream failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return 0;
    }
    g_gain = (mute && *mute && *mute != '0') ? 0.0f : 1.0f;
    SDL_SetAudioStreamGain(g_stream, g_gain);
    SDL_ResumeAudioStreamDevice(g_stream);
    g_frames_written = 0;
    g_muted = 0;   /* gain is g_gain now; re-apply the pause state on submit */
    if (path && *path) {
        g_pcm_dump = fopen(path, "wb");
        fprintf(stderr, "[XA2] PCM capture %s: %s (s16le stereo 48000 Hz)\n",
                g_pcm_dump ? "opened" : "failed", path);
    }
    fprintf(stderr, "[XA2] SDL audio initialized (%d Hz stereo 16-bit, %s)\n",
            XA2_SAMPLE_RATE, SDL_GetCurrentAudioDriver());
    return 1;
}

void xa2_shutdown(void)
{
    if (g_pcm_dump) {
        fclose(g_pcm_dump);
        g_pcm_dump = NULL;
    }
    if (!g_stream) return;
    SDL_DestroyAudioStream(g_stream);
    g_stream = NULL;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_frames_written);
}

int xa2_is_active(void)
{
    return g_stream != NULL;
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    int queued;

    if (!samples || num_samples <= 0 || num_samples > XA2_BUF_SAMPLES) return -1;
    if (!g_stream) return -1;
    const int bytes = num_samples * XA2_CHANNELS * (int)sizeof(int16_t);
    {   /* Paused in the background (fb_present.c): quiet, not a held note. */
        extern int xbox_FramebufferPaused(void);
        int paused = xbox_FramebufferPaused();
        if (paused != g_muted) {
            SDL_SetAudioStreamGain(g_stream, paused ? 0.0f : g_gain);
            g_muted = paused;
        }
    }
    queued = SDL_GetAudioStreamQueued(g_stream);
    if (queued < 0) return -1;
    if (queued >= XA2_NUM_BUFS * bytes) return 0;
    /* Ran dry: a short cushion of silence first, as the XAudio2 path does. */
    if (queued == 0) {
        static const int16_t silence[XA2_BUF_SAMPLES][2];
        if (g_frames_written)
            fprintf(stderr, "[XA2] underrun after %d buffers\n", g_frames_written);
        for (int i = 0; i < XA2_CUSHION; i++)
            SDL_PutAudioStreamData(g_stream, silence, bytes);
    }
    if (!SDL_PutAudioStreamData(g_stream, samples, bytes)) return -1;
    if (g_pcm_dump) {
        fwrite(samples, 1, (size_t)bytes, g_pcm_dump);
        fflush(g_pcm_dump);
    }
    g_frames_written++;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

#endif /* _WIN32 */
