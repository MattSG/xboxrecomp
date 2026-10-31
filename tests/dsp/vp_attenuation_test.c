/* Compile the production VP so this covers its initialization and lookup,
 * including reset behavior. Compare every uint16_t input against the former
 * formula, with exact float and mixed PCM equality rather than a tolerance. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t pow_calls;
static float counted_powf(float base, float exponent)
{
    ++pow_calls;
    return powf(base, exponent);
}
#define powf counted_powf
#include "../../src/apu/apu_vp.c"
#undef powf

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "VP attenuation check line %d: %s\n", __LINE__, #condition); \
    exit(1); } } while (0)

struct McpxApuDebug g_dbg, g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4];
uint8_t *g_apu_ram_ptr;
uint8_t *mcpx_apu_phys(uint64_t address)
{
    (void)address;
    CHECK(0); /* No guest-memory access is needed for volume conversion. */
    return NULL;
}
/* apu_vp.c's diagnostics, off here. */
const char *mcpx_apu_phys_class(uint64_t address) { (void)address; return "none"; }
int apu_env_on(const char *name) { (void)name; return 0; }
ApuIrqStats apu_irq_stats;

static float reference_attenuate(uint16_t volume)
{
    volume &= 0xFFF;
    return volume == 0xFFF ? 0.0f : powf(10.0f, volume / (64.0f * -20.0f));
}

static void check_all_inputs(MCPXAPUState *d)
{
    float expected_pcm[32], actual_pcm[32];
    int16_t expected_s16[32], actual_s16[32];
    for (unsigned input = 0; input <= UINT16_MAX; ++input) {
        float expected = reference_attenuate((uint16_t)input);
        float actual = attenuate(d, (uint16_t)input);
        CHECK(!memcmp(&expected, &actual, sizeof(expected)));
        for (unsigned sample = 0; sample < 32; ++sample) {
            float pcm = (float)((int)sample - 16) / 16.0f;
            float start = (input & 1) ? 0.0f : 1.0f;
            expected_pcm[sample] = (start + (expected - start) / 32 * (sample + 1)) * pcm;
            actual_pcm[sample] = (start + (actual - start) / 32 * (sample + 1)) * pcm;
        }
        CHECK(!memcmp(expected_pcm, actual_pcm, sizeof(expected_pcm)));
        src_float_to_short_array(expected_pcm, expected_s16, 32);
        src_float_to_short_array(actual_pcm, actual_s16, 32);
        CHECK(!memcmp(expected_s16, actual_s16, sizeof(expected_s16)));
    }
}

static __declspec(noinline) float bench_reference(const volatile uint16_t *inputs, unsigned count)
{
    float sum = 0.0f;
    for (unsigned repeat = 0; repeat < 1024; ++repeat)
        for (unsigned i = 0; i < count; ++i)
            sum += reference_attenuate(inputs[i]);
    return sum;
}

static __declspec(noinline) float bench_cached(MCPXAPUState *d, const volatile uint16_t *inputs, unsigned count)
{
    float sum = 0.0f;
    for (unsigned repeat = 0; repeat < 1024; ++repeat)
        for (unsigned i = 0; i < count; ++i)
            sum += attenuate(d, inputs[i]);
    return sum;
}

static void benchmark(MCPXAPUState *d)
{
    uint16_t inputs[8192];
    uint32_t random = 0x12345678;
    LARGE_INTEGER frequency, begin, middle, end;
    QueryPerformanceFrequency(&frequency);
    for (unsigned i = 0; i < 8192; ++i) {
        random = random * 1664525u + 1013904223u;
        inputs[i] = (uint16_t)(random >> 16);
    }
    for (unsigned trial = 0; trial < 5; ++trial) {
        QueryPerformanceCounter(&begin);
        float before = bench_reference(inputs, 8192);
        QueryPerformanceCounter(&middle);
        float after = bench_cached(d, inputs, 8192);
        QueryPerformanceCounter(&end);
        CHECK(!memcmp(&before, &after, sizeof(before)));
        printf("trial=%u calls=8388608 reference_ms=%.3f cached_ms=%.3f sum=%.9g\n",
               trial + 1, 1000.0 * (middle.QuadPart - begin.QuadPart) / frequency.QuadPart,
               1000.0 * (end.QuadPart - middle.QuadPart) / frequency.QuadPart, after);
    }
}

int main(int argc, char **argv)
{
    MCPXAPUState *d = calloc(1, sizeof(*d));
    MCPXAPUState *other = calloc(1, sizeof(*other));
    CHECK(d && other);
    mcpx_apu_vp_init(d);
    CHECK(pow_calls == 4095);
    check_all_inputs(d);
    CHECK(pow_calls == 4095);
    CHECK(attenuate(d, 0) == 1.0f && attenuate(d, 0xFFF) == 0.0f);
    CHECK(attenuate(d, 0xFFE) > 0.0f);
    mcpx_apu_vp_reset(d);
    check_all_inputs(d);
    CHECK(pow_calls == 4095);
    mcpx_apu_vp_init(other);
    CHECK(pow_calls == 8190);
    CHECK(!memcmp(d->vp.attenuation, other->vp.attenuation, sizeof(d->vp.attenuation)));
    {
        int saved_rounding = fegetround();
        const int modes[] = {FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO, FE_TONEAREST};
        float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME] = {{0}};
        d->regs[NV_PAPU_TVL2D] = d->regs[NV_PAPU_TVL3D] = d->regs[NV_PAPU_TVLMP] = 0xFFFF;
        for (unsigned mode = 0; mode < sizeof(modes) / sizeof(modes[0]); ++mode) {
            CHECK(!fesetround(modes[mode]));
            mcpx_apu_vp_frame(d, mixbins);
            uint64_t warmed_calls = pow_calls;
            check_all_inputs(d);
            mcpx_apu_vp_frame(d, mixbins);
            CHECK(pow_calls == warmed_calls);
        }
        CHECK(!fesetround(saved_rounding));
        mcpx_apu_vp_frame(d, mixbins);
    }
    if (argc > 1 && !strcmp(argv[1], "--bench")) benchmark(d);
    puts("PASS: 65536 encodings in all 4 rounding modes, gain ramps/PCM, reset, separate instances; no warm powf calls");
    mcpx_apu_vp_finalize(d);
    mcpx_apu_vp_finalize(other);
    free(other);
    free(d);
    return 0;
}
