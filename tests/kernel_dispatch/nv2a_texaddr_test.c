/* Texture address modes through the real nv2a_pb_exec.c: wrap_coord() for
 * wrap/mirror/clamp, and sample_tex() for border. No GPU is needed; texels
 * come from a local array, and border taps must not read it. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The types the stubs below take, from the headers the executor itself uses. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "nv2a_vsh_interp.h"
#include "nv2a_combiner.h"
#include "nv2a_d3d11.h"
#include "video_player.h"

/* The executor references the rest of the machine. None of it runs here:
 * these checks only reach wrap_coord() and the border path of sample_tex(),
 * so every stub aborts if it is ever called. */
#define STUB_HIT(name) do { fprintf(stderr, "unexpected call: %s\n", name); abort(); } while (0)
/* Texel memory for sample_tex(): a border tap must never reach it, so a regression
 * that samples instead reads 0x00AABBCC and fails the border colour check. */
static uint32_t test_ram[64];
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)test_ram; }
int xbox_FramebufferPaused(void) { return 0; }
void recomp_sampler_watch_current_thread(void) {}
void recomp_sampler_report(double ms) { (void)ms; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
void xbox_Nv2aFrameCounterFlip(void) { STUB_HIT("xbox_Nv2aFrameCounterFlip"); }
int xbox_ContiguousRangeAllocated(uint32_t physical_offset, uint32_t size)
{ (void)physical_offset; (void)size; STUB_HIT("xbox_ContiguousRangeAllocated"); return 0; }
static uint32_t released_dma, released_offset, release_count;
void xbox_Nv2aSemaphoreRelease(uint32_t dma_context, uint32_t semaphore_offset, uint32_t value)
{ released_dma = dma_context; released_offset = semaphore_offset; (void)value; release_count++; }
void xbox_Nv2aNop(uint32_t param) { (void)param; STUB_HIT("xbox_Nv2aNop"); }
void nv2a_vsh_set_load_slot(uint32_t slot) { (void)slot; STUB_HIT("nv2a_vsh_set_load_slot"); }
void nv2a_vsh_program_word(uint32_t word) { (void)word; STUB_HIT("nv2a_vsh_program_word"); }
void nv2a_vsh_set_start_slot(uint32_t slot) { (void)slot; STUB_HIT("nv2a_vsh_set_start_slot"); }
void nv2a_vsh_set_constant_load(uint32_t index) { (void)index; STUB_HIT("nv2a_vsh_set_constant_load"); }
void nv2a_vsh_constant_word(uint32_t word) { (void)word; STUB_HIT("nv2a_vsh_constant_word"); }
static uint32_t test_vsh_state;
size_t nv2a_vsh_state_size(void) { return sizeof test_vsh_state; }
void nv2a_vsh_state_save(void *out) { memcpy(out, &test_vsh_state, sizeof test_vsh_state); }
void nv2a_vsh_state_restore(const void *in) { memcpy(&test_vsh_state, in, sizeof test_vsh_state); }
void nv2a_vsh_set_cxt_write(uint32_t enable) { (void)enable; STUB_HIT("nv2a_vsh_set_cxt_write"); }
int nv2a_vsh_run(const float in[NV2A_VSH_INPUTS][4], Nv2aVshOutput *out)
{ (void)in; (void)out; STUB_HIT("nv2a_vsh_run"); return 0; }
const float *nv2a_vsh_constant(uint32_t index) { (void)index; STUB_HIT("nv2a_vsh_constant"); return NULL; }
void nv2a_vsh_constant_component(uint32_t index, uint32_t comp, uint32_t word)
{ (void)index; (void)comp; (void)word; STUB_HIT("nv2a_vsh_constant_component"); }
void nv2a_vsh_debug_dump(void) { STUB_HIT("nv2a_vsh_debug_dump"); }
void nv2a_rc_eval(const Nv2aCombiner *rc, const float v0[4], const float v1[4],
                  const float fog[4], const float t[4][4], float out[4])
{ (void)rc; (void)v0; (void)v1; (void)fog; (void)t; (void)out; STUB_HIT("nv2a_rc_eval"); }
void nv2a_rc_unpack(uint32_t argb, float out[4]) { (void)argb; (void)out; STUB_HIT("nv2a_rc_unpack"); }
void nv2a_d3d_draw_ub(const NvD3DState *st, int topology, const NvD3DAttrib attr[16],
                      const float def[16][4], int program, const uint32_t *idx, uint32_t ni,
                      uint32_t lo, uint32_t hi, const float (*pos)[4])
{ (void)st; (void)topology; (void)attr; (void)def; (void)program; (void)idx; (void)ni;
  (void)lo; (void)hi; (void)pos; STUB_HIT("nv2a_d3d_draw_ub"); }
int nv2a_d3d_widescreen(void) { STUB_HIT("nv2a_d3d_widescreen"); return 0; }
int nv2a_d3d_menu_rects_on(void) { STUB_HIT("nv2a_d3d_menu_rects_on"); return 0; }
void nv2a_d3d_set_menu_tints(const uint32_t *tints) { (void)tints; STUB_HIT("nv2a_d3d_set_menu_tints"); }
int nv2a_d3d_init(void) { STUB_HIT("nv2a_d3d_init"); return 0; }
void nv2a_d3d_distance_fog(const NvD3DState *st, float depth_to_world, float far_clip,
                           float start, float end, uint32_t argb, uint32_t flags,
                           const float sky_fade[4])
{ (void)st; (void)depth_to_world; (void)far_clip; (void)start; (void)end; (void)argb;
  (void)flags; (void)sky_fade; STUB_HIT("nv2a_d3d_distance_fog"); }
void nv2a_d3d_clear(const NvD3DState *st, uint32_t flags, uint32_t color, uint32_t zstencil,
                    uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{ (void)st; (void)flags; (void)color; (void)zstencil; (void)x0; (void)y0; (void)x1; (void)y1;
  STUB_HIT("nv2a_d3d_clear"); }
void nv2a_d3d_flip(uint32_t surface_addr, uint32_t pitch)
{ (void)surface_addr; (void)pitch; STUB_HIT("nv2a_d3d_flip"); }
void nv2a_d3d_zpass_enable(int on) { (void)on; STUB_HIT("nv2a_d3d_zpass_enable"); }
void nv2a_d3d_zpass_clear(void) { STUB_HIT("nv2a_d3d_zpass_clear"); }
uint32_t nv2a_d3d_zpass_read(void) { STUB_HIT("nv2a_d3d_zpass_read"); return 0; }
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{ (void)fb_va; (void)pitch; STUB_HIT("xbox_FramebufferWindowSet"); }
void xbox_FramebufferWindowStart(void) { STUB_HIT("xbox_FramebufferWindowStart"); }
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; STUB_HIT("xbox_FramebufferWindowFrameStats"); }
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{ (void)fb_va; (void)pitch; STUB_HIT("xbox_FramebufferWindowPresent"); }

static unsigned allocation_count, fail_allocation;
static void *test_malloc(size_t size)
{
    if (++allocation_count == fail_allocation) return NULL;
    return malloc(size);
}
#define malloc test_malloc
#include "../../src/kernel/nv2a_pb_exec.c"
#undef malloc

static int failures;
#define CHECK_EQ(name, got, want) do { \
    uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want); \
    if (g_ != w_) { fprintf(stderr, "FAIL %s: got %u want %u\n", name, g_, w_); failures++; } \
} while (0)

static void test_state_snapshot(void)
{
    PbState state = {0};
    s_gpu.idx_count = s_gpu.inline_count = 1;
    s_gpu.idx[0] = 17;
    s_gpu.inline_buf[0] = 23;
    test_vsh_state = 31;
    for (unsigned fail = 1; fail <= 6; fail++) {
        allocation_count = 0;
        fail_allocation = fail;
        CHECK_EQ("snapshot refuses each failed allocation", pb_state(&state, 1), 0);
        CHECK_EQ("failed snapshot releases partial storage",
                 !state.head && !state.mid && !state.tail && !state.vsh && !state.idx && !state.inl, 1);
        CHECK_EQ("failed snapshot preserves GPU indices", s_gpu.idx[0], 17);
        CHECK_EQ("failed snapshot preserves shader state", test_vsh_state, 31);
    }
    fail_allocation = 0;
    CHECK_EQ("snapshot retries after allocation failure", pb_state(&state, 1), 1);
    s_gpu.idx_count = s_gpu.inline_count = 0;
    s_gpu.idx[0] = s_gpu.inline_buf[0] = test_vsh_state = 0;
    CHECK_EQ("snapshot restores", pb_state(&state, 0), 1);
    CHECK_EQ("restores index count", s_gpu.idx_count, 1);
    CHECK_EQ("restores inline count", s_gpu.inline_count, 1);
    CHECK_EQ("restores indices", s_gpu.idx[0], 17);
    CHECK_EQ("restores inline data", s_gpu.inline_buf[0], 23);
    CHECK_EQ("restores shader state", test_vsh_state, 31);
    free(state.head); free(state.mid); free(state.tail);
    free(state.vsh); free(state.idx); free(state.inl);
}

/* Coordinates are passed as the uint32 the sampler computes (signed in
 * practice), so -1 is 0xFFFFFFFF. */
static void test_wrap_and_mirror(void)
{
    /* Power-of-two size: mirror period 8. */
    CHECK_EQ("mirror4 0", wrap_coord(0, 4, 2), 0);
    CHECK_EQ("mirror4 3", wrap_coord(3, 4, 2), 3);
    CHECK_EQ("mirror4 4", wrap_coord(4, 4, 2), 3);
    CHECK_EQ("mirror4 5", wrap_coord(5, 4, 2), 2);
    CHECK_EQ("mirror4 7", wrap_coord(7, 4, 2), 0);
    CHECK_EQ("mirror4 8", wrap_coord(8, 4, 2), 0);
    CHECK_EQ("mirror4 9", wrap_coord(9, 4, 2), 1);
    CHECK_EQ("mirror4 -1", wrap_coord((uint32_t)-1, 4, 2), 0);
    CHECK_EQ("mirror4 -2", wrap_coord((uint32_t)-2, 4, 2), 1);
    CHECK_EQ("mirror4 -4", wrap_coord((uint32_t)-4, 4, 2), 3);
    CHECK_EQ("mirror4 -5", wrap_coord((uint32_t)-5, 4, 2), 3);
    CHECK_EQ("mirror4 -6", wrap_coord((uint32_t)-6, 4, 2), 2);

    /* Non-power-of-two size: mirror period 6. */
    CHECK_EQ("mirror3 2", wrap_coord(2, 3, 2), 2);
    CHECK_EQ("mirror3 3", wrap_coord(3, 3, 2), 2);
    CHECK_EQ("mirror3 4", wrap_coord(4, 3, 2), 1);
    CHECK_EQ("mirror3 5", wrap_coord(5, 3, 2), 0);
    CHECK_EQ("mirror3 6", wrap_coord(6, 3, 2), 0);
    CHECK_EQ("mirror3 -1", wrap_coord((uint32_t)-1, 3, 2), 0);
    CHECK_EQ("mirror3 -3", wrap_coord((uint32_t)-3, 3, 2), 2);

    /* Wrap is unchanged. */
    CHECK_EQ("wrap4 4", wrap_coord(4, 4, 1), 0);
    CHECK_EQ("wrap4 -1", wrap_coord((uint32_t)-1, 4, 1), 3);
    CHECK_EQ("wrap3 -1", wrap_coord((uint32_t)-1, 3, 1), 2);

    /* Clamp-to-edge (3) and the fallback for unlisted modes both clamp. */
    CHECK_EQ("clamp 9", wrap_coord(9, 4, 3), 3);
    CHECK_EQ("clamp -1", wrap_coord((uint32_t)-1, 4, 3), 0);
}

static void test_border(void)
{
    Texture t;
    uint32_t argb = 0;

    memset(&t, 0, sizeof t);
    t.valid = 1;
    t.width = 4;
    t.height = 4;
    t.color = 0x12;                       /* LIN_A8R8G8B8: linear, no DXT */
    t.pitch = 16;
    t.border = 0xFF112233u;
    {
        size_t i;
        for (i = 0; i < sizeof test_ram / sizeof test_ram[0]; i++)
            test_ram[i] = 0x00AABBCCu;    /* what an in-range texel would return */
    }
    t.addr_u = 4;
    t.addr_v = 1;

    /* Outside on u reads the border colour, without touching memory. */
    CHECK_EQ("border u=4 returns", sample_tex(&t, 0, 4, 0, &argb), 1);
    CHECK_EQ("border u=4 colour", argb, 0xFF112233u);
    CHECK_EQ("border u=-1 colour", (sample_tex(&t, 0, (uint32_t)-1, 0, &argb), argb), 0xFF112233u);

    /* Border on v alone, with u in range: an outside v reads the border colour.
     * (An in-range texel would read guest memory, which this harness does not
     * map, so only the border path is exercised here.) */
    t.addr_u = 1;
    t.addr_v = 4;
    CHECK_EQ("v border returns", sample_tex(&t, 0, 0, 4, &argb), 1);
    CHECK_EQ("v border colour", argb, 0xFF112233u);
}

int main(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "all";

    test_state_snapshot();

    nv2a_pb_exec_method(0, NV097_SET_CONTEXT_DMA_SEMAPHORE, 7);
    nv2a_pb_exec_method(0, NV097_SET_SEMAPHORE_OFFSET, 16);
    s_replaying = 1;
    nv2a_pb_exec_method(0, NV097_SET_CONTEXT_DMA_SEMAPHORE, 9);
    nv2a_pb_exec_method(0, NV097_SET_SEMAPHORE_OFFSET, 32);
    nv2a_pb_exec_method(0, NV097_BACK_END_WRITE_SEMAPHORE_RELEASE, 1);
    CHECK_EQ("replay never publishes a semaphore", release_count, 0);
    s_replaying = 0;
    nv2a_pb_exec_method(0, NV097_BACK_END_WRITE_SEMAPHORE_RELEASE, 2);
    CHECK_EQ("title publishes once", release_count, 1);
    CHECK_EQ("replay preserves semaphore DMA", released_dma, 7);
    CHECK_EQ("replay preserves semaphore offset", released_offset, 16);

    if (strcmp(which, "wrap") == 0 || strcmp(which, "all") == 0)
        test_wrap_and_mirror();
    if (strcmp(which, "border") == 0 || strcmp(which, "all") == 0)
        test_border();

    if (failures) {
        fprintf(stderr, "nv2a-texaddr: %d failure(s)\n", failures);
        return 1;
    }
    printf("nv2a-texaddr: %s passed\n", which);
    return 0;
}
