/* Compile the current production monitor body; replace only its dependencies. */
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"monitor line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#define MIXER_FRAME_SAMPLES 256
#define WAVEOUT_NUM_BUFS 4
#define M_PI 3.14159265358979323846
static struct { bool active; double phase, amplitude, phase_inc; } g_test_tone;
static int g_audio_muted;
typedef struct { int lock, exiting; bool pause_requested; unsigned ep_frame_div; struct { int16_t frame_buf[256][2]; bool frame_mixed; } monitor; } MCPXAPUState;
static struct { bool initialized; HWAVEOUT hwo; int next_buf, frames_written; WAVEHDR hdrs[4]; int16_t bufs[4][256][2]; } g_waveout;
static MCPXAPUState *current;
static int active, busy, submissions, sleeps, cancel_at, complete_at, output_error, mixed, consumed;
static int16_t expected[256][2];
#define qatomic_read(p) (*(p))
static void qemu_mutex_unlock(int *lock) { CHECK(*lock == 1); *lock = 0; }
static void qemu_mutex_lock(int *lock) { CHECK(*lock == 0); *lock = 1; }
static void fake_sleep(DWORD ms) {
    CHECK(ms == 1 && current->lock == 0); sleeps++;
    if (sleeps == cancel_at) current->exiting = 1;
    if (sleeps == complete_at) g_waveout.hdrs[0].dwFlags = WHDR_DONE;
}
#define Sleep fake_sleep
static void mixer_render(int16_t frame[][2], int count) { CHECK(count == 256); (void)frame; mixed++; }
static int xa2_is_active(void) { return active; }
static int xa2_submit_samples(const int16_t *samples, int count) {
    CHECK(current->lock == 1 && count == 256);
    CHECK(!memcmp(samples, expected, sizeof(expected))); submissions++;
    if (output_error) return -1;
    return submissions <= busy ? 0 : 1;
}
static MMRESULT fake_write(HWAVEOUT device, WAVEHDR *header, UINT size) {
    (void)device; CHECK(size == sizeof(*header) && current->lock == 1);
    CHECK(!(header->dwFlags & WHDR_INQUEUE));
    CHECK(!memcmp(g_waveout.bufs[0], expected, sizeof(expected))); submissions++;
    return output_error ? MMSYSERR_ERROR : MMSYSERR_NOERROR;
}
#define waveOutWrite fake_write
static void mcpx_apu_dsp_pcm_consumed(MCPXAPUState *d) { CHECK(d->lock == 1); d->monitor.frame_mixed = false; consumed++; }
#include "monitor_under_test.inc"
static void reset(MCPXAPUState *d) {
    memset(d, 0, sizeof(*d)); memset(&g_waveout, 0, sizeof(g_waveout));
    current = d; d->lock = 1; d->ep_frame_div = 7;
    memset(expected, 0x35, sizeof(expected)); memcpy(d->monitor.frame_buf, expected, sizeof(expected));
    active = busy = submissions = sleeps = cancel_at = complete_at = output_error = mixed = consumed = 0;
}
int main(void) {
    MCPXAPUState d;
    reset(&d); active = 1; busy = 3; mcpx_apu_monitor_frame(&d);
    CHECK(submissions == 4 && sleeps == 3 && d.lock == 1 && consumed == 1);
    CHECK(d.monitor.frame_buf[0][0] == 0);
    reset(&d); active = 1; busy = 100; cancel_at = 2; mcpx_apu_monitor_frame(&d);
    CHECK(submissions == 2 && sleeps == 2 && d.lock == 1 && consumed == 0);
    CHECK(!memcmp(d.monitor.frame_buf, expected, sizeof(expected)));
    reset(&d); g_waveout.initialized = true; g_waveout.hdrs[0].dwFlags = WHDR_INQUEUE;
    complete_at = 60; mcpx_apu_monitor_frame(&d);
    CHECK(sleeps == 60 && submissions == 1 && g_waveout.next_buf == 1 && g_waveout.frames_written == 1 && consumed == 1);
    reset(&d); g_waveout.initialized = true; g_waveout.hdrs[0].dwFlags = WHDR_INQUEUE;
    cancel_at = 2; mcpx_apu_monitor_frame(&d);
    CHECK(sleeps == 2 && submissions == 0 && d.lock == 1 && g_waveout.next_buf == 0);
    CHECK(!memcmp(d.monitor.frame_buf, expected, sizeof(expected)));
    reset(&d); g_waveout.initialized = true; output_error = 1; mcpx_apu_monitor_frame(&d);
    CHECK(submissions == 1 && g_waveout.frames_written == 0 && g_waveout.next_buf == 0);
    CHECK(!memcmp(d.monitor.frame_buf, expected, sizeof(expected)));
    reset(&d); active = 1; output_error = 1; mcpx_apu_monitor_frame(&d);
    CHECK(consumed == 0 && submissions == 1 && sleeps == 0 && !memcmp(d.monitor.frame_buf, expected, sizeof(expected)));
    reset(&d); active = 1; d.exiting = 1; mcpx_apu_monitor_frame(&d);
    CHECK(submissions == 0 && sleeps == 0 && mixed == 0 && d.lock == 1);
    CHECK(!memcmp(d.monitor.frame_buf, expected, sizeof(expected)));
    reset(&d); g_waveout.initialized = true; d.exiting = 1; mcpx_apu_monitor_frame(&d);
    CHECK(submissions == 0 && sleeps == 0 && mixed == 0 && g_waveout.frames_written == 0);
    CHECK(!memcmp(d.monitor.frame_buf, expected, sizeof(expected)));
    /* A retained frame must not advance native voices or mix twice on retry. */
    reset(&d); active = 1; output_error = 1; mcpx_apu_monitor_frame(&d);
    output_error = 0; mcpx_apu_monitor_frame(&d);
    CHECK(mixed == 1 && consumed == 1 && submissions == 2);
    reset(&d); g_waveout.initialized = true; output_error = 1; mcpx_apu_monitor_frame(&d);
    output_error = 0; mcpx_apu_monitor_frame(&d);
    CHECK(mixed == 1 && consumed == 1 && submissions == 2);
    reset(&d); active = 1; busy = 100; cancel_at = 2; mcpx_apu_monitor_frame(&d);
    CHECK(d.monitor.frame_mixed && mixed == 1 && consumed == 0);
    d.exiting = 0; busy = 0; cancel_at = 0; mcpx_apu_monitor_frame(&d);
    CHECK(!d.monitor.frame_mixed && mixed == 1 && consumed == 1);
    memcpy(d.monitor.frame_buf, expected, sizeof(expected));
    mcpx_apu_monitor_frame(&d);
    CHECK(mixed == 2 && consumed == 2); /* Next frame really mixes again. */
    reset(&d); g_waveout.initialized = true; g_waveout.hdrs[0].dwFlags = WHDR_INQUEUE;
    cancel_at = 2; mcpx_apu_monitor_frame(&d);
    CHECK(d.monitor.frame_mixed && mixed == 1 && consumed == 0);
    d.exiting = 0; cancel_at = 0; g_waveout.hdrs[0].dwFlags = WHDR_DONE;
    mcpx_apu_monitor_frame(&d);
    CHECK(!d.monitor.frame_mixed && mixed == 1 && consumed == 1);
    puts("Production monitor retry, queued-storage preservation and cancellation passed");
    return 0;
}
