/* SDL3 GPU counterpart of the shader lookup and vertex upload regressions.
 *
 * The SDL renderer has no specialised pixel-shader table and no background
 * compile queue (every pipeline is created synchronously on first use). Its
 * equivalent of the D3D11 shader table is the pipeline cache in pipeline():
 * these cases check that it deduplicates identical keys, keeps distinct keys
 * distinct across all 4096 slots, replaces a home slot once full, and returns
 * a usable pipeline immediately. The vertex/index ring cases fail the Map of
 * the transfer buffer that holds the draw's data and require the next draw to
 * retry without drawing stale data. */
#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Failure injection for the ring uploads: the Map of one transfer buffer. */
static SDL_GPUTransferBuffer *fail_map_buffer;
static int fail_map_count;

static void *test_map_transfer(SDL_GPUDevice *device, SDL_GPUTransferBuffer *buffer, bool cycle)
{
    if (fail_map_count && buffer == fail_map_buffer) {
        fail_map_count--;
        return NULL;
    }
    return (SDL_MapGPUTransferBuffer)(device, buffer, cycle);
}

#define SDL_MapGPUTransferBuffer(device, buffer, cycle) test_map_transfer(device, buffer, cycle)
#include "../../src/video/nv2a_sdlgpu.c"
#include "../nv2a_sdlgpu_test_host.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static uint8_t ram[0x100000];

/* Distinct keys: the bias field is part of the key. */
static PipeKey number_key(unsigned i)
{
    PipeKey k;
    pipe_key_plain(&k, SH_CLEAR_VS, SH_CLEAR_PS, FMT_BGRA8, SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP);
    k.bias = (int32_t)i;
    return k;
}

/* Whether the cache holds this key, found by the same probe pipeline() uses
 * but without creating anything. */
static int resident(const PipeKey *k)
{
    uint64_t h = hash_mem((const uint8_t *)k, sizeof *k);
    unsigned n;
    for (n = 0; n < PIPE_SLOTS; n++) {
        const PipeSlot *s = &s_pipes[(h + n) & (PIPE_SLOTS - 1)];
        if (!s->pipe) return 0;
        if (!memcmp(&s->key, k, sizeof *k)) return 1;
    }
    return 0;
}

static int test_pipelines(void)
{
    static PipeKey keys[PIPE_SLOTS + 1];
    static SDL_GPUGraphicsPipeline *first[PIPE_SLOTS + 1];
    unsigned i, changed = 0;
    for (i = 0; i <= PIPE_SLOTS; i++) keys[i] = number_key(i);
    /* Fill every slot. Each key yields a usable pipeline on the first request. */
    for (i = 0; i < PIPE_SLOTS; i++) {
        first[i] = pipeline(&keys[i]);
        CHECK(first[i]);
    }
    /* Repeated lookups, all 4096 occupied slots: each returns its own pipeline. */
    for (i = 0; i < PIPE_SLOTS; i++) CHECK(pipeline(&keys[i]) == first[i]);
    /* Distinct keys never share a pipeline. */
    for (i = 1; i < PIPE_SLOTS; i += 97) CHECK(first[i] != first[i - 1]);
    /* A key past capacity replaces its home slot: exactly one earlier key
     * leaves the table. Residency is read from the table itself, so a lookup
     * (which would displace another key) does not disturb the count. */
    CHECK(resident(&keys[0]) && resident(&keys[PIPE_SLOTS - 1]));
    first[PIPE_SLOTS] = pipeline(&keys[PIPE_SLOTS]);
    CHECK(first[PIPE_SLOTS]);
    CHECK(resident(&keys[PIPE_SLOTS]));
    for (i = 0; i < PIPE_SLOTS; i++)
        if (!resident(&keys[i])) changed++;
    CHECK(changed == 1);
    CHECK(pipeline(&keys[PIPE_SLOTS]) == first[PIPE_SLOTS]);
    puts("PASS: 4096 pipeline keys deduplicate and stay distinct; a key past capacity displaces exactly one slot");
    return 0;
}

static int test_rings(void)
{
    NvD3DState st = {0};
    uint32_t green = 0xFF00FF00u, red = 0xFFFF0000u, blue = 0xFF0000FFu;
    uint32_t black = 0xFF000000u;
    st.color_addr = 0x10000; st.color_pitch = 64 * 4;
    st.surface_format = 0x28; st.clip_w = st.clip_h = 64;
    st.color_mask = 0x01010101;
    nv2a_test_ram = ram;
    nv2a_d3d_clear(&st, 0xF0, black, 0, 0, 0, 0xFFFF, 0xFFFF);
    nv2a_test_triangle(&st, green, 0);
    CHECK(nv2a_test_target_pixel(st.color_addr, 32, 32) == green);
    /* Index upload fails: the draw is skipped, nothing stale is drawn. */
    nv2a_d3d_clear(&st, 0xF0, black, 0, 0, 0, 0xFFFF, 0xFFFF);
    fail_map_buffer = s_itb; fail_map_count = 1;
    nv2a_test_triangle(&st, red, 0);
    CHECK(fail_map_count == 0);
    CHECK(nv2a_test_target_pixel(st.color_addr, 32, 32) == black);
    /* The retry in the same kick draws the new colour. */
    nv2a_test_triangle(&st, red, 0);
    CHECK(nv2a_test_target_pixel(st.color_addr, 32, 32) == red);
    /* Vertex upload fails: skipped, and the previous colour does not persist. */
    nv2a_d3d_clear(&st, 0xF0, black, 0, 0, 0, 0xFFFF, 0xFFFF);
    fail_map_buffer = s_vtb; fail_map_count = 1;
    nv2a_test_triangle(&st, blue, 0);
    CHECK(fail_map_count == 0);
    CHECK(nv2a_test_target_pixel(st.color_addr, 32, 32) == black);
    nv2a_test_triangle(&st, blue, 0);
    CHECK(nv2a_test_target_pixel(st.color_addr, 32, 32) == blue);
    fail_map_buffer = NULL;
    puts("PASS: failed vertex and index Maps skip the draw without stale pixels and the retry draws the new colour");
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    nv2a_test_ram = ram;
    CHECK(nv2a_test_init());
    if (!strcmp(argv[1], "pipelines")) return test_pipelines();
    if (!strcmp(argv[1], "rings")) return test_rings();
    fprintf(stderr, "unknown case %s\n", argv[1]);
    return 1;
}
