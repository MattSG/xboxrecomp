/* Exercise real draw setup and GPU pixels after guest texture uploads. */
#include "../../src/video/nv2a_d3d11.c"
#include "../nv2a_test_host.h"

static uint8_t *ram;
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)ram; }
HWND xbox_FramebufferWindowHandle(void) { return NULL; }
int xbox_FramebufferScanSource(uint32_t *va, uint32_t *pitch) { return 0; }
int xbox_FramebufferOverlay(XboxOverlay *ovl) { return 0; }
void xbox_FramebufferNoteFlip(uint32_t draws) {}
int xbox_GetDisplayMode(uint32_t *pitch, uint32_t *bpp, uint32_t *w, uint32_t *h) { (void)pitch; (void)bpp; (void)w; (void)h; return 0; }
void xbox_FramebufferRenderHostOverlay(void *device, void *context, void *target) {}
const uint32_t (*nv2a_vsh_program(uint32_t *start, uint32_t *version))[4] { return NULL; }
const float (*nv2a_vsh_constants(uint32_t *version))[4] { return NULL; }

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

/* Compressed staging memory contains blocks, not BGRA pixels. Fetch through
 * the real GPU decoder before checking a sampled compressed texture. */
static uint32_t read_bc_pixel_at(ID3D11ShaderResourceView *view, unsigned x, unsigned y)
{
    static const char shader[] =
        "Texture2D<float4> source : register(t0);"
        "RWBuffer<uint> output : register(u0);"
        "cbuffer Coordinates : register(b0) { uint2 coordinate; uint2 pad; };"
        "[numthreads(1,1,1)] void main() {"
        " uint4 c=(uint4)round(saturate(source.Load(int3(coordinate,0)))*255.0);"
        " output[0]=c.b|(c.g<<8)|(c.r<<16)|(c.a<<24); }";
    ID3DBlob *code = NULL, *errors = NULL;
    ID3D11ComputeShader *compute = NULL;
    ID3D11Buffer *coordinates = NULL, *output = NULL, *staging = NULL;
    ID3D11UnorderedAccessView *uav = NULL;
    D3D11_BUFFER_DESC buffer_desc;
    D3D11_SUBRESOURCE_DATA data;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    uint32_t position[4] = {x,y,0,0}, value = 0xDEADBEEFu;
    if (FAILED(D3DCompile(shader, sizeof shader - 1, "read_bc_pixel", NULL, NULL,
                          "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
        goto done;
    if (FAILED(ID3D11Device_CreateComputeShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                                ID3D10Blob_GetBufferSize(code), NULL, &compute)))
        goto done;
    memset(&buffer_desc, 0, sizeof buffer_desc);
    buffer_desc.ByteWidth = sizeof position;
    buffer_desc.Usage = D3D11_USAGE_IMMUTABLE;
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    memset(&data, 0, sizeof data);
    data.pSysMem = position;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &buffer_desc, &data, &coordinates))) goto done;
    buffer_desc.ByteWidth = sizeof value;
    buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    buffer_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &buffer_desc, NULL, &output))) goto done;
    memset(&uav_desc, 0, sizeof uav_desc);
    uav_desc.Format = DXGI_FORMAT_R32_UINT;
    uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.NumElements = 1;
    if (FAILED(ID3D11Device_CreateUnorderedAccessView(s_dev, (ID3D11Resource *)output, &uav_desc, &uav)))
        goto done;
    buffer_desc.Usage = D3D11_USAGE_STAGING;
    buffer_desc.BindFlags = 0;
    buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &buffer_desc, NULL, &staging))) goto done;
    ID3D11DeviceContext_CSSetShader(s_ctx, compute, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(s_ctx, 0, 1, &view);
    ID3D11DeviceContext_CSSetConstantBuffers(s_ctx, 0, 1, &coordinates);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(s_ctx, 0, 1, &uav, NULL);
    ID3D11DeviceContext_Dispatch(s_ctx, 1, 1, 1);
    {
        ID3D11UnorderedAccessView *no_uav = NULL;
        ID3D11ShaderResourceView *no_srv = NULL;
        ID3D11Buffer *no_cb = NULL;
        ID3D11DeviceContext_CSSetUnorderedAccessViews(s_ctx, 0, 1, &no_uav, NULL);
        ID3D11DeviceContext_CSSetShaderResources(s_ctx, 0, 1, &no_srv);
        ID3D11DeviceContext_CSSetConstantBuffers(s_ctx, 0, 1, &no_cb);
        ID3D11DeviceContext_CSSetShader(s_ctx, NULL, NULL, 0);
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)staging, (ID3D11Resource *)output);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        memcpy(&value, mapped.pData, sizeof value);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)staging, 0);
    }
done:
    if (code) ID3D10Blob_Release(code);
    if (errors) ID3D10Blob_Release(errors);
    if (compute) ID3D11ComputeShader_Release(compute);
    if (coordinates) ID3D11Buffer_Release(coordinates);
    if (uav) ID3D11UnorderedAccessView_Release(uav);
    if (output) ID3D11Buffer_Release(output);
    if (staging) ID3D11Buffer_Release(staging);
    return value;
}

static uint32_t read_pixel_at(ID3D11ShaderResourceView *view, unsigned x, unsigned y)
{
    ID3D11Resource *resource = NULL;
    ID3D11Texture2D *source = NULL, *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    uint32_t value = 0xDEADBEEFu;
    HRESULT hr;
    if (!view) return value;
    ID3D11ShaderResourceView_GetResource(view, &resource);
    hr = ID3D11Resource_QueryInterface(resource, &IID_ID3D11Texture2D, (void **)&source);
    ID3D11Resource_Release(resource);
    if (FAILED(hr)) return value;
    ID3D11Texture2D_GetDesc(source, &desc);
    if (desc.Format == DXGI_FORMAT_BC1_UNORM || desc.Format == DXGI_FORMAT_BC2_UNORM ||
        desc.Format == DXGI_FORMAT_BC3_UNORM) {
        ID3D11Texture2D_Release(source);
        return x < desc.Width && y < desc.Height ? read_bc_pixel_at(view, x, y) : value;
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = ID3D11Device_CreateTexture2D(s_dev, &desc, NULL, &staging);
    if (SUCCEEDED(hr)) {
        ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)staging,
                                       (ID3D11Resource *)source);
        hr = ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)staging, 0,
                                   D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr)) {
            if (x < desc.Width && y < desc.Height)
                memcpy(&value, (const uint8_t *)mapped.pData + y * mapped.RowPitch +
                       x * (desc.Format == DXGI_FORMAT_R16G16B16A16_UNORM ? 8 : 4),
                       sizeof value);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)staging, 0);
        }
        ID3D11Texture2D_Release(staging);
    }
    ID3D11Texture2D_Release(source);
    return value;
}

static uint32_t read_pixel(ID3D11ShaderResourceView *view)
{
    return read_pixel_at(view, 0, 0);
}

int main(int argc, char **argv)
{
    NvD3DState st = {0};
    Surface *sample, *target;
    ID3D11RenderTargetView *bound = NULL;
    ID3D11DepthStencilView *depth = NULL;
    D3D11_VIEWPORT vp;
    UINT nvp = 1;
    uint32_t gw, gh, zmax;
    unsigned i;
    ram = calloc(1, 0x100000);
    CHECK(ram);
    _putenv_s("RECOMP_MSAA", argc > 1 ? argv[1] : "1");
    _putenv_s("RECOMP_RENDER_SCALE", "1");
    _putenv_s("RECOMP_PS_SPEC", "0");
    _putenv_s("RECOMP_FPS_LIMIT", "0");
    CHECK(nv2a_d3d_init());
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
    CHECK(setup_pipeline(&st, NV_D3D_TRIANGLES, &target, &gw, &gh, &zmax));
    ID3D11DeviceContext_OMGetRenderTargets(s_ctx, 1, &bound, &depth);
    CHECK(bound == target->rtv);
    CHECK(depth);
    ID3D11DepthStencilView_Release(depth);
    ID3D11RenderTargetView_Release(bound);
    /* CPU change forces surface_sync to draw an upload during texture setup. */
    ram[sample->va] ^= 0xFF;
    s_epoch++;
    CHECK(setup_pipeline(&st, NV_D3D_TRIANGLES, &target, &gw, &gh, &zmax));
    bound = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(s_ctx, 1, &bound, &depth);
    CHECK(bound == target->rtv);
    CHECK(depth);
    ID3D11DepthStencilView_Release(depth);
    ID3D11RenderTargetView_Release(bound);
    ID3D11DeviceContext_RSGetViewports(s_ctx, &nvp, &vp);
    CHECK(nvp == 1 && vp.Width == 64 && vp.Height == 64);
    puts("PASS: sampled-surface upload preserves draw targets and viewport");

    /* A full cache must retain a recently used render target. The victim
     * search used to stop after slot 0, discarding it on every new target
     * even while much older resources occupied the other slots. */
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

    {
        NvD3DTexture texture = {0};
        ID3D11ShaderResourceView *view;
        uint32_t *pixels = (uint32_t *)(ram + 0x70000);
        uint32_t *palette = (uint32_t *)(ram + 0x73000);
        uint32_t frame;
        texture.addr = 0x70000;
        texture.color = 0x12; /* Linear A8R8G8B8. */
        texture.format = 0x1200;
        texture.width = texture.height = 4;
        texture.pitch = 16;
        texture.levels = 1;
        for (i = 0; i < 16; i++) pixels[i] = 0xFF000000u;
        view = texture_get(&texture);
        CHECK(view && read_pixel(view) == 0xFF000000u);
        nv2a_d3d_kick();
        CHECK(texture_get(&texture) == view); /* unchanged data keeps its upload */
        frame = s_frame;
        for (i = 0; i < 16; i++) pixels[i] = 0xFF00FF00u;
        nv2a_d3d_kick();
        CHECK(s_frame == frame);
        CHECK(read_pixel(texture_get(&texture)) == 0xFF00FF00u);
        nv2a_d3d_flip(0, 0);
        CHECK(s_frame == frame + 1);
        CHECK(read_pixel(texture_get(&texture)) == 0xFF00FF00u);
        puts("PASS: guest texture rewrite reaches GPU pixels before the next flip");

        memset(&texture, 0, sizeof texture);
        texture.addr = 0x72000;
        texture.color = 0x0B; /* P8, uniform indices avoid swizzle ambiguity. */
        texture.format = 0x0B00;
        texture.width = texture.height = 4;
        texture.levels = 1;
        texture.palette = 0x73000;
        texture.palette_len = 256;
        memset(ram + texture.addr, 7, 16);
        palette[7] = 0xFFFF0000u;
        CHECK(read_pixel(texture_get(&texture)) == 0xFFFF0000u);
        frame = s_frame;
        palette[7] = 0xFF0000FFu;
        nv2a_d3d_kick();
        CHECK(s_frame == frame);
        CHECK(read_pixel(texture_get(&texture)) == 0xFF0000FFu);
        nv2a_d3d_flip(0, 0);
        CHECK(s_frame == frame + 1);
        CHECK(read_pixel(texture_get(&texture)) == 0xFF0000FFu);
        puts("PASS: guest palette rewrite reaches GPU pixels before the next flip");
    }
    /* Pending readbacks must not follow a cache slot when the target is
     * released and replaced. Equal dimensions used to pass the readback
     * check and overwrite unrelated guest memory with the old frame. */
    for (i = 0; i < MAX_SURFACES; i++) surface_release(&s_surf[i]);
    {
        Surface *old, *replacement;
        uint32_t *pixels;
        float red[4] = { 1, 0, 0, 1 };
        unsigned same_address, j;
        for (same_address = 0; same_address < 2; same_address++) {
            uint32_t old_va = 0x20000, new_va = same_address ? old_va : 0x30000;
            old = surface_get(old_va, 128, 8, 32, 32, 0);
            CHECK(old);
            ID3D11DeviceContext_ClearRenderTargetView(s_ctx, old->rtv, red);
            old->ms_dirty = 1;
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

        /* Retained targets still write GPU pixels back, while a row the
         * CPU changed after submission stays intact for the next sync. */
        old = surface_get(0x20000, 128, 8, 32, 32, 0);
        CHECK(old);
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, old->rtv, red);
        old->ms_dirty = 1;
        old->gpu_dirty = 1;
        writeback_surfaces();
        CHECK(s_wb_pending && s_wb[old - s_surf].pending);
        pixels = (uint32_t *)(ram + old->va);
        for (j = 0; j < 32; j++) pixels[j] = 0xFF00FF00u;
        {
            ULONGLONG deadline = GetTickCount64() + 5000;
            /* The submission at writeback_surfaces must make progress even
             * if the title only polls for its fence and queues no more work. */
            while (nv2a_d3d_writeback_busy()) {
                CHECK(GetTickCount64() < deadline);
                Sleep(0);
            }
        }
        for (j = 0; j < 32; j++) CHECK(pixels[j] == 0xFF00FF00u);
        for (j = 32; j < 32 * 32; j++) CHECK(pixels[j] == 0xFFFF0000u);
        puts("PASS: retained target writeback preserves later CPU writes and updates untouched rows");
    }
    for (i = 0; i < MAX_SURFACES; i++) surface_release(&s_surf[i]);
    {
        Surface *sampled, *drawn;
        ID3D11ShaderResourceView *view = NULL;
        float red[4] = { 1, 0, 0, 1 };
        unsigned bpp, j;
        for (bpp = 2; bpp <= 4; bpp += 2) {
            uint32_t w = bpp == 2 ? 32 : 16, h = bpp == 2 ? 16 : 32;
            uint32_t va = 0x40000 + bpp * 0x1000;
            if (bpp == 2)
                for (j = 0; j < w * h; j++) ((uint16_t *)(ram + va))[j] = 0x001F;
            else
                for (j = 0; j < w * h; j++) ((uint32_t *)(ram + va))[j] = 0xFF0000FFu;
            sampled = surface_get(va, w * bpp, bpp == 2 ? 3 : 8, w, h, 1);
            CHECK(sampled && read_pixel(sampled->srv) == 0xFF0000FFu);
            ID3D11DeviceContext_ClearRenderTargetView(s_ctx, sampled->rtv, red);
            sampled->gen++;
            surface_sync(sampled, 1);
            CHECK(read_pixel(sampled->srv) == 0xFFFF0000u);
            /* Morton (3,5) is byte-independent texel index39 for both
             * rectangles: X contributes5, Y contributes34. */
            if (bpp == 2) ((uint16_t *)(ram + va))[39] = 0x07E0;
            else ((uint32_t *)(ram + va))[39] = 0xFF00FF00u;
            nv2a_d3d_kick();
            memset(&st, 0, sizeof st);
            st.color_addr = 0x10000; st.color_pitch = 64 * 4;
            st.surface_format = 8; st.clip_w = st.clip_h = 64;
            st.color_mask = 0x01010101; st.tex_used = 1;
            st.tex[0].addr = va; st.tex[0].color = bpp == 2 ? 0x05 : 0x06;
            st.tex[0].format = st.tex[0].color << 8;
            st.tex[0].width = w; st.tex[0].height = h; st.tex[0].levels = 1;
            CHECK(setup_pipeline(&st, NV_D3D_TRIANGLES, &drawn, &gw, &gh, &zmax));
            ID3D11DeviceContext_PSGetShaderResources(s_ctx, 0, 1, &view);
            CHECK(view == sampled->srv);
            CHECK(read_pixel_at(view, 3, 5) == 0xFF00FF00u);
            CHECK(read_pixel_at(view, 3, 4) == 0xFFFF0000u);
            CHECK(read_pixel_at(view, 0, 1) == 0xFFFF0000u);
            ID3D11ShaderResourceView_Release(view);
            view = NULL;
        }
        puts("PASS: swizzled16/32-bit CPU pixels synchronize while unchanged GPU pixels survive");

        /* Reuse the last target's block as ordinary DXT1 and P8 data.
         * Draw setup must bind their uploaded images rather than the old RT. */
        for (j = 0; j < 4 * 8; j++) {
            uint8_t *block = ram + st.tex[0].addr + j * 8;
            block[0] = 0xE0; block[1] = 0x07;
            memset(block + 2, 0, 6);
        }
        st.tex[0].color = 0x0C; st.tex[0].format = 0x0C00;
        nv2a_d3d_kick();
        CHECK(setup_pipeline(&st, NV_D3D_TRIANGLES, &drawn, &gw, &gh, &zmax));
        ID3D11DeviceContext_PSGetShaderResources(s_ctx, 0, 1, &view);
        CHECK(view != sampled->srv && read_pixel(view) == 0xFF00FF00u);
        ID3D11ShaderResourceView_Release(view);
        memset(ram + st.tex[0].addr, 7, 16 * 32);
        ((uint32_t *)(ram + 0x74000))[7] = 0xFF0000FFu;
        st.tex[0].color = 0x0B; st.tex[0].format = 0x0B00;
        st.tex[0].palette = 0x74000; st.tex[0].palette_len = 256;
        nv2a_d3d_kick();
        CHECK(setup_pipeline(&st, NV_D3D_TRIANGLES, &drawn, &gw, &gh, &zmax));
        view = NULL;
        ID3D11DeviceContext_PSGetShaderResources(s_ctx, 0, 1, &view);
        CHECK(view != sampled->srv && read_pixel(view) == 0xFF0000FFu);
        ID3D11ShaderResourceView_Release(view);
        puts("PASS: compressed/paletted uploads at old target addresses bind their decoded pixels");

        /* Y16 is an intentional reinterpretation: one32-bit colour/depth
         * pixel supplies two16-bit luminance texels. Preserve it, including
         * depth taking precedence when both targets share an address. */
        sampled = surface_get(0x50000, 128, 8, 32, 32, 0);
        CHECK(sampled);
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, sampled->rtv, red);
        sampled->ms_dirty = 1; sampled->gen++;
        memset(&st, 0, sizeof st);
        st.color_addr = 0x10000; st.color_pitch = 64 * 4;
        st.surface_format = 8; st.clip_w = st.clip_h = 64;
        st.color_mask = 0x01010101; st.tex_used = 1;
        st.tex[0].addr = sampled->va; st.tex[0].color = 0x35;
        st.tex[0].format = 0x3500; st.tex[0].pitch = 128;
        st.tex[0].width = 64; st.tex[0].height = 32; st.tex[0].levels = 1;
        nv2a_d3d_kick();
        CHECK(setup_pipeline(&st, NV_D3D_TRIANGLES, &drawn, &gw, &gh, &zmax));
        view = NULL;
        ID3D11DeviceContext_PSGetShaderResources(s_ctx, 0, 1, &view);
        CHECK(view && view == sampled->y16_srv);
        /* The readback helper returns R16/G16 packed for these views.
         * RedARGB has low16=0, high16=FFFF, yielding black then white. */
        CHECK(read_pixel_at(view, 0, 0) == 0);
        CHECK(read_pixel_at(view, 1, 0) == 0xFFFFFFFFu);
        ID3D11ShaderResourceView_Release(view);
        {
            Depth *zsample = depth_get(sampled->va, 32, 32, s_msaa);
            CHECK(zsample);
            zsample->zf = 2; zsample->w = zsample->h = 32;
            zsample->seq = ++s_surf_seq; zsample->gen++;
            ID3D11DeviceContext_ClearDepthStencilView(s_ctx, zsample->dsv,
                D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
            nv2a_d3d_kick();
            CHECK(setup_pipeline(&st, NV_D3D_TRIANGLES, &drawn, &gw, &gh, &zmax));
            view = NULL;
            ID3D11DeviceContext_PSGetShaderResources(s_ctx, 0, 1, &view);
            CHECK(view && view == zsample->y16_srv && view != sampled->y16_srv);
            CHECK(read_pixel_at(view, 1, 0) == 0xFFFFFFFFu);
            ID3D11ShaderResourceView_Release(view);
        }
        puts("PASS: colour/depth Y16 aliases retain GPU values and depth precedence");
    }
    {
        /* Targets that cannot bind are refused and counted, never silently dropped. */
        NvD3DState bad = {0};
        Surface *none_s;
        Depth *none_d;
        uint32_t none_w, none_h;
        unsigned before = s_target_refusals;
        bad.color_pitch = 64 * 4; bad.surface_format = 0x28;
        bad.clip_w = bad.clip_h = 64;
        bad.color_addr = 0x07FFF000; /* 64 x 64 x 4 runs past the guest window */
        CHECK(!bind_targets(&bad, 0, &none_s, &none_d, &none_w, &none_h));
        CHECK(s_target_refusals == before + 1);
        bad.color_addr = 0x10000; bad.surface_format = 0x00; /* no colour bpp */
        CHECK(!bind_targets(&bad, 0, &none_s, &none_d, &none_w, &none_h));
        CHECK(s_target_refusals == before + 2);
        bad.surface_format = 0x28; bad.clip_w = 5000;
        CHECK(!bind_targets(&bad, 0, &none_s, &none_d, &none_w, &none_h));
        CHECK(s_target_refusals == before + 3);
        puts("PASS: unbindable colour targets are refused and counted");
    }
    return 0;
}
