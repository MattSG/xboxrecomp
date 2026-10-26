/* The SDL3 GPU renderer's batched GPU readback, the counterpart of test_main.c.
 * A batch of surface copies is downloaded through one fence. While that fence is
 * unfinished, polls query it and never map the download buffers. Failures of the
 * fence and of the buffer map are injected into the renderer's own calls. The
 * pixels are checked in guest memory: format conversion, pitch padding and
 * later CPU rows. Each case runs in its own process. */
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int inspect, hold_fence, fail_fence, fail_map, fail_texture;
static unsigned fence_polls, fence_submits, maps, wait_idles;

static bool (*const real_query_fence)(SDL_GPUDevice *, SDL_GPUFence *) = SDL_QueryGPUFence;
static SDL_GPUFence *(*const real_submit_fence)(SDL_GPUCommandBuffer *) = SDL_SubmitGPUCommandBufferAndAcquireFence;
static void *(*const real_map_transfer)(SDL_GPUDevice *, SDL_GPUTransferBuffer *, bool) = SDL_MapGPUTransferBuffer;
static void (*const real_wait_idle)(SDL_GPUDevice *) = SDL_WaitForGPUIdle;
static SDL_GPUTexture *(*const real_create_texture)(SDL_GPUDevice *, const SDL_GPUTextureCreateInfo *) = SDL_CreateGPUTexture;
static bool test_query_fence(SDL_GPUDevice *dev, SDL_GPUFence *fence);
static SDL_GPUFence *test_submit_fence(SDL_GPUCommandBuffer *c);
static void *test_map_transfer(SDL_GPUDevice *dev, SDL_GPUTransferBuffer *tb, bool cycle);
static void test_wait_idle(SDL_GPUDevice *dev);
static SDL_GPUTexture *test_create_texture(SDL_GPUDevice *dev, const SDL_GPUTextureCreateInfo *ci);
#define SDL_QueryGPUFence test_query_fence
#define SDL_SubmitGPUCommandBufferAndAcquireFence test_submit_fence
#define SDL_MapGPUTransferBuffer test_map_transfer
#define SDL_WaitForGPUIdle test_wait_idle
#define SDL_CreateGPUTexture test_create_texture

#include "../../src/video/nv2a_sdlgpu.c"
#include "../nv2a_test_host.h"
#include "../nv2a_sdlgpu_host.h"

static bool test_query_fence(SDL_GPUDevice *dev, SDL_GPUFence *fence)
{
    if (inspect) fence_polls++;
    if (hold_fence) return false;
    return real_query_fence(dev, fence);
}
static SDL_GPUFence *test_submit_fence(SDL_GPUCommandBuffer *c)
{
    if (inspect) fence_submits++;
    if (inspect && fail_fence) {
        /* The work is submitted; its fence could not be acquired. */
        fail_fence--;
        SDL_SubmitGPUCommandBuffer(c);
        return NULL;
    }
    return real_submit_fence(c);
}
static void *test_map_transfer(SDL_GPUDevice *dev, SDL_GPUTransferBuffer *tb, bool cycle)
{
    if (inspect) {
        maps++;
        if (fail_map) { fail_map--; return NULL; }
    }
    return real_map_transfer(dev, tb, cycle);
}
static void test_wait_idle(SDL_GPUDevice *dev)
{
    if (inspect) wait_idles++;
    real_wait_idle(dev);
}
/* Only the downsample target (colour target and sampler) can fail here. */
static SDL_GPUTexture *test_create_texture(SDL_GPUDevice *dev, const SDL_GPUTextureCreateInfo *ci)
{
    if (inspect && fail_texture && ci->usage == USAGE_TARGET) {
        fail_texture = 0;
        return NULL;
    }
    return real_create_texture(dev, ci);
}

static uint8_t *ram;
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)ram; }
int xbox_FramebufferScanSource(uint32_t *va, uint32_t *pitch) { return 0; }
int xbox_FramebufferOverlay(XboxOverlay *ovl) { return 0; }
void xbox_FramebufferNoteFlip(uint32_t draws) {}
void xbox_FramebufferRenderHostOverlay(void *device, void *context, void *target) { (void)device; (void)context; (void)target; }
const uint32_t (*nv2a_vsh_program(uint32_t *start, uint32_t *version))[4] { return NULL; }
const float (*nv2a_vsh_constants(uint32_t *version))[4] { return NULL; }

static void reset_counts(void)
{
    fence_polls = fence_submits = maps = wait_idles = 0;
}

static Surface *new_surface(unsigned i, unsigned bpp, unsigned w, unsigned h, unsigned padding)
{
    uint32_t va = 0x10000 + i * 0x4000, pitch = w * bpp + padding;
    memset(ram + va, 0x55, (size_t)pitch * h);
    return surface_get(va, pitch, bpp == 2 ? 3 : 8, w, h, 0);
}

/* Clears a surface to green or red on the GPU. */
static void draw_color(Surface *s, int green)
{
    NvD3DState st = {0};
    st.color_addr = s->va; st.color_pitch = s->pitch;
    st.surface_format = s->color_fmt; st.clip_w = s->w; st.clip_h = s->h;
    st.color_mask = 0x01010101;
    nv2a_d3d_clear(&st, 0xF0 | 0x80, green ? 0xFF00FF00u : 0xFFFF0000u, 0, 0, 0, s->w - 1, s->h - 1);
    flush_batch();
}

static void complete(void)
{
    Uint64 deadline = SDL_GetTicks() + 5000;
    while (nv2a_d3d_writeback_busy()) {
        CHECK(SDL_GetTicks() < deadline);
        SDL_Delay(0);
    }
    CHECK(!s_wb_mask && !s_wb_pending);
}

static void pixels_match(Surface *s, int green, int later_row)
{
    unsigned x, y;
    for (y = 0; y < s->h; y++) {
        const uint8_t *row = ram + s->va + (size_t)y * s->pitch;
        for (x = 0; x < s->w; x++) {
            if (s->bpp == 4) {
                uint32_t value, expected = later_row && y == 0 ? 0xFF0000FFu : green ? 0xFF00FF00u : 0xFFFF0000u;
                memcpy(&value, row + x * 4, 4);
                CHECK(value == expected);
            } else {
                uint16_t value, expected = later_row && y == 0 ? 0x001Fu : green ? 0x07E0u : 0xF800u;
                memcpy(&value, row + x * 2, 2);
                CHECK(value == expected);
            }
        }
        for (x = s->w * s->bpp; x < s->pitch; x++) CHECK(row[x] == 0x55);
    }
}

static void late_blue_row(Surface *s)
{
    unsigned x;
    uint8_t *row = ram + s->va;
    for (x = 0; x < s->w; x++) {
        if (s->bpp == 4) { uint32_t blue = 0xFF0000FFu; memcpy(row + x * 4, &blue, 4); }
        else { uint16_t blue = 0x001Fu; memcpy(row + x * 2, &blue, 2); }
    }
}

static void test_batch(void)
{
    Surface *surfaces[8];
    unsigned i;
    for (i = 0; i < 8; i++) {
        surfaces[i] = new_surface(i, i & 1 ? 2 : 4, i & 2 ? 32 : 16, i & 4 ? 16 : 8, 16);
        CHECK(surfaces[i]);
        CHECK(copy_begin()); /* upload/copy still open when a clear changes targets */
        draw_color(surfaces[i], i & 1);
        CHECK(!s_copy);
    }
    reset_counts(); inspect = 1;
    writeback_surfaces();
    CHECK(s_wb_pending && fence_submits == 1 && s_wb_fence);
    CHECK(s_wb_mask == 0xFF);
    hold_fence = 1;
    for (i = 0; i < 64; i++) CHECK(nv2a_d3d_writeback_busy());
    /* One fence query per unfinished poll, and no download buffer is mapped. */
    CHECK(fence_polls == 64 && maps == 0 && fence_submits == 1);
    for (i = 0; i < 8; i++) late_blue_row(surfaces[i]);
    hold_fence = 0;
    complete();
    CHECK(maps == 8 && fence_submits == 1);
    for (i = 0; i < 8; i++) pixels_match(surfaces[i], i & 1, 1);
    inspect = 0;
    puts("PASS: eight GPU copies in one fence, one query per unfinished poll and zero maps, exact 16/32-bit pixels, padding and late-row merge");
}

/* A failed fence acquisition: the work is submitted, so the readback must wait
 * for the GPU before it maps the download buffer. */
static void test_fence_create_retry(void)
{
    Surface *s = new_surface(0, 4, 16, 16, 16);
    CHECK(s);
    draw_color(s, 0);
    reset_counts(); inspect = 1; fail_fence = 1;
    writeback_surfaces();
    CHECK(!fail_fence && !s_wb_fence && s_wb_pending);
    CHECK(wait_idles >= 1);            /* the copy finishes before the map */
    complete();
    CHECK(maps == 1);
    pixels_match(s, 0, 0);
    /* The next batch acquires its fence as usual. */
    draw_color(s, 1);
    reset_counts();
    writeback_surfaces();
    CHECK(fence_submits == 1 && s_wb_fence);
    complete();
    pixels_match(s, 1, 0);
    inspect = 0;
    puts("PASS: a failed fence acquisition waits for the GPU before mapping; the next batch uses its fence");
}

/* A download buffer that is busy when mapped keeps its copy for a later poll;
 * the later CPU row survives. */
static void test_map_retry(void)
{
    Surface *s = new_surface(0, 4, 16, 16, 16);
    CHECK(s);
    draw_color(s, 0);
    reset_counts(); inspect = 1;
    writeback_surfaces();
    /* Wait for the copy itself, then make the first map fail. */
    CHECK(SDL_WaitForGPUFences(s_dev, true, &s_wb_fence, 1));
    SDL_ReleaseGPUFence(s_dev, s_wb_fence);
    s_wb_fence = NULL;
    fail_map = 1;
    CHECK(nv2a_d3d_writeback_busy());
    CHECK(s_wb_pending && s_wb_mask == 1 && s_wb[0].pending);
    late_blue_row(s);
    complete();
    CHECK(maps == 2);
    pixels_match(s, 0, 1);
    inspect = 0;
    puts("PASS: a busy download map keeps its copy for a later poll; the later CPU row survives");
}

static void test_drop_resize(void)
{
    Surface *s[2], *grown;
    unsigned i, j;
    for (i = 0; i < 2; i++) {
        s[i] = new_surface(i, 4, 16, 16, 0);
        CHECK(s[i]);
        draw_color(s[i], 0);
    }
    inspect = 1; reset_counts(); writeback_surfaces(); hold_fence = 1;
    CHECK(nv2a_d3d_writeback_busy());
    surface_release(s[0]);
    CHECK(s_wb_mask == 2 && s_wb_pending);
    CHECK(nv2a_d3d_writeback_busy());
    surface_release(s[1]);
    CHECK(!s_wb_mask && !s_wb_pending);
    CHECK(!nv2a_d3d_writeback_busy());
    CHECK(!maps);
    hold_fence = 0;
    s[0] = new_surface(0, 4, 16, 16, 0);
    CHECK(s[0]);
    draw_color(s[0], 0);
    writeback_surfaces();
    CHECK(s_wb_pending);
    /* Same address and pitch, taller: the old slot's pixels are cancelled. */
    for (j = 0; j < 16 * 32; j++) ((uint32_t *)(ram + s[0]->va))[j] = 0xFF0000FFu;
    grown = surface_get(s[0]->va, 64, 8, 16, 32, 0);
    CHECK(grown == s[0]);
    CHECK(!nv2a_d3d_writeback_busy());
    for (j = 0; j < 16 * 32; j++) CHECK(((uint32_t *)(ram + grown->va))[j] == 0xFF0000FFu);
    draw_color(grown, 1);
    writeback_surfaces();
    CHECK(s_wb[grown - s_surf].h == 32 && s_wb_pending);
    complete();
    pixels_match(grown, 1, 0);
    inspect = 0;
    puts("PASS: dropping all copied resources cancels the wait; a resized address never inherits old pixels and reuses a resized staging slot");
}

/* The render target is scaled: its copy goes through a downsampled target. If
 * that target cannot be created, the surface stays dirty for the next batch. */
static void test_down_retry(void)
{
    Surface *s;
    s_scale = 2;
    s = new_surface(0, 4, 16, 16, 0);
    CHECK(s && s->iw == 32 && s->ih == 32);
    draw_color(s, 0);
    inspect = 1; reset_counts();
    fail_texture = 1;
    writeback_surfaces();
    CHECK(!fail_texture && s->gpu_dirty && !s_wb_pending);
    CHECK(!s_wb[0].pending && !fence_submits && !s_wb[0].down);
    writeback_surfaces();
    CHECK(!s->gpu_dirty && s_wb_pending && s_wb[0].down && fence_submits == 1);
    complete();
    pixels_match(s, 0, 0);
    inspect = 0;
    puts("PASS: a failed downsample target keeps the surface dirty; the next batch retries and reads the exact pixels");
}

static void test_common(void)
{
    nv2a_common_checks();
    puts("PASS: shared layout, texel and draw helpers (nv2a_render_common.h)");
}

int main(int argc, char **argv)
{
    CHECK(argc >= 2);
    ram = calloc(1, 0x100000);
    CHECK(ram);
    test_setenv("RECOMP_RENDER_SCALE", "1");
    test_setenv("RECOMP_ASPECT", "4:3");
    test_setenv("RECOMP_FPS_LIMIT", "0");
    if (!strcmp(argv[1], "common")) { test_common(); return 0; }
    CHECK(nv2a_d3d_init());
    if (!strcmp(argv[1], "batch")) test_batch();
    else if (!strcmp(argv[1], "fence_create_retry")) test_fence_create_retry();
    else if (!strcmp(argv[1], "map_retry")) test_map_retry();
    else if (!strcmp(argv[1], "drop_resize")) test_drop_resize();
    else if (!strcmp(argv[1], "down_retry")) test_down_retry();
    else CHECK(0);
    return 0;
}
