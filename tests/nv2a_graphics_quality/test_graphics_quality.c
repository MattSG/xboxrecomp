/* Run the production renderer and its actual HLSL on the selected D3D11 GPU. */
#include "../../src/video/nv2a_d3d11.c"
#include "../nv2a_test_host.h"
#include <math.h>

static uint8_t ram[0x400000];
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)ram; }
HWND xbox_FramebufferWindowHandle(void) { return NULL; }
int xbox_FramebufferScanSource(uint32_t *va, uint32_t *pitch) { return 0; }
int xbox_FramebufferOverlay(XboxOverlay *ovl) { return 0; }
void xbox_FramebufferNoteFlip(uint32_t draws) {}
void xbox_FramebufferRenderHostOverlay(void *device, void *context, void *target) {}
int xbox_GetDisplayMode(uint32_t *pitch, uint32_t *bpp, uint32_t *w, uint32_t *h) { return 0; }
const uint32_t (*nv2a_vsh_program(uint32_t *start, uint32_t *version))[4] { return NULL; }
const float (*nv2a_vsh_constants(uint32_t *version))[4] { return NULL; }

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#define TAPS 128
#define TEX_W 8
#define TEX_H 8

static ID3DBlob *compile_fixture(const char *suffix, const char *entry, const char *profile)
{
    size_t length = strlen(s_hlsl) + strlen(suffix) + 1;
    char *source = malloc(length);
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr;
    CHECK(source);
    strcpy_s(source, length, s_hlsl); strcat_s(source, length, suffix);
    hr = D3DCompile(source, length - 1, "quality_fixture", NULL, NULL, entry, profile,
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr) && errors) fprintf(stderr,"%s\n",(char *)ID3D10Blob_GetBufferPointer(errors));
    free(source); if (errors) ID3D10Blob_Release(errors);
    CHECK(SUCCEEDED(hr) && code); return code;
}

static ID3D11Buffer *default_buffer(UINT bytes, UINT bind)
{
    D3D11_BUFFER_DESC desc = {0}; ID3D11Buffer *buffer = NULL;
    desc.ByteWidth = bytes; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = bind;
    CHECK(SUCCEEDED(ID3D11Device_CreateBuffer(s_dev, &desc, NULL, &buffer)));
    return buffer;
}

static void read_buffer(ID3D11Buffer *source, void *out, size_t bytes)
{
    D3D11_BUFFER_DESC desc; ID3D11Buffer *staging = NULL;
    D3D11_MAPPED_SUBRESOURCE mapped;
    ID3D11Buffer_GetDesc(source, &desc);
    CHECK(bytes <= desc.ByteWidth);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(SUCCEEDED(ID3D11Device_CreateBuffer(s_dev, &desc, NULL, &staging)));
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)staging, (ID3D11Resource *)source);
    CHECK(SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped)));
    memcpy(out, mapped.pData, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)staging, 0);
    ID3D11Buffer_Release(staging);
}

static int wrapped(int value, int size)
{
    int result = value % size; return result < 0 ? result + size : result;
}

/* Independent cardinal cubic kernel, evaluated at 16 texels, rather than the
 * shader's grouped four bilinear taps. */
static double cubic_kernel(double distance)
{
    double a = fabs(distance);
    if (a < 1.0) return 2.0 / 3.0 - a * a + a * a * a / 2.0;
    if (a < 2.0) return pow(2.0 - a, 3.0) / 6.0;
    return 0;
}

static void reference_tap(const float pixels[TEX_W * TEX_H][4], double u, double v,
                          int cubic, double result[4])
{
    double px = u * TEX_W - 0.5, py = v * TEX_H - 0.5;
    int bx = (int)floor(px), by = (int)floor(py), x, y, c;
    memset(result, 0, 4 * sizeof(double));
    for (y = cubic ? -1 : 0; y <= (cubic ? 2 : 1); ++y)
        for (x = cubic ? -1 : 0; x <= (cubic ? 2 : 1); ++x) {
            double wx = cubic ? cubic_kernel(px - (bx + x)) :
                         x ? px - bx : 1.0 - (px - bx);
            double wy = cubic ? cubic_kernel(py - (by + y)) :
                         y ? py - by : 1.0 - (py - by);
            unsigned index = wrapped(by + y, TEX_H) * TEX_W + wrapped(bx + x, TEX_W);
            for (c = 0; c < 4; ++c) result[c] += pixels[index][c] * wx * wy;
        }
}

static void test_panorama(void)
{
    static const char suffix[] =
        "cbuffer Quality:register(b4) { uint quality_stage; uint3 quality_pad; };\n"
        "float4 quality_ps(float4 position:SV_Position):SV_Target {\n"
        " uint id=(uint)position.x;\n"
        " float2 uv=float2(-.125+(id%16)*.0875, -.25+(id/16)*.19375);\n"
        " return samp2(quality_stage,uv); }\n";
    ID3DBlob *code = compile_fixture(suffix,"quality_ps","ps_5_0");
    ID3D11PixelShader *pixel_shader = NULL; ID3D11Texture2D *texture = NULL;
    ID3D11ShaderResourceView *view = NULL, *views[2];
    ID3D11Texture2D *output = NULL, *staging = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11Buffer *settings = make_buffer(16,D3D11_BIND_CONSTANT_BUFFER);
    ID3D11SamplerState *state = NULL, *states[2];
    D3D11_TEXTURE2D_DESC td = {0}; D3D11_SUBRESOURCE_DATA data = {0};
    D3D11_SAMPLER_DESC sd = {0};
    D3D11_VIEWPORT vp = {0,0,TAPS,1,0,1}; D3D11_RECT sc = {0,0,TAPS,1};
    float pixels[TEX_W * TEX_H][4], actual[TAPS][4];
    unsigned i, c, pass, changed = 0; double maximum_error = 0;
    CHECK(settings);
    CHECK(SUCCEEDED(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                                   ID3D10Blob_GetBufferSize(code), NULL, &pixel_shader)));
    ID3D10Blob_Release(code);
    for (i = 0; i < TEX_W * TEX_H; ++i) {
        pixels[i][0] = ((i * 37 + 11) % 101) / 100.0f;
        pixels[i][1] = ((i * 17 + 31) % 101) / 100.0f;
        pixels[i][2] = ((i * 23 + 53) % 101) / 100.0f;
        pixels[i][3] = (i % TEX_W) < 4 ? 0.0f : 1.0f;
    }
    td.Width = TEX_W; td.Height = TEX_H; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    data.pSysMem = pixels; data.SysMemPitch = TEX_W * 16;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev,&td,&data,&texture)));
    CHECK(SUCCEEDED(ID3D11Device_CreateShaderResourceView(s_dev,(ID3D11Resource *)texture,NULL,&view)));
    td.Width = TAPS; td.Height = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev,&td,NULL,&output)));
    CHECK(SUCCEEDED(ID3D11Device_CreateRenderTargetView(s_dev,(ID3D11Resource *)output,NULL,&rtv)));
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev,&td,NULL,&staging)));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    CHECK(SUCCEEDED(ID3D11Device_CreateSamplerState(s_dev,&sd,&state)));
    views[0] = views[1] = view; states[0] = states[1] = state;
    for (pass = 0; pass < 3; ++pass) {
        PSConsts pc = {0}; uint32_t setting[4] = {pass == 2,0,0,0};
        D3D11_MAPPED_SUBRESOURCE mapped;
        pc.ps_flags = pass ? 4u : 0u;
        CHECK(upload_cb(s_cb_ps,&pc,sizeof pc) && upload_cb(settings,setting,sizeof setting));
        unbind_targets(); unbind_textures();
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx,1,&rtv,NULL);
        ID3D11DeviceContext_OMSetBlendState(s_ctx,s_blend_opaque,NULL,0xFFFFFFFF);
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx,s_ds_off,0);
        ID3D11DeviceContext_RSSetState(s_ctx,s_rs_plain);
        ID3D11DeviceContext_RSSetViewports(s_ctx,1,&vp);
        ID3D11DeviceContext_RSSetScissorRects(s_ctx,1,&sc);
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        bind_vertex_shader(s_y16_vs); bind_pixel_shader(pixel_shader);
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx,1,1,&s_cb_ps);
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx,4,1,&settings);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx,0,2,views);
        bind_pixel_samplers(2,states);
        ID3D11DeviceContext_Draw(s_ctx,3,0);
        unbind_targets();
        ID3D11DeviceContext_CopyResource(s_ctx,(ID3D11Resource *)staging,(ID3D11Resource *)output);
        CHECK(SUCCEEDED(ID3D11DeviceContext_Map(s_ctx,(ID3D11Resource *)staging,0,D3D11_MAP_READ,0,&mapped)));
        memcpy(actual,mapped.pData,sizeof actual);
        ID3D11DeviceContext_Unmap(s_ctx,(ID3D11Resource *)staging,0);
        for (i = 0; i < TAPS; ++i) {
            /* Match the float UV supplied to the GPU; evaluate the reference
             * itself in double precision, with an independent kernel. */
            float u = -.125f + (i % 16) * .0875f, v = -.25f + (i / 16) * .19375f;
            double expected[4], bilinear[4];
            reference_tap(pixels,u,v,pass == 1,expected);
            reference_tap(pixels,u,v,0,bilinear);
            for (c = 0; c < 4; ++c) {
                double error = fabs(actual[i][c] - expected[c]);
                if (error > maximum_error) maximum_error = error;
                /* D3D11 bilinear weight precision can quantize fractions. */
                CHECK(error < .004);
                CHECK(actual[i][c] >= -.0001f && actual[i][c] <= 1.0001f);
                if (pass == 1 && fabs(expected[c] - bilinear[c]) > .03) ++changed;
            }
        }
    }
    CHECK(changed > 100);
    unbind_textures(); bind_pixel_shader(s_psv[0]);
    ID3D11SamplerState_Release(state);
    ID3D11Buffer_Release(settings); ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(output); ID3D11Texture2D_Release(staging);
    ID3D11ShaderResourceView_Release(view); ID3D11Texture2D_Release(texture);
    ID3D11PixelShader_Release(pixel_shader);
    printf("PASS: 1536 native RGBA samples versus independent cubic/bilinear references, wrap seams and stage isolation; max error %.6f\n",maximum_error);
}

static NvD3DState state_for(unsigned width, unsigned height)
{
    NvD3DState st = {0};
    st.color_addr = 0x10000; st.color_pitch = width * 4;
    st.surface_format = 0x28; st.clip_w = width; st.clip_h = height;
    st.color_mask = 0x01010101; return st;
}

static void assert_panorama_gate(NvD3DState *st, int expected)
{
    Surface *target; uint32_t w,h,z; PSConsts pc;
    CHECK(setup_pipeline(st,NV_D3D_TRIANGLES,&target,&w,&h,&z));
    read_buffer(s_cb_ps,&pc,sizeof pc);
    CHECK(((pc.ps_flags & 4u) != 0) == expected);
}

static void test_gates(void)
{
    NvD3DState base = state_for(64,64), st;
    unsigned i;
    base.tex_used = 1; base.blend_enable = 1;
    base.blend_sfactor = 0x304; base.blend_dfactor = 0x305;
    base.tex[0].addr = 0x100000; base.tex[0].color = 0x0F;
    base.tex[0].format = 0x09000F00; base.tex[0].levels = 9;
    base.tex[0].width = base.tex[0].height = 256;
    base.tex[0].filter = (6u << 16) | (2u << 24);
    base.tex[0].control0 = 8u * 256u << 6; base.tex[0].address = 0x030101;
    memset(ram + 0x100000,0x55,0x90000);
    nv2a_d3d_set_bspline_blend(0,0); assert_panorama_gate(&base,0);
    nv2a_d3d_set_bspline_blend(0x304,0x305); assert_panorama_gate(&base,1);
    for (i = 0; i < 10; ++i) {
        st = base;
        switch(i) {
        case 0: st.depth_test = 1; st.zeta_addr = 0x20000; st.zeta_pitch = 64*4; break;
        case 1: st.blend_enable = 0; break;
        case 2: st.blend_sfactor = 0x302; break;
        case 3: st.blend_dfactor = 0x303; break;
        case 4: st.tex[0].width = 128; st.tex[0].format++; break;
        case 5: st.tex[0].height = 128; st.tex[0].format++; break;
        case 6: st.tex[0].color = 0x0C; st.tex[0].format++; break;
        case 7: st.tex[0].levels = 4; st.tex[0].format++; break;
        case 8: st.tex[0].filter = (5u << 16) | (2u << 24); break;
        case 9: st.tex[0].filter = (6u << 16) | (1u << 24); break;
        }
        assert_panorama_gate(&st,0);
    }
    st = base; st.tex[0].cube = 1; st.tex[0].format++;
    assert_panorama_gate(&st,0);
    st = base; st.rc_seen = 1; st.rc.stage_program = 1u << 5;
    st.tex[1] = st.tex[0]; memset(&st.tex[0],0,sizeof st.tex[0]);
    assert_panorama_gate(&st,0);
    nv2a_d3d_set_bspline_blend(0,0); assert_panorama_gate(&base,0);
    puts("PASS: actual pixel constants select only title-enabled stage-zero 256x256 linearly-filtered DXT5 no-depth panorama; twelve ordinary/cube/stage/mip/blend/filter cases remain unchanged");
}

static void test_depth(void)
{
    const UINT samples[] = {1,4}; unsigned variant,scale,x,y;
    ID3D11DepthStencilState *write_depth = NULL;
    D3D11_DEPTH_STENCIL_DESC dd = {0};
    dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    CHECK(SUCCEEDED(ID3D11Device_CreateDepthStencilState(s_dev,&dd,&write_depth)));
    for (variant = 0; variant < 2; ++variant) {
        Depth *d = depth_get(0x300000 + variant * 0x10000,16,2,samples[variant]);
        D3D11_VIEWPORT vp = {0,0,16,2,0,1}; D3D11_RECT sc = {0,0,1,2};
        float depth[16];
        CHECK(d && d->dsv && d->zsrv);
        unbind_textures(); unbind_targets();
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx,0,NULL,d->dsv);
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx,write_depth,0);
        ID3D11DeviceContext_RSSetState(s_ctx,s_rs_plain);
        ID3D11DeviceContext_RSSetViewports(s_ctx,1,&vp);
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        bind_vertex_shader(s_clear_vs); bind_vertex_constants(s_cb_clear);
        bind_pixel_shader(s_clear_ps);
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx,0,1,&s_cb_clear);
        for (x = 0; x < 16; ++x) {
            float cc[8] = {0}; depth[x] = ((x * 7) % 16 + 0.5f) / 16.0f;
            cc[4] = depth[x]; CHECK(upload_cb(s_cb_clear,cc,sizeof cc));
            sc.left = x; sc.right = x+1;
            ID3D11DeviceContext_RSSetScissorRects(s_ctx,1,&sc);
            ID3D11DeviceContext_Draw(s_ctx,4,0);
        }
        unbind_targets();
        for (scale = 1; scale <= 8; scale *= 2) {
            ID3D11ShaderResourceView *view; ID3D11Texture2D *staging = NULL;
            D3D11_TEXTURE2D_DESC desc; D3D11_MAPPED_SUBRESOURCE mapped;
            d->w = 16 / scale; d->h = 2; ++d->gen;
            view = depth_y16(d); CHECK(view);
            ID3D11Texture2D_GetDesc(d->y16,&desc);
            desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev,&desc,NULL,&staging)));
            ID3D11DeviceContext_CopyResource(s_ctx,(ID3D11Resource *)staging,(ID3D11Resource *)d->y16);
            CHECK(SUCCEEDED(ID3D11DeviceContext_Map(s_ctx,(ID3D11Resource *)staging,0,D3D11_MAP_READ,0,&mapped)));
            for (y = 0; y < 2; ++y) for (x = 0; x < 32; ++x) {
                /* A high-word tap is centered a quarter guest pixel right.
                 * The view must compensate in host coordinates, including
                 * the left boundary where the coordinate clamps to zero. */
                int column = (int)floor((x + .5) / 2.0 - .25 * scale);
                unsigned expected; const uint16_t *pixel;
                if (column < 0) column = 0;
                expected = (unsigned)floor(depth[column] * 16777215.0 + .5) >> 8;
                pixel = (const uint16_t *)((const uint8_t *)mapped.pData + y * mapped.RowPitch) + x*4;
                CHECK(abs((int)pixel[0] - (int)expected) <= 1);
                CHECK(pixel[0] == pixel[1] && pixel[1] == pixel[2] && pixel[3] == 65535);
            }
            ID3D11DeviceContext_Unmap(s_ctx,(ID3D11Resource *)staging,0);
            ID3D11Texture2D_Release(staging);
        }
    }
    ID3D11DepthStencilState_Release(write_depth);
    puts("PASS: 512 native depth-view pixels at scales 1/2/4/8, single-sample and 4x MSAA; quarter-pixel compensation and left clamping preserve packed depth");
}

static void test_placement(void)
{
    static const char suffix[] =
        "struct QualityPosition { float4 pos:SV_Position; };\n"
        "[maxvertexcount(1)] void quality_gs(point QualityPosition input[1],\n"
        " inout PointStream<QualityPosition> output) { output.Append(input[0]); }\n";
    ID3DBlob *code = compile_fixture(suffix,"quality_gs","gs_5_0");
    ID3D11GeometryShader *geometry = NULL;
    D3D11_SO_DECLARATION_ENTRY declaration = {0,"SV_POSITION",0,0,4,0};
    UINT stride = 16, offset = 0;
    ID3D11Buffer *output = default_buffer(7*16,D3D11_BIND_STREAM_OUTPUT);
    float vertices[7][4] = {{.53125f,.53125f,0,1},{640.53125f,480.53125f,0,1},
        {12,16,0,1},{12.53125f,16.53125f,0,1},{12.4f,16.4f,0,2},
        {.5625f,.5625f,0,1},{.5f,.55f,0,1}};
    uint32_t indices[7] = {0,1,2,3,4,5,6}; unsigned target,i;
    CHECK(SUCCEEDED(ID3D11Device_CreateGeometryShaderWithStreamOutput(s_dev,
        ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),&declaration,1,
        &stride,1,D3D11_SO_NO_RASTERIZED_STREAM,NULL,&geometry)));
    ID3D10Blob_Release(code);
    for (target = 0; target < 2; ++target) {
        unsigned w = target ? 64 : 640, h = target ? 64 : 480;
        NvD3DState st = state_for(w,h); NvD3DAttrib attr[16] = {{0}};
        float def[16][4] = {{0}}, captured[7][4]; ID3D11Buffer *none = NULL;
        Surface *surface; uint32_t gw,gh,z;
        attr[0].enabled = 1; attr[0].type = 2; attr[0].size = 4;
        attr[0].stride = 16; attr[0].data = (const uint8_t *)vertices;
        def[3][0] = def[3][1] = def[3][2] = def[3][3] = 1;
        /* First use uploads CPU framebuffer pixels through a draw. Complete
         * those real setup blits before attaching stream output so this
         * buffer contains only the seven requested guest vertices. */
        CHECK(setup_pipeline(&st,NV_D3D_POINTS,&surface,&gw,&gh,&z));
        ID3D11DeviceContext_GSSetShader(s_ctx,geometry,NULL,0);
        ID3D11DeviceContext_SOSetTargets(s_ctx,1,&output,&offset);
        nv2a_d3d_draw_ub(&st,NV_D3D_POINTS,attr,(const float (*)[4])def,0,indices,7,0,6,NULL);
        ID3D11DeviceContext_SOSetTargets(s_ctx,1,&none,&offset);
        ID3D11DeviceContext_GSSetShader(s_ctx,NULL,NULL,0);
        read_buffer(output,captured,sizeof captured);
        for (i = 0; i < 7; ++i) {
            double x = vertices[i][0], y = vertices[i][1], rw = vertices[i][3],
                   clip_w = 1.0 / rw, fx = x - floor(x), fy = y - floor(y);
            if (target) {
                if (x >= .5 && x < .5625) x = 0;
                if (y >= .5 && y < .5625) y = 0;
            } else {
                if (rw != 1.0 || (fx >= .5 && fx < .5625)) x -= .53125;
                if (rw != 1.0 || (fy >= .5 && fy < .5625)) y -= .53125;
            }
            if (fabs(captured[i][0] - (2*x/w-1)*clip_w) >= 2e-6 ||
                fabs(captured[i][1] - (1-2*y/h)*clip_w) >= 2e-6)
                fprintf(stderr,"placement target=%u vertex=%u native=(%.9g,%.9g) expected=(%.9g,%.9g)\n",
                    target,i,captured[i][0],captured[i][1],(2*x/w-1)*clip_w,(1-2*y/h)*clip_w);
            CHECK(fabs(captured[i][0] - (2*x/w-1)*clip_w) < 2e-6);
            CHECK(fabs(captured[i][1] - (1-2*y/h)*clip_w) < 2e-6);
            CHECK(captured[i][2] == 0 && fabs(captured[i][3] - clip_w) < 1e-6);
        }
    }
    ID3D11Buffer_Release(output); ID3D11GeometryShader_Release(geometry);
    puts("PASS: production vertex output places display full-screen edges exactly at bounds, retains integer menu coordinates, translates perspective geometry, and preserves offscreen pixel-zero snap");
}

int main(int argc, char **argv)
{
    _putenv_s("RECOMP_RENDER_SCALE","1"); _putenv_s("RECOMP_MSAA","1");
    _putenv_s("RECOMP_ASPECT","4:3"); _putenv_s("RECOMP_PS_SPEC","0");
    _putenv_s("RECOMP_PS_PREWARM","0"); _putenv_s("RECOMP_SHADER_CACHE","0");
    CHECK(argc == 2 && nv2a_d3d_init());
    if (!strcmp(argv[1],"panorama")) test_panorama();
    else if (!strcmp(argv[1],"gates")) test_gates();
    else if (!strcmp(argv[1],"depth")) test_depth();
    else if (!strcmp(argv[1],"placement")) test_placement();
    else CHECK(0);
    return 0;
}

/* These renderer-only fixtures never request executor frame replay. */
void nv2a_pb_interp_enable(int on) { (void)on; abort(); }
long nv2a_pb_frames_done(void) { abort(); return 0; }
int nv2a_pb_replay(float t) { (void)t; abort(); return 0; }
