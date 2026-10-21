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
static uint32_t test_frame_flips;
void xbox_Nv2aFrameCounterFlip(void) { test_frame_flips++; }
int xbox_ContiguousRangeAllocated(uint32_t physical_offset, uint32_t size)
{ (void)physical_offset; (void)size; STUB_HIT("xbox_ContiguousRangeAllocated"); return 0; }
static uint32_t released_dma, released_offset, release_count;
void xbox_Nv2aSemaphoreRelease(uint32_t dma_context, uint32_t semaphore_offset, uint32_t value)
{ released_dma = dma_context; released_offset = semaphore_offset; (void)value; release_count++; }
static unsigned native_nops;
void xbox_Nv2aNop(uint32_t param) { (void)param; native_nops++; }
void nv2a_vsh_set_load_slot(uint32_t slot) { (void)slot; STUB_HIT("nv2a_vsh_set_load_slot"); }
void nv2a_vsh_program_word(uint32_t word) { (void)word; STUB_HIT("nv2a_vsh_program_word"); }
void nv2a_vsh_set_start_slot(uint32_t slot) { (void)slot; STUB_HIT("nv2a_vsh_set_start_slot"); }
void nv2a_vsh_set_constant_load(uint32_t index) { (void)index; STUB_HIT("nv2a_vsh_set_constant_load"); }
void nv2a_vsh_constant_word(uint32_t word) { (void)word; }
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
int nv2a_d3d_init(void) { return 1; }
static uint32_t replay_observed_color;
void nv2a_d3d_clear(const NvD3DState *st, uint32_t flags, uint32_t color, uint32_t zstencil,
                    uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{ (void)st; (void)flags; (void)color; (void)zstencil; (void)x0; (void)y0; (void)x1; (void)y1;
  replay_observed_color = color; }
void nv2a_d3d_flip(uint32_t surface_addr, uint32_t pitch)
{ (void)surface_addr; (void)pitch; STUB_HIT("nv2a_d3d_flip"); }
void nv2a_d3d_flip_present(uint32_t surface_addr, uint32_t pitch, int show)
{ (void)surface_addr; (void)pitch; (void)show; STUB_HIT("nv2a_d3d_flip_present"); }
void nv2a_d3d_zpass_enable(int on) { (void)on; STUB_HIT("nv2a_d3d_zpass_enable"); }
void nv2a_d3d_zpass_clear(void) { STUB_HIT("nv2a_d3d_zpass_clear"); }
uint32_t nv2a_d3d_zpass_read(void) { STUB_HIT("nv2a_d3d_zpass_read"); return 0; }
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{ (void)fb_va; (void)pitch; STUB_HIT("xbox_FramebufferWindowSet"); }
void xbox_FramebufferWindowStart(void) { STUB_HIT("xbox_FramebufferWindowStart"); }
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{ (void)fb_va; (void)pitch; STUB_HIT("xbox_FramebufferWindowPresent"); }
int xbox_PhysicalAddressResolve(uint32_t physical_address, uint32_t bytes, uint32_t *guest_va)
{ (void)physical_address; (void)bytes; (void)guest_va; STUB_HIT("xbox_PhysicalAddressResolve"); return 0; }

static unsigned allocation_count, fail_allocation;
static unsigned realloc_count, fail_reallocation;
static unsigned calloc_count, fail_calloc;
static DWORD test_tick;
static DWORD test_get_tick_count(void) { return test_tick; }
static void *test_malloc(size_t size)
{
    if (++allocation_count == fail_allocation) return NULL;
    return malloc(size);
}
static void *test_realloc(void *ptr, size_t size)
{
    if (++realloc_count == fail_reallocation) return NULL;
    return realloc(ptr, size);
}
static void *test_calloc(size_t count, size_t size)
{
    if (++calloc_count == fail_calloc) return NULL;
    return calloc(count, size);
}
#define malloc test_malloc
#define realloc test_realloc
#define calloc test_calloc
#define GetTickCount test_get_tick_count
#include "../../src/kernel/nv2a_pb_exec.c"
#undef malloc
#undef realloc
#undef calloc
#undef GetTickCount

static int failures;
#define CHECK_EQ(name, got, want) do { \
    uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want); \
    if (g_ != w_) { fprintf(stderr, "FAIL %s: got %u want %u\n", name, g_, w_); failures++; } \
} while (0)

static void test_even_tick_interpolation_timestamp(void)
{
    test_tick = 2000;
    nv2a_pb_interp_enable(1);
    CHECK_EQ("even tick starts interpolation", interp_on(), 1);
    test_tick = 2249;
    CHECK_EQ("interpolation remains enabled for 249ms", interp_on(), 1);
    test_tick = 2250;
    CHECK_EQ("interpolation expires at 250ms", interp_on(), 0);
    nv2a_pb_interp_enable(0);
}

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

static void test_replay_state(void)
{
    const uint32_t recorded_color = 0xFF112233u, next_color = 0xFFAABBCCu;
    NvD3DAttrib attr[NV_VERTEX_ATTRS] = {0};
    uint8_t vertex[16] = {0};

    /* The first frame inherits clear color from its predecessor. The test
     * begins recording at a frame boundary and captures only the clear. */
    test_tick = 3000;
    nv2a_pb_interp_enable(0);
    pb_discard_recording();
    nv2a_pb_exec_method(0, NV097_FLIP_STALL, 0);
    nv2a_pb_exec_method(0, 0x1D90, recorded_color);
    nv2a_pb_exec_method(0, NV097_FLIP_STALL, 0);
    nv2a_pb_interp_enable(1);
    CHECK_EQ("interpolation enabled before capture", interp_on(), 1);
    nv2a_pb_exec_method(0, NV097_CLEAR_SURFACE, 0);
    CHECK_EQ("recording captures method", s_rec_f->ncmd > 0, 1);
    CHECK_EQ("recorded clear uses inherited color", replay_observed_color, recorded_color);
    nv2a_pb_exec_method(0, NV097_FLIP_STALL, 0);
    CHECK_EQ("recorded completed frame is valid", s_last_f->invalid, 0);
    CHECK_EQ("recorded completed frame has start state", s_last_f->start_valid, 1);
    CHECK_EQ("recorded completed frame has commands", s_last_f->ncmd > 0, 1);
    CHECK_EQ("recorded completed frame has pair mask", s_last_f->has != NULL, 1);

    /* The next frame has already changed executor state when the prior frame
     * is replayed between guest commands. Replay must temporarily load the
     * recorded frame's starting state, then restore this newer state. */
    nv2a_pb_exec_method(0, 0x1D90, next_color);
    CHECK_EQ("next-frame color is installed", s_gpu.clear_color, next_color);
    CHECK_EQ("recorded frame replays", nv2a_pb_replay(1.0f), 1);
    CHECK_EQ("replay clear uses recorded inherited color", replay_observed_color, recorded_color);
    CHECK_EQ("replay restores in-progress next-frame color", s_gpu.clear_color, next_color);

    /* If interpolation is enabled after this frame has already begun, its
     * recorded suffix is incomplete and must never be offered for replay. */
    nv2a_pb_interp_enable(0);
    pb_discard_recording();
    nv2a_pb_exec_method(0, NV097_FLIP_STALL, 0);
    nv2a_pb_exec_method(0, 0x1D90, recorded_color);
    nv2a_pb_interp_enable(1);
    nv2a_pb_exec_method(0, NV097_CLEAR_SURFACE, 0);
    nv2a_pb_exec_method(0, NV097_FLIP_STALL, 0);
    CHECK_EQ("mid-frame activation refuses a partial replay", nv2a_pb_replay(1.0f), 0);

    /* A recording can also become incomplete if interpolation expires or is
     * disabled after its first methods, then is re-enabled before the flip. */
    test_tick = 4000;
    nv2a_pb_interp_enable(0);
    pb_discard_recording();
    nv2a_pb_exec_method(0, NV097_FLIP_STALL, 0);
    nv2a_pb_interp_enable(1);
    nv2a_pb_exec_method(0, 0x1D90, recorded_color);
    nv2a_pb_interp_enable(0);
    nv2a_pb_exec_method(0, NV097_CLEAR_SURFACE, 0);
    nv2a_pb_interp_enable(1);
    nv2a_pb_exec_method(0, 0x1D90, next_color);
    nv2a_pb_exec_method(0, NV097_FLIP_STALL, 0);
    CHECK_EQ("disable and re-enable invalidates partial recording", s_last_f->invalid, 1);
    CHECK_EQ("partial recording after re-enable is not replayed", nv2a_pb_replay(1.0f), 0);

    /* The method can be recorded at dispatch entry while its draw payload is
     * captured later. Expiration between those points must invalidate it. */
    test_tick = 6000;
    nv2a_pb_interp_enable(0);
    pb_discard_recording();
    nv2a_pb_exec_method(0, NV097_FLIP_STALL, 0);
    nv2a_pb_interp_enable(1);
    nv2a_pb_exec_method(0, 0x1D90, recorded_color);
    test_tick = 6250;
    attr[0] = (NvD3DAttrib){ 1, 2, 4, 16, vertex };
    pb_draw_vertices(attr, 0, 0);
    CHECK_EQ("expired vertex capture invalidates in-progress frame", s_rec_f->invalid, 1);
    nv2a_pb_interp_enable(1);
    nv2a_pb_exec_method(0, 0x1D90, next_color);
    nv2a_pb_exec_method(0, NV097_FLIP_STALL, 0);
    CHECK_EQ("expired vertex-capture frame remains invalid", s_last_f->invalid, 1);
    CHECK_EQ("expired vertex-capture frame is not replayed", nv2a_pb_replay(1.0f), 0);
    CHECK_EQ("real flip stalls were exercised", test_frame_flips > 0, 1);
}

static void test_invalid_predecessor_clears_pair_mask(void)
{
    PbFrame later = {0}, previous = {0};
    PbSeg later_seg = { 0x1234, 0, 0, 1 }, previous_seg = { 0x1234, 0, 0, 1 };
    uint32_t later_word = 0x3F800000u, previous_word = 0x3F800000u;

    later.tc = &later_word;
    previous.tc = &previous_word;
    later.seg = &later_seg;
    previous.seg = &previous_seg;
    later.ntc = previous.ntc = later.nseg = previous.nseg = 1;
    later.has = (uint8_t *)malloc(1);
    if (!later.has) { failures++; return; }
    *later.has = 0;

    pb_pair(&later, &previous);
    CHECK_EQ("valid predecessor pairs constants", later.has[0], 1);
    previous.invalid = 1;
    pb_pair(&later, &previous);
    CHECK_EQ("invalid predecessor clears reused pair mask", later.has[0], 0);
    previous.invalid = 0;
    pb_pair(&later, &previous);
    CHECK_EQ("valid predecessor pairs after invalid frame", later.has[0], 1);
    free(later.prev);
    free(later.has);
}

static void test_recording_allocation_failures(void)
{
    NvD3DAttrib attr[NV_VERTEX_ATTRS] = {0};
    uint8_t vertex[16] = {0};
    unsigned stage;

    for (stage = 0; stage < 4; ++stage) {
        test_tick = 5000 + stage;
        nv2a_pb_interp_enable(0);
        pb_discard_recording();
        nv2a_pb_exec_method(0, NV097_FLIP_STALL, 0);
        nv2a_pb_interp_enable(1);
        fail_allocation = fail_reallocation = fail_calloc = 0;
        realloc_count = calloc_count = 0;

        /* The first recorded method exercises pb_record's command growth. */
        if (stage == 0) {
            realloc_count = 0;
            fail_reallocation = 1;
            nv2a_pb_exec_method(0, 0x1D90, 0xFF112233u);
            fail_reallocation = 0;
        } else {
            nv2a_pb_exec_method(0, 0x1D90, 0xFF112233u);
            if (stage == 1) {
                /* A recorded constant causes pb_segment to grow its words. */
                nv2a_pb_exec_method(0, 0x0B80, 0x3F800000u);
                realloc_count = 0;
                fail_reallocation = 1;
            } else {
                attr[0] = (NvD3DAttrib){ 1, 2, 4, 16, vertex };
                realloc_count = 0;
                fail_reallocation = stage == 2 ? 1 : 2;
                pb_draw_vertices(attr, 0, 0);
            }
        }
        nv2a_pb_exec_method(0, NV097_FLIP_STALL, 0);
        fail_reallocation = fail_calloc = 0;
        CHECK_EQ("allocation failure invalidates incomplete frame", s_last_f->invalid, 1);
        CHECK_EQ("incomplete frame is not replayed", nv2a_pb_replay(0.5f), 0);
    }
}

static void test_first_frame_captures_initialized_state(void)
{
    uint32_t mask = 0;
    test_tick = 7000;
    nv2a_pb_interp_enable(1);
    nv2a_pb_exec_method(0, NV097_CLEAR_SURFACE, 0);
    nv2a_pb_exec_method(0, NV097_FLIP_STALL, 0);
    CHECK_EQ("first recorded frame is replayable", nv2a_pb_replay(1.0f), 1);
    if (s_last_f->start && s_last_f->start->tail)
        memcpy(&mask, s_last_f->start->tail + GPU_AT(color_mask) - GPU_AT(inline_count), sizeof mask);
    CHECK_EQ("first snapshot includes executor defaults", mask, 0x01010101u);
}

static unsigned observed_nops, ended_nops;
static int test_nop_hook(const NvD3DState *state, uint32_t subchannel, uint32_t value)
{
    if (!state) { ended_nops++; return 0; }
    CHECK_EQ("NOP hook preserves subchannel", subchannel, 0);
    observed_nops++;
    return value == 0x12345678;
}

static void test_nop_extension(void)
{
    unsigned before = native_nops;
    nv2a_pb_set_nop_hook(NULL);
    nv2a_pb_exec_method(0, NV097_NO_OPERATION, 5);
    CHECK_EQ("unregistered NOP retains notify", native_nops, before + 1);
    nv2a_pb_set_nop_hook(test_nop_hook);
    nv2a_pb_exec_method(0, NV097_NO_OPERATION, 5);
    nv2a_pb_exec_method(0, NV097_NO_OPERATION, 0x12345678);
    CHECK_EQ("unrecognized NOP retains notify", native_nops, before + 2);
    CHECK_EQ("hook sees both NOP words", observed_nops, 2);
    nv2a_pb_exec_method(0, NV097_SET_SEMAPHORE_OFFSET, 16);
    CHECK_EQ("interruption ends NOP run", ended_nops, 1);
    nv2a_pb_set_nop_hook(NULL);
}

int main(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "all";

    test_even_tick_interpolation_timestamp();

    if (strcmp(which, "first-frame") == 0) {
        test_first_frame_captures_initialized_state();
        if (failures) return 1;
        puts("nv2a-texaddr: first-frame passed");
        return 0;
    }

    test_state_snapshot();
    test_nop_extension();

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
    if (strcmp(which, "replay") == 0)
    {
        test_replay_state();
        test_invalid_predecessor_clears_pair_mask();
    }
    if (strcmp(which, "allocation") == 0)
        test_recording_allocation_failures();

    if (failures) {
        fprintf(stderr, "nv2a-texaddr: %d failure(s)\n", failures);
        return 1;
    }
    printf("nv2a-texaddr: %s passed\n", which);
    return 0;
}
