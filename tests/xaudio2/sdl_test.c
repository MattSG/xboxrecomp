/* xaudio2 POSIX backend test: the SDL3 audio-stream path of apu_xaudio2.c,
 * run against SDL's "dummy" audio driver so no sound card is needed. The
 * device is paused after each init, so the queue only changes when the test
 * changes it. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "../../src/apu/apu_xaudio2.c"

/* fb_present.c is not linked: a controllable stand-in. */
int g_test_paused;
int xbox_FramebufferPaused(void) { return g_test_paused; }

#define SUB_SAMPLES 256
#define SUB_BYTES   (SUB_SAMPLES * XA2_CHANNELS * (int)sizeof(int16_t))  /* 1024 */

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); failures++; \
} } while (0)

static int16_t sub[SUB_SAMPLES * XA2_CHANNELS];

static void fill(int base)
{
    for (int i = 0; i < SUB_SAMPLES * XA2_CHANNELS; i++)
        sub[i] = (int16_t)(base + i);
}

static int start(void)
{
    int ok = xa2_init();
    if (ok) SDL_PauseAudioStreamDevice(g_stream);
    return ok;
}

static void test_before_init(void)
{
    CHECK(xa2_is_active() == 0);
    CHECK(xa2_submit_samples(sub, SUB_SAMPLES) == -1);
    CHECK(xa2_get_buffer_size() == XA2_BUF_SAMPLES);
}

static void test_invalid_input(void)
{
    int before = SDL_GetAudioStreamQueued(g_stream);
    CHECK(xa2_submit_samples(NULL, SUB_SAMPLES) == -1);
    CHECK(xa2_submit_samples(sub, 0) == -1);
    CHECK(xa2_submit_samples(sub, -1) == -1);
    CHECK(xa2_submit_samples(sub, XA2_BUF_SAMPLES + 1) == -1);
    CHECK(SDL_GetAudioStreamQueued(g_stream) == before);
}

static void test_queue_cap(void)
{
    int accepted, r;
    fill(1);
    CHECK(xa2_submit_samples(sub, SUB_SAMPLES) == 1);
    /* First submission ran the queue dry: cushion of silence in front. */
    CHECK(SDL_GetAudioStreamQueued(g_stream) == (XA2_CUSHION + 1) * SUB_BYTES);
    accepted = 1;
    while ((r = xa2_submit_samples(sub, SUB_SAMPLES)) == 1)
        accepted++;
    CHECK(r == 0);
    CHECK(accepted == XA2_NUM_BUFS - XA2_CUSHION);
    CHECK(SDL_GetAudioStreamQueued(g_stream) == XA2_NUM_BUFS * SUB_BYTES);
    /* Still full: rejected without touching the queue. */
    CHECK(xa2_submit_samples(sub, SUB_SAMPLES) == 0);
    CHECK(SDL_GetAudioStreamQueued(g_stream) == XA2_NUM_BUFS * SUB_BYTES);
}

static void test_underrun_cushion(void)
{
    static uint8_t out[(XA2_CUSHION + 1) * SUB_BYTES];
    static const uint8_t zeros[XA2_CUSHION * SUB_BYTES];
    int got;

    SDL_ClearAudioStream(g_stream);
    CHECK(SDL_GetAudioStreamQueued(g_stream) == 0);
    fill(100);
    CHECK(xa2_submit_samples(sub, SUB_SAMPLES) == 1);
    CHECK(SDL_GetAudioStreamQueued(g_stream) == (int)sizeof out);
    got = SDL_GetAudioStreamData(g_stream, out, (int)sizeof out);
    CHECK(got == (int)sizeof out);
    CHECK(memcmp(out, zeros, sizeof zeros) == 0);
    CHECK(memcmp(out + sizeof zeros, sub, SUB_BYTES) == 0);
}

static void test_pcm_dump(void)
{
    static const char path[] = "xaudio2_sdl_dump.raw";
    static int16_t first[SUB_SAMPLES * XA2_CHANNELS], second[SUB_SAMPLES * XA2_CHANNELS];
    static uint8_t got[2 * SUB_BYTES + 1];   /* one spare byte: detect extra PCM */
    FILE *f;
    size_t n;

    setenv("RECOMP_XA2_PCM_DUMP", path, 1);
    CHECK(start());
    unsetenv("RECOMP_XA2_PCM_DUMP");
    fill(7);
    memcpy(first, sub, sizeof first);
    fill(900);
    memcpy(second, sub, sizeof second);
    CHECK(xa2_submit_samples(first, SUB_SAMPLES) == 1);
    CHECK(xa2_submit_samples(second, SUB_SAMPLES) == 1);
    xa2_shutdown();

    f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f) return;
    n = fread(got, 1, sizeof got, f);
    fclose(f);
    /* Only submitted PCM is captured, not the cushion. */
    CHECK(n == 2 * SUB_BYTES);
    CHECK(memcmp(got, first, SUB_BYTES) == 0);
    CHECK(memcmp(got + SUB_BYTES, second, SUB_BYTES) == 0);
    remove(path);
}

static void test_mute_env(void)
{
    setenv("RECOMP_AUDIO_MUTE", "1", 1);
    CHECK(start());
    CHECK(SDL_GetAudioStreamGain(g_stream) == 0.0f);
    xa2_shutdown();

    setenv("RECOMP_AUDIO_MUTE", "0", 1);
    CHECK(start());
    CHECK(SDL_GetAudioStreamGain(g_stream) == 1.0f);
    xa2_shutdown();
    unsetenv("RECOMP_AUDIO_MUTE");
}

static void test_paused_gain(void)
{
    CHECK(start());
    fill(3);
    g_test_paused = 1;
    CHECK(xa2_submit_samples(sub, SUB_SAMPLES) == 1);
    CHECK(SDL_GetAudioStreamGain(g_stream) == 0.0f);
    g_test_paused = 0;
    CHECK(xa2_submit_samples(sub, SUB_SAMPLES) == 1);
    CHECK(SDL_GetAudioStreamGain(g_stream) == 1.0f);
    xa2_shutdown();
}

/* Shut down while paused, then init and submit while still paused: the
 * stream must come up muted again. */
static void test_paused_across_reinit(void)
{
    CHECK(start());
    fill(5);
    g_test_paused = 1;
    CHECK(xa2_submit_samples(sub, SUB_SAMPLES) == 1);
    xa2_shutdown();

    CHECK(start());
    CHECK(xa2_submit_samples(sub, SUB_SAMPLES) == 1);
    CHECK(SDL_GetAudioStreamGain(g_stream) == 0.0f);
    g_test_paused = 0;
    xa2_shutdown();
}

static void test_lifecycle(void)
{
    CHECK(start());
    CHECK(xa2_is_active() == 1);
    CHECK(xa2_init() == 1);       /* idempotent */
    CHECK(xa2_is_active() == 1);
    xa2_shutdown();
    CHECK(xa2_is_active() == 0);
    CHECK(xa2_submit_samples(sub, SUB_SAMPLES) == -1);
}

int main(void)
{
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");

    test_before_init();
    test_lifecycle();

    CHECK(start());
    test_invalid_input();
    test_queue_cap();
    test_underrun_cushion();
    xa2_shutdown();

    test_pcm_dump();
    test_mute_env();
    test_paused_gain();
    test_paused_across_reinit();

    if (failures) {
        fprintf(stderr, "xaudio2 sdl_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("xaudio2 sdl_test: all checks passed\n");
    return 0;
}
