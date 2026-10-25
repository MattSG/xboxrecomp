/* The SDL3 GPU renderer's sampler requests, the counterpart of test_samplers.c.
 * SDL samplers are opaque, so each created descriptor is captured at creation and
 * compared with an independent reference. The sampler each draw binds is read
 * back from the batch the renderer records, and the GPU pixels come from real
 * draws of the renderer. The border colour has no SDL field (addressing
 * border is done in the shader), so it is not part of the descriptor. */
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Creation is wrapped before the renderer is included. The real function is
 * taken first, so the wrapper calls it, not itself. */
static SDL_GPUSampler *(*const real_create_sampler)(SDL_GPUDevice *, const SDL_GPUSamplerCreateInfo *) = SDL_CreateGPUSampler;
static void (*const real_bind_fragment_samplers)(SDL_GPURenderPass *, Uint32, const SDL_GPUTextureSamplerBinding *, Uint32) = SDL_BindGPUFragmentSamplers;
static SDL_GPUSampler *test_create_sampler(SDL_GPUDevice *dev, const SDL_GPUSamplerCreateInfo *info);
static void test_bind_fragment_samplers(SDL_GPURenderPass *rp, Uint32 first,
                                        const SDL_GPUTextureSamplerBinding *b, Uint32 n);
#define SDL_CreateGPUSampler test_create_sampler
#define SDL_BindGPUFragmentSamplers test_bind_fragment_samplers

static unsigned create_calls;
static int fail_sampler;
#define CREATED_LOG 8192
static struct { SDL_GPUSampler *smp; SDL_GPUSamplerCreateInfo info; } created[CREATED_LOG];
static unsigned created_n;
static SDL_GPUTextureSamplerBinding bound[9];
static unsigned bind_calls;

static SDL_GPUSampler *test_create_sampler(SDL_GPUDevice *dev, const SDL_GPUSamplerCreateInfo *info)
{
    SDL_GPUSampler *s;
    create_calls++;
    if (fail_sampler) { fail_sampler--; return NULL; }
    s = real_create_sampler(dev, info);
    if (s) {
        created[created_n % CREATED_LOG].smp = s;
        created[created_n % CREATED_LOG].info = *info;
        created_n++;
    }
    return s;
}

static void test_bind_fragment_samplers(SDL_GPURenderPass *rp, Uint32 first,
                                        const SDL_GPUTextureSamplerBinding *b, Uint32 n)
{
    if (first == 0 && n == 9) {
        memcpy(bound, b, sizeof bound);
        bind_calls++;
    }
    real_bind_fragment_samplers(rp, first, b, n);
}

#include "../../src/video/nv2a_sdlgpu.c"
#include "../nv2a_test_host.h"
#include "../nv2a_sdlgpu_host.h"

static uint8_t ram[0x100000];
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)ram; }
int xbox_FramebufferScanSource(uint32_t *va, uint32_t *pitch) { return 0; }
int xbox_FramebufferOverlay(XboxOverlay *ovl) { return 0; }
void xbox_FramebufferNoteFlip(uint32_t draws) {}
void xbox_FramebufferRenderHostOverlay(void *device, void *context, void *target) { (void)device; (void)context; (void)target; }
const uint32_t (*nv2a_vsh_program(uint32_t *start, uint32_t *version))[4] { return NULL; }
const float (*nv2a_vsh_constants(uint32_t *version))[4] { return NULL; }

/* The descriptor the D3D11 renderer's rules give a request, written again
 * here from the NV2A register semantics rather than read from the renderer. */
static SDL_GPUSamplerAddressMode reference_address(uint32_t m)
{
    if (m == 1) return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    if (m == 2) return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
}

static void reference_descriptor(const NvD3DTexture *t, int levels, int reduce,
                                 SDL_GPUSamplerCreateInfo *e)
{
    uint32_t minf = (t->filter >> 16) & 0xFF, magf = (t->filter >> 24) & 0xF;
    int bias = (int)(t->filter & 0x1FFF), min_lin, mip_lin, mag_lin;
    if (bias & 0x1000) bias -= 0x2000;
    min_lin = minf == 2 || minf == 4 || minf == 6;
    mip_lin = minf == 5 || minf == 6;
    mag_lin = magf != 1;
    memset(e, 0, sizeof *e);
    e->min_filter = min_lin ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    e->mag_filter = mag_lin ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    e->mipmap_mode = mip_lin ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    e->address_mode_u = reference_address(t->address & 0xF);
    e->address_mode_v = reference_address((t->address >> 8) & 0xF);
    e->address_mode_w = reference_address((t->address >> 16) & 0xF);
    e->enable_anisotropy = min_lin && mag_lin && s_aniso > 1;
    e->max_anisotropy = (float)s_aniso;
    e->mip_lod_bias = (float)bias / 256.0f;
    if (minf <= 2 || minf == 7 || levels <= 1) {
        e->min_lod = e->max_lod = 0;
    } else {
        e->min_lod = (float)((t->control0 >> 18) & 0xFFF) / 256.0f;
        e->max_lod = (float)((t->control0 >> 6) & 0xFFF) / 256.0f;
        if (e->max_lod < e->min_lod) e->max_lod = 1000.0f;
    }
    if (reduce) {
        e->mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        e->min_lod = 0;
        e->max_lod = 1000.0f;
    }
}

static int same_descriptor(const SDL_GPUSamplerCreateInfo *a, const SDL_GPUSamplerCreateInfo *b)
{
    return a->min_filter == b->min_filter && a->mag_filter == b->mag_filter &&
           a->mipmap_mode == b->mipmap_mode &&
           a->address_mode_u == b->address_mode_u && a->address_mode_v == b->address_mode_v &&
           a->address_mode_w == b->address_mode_w &&
           a->enable_anisotropy == b->enable_anisotropy &&
           a->max_anisotropy == b->max_anisotropy && a->mip_lod_bias == b->mip_lod_bias &&
           a->min_lod == b->min_lod && a->max_lod == b->max_lod;
}

/* The descriptor captured when this sampler was created. */
static const SDL_GPUSamplerCreateInfo *created_info(SDL_GPUSampler *s)
{
    unsigned i, n = created_n < CREATED_LOG ? created_n : CREATED_LOG;
    for (i = n; i-- > 0;)
        if (created[i % CREATED_LOG].smp == s) return &created[i % CREATED_LOG].info;
    return NULL;
}

static void expect_sampler(SDL_GPUSampler *s, const NvD3DTexture *t, int levels, int reduce)
{
    SDL_GPUSamplerCreateInfo want;
    const SDL_GPUSamplerCreateInfo *got;
    CHECK(s);
    got = created_info(s);
    CHECK(got);
    reference_descriptor(t, levels, reduce, &want);
    if (!same_descriptor(got, &want))
        fprintf(stderr, "descriptor mismatch: filters %d/%d/%d addr %d/%d/%d aniso %d/%g bias %g lod %g..%g\n",
                got->min_filter, got->mag_filter, got->mipmap_mode, got->address_mode_u,
                got->address_mode_v, got->address_mode_w, got->enable_anisotropy,
                got->max_anisotropy, got->mip_lod_bias, got->min_lod, got->max_lod);
    CHECK(same_descriptor(got, &want));
}

static NvD3DTexture request(unsigned i)
{
    NvD3DTexture t = {0};
    static const unsigned modes[] = {1, 2, 3, 4};
    t.filter = ((1 + i % 7) << 16) | ((1 + (i / 7) % 2) << 24) | ((i & 1) ? 0x1F80 : 0x80);
    t.address = modes[i % 4] | (modes[(i / 4) % 4] << 8) | (modes[(i / 16) % 4] << 16);
    t.control0 = (((i % 4) * 256) << 18) | ((((i / 4) % 4) * 256) << 6);
    t.border = 0x80336699u + i * 0x01010203u;
    return t;
}

/* A draw that samples texture 0 at (u, v) with the renderer's own pipeline,
 * and the pixel it wrote at (20,20) of a 64x64 target. */
static uint32_t draw_sample(const NvD3DTexture *t, float u, float v)
{
    NvD3DState st = {0};
    NvD3DAttrib attr[16] = {0};
    float positions[3][4] = { { 8, 8, 0, 1 }, { 56, 8, 0, 1 }, { 8, 56, 0, 1 } };
    float def[16][4] = { { 0 } };
    uint32_t idx[3] = { 0, 1, 2 }, pixels[64 * 64];
    st.color_addr = 0x10000; st.color_pitch = 64 * 4;
    st.surface_format = 0x25; st.clip_w = st.clip_h = 64;
    st.color_mask = 0x01010101;
    st.tex_used = 1; st.tex[0] = *t;
    def[3][0] = def[3][1] = def[3][2] = def[3][3] = 1;
    def[9][0] = u; def[9][1] = v; def[9][3] = 1;
    attr[0] = (NvD3DAttrib){ 1, 2, 4, 16, (const uint8_t *)positions };
    nv2a_d3d_clear(&st, 0xF0, 0xFF000000, 0, 0, 0, 63, 63);
    nv2a_d3d_draw_ub(&st, NV_D3D_TRIANGLES, attr, def, 0, idx, 3, 0, 2, NULL);
    gpu_download(surface_find(0x10000)->tex, 64, 64, pixels);
    return pixels[20 * 64 + 20];
}

static void expect_pixel(uint32_t got, uint32_t want)
{
    if (got != want) fprintf(stderr, "pixel %08X, expected %08X\n", got, want);
    CHECK(got == want);
}

/* A 8x8 linear A8R8G8B8 guest texture whose texels encode their coordinates. */
static void make_guest_texture(void)
{
    uint32_t *p = (uint32_t *)(ram + 0x20000), x, y;
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++)
            p[y * 8 + x] = 0xFF000000u | (x * 32) << 16 | (y * 32) << 8 | 0x10;
}
static uint32_t texel_at(unsigned x, unsigned y)
{
    return 0xFF000000u | (x * 32) << 16 | (y * 32) << 8 | 0x10;
}
static NvD3DTexture guest_texture(uint32_t filter, uint32_t address)
{
    NvD3DTexture t = {0};
    t.addr = 0x20000; t.color = 0x12; t.format = 0x1200;
    t.width = t.height = 8; t.pitch = 32; t.levels = 1;
    t.filter = filter; t.address = address;
    return t;
}

static void test_pixels(void)
{
    unsigned i, repeat, calls;
    for (i = 0; i < 112; i++) {
        NvD3DTexture t = request(i);
        int levels = (int)(i % 5) + 1, reduce = (int)((i / 7) % 2);
        SDL_GPUSampler *now = sampler(&t, levels, reduce, no_pinned_samplers);
        expect_sampler(now, &t, levels, reduce);
        calls = create_calls;
        for (repeat = 0; repeat < 8; repeat++)
            CHECK(sampler(&t, levels, reduce, no_pinned_samplers) == now);
        CHECK(create_calls == calls);
        /* Bits this descriptor does not read (filter bit 15, border colour)
         * keep the same sampler: the key is the descriptor, not the raw words. */
        t.filter ^= 0x8000;
        CHECK(sampler(&t, levels, reduce, no_pinned_samplers) == now);
        t.border ^= 0xFFFFFFu;
        CHECK(sampler(&t, levels, reduce, no_pinned_samplers) == now);
        CHECK(create_calls == calls);
    }
    /* Native GPU taps through the renderer's own draw: point sampling, and
     * the address modes one texel past the 8-texel edge (wrap to texel 0,
     * clamp to texel 7). */
    make_guest_texture();
    {
        NvD3DTexture point = guest_texture((1u << 16) | (1u << 24), 0x010101);
        NvD3DTexture clamp = guest_texture((1u << 16) | (1u << 24), 0x000000);
        /* A linear texture takes its coordinates in texels (tex_scale = 1/width). */
        expect_pixel(draw_sample(&point, 0.0625f, 0.0625f), texel_at(0, 0));
        expect_pixel(draw_sample(&point, 4.5f, 2.5f), texel_at(4, 2));
        expect_pixel(draw_sample(&point, 8.0625f, 0.0625f), texel_at(0, 0));
        expect_pixel(draw_sample(&clamp, 8.0625f, 0.0625f), texel_at(7, 0));
    }
    printf("PASS: 112 descriptors checked against the reference, repeated lookups reuse the sampler, unread bits keep it, and SDL GPU pixels for point/wrap/clamp sampling\n");
}

static void test_eviction(void)
{
    NvD3DTexture t = request(2), other = request(4);
    SDL_GPUSampler *old = sampler(&t, 4, 0, no_pinned_samplers), *now;
    unsigned i, calls;
    int evicted = 0;
    expect_sampler(old, &t, 4, 0);
    for (i = 0; i < 32768 && !evicted; i++) {
        /* Control bits outside the LOD fields vary the key, not the result. */
        other.control0 = (i << 6) & 0x3FFFFFC0u;
        CHECK(sampler(&other, 4, 0, no_pinned_samplers));
        evicted = 1;
        for (unsigned j = 0; j < SAMP_SLOTS; j++)
            if (s_samp[j].resource == old) { evicted = 0; break; }
    }
    CHECK(evicted && i > SAMP_SLOTS);
    calls = create_calls;
    now = sampler(&t, 4, 0, no_pinned_samplers);
    CHECK(now && create_calls == calls + 1);
    expect_sampler(now, &t, 4, 0);
    CHECK(sampler(&t, 4, 0, no_pinned_samplers) == now && create_calls == calls + 1);
    printf("PASS: sampler table evicted the oldest key after %u requests; the same request is recreated with the reference descriptor\n", i);
}

static void test_failure(void)
{
    NvD3DTexture t = request(81);
    SDL_GPUSampler *now;
    unsigned calls = create_calls;
    fail_sampler = 1;
    CHECK(!sampler(&t, 4, 1, no_pinned_samplers));
    CHECK(create_calls == calls + 1 && !fail_sampler);
    now = sampler(&t, 4, 1, no_pinned_samplers);
    CHECK(now && create_calls == calls + 2);
    expect_sampler(now, &t, 4, 1);
    CHECK(sampler(&t, 4, 1, no_pinned_samplers) == now && create_calls == calls + 2);
    printf("PASS: failed sampler creation publishes no cached hit; the same request retries and matches the reference\n");
}

/* The levels the renderer reports for a texture (tex_layout's count, through
 * the cache), and what each draw binds in slot 0. */
static int old_levels(const NvD3DTexture *t)
{
    TexLayout l;
    return tex_layout(t, &l) ? l.levels : 1;
}

static void check_bound(const NvD3DTexture *t, int levels, int has_view, int tex_used)
{
    NvD3DState st = {0};
    NvD3DAttrib attr[16] = {0};
    float positions[3][4] = { { 8, 8, 0, 1 }, { 56, 8, 0, 1 }, { 8, 56, 0, 1 } };
    float def[16][4] = { { 0 } };
    uint32_t idx[3] = { 0, 1, 2 };
    unsigned sampler_calls = bind_calls;
    st.color_addr = 0x10000; st.color_pitch = 64 * 4;
    st.surface_format = 0x25; st.clip_w = st.clip_h = 64;
    st.color_mask = 0x01010101;
    st.tex_used = (uint8_t)tex_used; st.tex[0] = *t;
    def[3][3] = 1;
    attr[0] = (NvD3DAttrib){ 1, 2, 4, 16, (const uint8_t *)positions };
    nv2a_d3d_draw_ub(&st, NV_D3D_TRIANGLES, attr, def, 0, idx, 3, 0, 2, NULL);
    flush_batch();
    CHECK(bind_calls == sampler_calls + 1);
    if (has_view) {
        CHECK(bound[0].texture && bound[0].texture != s_dummy_2d);
        CHECK(bound[0].sampler == sampler(t, levels, 0, no_pinned_samplers));
    } else {
        CHECK(bound[0].texture == s_dummy_2d);
        CHECK(bound[0].sampler == s_linear_clamp);
    }
}

static void test_levels(void)
{
    NvD3DTexture t = {0};
    int levels = -7;
    SDL_GPUTexture *view;
    make_guest_texture();
    t.addr = 0x20000; t.color = 0x06; t.format = 0x0600;
    t.width = t.height = 8; t.levels = 4;
    t.filter = (6u << 16) | (1u << 24); t.control0 = 3 * 256 << 6; t.address = 0x030303;
    view = texture_get_with_levels(&t, &levels);
    CHECK(view && levels == old_levels(&t) && levels == 4);
    CHECK(texture_get_with_levels(&t, &levels) == view);        /* same-kick cache hit */
    check_bound(&t, 4, 1, 1);
    t.levels = 0;
    CHECK(texture_get_with_levels(&t, &levels) && levels == old_levels(&t) && levels == 1);
    t.levels = 15;
    CHECK(texture_get_with_levels(&t, &levels) && levels == old_levels(&t) && levels == 4);
    check_bound(&t, 4, 1, 1);
    t.cube = 1; t.addr = 0x24000;
    CHECK(texture_get_with_levels(&t, &levels) && levels == old_levels(&t));
    t.cube = 0; t.addr = 0x20000;
    t.width = 0;
    CHECK(!texture_get_with_levels(&t, &levels) && levels == old_levels(&t));
    check_bound(&t, 1, 0, 1);
    t.width = 8;
    t.color = 0xFF;
    CHECK(!texture_get_with_levels(&t, &levels));
    t.color = 0x06;
    t.addr = 0x07FFFFF0;
    CHECK(!texture_get_with_levels(&t, &levels));
    t.addr = 0x20000;
    /* Disabled stage, address zero and control0 = 0: the stage binds nothing. */
    {
        NvD3DTexture u = t;
        check_bound(&u, 1, 0, 0);
        u.addr = 0;
        check_bound(&u, 1, 0, 1);
        u.addr = t.addr; u.control0 = 0;
        check_bound(&u, old_levels(&u), 1, 1);
    }
    /* A linear texture has one level, whatever the title asked for. */
    t.color = 0x12; t.pitch = 32; t.levels = 15; t.format = 0x1200;
    CHECK(texture_get_with_levels(&t, &levels) && levels == 1 && old_levels(&t) == 1);
    check_bound(&t, 1, 1, 1);
    printf("PASS: supplied levels match the layout on defaults, clamps, cube, bounds and colour, and the bound sampler/view per draw\n");
}

static void test_common(void)
{
    nv2a_common_checks();
    printf("PASS: shared layout, texel and draw helpers (nv2a_render_common.h)\n");
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    test_setenv("RECOMP_RENDER_SCALE", "1");
    test_setenv("RECOMP_ANISO", "16");
    test_setenv("RECOMP_ASPECT", "4:3");
    test_setenv("RECOMP_FPS_LIMIT", "0");
    if (!strcmp(argv[1], "common")) { test_common(); return 0; }
    CHECK(nv2a_d3d_init());
    if (!strcmp(argv[1], "pixels")) test_pixels();
    else if (!strcmp(argv[1], "eviction")) test_eviction();
    else if (!strcmp(argv[1], "failure")) test_failure();
    else if (!strcmp(argv[1], "levels")) test_levels();
    else CHECK(0);
    return 0;
}
