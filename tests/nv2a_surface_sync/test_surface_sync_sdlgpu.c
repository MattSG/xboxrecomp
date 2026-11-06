/* The SDL3 GPU renderer's surface sync, the counterpart of test_surface_sync.c.
 * Failed uploads must retain pending CPU writes and retry in the same epoch.
 * Allocation, texture creation and upload-buffer map failures are injected
 * into the renderer's own calls, and the GPU pixels are read back. SDL has no
 * separate shader-resource view object, so the SRV cases of the D3D11 test have
 * no counterpart here; the texture cases cover the same upload step. */
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int inspect, fail_malloc, fail_realloc, fail_texture, fail_upload_map;
static unsigned allocations, updates, draws;
static uint32_t *late_write;
static int controlled_tick;
static Uint64 tick_now;

static Uint64 (*const real_get_ticks)(void) = SDL_GetTicks;
static Uint64 test_get_ticks(void) { return controlled_tick ? tick_now : real_get_ticks(); }
static SDL_GPUTexture *(*const real_create_texture)(SDL_GPUDevice *, const SDL_GPUTextureCreateInfo *) = SDL_CreateGPUTexture;
static void *(*const real_map_transfer)(SDL_GPUDevice *, SDL_GPUTransferBuffer *, bool) = SDL_MapGPUTransferBuffer;
static void (*const real_upload)(SDL_GPUCopyPass *, const SDL_GPUTextureTransferInfo *, const SDL_GPUTextureRegion *, bool) = SDL_UploadToGPUTexture;
static void (*const real_draw)(SDL_GPURenderPass *, Uint32, Uint32, Uint32, Uint32) = SDL_DrawGPUPrimitives;

static Uint64 test_ticks_wrapper(void);
static SDL_GPUTexture *test_create_texture(SDL_GPUDevice *dev, const SDL_GPUTextureCreateInfo *ci);
static void *test_map_transfer(SDL_GPUDevice *dev, SDL_GPUTransferBuffer *tb, bool cycle);
static void test_upload(SDL_GPUCopyPass *cp, const SDL_GPUTextureTransferInfo *src, const SDL_GPUTextureRegion *dst, bool cycle);
static void test_draw(SDL_GPURenderPass *rp, Uint32 count, Uint32 instances, Uint32 first, Uint32 first_instance);
static void *test_malloc(size_t bytes);
static void *test_realloc(void *old, size_t bytes);
#define SDL_GetTicks test_ticks_wrapper
#define SDL_CreateGPUTexture test_create_texture
#define SDL_MapGPUTransferBuffer test_map_transfer
#define SDL_UploadToGPUTexture test_upload
#define SDL_DrawGPUPrimitives test_draw
#define malloc test_malloc
#define realloc test_realloc

#include "../../src/video/nv2a_sdlgpu.c"
#include "../nv2a_test_host.h"
#include "../nv2a_sdlgpu_host.h"

#undef malloc
#undef realloc

static Uint64 test_ticks_wrapper(void) { return test_get_ticks(); }

static void *test_malloc(size_t bytes)
{
    if (inspect) { allocations++; if (fail_malloc && !--fail_malloc) return NULL; }
    return malloc(bytes);
}
static void *test_realloc(void *old, size_t bytes)
{
    if (inspect) { allocations++; if (fail_realloc && !--fail_realloc) return NULL; }
    return realloc(old, bytes);
}
static SDL_GPUTexture *test_create_texture(SDL_GPUDevice *dev, const SDL_GPUTextureCreateInfo *ci)
{
    if (inspect && fail_texture && (ci->usage == SDL_GPU_TEXTUREUSAGE_SAMPLER || ci->format == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM) && !--fail_texture)
        return NULL;
    return real_create_texture(dev, ci);
}
static void *test_map_transfer(SDL_GPUDevice *dev, SDL_GPUTransferBuffer *tb, bool cycle)
{
    if (inspect && tb == s_utb) {
        if (late_write) { *late_write = 0xFF00FF00u; late_write = NULL; }
        if (fail_upload_map) { fail_upload_map = 0; return NULL; }
    }
    return real_map_transfer(dev, tb, cycle);
}
static void test_upload(SDL_GPUCopyPass *cp, const SDL_GPUTextureTransferInfo *src, const SDL_GPUTextureRegion *dst, bool cycle)
{
    if (inspect) updates++;
    real_upload(cp, src, dst, cycle);
}
static void test_draw(SDL_GPURenderPass *rp, Uint32 count, Uint32 instances, Uint32 first, Uint32 first_instance)
{
    if (inspect) draws++;
    real_draw(rp, count, instances, first, first_instance);
}

static uint8_t ram[0x100000];
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)ram; }
int xbox_FramebufferScanSource(uint32_t *va, uint32_t *pitch) { return 0; }
int xbox_FramebufferOverlay(XboxOverlay *ovl) { return 0; }
void xbox_FramebufferNoteFlip(uint32_t n) {}
void xbox_FramebufferRenderHostOverlay(void *device, void *context, void *target) { (void)device; (void)context; (void)target; }
const uint32_t (*nv2a_vsh_program(uint32_t *start, uint32_t *version))[4] { return NULL; }
const float (*nv2a_vsh_constants(uint32_t *version))[4] { return NULL; }

/* One pixel of a surface's GPU copy, after all recorded work has run. The
 * work itself is counted, so it runs with the counters armed. */
static uint32_t pixel(Surface *s, unsigned x, unsigned y)
{
    static uint32_t px[64 * 64];
    int saved = inspect;
    flush_batch();
    inspect = 0;
    gpu_download(s->tex, s->iw, s->ih, px);
    inspect = saved;
    return px[y * s->iw + x];
}

static void reset_slot(int k, int texture)
{
    if (texture) release_texture(&s_up_tex[k]);
}
static void clear_gpu(Surface *s)
{
    NvD3DState st = {0};
    st.color_addr = s->va; st.color_pitch = s->pitch;
    st.surface_format = s->color_fmt; st.clip_w = s->w; st.clip_h = s->h;
    st.color_mask = 0x01010101;
    if (s->swizzled) {
        uint32_t l = 0, m = s->w, n = s->h;
        while (m >> 1) { m >>= 1; l++; }
        st.surface_format |= (2u << 8) | (l << 16);
        l = 0; m = s->h;
        while (m >> 1) { m >>= 1; l++; }
        st.surface_format |= l << 24;
    }
    nv2a_d3d_clear(&st, 0xF0, 0xFFFF0000u, 0, 0, 0, s->w - 1, s->h - 1);
    flush_batch();
}
static void clear_snapshot(void)
{
    free(s_sync_rows); s_sync_rows = NULL; s_sync_cap = 0;
}
static void clear_mask(void)
{
    free(s_mask_pixels); s_mask_pixels = NULL; s_mask_cap = 0;
}
static void clear_pixels(void)
{
    free(s_up_pixels); s_up_pixels = NULL; s_up_cap = 0;
}
static void check_distinct_shadows(const Surface *large_surface, const Surface *small_surface)
{
    CHECK(large_surface->shadow && small_surface->shadow && large_surface->shadow != small_surface->shadow);
    CHECK(s_sync_rows != large_surface->shadow && s_sync_rows != small_surface->shadow);
}

static void test_multiple_surfaces(void)
{
    Surface *large_surface = surface_get(0x20000, 64 * 4, 8, 64, 32, 0);
    Surface *small_surface = surface_get(0x30000, 8 * 4, 8, 8, 8, 0);
    uint32_t *large_pixels = (uint32_t *)(ram + 0x20000);
    uint32_t *small_pixels = (uint32_t *)(ram + 0x30000);
    uint32_t generation;
    CHECK(large_surface && small_surface);
    clear_gpu(large_surface); clear_gpu(small_surface);
    inspect = 1;

    large_pixels[31 * 64 + 63] = 0xFF0000FFu;
    surface_sync(large_surface, 1);
    check_distinct_shadows(large_surface, small_surface);
    CHECK(pixel(large_surface, 63, 31) == 0xFF0000FFu && pixel(large_surface, 62, 31) == 0xFFFF0000u);
    CHECK(pixel(small_surface, 7, 7) == 0xFFFF0000u);

    small_pixels[7 * 8 + 7] = 0xFF00FF00u;
    surface_sync(small_surface, 1);
    check_distinct_shadows(large_surface, small_surface);
    CHECK(pixel(small_surface, 7, 7) == 0xFF00FF00u && pixel(small_surface, 6, 7) == 0xFFFF0000u);
    CHECK(pixel(large_surface, 63, 31) == 0xFF0000FFu);

    /* The reusable buffer now came from the smaller surface and must grow. */
    large_pixels[31 * 64 + 62] = 0xFFFFFFFFu;
    surface_sync(large_surface, 1);
    check_distinct_shadows(large_surface, small_surface);
    CHECK(pixel(large_surface, 62, 31) == 0xFFFFFFFFu && pixel(large_surface, 63, 31) == 0xFF0000FFu);
    CHECK(pixel(small_surface, 7, 7) == 0xFF00FF00u);
    CHECK(((uint32_t *)large_surface->shadow)[31 * 64 + 62] == 0xFFFFFFFFu);
    CHECK(((uint32_t *)small_surface->shadow)[7 * 8 + 7] == 0xFF00FF00u);

    surface_release(small_surface);
    CHECK(!small_surface->shadow && s_sync_rows != large_surface->shadow);
    generation = large_surface->gen;
    large_pixels[0] = 0xFF00FF00u;
    fail_upload_map = 1;
    surface_sync(large_surface, 1);
    CHECK(!fail_upload_map && large_surface->gen == generation && !((uint32_t *)large_surface->shadow)[0]);
    CHECK(pixel(large_surface, 0, 0) == 0xFFFF0000u);
    surface_sync(large_surface, 0);
    CHECK(large_surface->gen == generation + 1 && s_sync_rows != large_surface->shadow);
    CHECK(pixel(large_surface, 0, 0) == 0xFF00FF00u && pixel(large_surface, 1, 0) == 0xFFFF0000u);
    CHECK(pixel(large_surface, 62, 31) == 0xFFFFFFFFu && pixel(large_surface, 63, 31) == 0xFF0000FFu);
    flush_batch();
    CHECK(draws == 4);
    surface_release(large_surface);
    clear_snapshot(); /* Must not alias either released surface's allocation. */
    printf("PASS: differently sized surface buffers alternate, grow, release and retry\n");
}

static void test_idle_scanout(void)
{
    Surface *s = surface_get(0x20000, 32 * 4, 8, 32, 32, 0);
    uint32_t *rows = (uint32_t *)(ram + 0x20000);
    uint32_t generation, epoch = s_epoch;
    unsigned previous_updates, previous_draws, previous_allocations;
    CHECK(s && s->shadow);
    clear_gpu(s);
    s_shown = s;
    generation = s->gen;
    controlled_tick = 1;
    tick_now = 1000;
    s_last_flip = tick_now - 100;
    s_last_present = tick_now - 100;
    inspect = 1;

    /* No scanout override and no new GPU commands: the displayed buffer is
     * still live memory while a title waits for Start. */
    rows[7 * 32 + 5] = 0xFF0000FFu;
    nv2a_d3d_tick();
    flush_batch();
    CHECK(s->gen == generation + 1 && draws == 1);
    CHECK(s->cpu_present_pending); /* no window: failed presentation must retry */
    CHECK(pixel(s, 5, 7) == 0xFF0000FFu);
    CHECK(pixel(s, 6, 7) == 0xFFFF0000u); /* preserve unrelated GPU pixels */

    previous_updates = updates; previous_draws = draws; previous_allocations = allocations;
    tick_now += 20;
    nv2a_d3d_tick();
    flush_batch();
    CHECK(updates == previous_updates && draws == previous_draws && allocations == previous_allocations);
    CHECK(s->gen == generation + 1);
    CHECK(s->cpu_present_pending);

    /* The command epoch does not advance between CPU animation frames. */
    rows[7 * 32 + 5] = 0xFF00FF00u;
    tick_now += 20;
    nv2a_d3d_tick();
    flush_batch();
    CHECK(s_epoch == epoch && s->gen == generation + 2 && draws == 2);
    CHECK(pixel(s, 5, 7) == 0xFF00FF00u && pixel(s, 6, 7) == 0xFFFF0000u);

    /* Recent GPU flips keep their normal submission and synchronisation path. */
    rows[7 * 32 + 5] = 0xFFFFFFFFu;
    tick_now += 20;
    s_last_flip = tick_now;
    previous_updates = updates; previous_draws = draws; previous_allocations = allocations;
    nv2a_d3d_tick();
    flush_batch();
    CHECK(updates == previous_updates && draws == previous_draws && allocations == previous_allocations);
    CHECK(s->gen == generation + 2 && pixel(s, 5, 7) == 0xFF00FF00u);
    tick_now += 100;
    nv2a_d3d_tick();
    flush_batch();
    CHECK(s_epoch == epoch && s->gen == generation + 3 && draws == 3);
    CHECK(pixel(s, 5, 7) == 0xFFFFFFFFu && pixel(s, 6, 7) == 0xFFFF0000u);

    /* A readback still uses the old shadow to preserve CPU changes; do not
     * commit those changes until that older GPU copy has finished. */
    rows[7 * 32 + 5] = 0xFF0000FFu;
    s_wb_pending = 1;
    tick_now += 20;
    previous_updates = updates; previous_draws = draws;
    nv2a_d3d_tick();
    flush_batch();
    CHECK(updates == previous_updates && draws == previous_draws && s->gen == generation + 3);
    CHECK(((uint32_t *)s->shadow)[7 * 32 + 5] == 0xFFFFFFFFu);
    CHECK(pixel(s, 5, 7) == 0xFFFFFFFFu);
    s_wb_pending = 0;
    tick_now += 20;
    nv2a_d3d_tick();
    flush_batch();
    CHECK(s_epoch == epoch && s->gen == generation + 4 && draws == 4);
    CHECK(pixel(s, 5, 7) == 0xFF0000FFu && pixel(s, 6, 7) == 0xFFFF0000u);
    CHECK(s->cpu_present_pending);
    printf("PASS: idle CPU scanout, same-epoch animation, unchanged/active skips, pending-readback deferral and present retry\n");
}

static void test_common(void)
{
    nv2a_common_checks();
    printf("PASS: shared layout, texel and draw helpers (nv2a_render_common.h)\n");
}

static void test_depth_view(int shifted)
{
    NvD3DState st={0};
    st.color_addr=0x20000; st.zeta_addr=0x40000;
    st.color_pitch=st.zeta_pitch=128; st.surface_format=0x128;
    st.clip_w=st.clip_h=32;
    nv2a_d3d_clear(&st,1,0,0x40000000,0,0,31,31);
    nv2a_d3d_clear(&st,1,0,0xC0000000,16,0,31,31);
    Depth *d=depth_find(phys(st.zeta_addr));
    CHECK(d && d->iw==64 && d->w==32);
    st.tex_used=1; st.tex[0].addr=st.zeta_addr; st.tex[0].color=0x35;
    st.tex[0].width=64; st.tex[0].height=32; st.tex[0].pitch=128; st.tex[0].levels=1;
    Surface *unused; uint32_t gw,gh,zmax;
#ifndef XBOXRECOMP_DEPTH_VIEW_SHADER
    CHECK(!shifted && !depth_y16(d) && !d->y16);
    CHECK(!setup_pipeline(&st,3,&unused,&gw,&gh,&zmax,0));
    puts("PASS: absent SDL depth converter reports unsupported without a proxy");
    return;
#else
    inspect=1; fail_texture=1;
    CHECK(!setup_pipeline(&st,3,&unused,&gw,&gh,&zmax,0));
    CHECK(!fail_texture && !d->y16); inspect=0;
    CHECK(setup_pipeline(&st,3,&unused,&gw,&gh,&zmax,0));
    SDL_GPUTexture *view=depth_y16(d); CHECK(view);
    SDL_GPUTransferBufferCreateInfo ci={0};
    ci.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; ci.size=8;
    SDL_GPUTransferBuffer *tb=SDL_CreateGPUTransferBuffer(s_dev,&ci); CHECK(tb);
    CHECK(copy_begin());
    SDL_GPUTextureRegion source={0}; source.texture=view; source.x=64; source.y=1;
    source.w=source.h=source.d=1;
    SDL_GPUTextureTransferInfo dest={0}; dest.transfer_buffer=tb;
    SDL_DownloadFromGPUTexture(s_copy,&source,&dest); copy_end();
    SDL_GPUFence *fence=NULL; submit(&fence); CHECK(fence);
    CHECK(SDL_WaitForGPUFences(s_dev,true,&fence,1)); SDL_ReleaseGPUFence(s_dev,fence);
    uint16_t *pixel=SDL_MapGPUTransferBuffer(s_dev,tb,false); CHECK(pixel);
    CHECK(pixel[0]==65535 && pixel[1]==(shifted?0x4040:0xC0C0) && pixel[2]==0);
    SDL_UnmapGPUTransferBuffer(s_dev,tb); SDL_ReleaseGPUTransferBuffer(s_dev,tb);
    printf("PASS: depth view %s alignment actual SDL GPU pixels\n",shifted?"caller-shifted":"unshifted");
#endif
}

int main(int argc, char **argv)
{
    const char *test;
    Surface *s;
    uint32_t generation, *row;
    int initial, concurrent, bpp16, swizzled;
    CHECK(argc == 2);
    test = argv[1];
    if (!strcmp(test, "common")) { test_common(); return 0; }
    initial = !strncmp(test, "initial_", 8) || !strcmp(test, "concurrent_initial");
    concurrent = !strncmp(test, "concurrent_", 11);
    bpp16 = !strcmp(test, "padded16");
    swizzled = !strcmp(test, "swizzled");
    test_setenv("RECOMP_RENDER_SCALE", !strncmp(test,"depth_view",10)?"2":"1");
    test_setenv("RECOMP_ASPECT", "4:3");
    test_setenv("RECOMP_FPS_LIMIT", "0");
    ram[0] = 0;
    CHECK(nv2a_d3d_init());
    if (!strncmp(test,"depth_view",10)) { test_depth_view(!strcmp(test,"depth_view_shifted")); return 0; }
    if (!strcmp(test, "multiple_surfaces")) { test_multiple_surfaces(); return 0; }
    if (!strcmp(test, "idle_scanout")) { test_idle_scanout(); return 0; }
    s = surface_get(0x20000, bpp16 ? 80 : 128, bpp16 ? 3 : 8, 32, 32, swizzled);
    CHECK(s && s->shadow);
    clear_gpu(s); generation = s->gen;
    row = (uint32_t *)(ram + s->va);
    if (initial) { free(s->shadow); s->shadow = NULL; }
    if (bpp16) {
        uint16_t blue = 0x001F;
        memcpy(ram + s->va + 7 * s->pitch + 5 * 2, &blue, sizeof blue);
        memset(ram + s->va + 64, 0xFF, 16); /* padding must not upload */
    } else if (swizzled) {
        /* Morton address of pixel (5,7): y2 x2 y1 x1 y0 x0 = 59. */
        row[59] = 0xFF0000FFu;
    } else if (strcmp(test, "unchanged")) row[0] = 0xFF0000FFu;

    if (!strcmp(test, "initial_snapshot") || !strcmp(test, "dirty_snapshot")) {
        clear_snapshot(); fail_realloc = 1;
    } else if (!strcmp(test, "dirty_mask")) { clear_snapshot(); clear_mask(); fail_realloc = 2; }
    else if (strstr(test, "pixels")) { clear_pixels(); fail_malloc = 1; }
    else if (!strcmp(test, "initial_texture") || !strcmp(test, "dirty_texture")) {
        reset_slot(0, 1); fail_texture = 1;
    } else if (!strcmp(test, "mask_texture")) { reset_slot(1, 1); fail_texture = 1; }
    else if (strstr(test, "constants")) fail_upload_map = 1;
    if (concurrent) late_write = row;
    inspect = 1;
    surface_sync(s, 1);
    flush_batch();
    if (!strcmp(test, "unchanged")) {
        surface_sync(s, 0); s_epoch++; surface_sync(s, 0);
        flush_batch();
        CHECK(!allocations && !updates && !draws && s->gen == generation);
        CHECK(pixel(s, 0, 0) == 0xFFFF0000u);
    } else if (bpp16 || swizzled) {
        CHECK(draws == 1 && pixel(s, 5, 7) == 0xFF0000FFu);
        CHECK(pixel(s, 6, 7) == 0xFFFF0000u);
        s_epoch++; surface_sync(s, 0); flush_batch();
        CHECK(draws == 1);
    } else if (concurrent) {
        CHECK(!late_write && draws == 1 && pixel(s, 0, 0) == 0xFF0000FFu);
        CHECK(*(uint32_t *)s->shadow == 0xFF0000FFu && row[0] == 0xFF00FF00u);
        s_epoch++; surface_sync(s, 0); flush_batch();
        CHECK(draws == 2 && pixel(s, 0, 0) == 0xFF00FF00u);
        CHECK(initial || pixel(s, 1, 0) == 0xFFFF0000u);
    } else {
        CHECK(!fail_malloc && !fail_realloc && !fail_texture && !fail_upload_map);
        CHECK(!draws && s->gen == generation && s->synced_frame != s_epoch);
        CHECK(initial ? !s->shadow : *(uint32_t *)s->shadow == 0);
        CHECK(pixel(s, 0, 0) == 0xFFFF0000u);
        surface_sync(s, 0); /* no epoch advance: failures must remain retryable */
        flush_batch();
        CHECK(draws == 1 && s->gen == generation + 1 && s->synced_frame == s_epoch);
        CHECK(*(uint32_t *)s->shadow == 0xFF0000FFu && pixel(s, 0, 0) == 0xFF0000FFu);
        CHECK(initial || pixel(s, 1, 0) == 0xFFFF0000u);
        s_epoch++; surface_sync(s, 0); flush_batch();
        CHECK(draws == 1);
    }
    printf("PASS: %s; exact GPU pixels, successful shadow commit and retry\n", test);
    return 0;
}
