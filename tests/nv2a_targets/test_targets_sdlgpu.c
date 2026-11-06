/* The SDL3 GPU renderer's render targets and guest textures, the counterpart of
 * test_targets.c. Pixels are read back from the GPU after the renderer's own
 * uploads, draws and writebacks. The bound texture of each draw is read back
 * from the batch the renderer records. Each case runs in its own process. */
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void (*const real_bind_fragment_samplers)(SDL_GPURenderPass *, Uint32, const SDL_GPUTextureSamplerBinding *, Uint32) = SDL_BindGPUFragmentSamplers;
static void test_bind_fragment_samplers(SDL_GPURenderPass *rp, Uint32 first,
                                        const SDL_GPUTextureSamplerBinding *b, Uint32 n);
#define SDL_BindGPUFragmentSamplers test_bind_fragment_samplers

static SDL_GPUTextureSamplerBinding bound[13];
static unsigned bind_calls;
static int guest_levels;

#include "../../src/video/nv2a_sdlgpu.c"
#include "../nv2a_test_host.h"
#include "../nv2a_sdlgpu_host.h"

/* The texture a guest texture binds to (the renderer's cached upload). */
static SDL_GPUTexture *texture_get(const NvD3DTexture *t)
{
    return texture_get_with_levels(t, &guest_levels);
}

static void test_bind_fragment_samplers(SDL_GPURenderPass *rp, Uint32 first,
                                        const SDL_GPUTextureSamplerBinding *b, Uint32 n)
{
    if (first == 0 && n == 13) {
        memcpy(bound, b, sizeof bound);
        bind_calls++;
    }
    real_bind_fragment_samplers(rp, first, b, n);
}

static uint8_t *ram;
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)ram; }
int xbox_FramebufferScanSource(uint32_t *va, uint32_t *pitch) { return 0; }
int xbox_FramebufferOverlay(XboxOverlay *ovl) { return 0; }
void xbox_FramebufferNoteFlip(uint32_t draws) {}
void xbox_FramebufferRenderHostOverlay(void *device, void *context, void *target) { (void)device; (void)context; (void)target; }
const uint32_t (*nv2a_vsh_program(uint32_t *start, uint32_t *version))[4] { return NULL; }
const float (*nv2a_vsh_constants(uint32_t *version))[4] { return NULL; }

/* Pixels of a texture with 8-byte texels (R16G16B16A16). */
static void download_wide(SDL_GPUTexture *t, uint32_t w, uint32_t h, uint64_t *out)
{
    SDL_GPUTransferBufferCreateInfo ci;
    SDL_GPUTransferBuffer *tb;
    SDL_GPUTextureRegion src;
    SDL_GPUTextureTransferInfo dst;
    SDL_GPUFence *fence = NULL;
    const uint8_t *m;
    memset(&ci, 0, sizeof ci);
    ci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    ci.size = w * h * 8;
    tb = SDL_CreateGPUTransferBuffer(s_dev, &ci);
    CHECK(tb && copy_begin());
    memset(&src, 0, sizeof src);
    memset(&dst, 0, sizeof dst);
    src.texture = t; src.w = w; src.h = h; src.d = 1;
    dst.transfer_buffer = tb;
    SDL_DownloadFromGPUTexture(s_copy, &src, &dst);
    copy_end();
    submit(&fence);
    CHECK(fence && SDL_WaitForGPUFences(s_dev, true, &fence, 1));
    SDL_ReleaseGPUFence(s_dev, fence);
    m = (const uint8_t *)SDL_MapGPUTransferBuffer(s_dev, tb, false);
    CHECK(m);
    memcpy(out, m, (size_t)w * h * 8);
    SDL_UnmapGPUTransferBuffer(s_dev, tb);
    SDL_ReleaseGPUTransferBuffer(s_dev, tb);
}

/* Pixel (x, y) of a BGRA texture. */
static uint32_t pixel_of(SDL_GPUTexture *t, uint32_t w, uint32_t h, uint32_t x, uint32_t y)
{
    uint32_t *px = (uint32_t *)malloc((size_t)w * h * 4), v;
    CHECK(px);
    gpu_download(t, w, h, px);
    v = px[y * w + x];
    free(px);
    return v;
}

/* Whatever a bound texture holds, decoded to BGRA: it is drawn into a
 * render target of the same size (the renderer's own blit), then read. */
static uint32_t bound_pixel(SDL_GPUTexture *t, uint32_t w, uint32_t h, uint32_t x, uint32_t y)
{
    SDL_GPUTexture *out = make_texture(w, h, 1, 0, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, USAGE_TARGET);
    int clear = 1;
    uint32_t v;
    CHECK(out);
    CHECK(blit_span(t, NULL, out, &clear, w, h, -1.0f, 0, 1, 0, 1));
    v = pixel_of(out, w, h, x, y);
    release_texture(&out);
    return v;
}

static uint32_t log2_of(uint32_t v)
{
    uint32_t n = 0;
    while (v >> 1) { v >>= 1; n++; }
    return n;
}

/* Clears a surface to red on the GPU, as the title's clear would: a swizzled
 * surface is described to the renderer by its swizzle format. */
static void clear_red(Surface *s)
{
    NvD3DState st = {0};
    uint32_t fmt = s->color_fmt;
    if (s->swizzled) fmt |= (2u << 8) | (log2_of(s->w) << 16) | (log2_of(s->h) << 24);
    st.color_addr = s->va; st.color_pitch = s->pitch;
    st.surface_format = fmt; st.clip_w = s->w; st.clip_h = s->h;
    st.color_mask = 0x01010101;
    nv2a_d3d_clear(&st, 0xF0, 0xFFFF0000u, 0, 0, 0, s->w - 1, s->h - 1);
}

/* 1. A sampled surface's CPU upload during texture setup keeps the draw target
 * and viewport. */
static void test_sampled_upload(void)
{
    NvD3DState st = {0};
    Surface *sample, *target;
    uint32_t gw, gh, zmax;
    sample = surface_get(0x20000, 32 * 4, 8, 32, 32, 0);
    CHECK(sample);
    st.color_addr = 0x10000; st.color_pitch = 64 * 4;
    st.surface_format = 0x28; st.clip_w = st.clip_h = 64;
    st.zeta_addr = 0x30000; st.depth_test = st.depth_mask = 1; st.depth_func = 7;
    st.color_mask = 0x01010101;
    st.tex_used = 1;
    st.tex[0].addr = sample->va;
    st.tex[0].color = 0x12; st.tex[0].width = st.tex[0].height = 32;
    st.tex[0].pitch = 32 * 4; st.tex[0].levels = 1;
    CHECK(setup_pipeline(&st, NV_D3D_TRIANGLES, &target, &gw, &gh, &zmax, 1));
    CHECK(s_b.color == target->tex && s_b.depth != NULL);
    /* A CPU change forces surface_sync to draw an upload during texture setup. */
    ram[sample->va] ^= 0xFF;
    s_epoch++;
    CHECK(setup_pipeline(&st, NV_D3D_TRIANGLES, &target, &gw, &gh, &zmax, 1));
    CHECK(s_b.color == target->tex && s_b.depth != NULL);
    CHECK(s_last_draw_state.vp.w == 64 && s_last_draw_state.vp.h == 64);
    puts("PASS: sampled-surface upload preserves draw targets and viewport");
}

/* 2. A full surface cache evicts the least recently used target. */
static void test_lru(void)
{
    unsigned i;
    for (i = 0; i < MAX_SURFACES; i++) surface_release(&s_surf[i]);
    for (i = 0; i < MAX_SURFACES; i++) {
        s_frame = i + 1;
        CHECK(surface_get(0x10000 + i * 0x4000, 128, 8, 32, 32, 0));
    }
    for (i = 0; i < MAX_SURFACES; i++)
        CHECK(surface_find(0x10000 + i * 0x4000));
    surface_find(0x10000)->used_frame = MAX_SURFACES + 10;
    surface_find(0x10000 + 11 * 0x4000)->used_frame = 0;
    s_frame = MAX_SURFACES + 11;
    CHECK(surface_get(0xE0000, 128, 8, 32, 32, 0));
    CHECK(surface_find(0x10000));
    CHECK(!surface_find(0x10000 + 11 * 0x4000));
    for (i = 1; i < MAX_SURFACES; i++)
        if (i != 11) CHECK(surface_find(0x10000 + i * 0x4000));
    CHECK(surface_find(0xE0000));
    puts("PASS: full surface cache evicts oldest resource and preserves recent targets");
}

/* 3. A rewritten guest texture reaches GPU pixels before the next flip; an
 * unchanged one keeps its upload. */
static void test_texture_rewrite(void)
{
    NvD3DTexture texture = {0};
    uint32_t *pixels = (uint32_t *)(ram + 0x70000), px[16], i, frame;
    SDL_GPUTexture *view;
    texture.addr = 0x70000; texture.color = 0x12; texture.format = 0x1200;
    texture.width = texture.height = 4; texture.pitch = 16; texture.levels = 1;
    for (i = 0; i < 16; i++) pixels[i] = 0xFF000000u;
    view = texture_get(&texture);
    CHECK(view);
    gpu_download(view, 4, 4, px); CHECK(px[0] == 0xFF000000u);
    nv2a_d3d_kick();
    CHECK(texture_get(&texture) == view);          /* unchanged data keeps its upload */
    frame = s_frame;
    for (i = 0; i < 16; i++) pixels[i] = 0xFF00FF00u;
    nv2a_d3d_kick();
    CHECK(s_frame == frame);
    view = texture_get(&texture);
    gpu_download(view, 4, 4, px); CHECK(px[0] == 0xFF00FF00u);
    nv2a_d3d_flip(0, 0);
    CHECK(s_frame == frame + 1);
    view = texture_get(&texture);
    gpu_download(view, 4, 4, px); CHECK(px[0] == 0xFF00FF00u);
    puts("PASS: guest texture rewrite reaches GPU pixels before the next flip");
}

/* 4. A rewritten palette reaches GPU pixels before the next flip. */
static void test_palette_rewrite(void)
{
    NvD3DTexture texture = {0};
    uint32_t *palette = (uint32_t *)(ram + 0x73000), px[16], frame;
    SDL_GPUTexture *view;
    texture.addr = 0x72000; texture.color = 0x0B; texture.format = 0x0B00;
    texture.width = texture.height = 4; texture.levels = 1;
    texture.palette = 0x73000; texture.palette_len = 256;
    memset(ram + texture.addr, 7, 16);
    palette[7] = 0xFFFF0000u;
    view = texture_get(&texture);
    gpu_download(view, 4, 4, px); CHECK(px[0] == 0xFFFF0000u);
    frame = s_frame;
    palette[7] = 0xFF0000FFu;
    nv2a_d3d_kick();
    CHECK(s_frame == frame);
    view = texture_get(&texture);
    gpu_download(view, 4, 4, px); CHECK(px[0] == 0xFF0000FFu);
    nv2a_d3d_flip(0, 0);
    CHECK(s_frame == frame + 1);
    view = texture_get(&texture);
    gpu_download(view, 4, 4, px); CHECK(px[0] == 0xFF0000FFu);
    puts("PASS: guest palette rewrite reaches GPU pixels before the next flip");
}

/* 5. A queued writeback cannot overwrite a replacement at a new or reused
 * guest address. */
static void test_writeback_replacement(void)
{
    Surface *old, *replacement;
    uint32_t *pixels, j, same_address, old_va, new_va;
    for (same_address = 0; same_address < 2; same_address++) {
        old_va = 0x20000;
        new_va = same_address ? old_va : 0x30000;
        old = surface_get(old_va, 128, 8, 32, 32, 0);
        CHECK(old);
        clear_red(old);
        old->gpu_dirty = 1;
        writeback_surfaces();
        CHECK(s_wb_pending && s_wb[old - s_surf].pending);
        surface_release(old);
        pixels = (uint32_t *)(ram + new_va);
        for (j = 0; j < 32 * 32; j++) pixels[j] = 0xFF0000FFu;
        replacement = surface_get(new_va, 128, 8, 32, 32, 0);
        CHECK(replacement == old);
        CHECK(writeback_finish(1));
        for (j = 0; j < 32 * 32; j++) CHECK(pixels[j] == 0xFF0000FFu);
        surface_release(replacement);
    }
    puts("PASS: queued pixels cannot overwrite replacements at new or reused guest addresses");
}

/* 6. A retained target writes its GPU pixels back, and a row the CPU changed
 * after submission stays intact. */
static void test_writeback_retained(void)
{
    Surface *old;
    uint32_t *pixels, j;
    Uint64 deadline_ticks;
    old = surface_get(0x20000, 128, 8, 32, 32, 0);
    CHECK(old);
    clear_red(old);
    writeback_surfaces();
    CHECK(s_wb_pending && s_wb[old - s_surf].pending);
    pixels = (uint32_t *)(ram + old->va);
    for (j = 0; j < 32; j++) pixels[j] = 0xFF00FF00u;
    /* The submission made at writeback_surfaces must progress even if the title
     * only polls for its fence and queues no more work. */
    deadline_ticks = SDL_GetTicks() + 5000;
    while (nv2a_d3d_writeback_busy()) {
        CHECK(SDL_GetTicks() < deadline_ticks);
        SDL_Delay(0);
    }
    for (j = 0; j < 32; j++) CHECK(pixels[j] == 0xFF00FF00u);
    for (j = 32; j < 32 * 32; j++) CHECK(pixels[j] == 0xFFFF0000u);
    puts("PASS: retained target writeback preserves later CPU writes and updates untouched rows");
}

/* 7. Swizzled 16/32-bit CPU pixels synchronise, unchanged GPU pixels survive. */
static void test_swizzled_sampled(void)
{
    unsigned bpp, j;
    for (bpp = 2; bpp <= 4; bpp += 2) {
        uint32_t w = bpp == 2 ? 32 : 16, h = bpp == 2 ? 16 : 32;
        uint32_t va = 0x40000 + bpp * 0x1000, px[32 * 32];
        Surface *sampled;
        NvD3DState st = {0};
        Surface *drawn;
        uint32_t gw, gh, zmax;
        if (bpp == 2)
            for (j = 0; j < w * h; j++) ((uint16_t *)(ram + va))[j] = 0x001F;
        else
            for (j = 0; j < w * h; j++) ((uint32_t *)(ram + va))[j] = 0xFF0000FFu;
        sampled = surface_get(va, w * bpp, bpp == 2 ? 3 : 8, w, h, 1);
        CHECK(sampled);
        gpu_download(sampled->tex, w, h, px);
        CHECK(px[0] == 0xFF0000FFu);
        clear_red(sampled);
        sampled->gen++;
        surface_sync(sampled, 1);
        gpu_download(sampled->tex, w, h, px);
        CHECK(px[0] == 0xFFFF0000u);
        /* Morton (3,5) in both rectangles. */
        if (bpp == 2) ((uint16_t *)(ram + va))[39] = 0x07E0;
        else ((uint32_t *)(ram + va))[39] = 0xFF00FF00u;
        nv2a_d3d_kick();
        st.color_addr = 0x10000; st.color_pitch = 64 * 4;
        st.surface_format = 8; st.clip_w = st.clip_h = 64;
        st.color_mask = 0x01010101; st.tex_used = 1;
        st.tex[0].addr = va; st.tex[0].color = bpp == 2 ? 0x05 : 0x06;
        st.tex[0].format = st.tex[0].color << 8;
        st.tex[0].width = w; st.tex[0].height = h; st.tex[0].levels = 1;
        CHECK(setup_pipeline(&st, NV_D3D_TRIANGLES, &drawn, &gw, &gh, &zmax, 1));
        flush_batch();
        CHECK(bound[0].texture == sampled->tex);   /* the target itself is sampled */
        CHECK(bound_pixel(bound[0].texture, w, h, 3, 5) == 0xFF00FF00u);
        CHECK(bound_pixel(bound[0].texture, w, h, 3, 4) == 0xFFFF0000u);
        CHECK(bound_pixel(bound[0].texture, w, h, 0, 1) == 0xFFFF0000u);
        surface_release(sampled);
    }
    puts("PASS: swizzled16/32-bit CPU pixels synchronize while unchanged GPU pixels survive");
}

/* 8. A reused block as DXT1 or palette data binds its decoded image, not the
 * old target. */
static void test_compressed_palette(void)
{
    NvD3DState st = {0};
    Surface *sampled, *drawn;
    uint32_t gw, gh, zmax, j, px[32 * 32];
    sampled = surface_get(0x40000 + 4 * 0x1000, 128, 8, 32, 32, 1);
    CHECK(sampled);
    clear_red(sampled);
    sampled->gen++;
    surface_sync(sampled, 1);
    for (j = 0; j < 4 * 8; j++) {
        uint8_t *block = ram + sampled->va + j * 8;
        block[0] = 0xE0; block[1] = 0x07;
        memset(block + 2, 0, 6);
    }
    st.color_addr = 0x10000; st.color_pitch = 64 * 4;
    st.surface_format = 8; st.clip_w = st.clip_h = 64;
    st.color_mask = 0x01010101; st.tex_used = 1;
    st.tex[0].addr = sampled->va; st.tex[0].color = 0x0C; st.tex[0].format = 0x0C00;
    st.tex[0].width = 32; st.tex[0].height = 16; st.tex[0].levels = 1;
    nv2a_d3d_kick();
    CHECK(setup_pipeline(&st, NV_D3D_TRIANGLES, &drawn, &gw, &gh, &zmax, 1));
    flush_batch();
    CHECK(bound[0].texture != sampled->tex && bound[0].texture != s_dummy_2d);
    CHECK(bound_pixel(bound[0].texture, 32, 16, 0, 0) == 0xFF00FF00u);
    memset(ram + sampled->va, 7, 16 * 32);
    ((uint32_t *)(ram + 0x74000))[7] = 0xFF0000FFu;
    st.tex[0].color = 0x0B; st.tex[0].format = 0x0B00;
    st.tex[0].palette = 0x74000; st.tex[0].palette_len = 256;
    st.tex[0].width = st.tex[0].height = 4;
    nv2a_d3d_kick();
    CHECK(setup_pipeline(&st, NV_D3D_TRIANGLES, &drawn, &gw, &gh, &zmax, 1));
    flush_batch();
    CHECK(bound[0].texture != sampled->tex && bound[0].texture != s_dummy_2d);
    gpu_download(bound[0].texture, 4, 4, px);
    CHECK(px[0] == 0xFF0000FFu);
    puts("PASS: compressed and paletted uploads at an old target address bind their decoded pixels");
}

/* 9. Colour and depth Y16 aliases keep their GPU values and depth precedence. */
static void test_y16_aliases(void)
{
    NvD3DState st = {0};
    Surface *sampled, *drawn;
    Depth *zsample;
    uint32_t gw, gh, zmax;
    uint64_t wide[64 * 32];   /* 2*32 texels by 32 rows of 8 bytes */
    sampled = surface_get(0x50000, 128, 8, 32, 32, 0);
    CHECK(sampled);
    clear_red(sampled);
    sampled->gen++;
    st.color_addr = 0x10000; st.color_pitch = 64 * 4;
    st.surface_format = 8; st.clip_w = st.clip_h = 64;
    st.color_mask = 0x01010101; st.tex_used = 1;
    st.tex[0].addr = sampled->va; st.tex[0].color = 0x35;
    st.tex[0].format = 0x3500; st.tex[0].pitch = 128;
    st.tex[0].width = 64; st.tex[0].height = 32; st.tex[0].levels = 1;
    nv2a_d3d_kick();
    CHECK(setup_pipeline(&st, NV_D3D_TRIANGLES, &drawn, &gw, &gh, &zmax, 1));
    flush_batch();
    CHECK(bound[0].texture == sampled->y16);
    download_wide(sampled->y16, 2 * sampled->iw, sampled->ih, wide);
    /* Y16 R is one; G contains each source word's high byte. */
    CHECK(((uint16_t *)&wide[0])[0] == 0xFFFF && ((uint16_t *)&wide[0])[1] == 0);
    CHECK(((uint16_t *)&wide[1])[0] == 0xFFFF && ((uint16_t *)&wide[1])[1] == 0xFFFF);
    zsample = depth_get(sampled->va, 32, 32);
    CHECK(zsample);
    zsample->zf = 2; zsample->w = zsample->h = 32;
    zsample->seq = ++s_surf_seq; zsample->gen++;
    {
        NvD3DState zst = {0};
        zst.zeta_addr = sampled->va; zst.zeta_pitch = 128;
        zst.surface_format = 0x20; zst.clip_w = zst.clip_h = 32;
        nv2a_d3d_clear(&zst, 0x1, 0, 0xFFFFFF00u, 0, 0, 31, 31);
    }
    nv2a_d3d_kick();
    CHECK(setup_pipeline(&st, NV_D3D_TRIANGLES, &drawn, &gw, &gh, &zmax, 1));
    flush_batch();
    CHECK(bound[0].texture == zsample->y16 && zsample->y16 != sampled->y16);
    download_wide(zsample->y16, 2 * zsample->iw, zsample->ih, wide);
    CHECK(((uint16_t *)&wide[1])[0] == 0xFFFF && ((uint16_t *)&wide[1])[1] == 0xFFFF);
    puts("PASS: colour/depth Y16 aliases retain GPU values and depth precedence");
}

static void test_common(void)
{
    nv2a_common_checks();
    puts("PASS: shared layout, texel and draw helpers (nv2a_render_common.h)");
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    ram = calloc(1, 0x100000);
    CHECK(ram);
    test_setenv("RECOMP_RENDER_SCALE", "1");
    test_setenv("RECOMP_ASPECT", "4:3");
    test_setenv("RECOMP_FPS_LIMIT", "0");
    if (!strcmp(argv[1], "common")) { test_common(); return 0; }
    CHECK(nv2a_d3d_init());
    if (!strcmp(argv[1], "sampled_upload")) test_sampled_upload();
    else if (!strcmp(argv[1], "lru")) test_lru();
    else if (!strcmp(argv[1], "texture_rewrite")) test_texture_rewrite();
    else if (!strcmp(argv[1], "palette_rewrite")) test_palette_rewrite();
    else if (!strcmp(argv[1], "writeback_replacement")) test_writeback_replacement();
    else if (!strcmp(argv[1], "writeback_retained")) test_writeback_retained();
    else if (!strcmp(argv[1], "swizzled_sampled")) test_swizzled_sampled();
    else if (!strcmp(argv[1], "compressed_palette")) test_compressed_palette();
    else if (!strcmp(argv[1], "y16_aliases")) test_y16_aliases();
    else CHECK(0);
    return 0;
}
