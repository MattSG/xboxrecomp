/* Compare the production HRTF filter with its prior modulo-index reference.
 * Every coefficient/history/PCM byte must match; no tolerance or reordered
 * floating arithmetic can hide an audible behavior change. */
#include "../../src/apu/apu_state.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "HRTF check line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)

static void reference_process(HrtfFilter *f, float in[][2], float out[][2])
{
    for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; n++) {
        hrtf_filter_step_parameters(f);
        for (int ch = 0; ch < 2; ch++) {
            float *buf = f->ch[ch].buf;
            float *coeff = f->ch[ch].hrir_coeff_cur;
            buf[f->buf_pos] = in[n][ch];
            float d = f->itd_cur * (ch == 0 ? +1.0f : -1.0f);
            if (d < 0.0f) d = 0.0f;
            int di = (int)d;
            float dfrac = d - di;
            float acc = 0.0f;
            for (int k = 0; k < HRTF_NUM_TAPS; k++) {
                int idx1 = (f->buf_pos - di - k + HRTF_BUFLEN) % HRTF_BUFLEN;
                float s = buf[idx1];
                if (dfrac > 0.0f) {
                    int idx2 = (idx1 - 1 + HRTF_BUFLEN) % HRTF_BUFLEN;
                    s = s * (1 - dfrac) + buf[idx2] * dfrac;
                }
                acc += coeff[k] * s;
            }
            out[n][ch] = acc;
        }
        f->buf_pos = (f->buf_pos + 1) % HRTF_BUFLEN;
    }
}

static uint32_t random_state = 0x12345678;
static float value(void)
{
    random_state = random_state * 1664525u + 1013904223u;
    return ((int32_t)(random_state >> 8) - 0x7fffff) / 8388608.0f;
}
static void initialize(HrtfFilter *filter, int position, float current, float target)
{
    hrtf_filter_init(filter);
    filter->buf_pos = position;
    filter->itd_cur = current; filter->itd_tar = target;
    for (int ch = 0; ch < 2; ++ch) {
        for (int i = 0; i < HRTF_BUFLEN; ++i) filter->ch[ch].buf[i] = value();
        for (int i = 0; i < HRTF_NUM_TAPS; ++i) {
            filter->ch[ch].hrir_coeff_cur[i] = value() / 31;
            filter->ch[ch].hrir_coeff_tar[i] = value() / 31;
        }
    }
}
static void input(float data[HRTF_SAMPLES_PER_FRAME][2], unsigned kind)
{
    for (int n = 0; n < HRTF_SAMPLES_PER_FRAME; ++n)
        for (int ch = 0; ch < 2; ++ch)
            data[n][ch] = kind == 0 ? 0.0f : kind == 1 ? (n == 0 ? 1.0f : 0.0f) : value();
}
int main(void)
{
    const float delays[] = {-42, -41.999f, -41.5f, -20.75f, -1.5f, -1, -0.5f,
                            -0.0f, 0, 0.25f, 1, 1.5f, 20.5f, 41.5f, 41.999f, 42};
    unsigned frames = 0;
    for (int position = 0; position < HRTF_BUFLEN; ++position)
    for (unsigned delay = 0; delay < sizeof delays / sizeof delays[0]; ++delay)
    for (unsigned transition = 0; transition < 4; ++transition)
    for (unsigned inplace = 0; inplace < 2; ++inplace) {
        float target = transition == 0 ? delays[delay] : transition == 1 ? -42 : transition == 2 ? 42 : 0;
        HrtfFilter reference, actual;
        initialize(&reference, position, delays[delay], target); actual = reference;
        for (unsigned frame = 0; frame < 4; ++frame) {
            float left[HRTF_SAMPLES_PER_FRAME][2], right[HRTF_SAMPLES_PER_FRAME][2];
            float left_out[HRTF_SAMPLES_PER_FRAME][2], right_out[HRTF_SAMPLES_PER_FRAME][2];
            input(left, frame % 3); memcpy(right, left, sizeof left);
            if (inplace) {
                reference_process(&reference, left, left);
                hrtf_filter_process(&actual, right, right);
                CHECK(!memcmp(left, right, sizeof left));
            } else {
                reference_process(&reference, left, left_out);
                hrtf_filter_process(&actual, right, right_out);
                CHECK(!memcmp(left_out, right_out, sizeof left_out));
                CHECK(!memcmp(left, right, sizeof left));
            }
            CHECK(!memcmp(&reference, &actual, sizeof reference));
            ++frames;
        }
    }
    printf("HRTF exact state/PCM equivalence passed: %u frames\n", frames);
    return 0;
}
