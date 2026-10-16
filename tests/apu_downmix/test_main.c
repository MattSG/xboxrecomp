/* mcpx_apu_dsp_frame mixes the bins down by DirectSound's speaker layout.
 *
 * Every even bin went left and every odd bin right, so centre and LFE each
 * landed on one side, and the effect sends (bin 10 and up) were added dry
 * on top of the voices they copy. The centre, LFE and dry-send checks fail
 * on that rule.
 *
 * Run with: ctest --test-dir <build> */
#include "apu_state.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime links against the title's dispatch; nothing here calls it. */
void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

static int failures;
static MCPXAPUState *s_d;
static float s_bins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];

static void check(int ok, const char *what)
{
    printf("  %-56s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* One DSP frame with `level` in each listed bin; returns the first sample. */
static void frame(const int *bins, int n, float level, float out[2])
{
    int i, b;
    memset(s_bins, 0, sizeof s_bins);
    for (b = 0; b < n; b++)
        for (i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            s_bins[bins[b]][i] = level;
    s_d->ep_frame_div = 0;
    mcpx_apu_dsp_frame(s_d, s_bins);
    out[0] = s_d->monitor.frame_buf[0][0] / 32767.0f;
    out[1] = s_d->monitor.frame_buf[0][1] / 32767.0f;
}

static int close_to(float a, float b) { return fabsf(a - b) < 0.002f; }

int main(void)
{
    static const int front[] = { 0, 1 }, centre[] = { 2 }, lfe[] = { 3 },
                     pair[] = { 6 }, send[] = { 10 };
    float o[2];
    int f, heard = 0;

    s_d = (MCPXAPUState *)calloc(1, sizeof(*s_d));
    if (!s_d) return 2;

    frame(front, 2, 0.5f, o);
    check(close_to(o[0], 0.5f) && close_to(o[1], 0.5f), "front left/right pass through");
    frame(centre, 1, 0.5f, o);
    check(close_to(o[0], 0.3536f) && close_to(o[1], 0.3536f),
          "centre reaches both speakers at -3 dB");
    frame(lfe, 1, 0.5f, o);
    check(close_to(o[0], 0.25f) && close_to(o[1], 0.25f),
          "LFE reaches both speakers at -6 dB");
    frame(pair, 1, 0.5f, o);
    check(close_to(o[0], 0.5f) && close_to(o[1], 0.0f), "a 3D left bin stays left");

    /* The reverb's shortest comb is 1548 samples, so its first frame is
     * silent and the tail arrives later. */
    frame(send, 1, 0.5f, o);
    check(close_to(o[0], 0.0f) && close_to(o[1], 0.0f), "an effect send is not played dry");
    for (f = 0; f < 80; f++) {
        frame(send, 1, 0.5f, o);
        if (fabsf(o[0]) > 0.001f) heard = 1;
    }
    check(heard, "it comes back as a reverb tail");

    free(s_d);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
