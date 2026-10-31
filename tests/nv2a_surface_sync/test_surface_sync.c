/* Failed surface uploads must retain pending CPU writes and retry in the
 * same epoch. Exercise real D3D11 resources, masked draws and GPU pixels. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int inspect, fail_malloc, fail_realloc, fail_texture, fail_srv, fail_constants, fail_rtv;
static unsigned allocations, updates, draws;
static uint32_t *late_write;
static ID3D11Resource *present_buffer;
static int controlled_tick;
static DWORD tick_now;

static DWORD watched_tick(void)
{
    return controlled_tick ? tick_now : GetTickCount();
}

static void *watched_malloc(size_t bytes)
{
    if (inspect) { allocations++; if (fail_malloc && !--fail_malloc) return NULL; }
    return malloc(bytes);
}
static void *watched_realloc(void *old, size_t bytes)
{
    if (inspect) { allocations++; if (fail_realloc && !--fail_realloc) return NULL; }
    return realloc(old, bytes);
}
static HRESULT watched_texture(ID3D11Device *dev, const D3D11_TEXTURE2D_DESC *d,
                                const D3D11_SUBRESOURCE_DATA *data, ID3D11Texture2D **out)
{
    if (inspect && fail_texture && !--fail_texture) { *out = NULL; return E_OUTOFMEMORY; }
    return dev->lpVtbl->CreateTexture2D(dev, d, data, out);
}
static HRESULT watched_srv(ID3D11Device *dev, ID3D11Resource *res,
                            const D3D11_SHADER_RESOURCE_VIEW_DESC *d, ID3D11ShaderResourceView **out)
{
    if (inspect && fail_srv && !--fail_srv) { *out = NULL; return E_OUTOFMEMORY; }
    return dev->lpVtbl->CreateShaderResourceView(dev, res, d, out);
}
static HRESULT watched_rtv(ID3D11Device *dev,ID3D11Resource *res,
                          const D3D11_RENDER_TARGET_VIEW_DESC *desc,ID3D11RenderTargetView **out)
{
    if (inspect && fail_rtv && !--fail_rtv) { *out=NULL; return E_OUTOFMEMORY; }
    return dev->lpVtbl->CreateRenderTargetView(dev,res,desc,out);
}
static HRESULT watched_map(ID3D11DeviceContext *ctx, ID3D11Resource *res, UINT sub,
                            D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *out)
{
    if (inspect && res == present_buffer) {
        if (late_write) { *late_write = 0xFF00FF00u; late_write = NULL; }
        if (fail_constants) { fail_constants = 0; return E_OUTOFMEMORY; }
    }
    return ctx->lpVtbl->Map(ctx, res, sub, type, flags, out);
}
static void watched_update(ID3D11DeviceContext *ctx, ID3D11Resource *res, UINT sub,
                            const D3D11_BOX *box, const void *src, UINT row, UINT depth)
{
    if (inspect) updates++;
    ctx->lpVtbl->UpdateSubresource(ctx, res, sub, box, src, row, depth);
}
static void watched_draw(ID3D11DeviceContext *ctx, UINT count, UINT start)
{
    if (inspect) draws++;
    ctx->lpVtbl->Draw(ctx, count, start);
}
#define malloc watched_malloc
#define realloc watched_realloc
#undef ID3D11Device_CreateTexture2D
#define ID3D11Device_CreateTexture2D watched_texture
#undef ID3D11Device_CreateShaderResourceView
#define ID3D11Device_CreateShaderResourceView watched_srv
#undef ID3D11Device_CreateRenderTargetView
#define ID3D11Device_CreateRenderTargetView watched_rtv
#undef ID3D11DeviceContext_Map
#define ID3D11DeviceContext_Map watched_map
#undef ID3D11DeviceContext_UpdateSubresource
#define ID3D11DeviceContext_UpdateSubresource watched_update
#undef ID3D11DeviceContext_Draw
#define ID3D11DeviceContext_Draw watched_draw
#define GetTickCount watched_tick
#include "../../src/video/nv2a_d3d11.c"
#include "../nv2a_test_host.h"
#undef GetTickCount
#undef malloc
#undef realloc

static uint8_t ram[0x100000];
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)ram; }
HWND xbox_FramebufferWindowHandle(void) { return NULL; }
int xbox_FramebufferScanSource(uint32_t *va, uint32_t *pitch) { return 0; }
int xbox_FramebufferOverlay(XboxOverlay *ovl) { return 0; }
void xbox_FramebufferNoteFlip(uint32_t n) {}
void xbox_FramebufferRenderHostOverlay(void *device, void *context, void *target) {}
int xbox_GetDisplayMode(uint32_t *pitch, uint32_t *bpp, uint32_t *w, uint32_t *h) { (void)pitch; (void)bpp; (void)w; (void)h; return 0; }
const uint32_t (*nv2a_vsh_program(uint32_t *start, uint32_t *version))[4] { return NULL; }
const float (*nv2a_vsh_constants(uint32_t *version))[4] { return NULL; }
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static uint32_t pixel(Surface *s, unsigned x, unsigned y)
{
    ID3D11Texture2D *stage;
    D3D11_TEXTURE2D_DESC d;
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t value;
    int saved_inspect = inspect;
    inspect = 0;
    surface_resolve(s);
    ID3D11Texture2D_GetDesc(s->tex, &d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = d.MiscFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &stage)));
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)stage, (ID3D11Resource *)s->tex);
    CHECK(SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)stage, 0, D3D11_MAP_READ, 0, &m)));
    memcpy(&value, (uint8_t *)m.pData + y * m.RowPitch + x * 4, sizeof value);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)stage, 0);
    ID3D11Texture2D_Release(stage);
    inspect = saved_inspect;
    return value;
}
static void reset_slot(int k, int texture)
{
    if (s_up_srv[k]) ID3D11ShaderResourceView_Release(s_up_srv[k]);
    s_up_srv[k] = NULL;
    if (texture && s_up_tex[k]) { ID3D11Texture2D_Release(s_up_tex[k]); s_up_tex[k] = NULL; }
}
static void clear_gpu(Surface *s)
{
    float red[4] = {1, 0, 0, 1};
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s->rtv, red);
    s->ms_dirty = s->gpu_dirty = 1;
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
    fail_constants = 1;
    surface_sync(large_surface, 1);
    CHECK(!fail_constants && large_surface->gen == generation && !((uint32_t *)large_surface->shadow)[0]);
    CHECK(pixel(large_surface, 0, 0) == 0xFFFF0000u);
    surface_sync(large_surface, 0);
    CHECK(large_surface->gen == generation + 1 && s_sync_rows != large_surface->shadow);
    CHECK(pixel(large_surface, 0, 0) == 0xFF00FF00u && pixel(large_surface, 1, 0) == 0xFFFF0000u);
    CHECK(pixel(large_surface, 62, 31) == 0xFFFFFFFFu && pixel(large_surface, 63, 31) == 0xFF0000FFu);
    CHECK(draws == 4);
    surface_release(large_surface);
    clear_snapshot(); /* Must not alias either released surface's allocation. */
    printf("PASS: differently sized surface buffers alternate, grow, release and retry; %ux MSAA\n", s_msaa);
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

    /* No AvSetDisplayMode/scanout override and no new GPU commands: the
     * displayed buffer is still live memory while a title waits for Start. */
    rows[7 * 32 + 5] = 0xFF0000FFu;
    nv2a_d3d_tick();
    CHECK(s->gen == generation + 1 && draws == 1);
    CHECK(s->cpu_present_pending); /* no window: failed presentation must retry */
    CHECK(pixel(s, 5, 7) == 0xFF0000FFu);
    CHECK(pixel(s, 6, 7) == 0xFFFF0000u); /* preserve unrelated GPU pixels */

    previous_updates = updates; previous_draws = draws; previous_allocations = allocations;
    tick_now += 20;
    nv2a_d3d_tick();
    CHECK(updates == previous_updates && draws == previous_draws && allocations == previous_allocations);
    CHECK(s->gen == generation + 1);
    CHECK(s->cpu_present_pending);

    /* The command epoch does not advance between CPU animation frames. */
    rows[7 * 32 + 5] = 0xFF00FF00u;
    tick_now += 20;
    nv2a_d3d_tick();
    CHECK(s_epoch == epoch && s->gen == generation + 2 && draws == 2);
    CHECK(pixel(s, 5, 7) == 0xFF00FF00u && pixel(s, 6, 7) == 0xFFFF0000u);

    /* Recent GPU flips retain their normal submission/synchronization path. */
    rows[7 * 32 + 5] = 0xFFFFFFFFu;
    tick_now += 20;
    s_last_flip = tick_now;
    previous_updates = updates; previous_draws = draws; previous_allocations = allocations;
    nv2a_d3d_tick();
    CHECK(updates == previous_updates && draws == previous_draws && allocations == previous_allocations);
    CHECK(s->gen == generation + 2 && pixel(s, 5, 7) == 0xFF00FF00u);
    tick_now += 100;
    nv2a_d3d_tick();
    CHECK(s_epoch == epoch && s->gen == generation + 3 && draws == 3);
    CHECK(pixel(s, 5, 7) == 0xFFFFFFFFu && pixel(s, 6, 7) == 0xFFFF0000u);

    /* A readback still uses the old shadow to preserve CPU changes; do not
     * commit those changes until that older GPU copy has finished. */
    rows[7 * 32 + 5] = 0xFF0000FFu;
    s_wb_pending = 1;
    tick_now += 20;
    previous_updates = updates; previous_draws = draws;
    nv2a_d3d_tick();
    CHECK(updates == previous_updates && draws == previous_draws && s->gen == generation + 3);
    CHECK(((uint32_t *)s->shadow)[7 * 32 + 5] == 0xFFFFFFFFu);
    CHECK(pixel(s, 5, 7) == 0xFFFFFFFFu);
    s_wb_pending = 0;
    tick_now += 20;
    nv2a_d3d_tick();
    CHECK(s_epoch == epoch && s->gen == generation + 4 && draws == 4);
    CHECK(pixel(s, 5, 7) == 0xFF0000FFu && pixel(s, 6, 7) == 0xFFFF0000u);
    CHECK(s->cpu_present_pending);
    printf("PASS: idle CPU scanout, same-epoch animation, unchanged/active skips, pending-readback deferral and present retry; %ux MSAA\n", s_msaa);
}

static void test_colour_view_retry(void)
{
    Surface *s=surface_get(0x20000,128,8,32,32,0); CHECK(s);
    clear_gpu(s);
    for (unsigned failure=0; failure<3; failure++) {
        inspect=1; fail_texture=failure==0; fail_srv=failure==1; fail_rtv=failure==2;
        CHECK(!surface_y16(s));
        CHECK(!fail_texture && !fail_srv && !fail_rtv && !s->y16 && !s->y16_srv && !s->y16_rtv);
        inspect=0;
    }
    CHECK(surface_y16(s) && s->y16 && s->y16_rtv && s->y16_gen==s->gen);
    puts("PASS: colour conversion target failures release partial objects and retry");
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
    CHECK(!setup_pipeline(&st,3,&unused,&gw,&gh,&zmax));
    puts("PASS: absent depth converter reports unsupported without a proxy");
    return;
#else
    for (unsigned failure=0; failure<3; failure++) {
        inspect=1; fail_texture=failure==0; fail_srv=failure==1; fail_rtv=failure==2;
        CHECK(!setup_pipeline(&st,3,&unused,&gw,&gh,&zmax));
        CHECK(!fail_texture && !fail_srv && !fail_rtv && !d->y16 && !d->y16_srv && !d->y16_rtv);
        inspect=0;
    }
    CHECK(setup_pipeline(&st,3,&unused,&gw,&gh,&zmax));
    CHECK(depth_y16(d));
    D3D11_TEXTURE2D_DESC desc; ID3D11Texture2D *stage=NULL;
    ID3D11Texture2D_GetDesc(d->y16,&desc);
    desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0;
    desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ; desc.MiscFlags=0;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev,&desc,NULL,&stage)));
    ID3D11DeviceContext_CopyResource(s_ctx,(ID3D11Resource *)stage,(ID3D11Resource *)d->y16);
    D3D11_MAPPED_SUBRESOURCE map;
    CHECK(SUCCEEDED(ID3D11DeviceContext_Map(s_ctx,(ID3D11Resource *)stage,0,D3D11_MAP_READ,0,&map)));
    uint16_t *pixel=(uint16_t *)((uint8_t *)map.pData+map.RowPitch+64*8);
    CHECK(pixel[0]==65535 && pixel[1]==(shifted?0x4040:0xC0C0) && pixel[2]==0);
    ID3D11DeviceContext_Unmap(s_ctx,(ID3D11Resource *)stage,0); ID3D11Texture2D_Release(stage);
    printf("PASS: depth view %s alignment, %ux MSAA actual GPU pixels\n",shifted?"caller-shifted":"unshifted",s_msaa);
#endif
}

int main(int argc, char **argv)
{
    const char *test;
    Surface *s;
    uint32_t generation, *row;
    int initial, concurrent, bpp16, swizzled;
    CHECK(argc == 3); test = argv[1];
    initial = !strncmp(test, "initial_", 8) || !strcmp(test, "concurrent_initial");
    concurrent = !strncmp(test, "concurrent_", 11);
    bpp16 = !strcmp(test, "padded16"); swizzled = !strcmp(test, "swizzled");
    _putenv_s("RECOMP_MSAA", argv[2]); _putenv_s("RECOMP_RENDER_SCALE", !strncmp(test,"depth_view",10)?"2":"1");
    _putenv_s("RECOMP_PS_SPEC", "0"); _putenv_s("RECOMP_FPS_LIMIT", "0");
    CHECK(nv2a_d3d_init());
    present_buffer = (ID3D11Resource *)s_cb_present;
    if (!strcmp(test,"colour_view_retry")) { test_colour_view_retry(); return 0; }
    if (!strncmp(test,"depth_view",10)) { test_depth_view(!strcmp(test,"depth_view_shifted")); return 0; }
    if (!strcmp(test, "multiple_surfaces")) { test_multiple_surfaces(); return 0; }
    if (!strcmp(test, "idle_scanout")) { test_idle_scanout(); return 0; }
    s = surface_get(0x20000, bpp16 ? 80 : 128, bpp16 ? 3 : 8, 32, 32, swizzled);
    CHECK(s && s->shadow); present_buffer = (ID3D11Resource *)s_cb_present;
    clear_gpu(s); generation = s->gen;
    row = (uint32_t *)(ram + s->va);
    if (initial) { free(s->shadow); s->shadow = NULL; }
    if (bpp16) {
        uint16_t blue = 0x001F;
        memcpy(ram + s->va + 7 * s->pitch + 5 * 2, &blue, sizeof blue);
        memset(ram + s->va + 64, 0xFF, 16); /* padding must not upload */
    } else if (swizzled) {
        /* Morton address of pixel (5,7), with five interleaved x/y bits. */
        row[3 + 8 + 48] = 0xFF0000FFu;
    } else if (strcmp(test, "unchanged")) row[0] = 0xFF0000FFu;

    if (!strcmp(test, "initial_snapshot") || !strcmp(test, "dirty_snapshot")) {
        clear_snapshot(); fail_realloc = 1;
    } else if (!strcmp(test, "dirty_mask")) { clear_snapshot(); clear_mask(); fail_realloc = 2; }
    else if (strstr(test, "pixels")) { clear_pixels(); fail_malloc = 1; }
    else if (!strcmp(test, "initial_texture") || !strcmp(test, "dirty_texture")) {
        reset_slot(0, 1); fail_texture = 1;
    } else if (!strcmp(test, "initial_srv") || !strcmp(test, "dirty_srv")) {
        reset_slot(0, 0); fail_srv = 1;
    } else if (!strcmp(test, "mask_texture")) { reset_slot(1, 1); fail_texture = 1; }
    else if (!strcmp(test, "mask_srv")) { reset_slot(1, 1); fail_srv = 1; }
    else if (strstr(test, "constants")) fail_constants = 1;
    if (concurrent) late_write = row;
    inspect = 1;
    surface_sync(s, 1);
    if (!strcmp(test, "unchanged")) {
        surface_sync(s, 0); s_epoch++; surface_sync(s, 0);
        CHECK(!allocations && !updates && !draws && s->gen == generation);
        CHECK(pixel(s, 0, 0) == 0xFFFF0000u);
    } else if (bpp16 || swizzled) {
        CHECK(draws == 1 && pixel(s, 5, 7) == 0xFF0000FFu);
        CHECK(pixel(s, 6, 7) == 0xFFFF0000u);
        s_epoch++; surface_sync(s, 0); CHECK(draws == 1);
    } else if (concurrent) {
        CHECK(!late_write && draws == 1 && pixel(s, 0, 0) == 0xFF0000FFu);
        CHECK(*(uint32_t *)s->shadow == 0xFF0000FFu && row[0] == 0xFF00FF00u);
        s_epoch++; surface_sync(s, 0);
        CHECK(draws == 2 && pixel(s, 0, 0) == 0xFF00FF00u);
        CHECK(initial || pixel(s, 1, 0) == 0xFFFF0000u);
    } else {
        CHECK(!fail_malloc && !fail_realloc && !fail_texture && !fail_srv && !fail_constants);
        CHECK(!draws && s->gen == generation && s->synced_frame != s_epoch);
        CHECK(initial ? !s->shadow : *(uint32_t *)s->shadow == 0);
        CHECK(pixel(s, 0, 0) == 0xFFFF0000u);
        surface_sync(s, 0); /* no epoch advance: failures must remain retryable */
        CHECK(draws == 1 && s->gen == generation + 1 && s->synced_frame == s_epoch);
        CHECK(*(uint32_t *)s->shadow == 0xFF0000FFu && pixel(s, 0, 0) == 0xFF0000FFu);
        CHECK(initial || pixel(s, 1, 0) == 0xFFFF0000u);
        s_epoch++; surface_sync(s, 0); CHECK(draws == 1);
    }
    printf("PASS: %s, %ux MSAA; exact GPU pixels, successful shadow commit and retry\n", test, s_msaa);
    return 0;
}

/* These renderer-only fixtures never request executor frame replay. */
void nv2a_pb_interp_enable(int on) { (void)on; abort(); }
long nv2a_pb_frames_done(void) { abort(); return 0; }
int nv2a_pb_replay(float t) { (void)t; abort(); return 0; }
