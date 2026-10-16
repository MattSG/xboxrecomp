/* Dedicated host fog pass. Included after the renderer's surface helpers. */
static const char s_native_fog_hlsl[] =
"cbuffer Fog : register(b0) { float4 band; float4 colour; };\n"
"#if FOG_MSAA\n"
"Texture2DMS<float> Depth : register(t0);\n"
"#else\n"
"Texture2D<float> Depth : register(t0);\n"
"#endif\n"
"float4 fog_vs(uint id : SV_VertexID) : SV_Position {\n"
"  float2 p = float2((id << 1) & 2, id & 2);\n"
"  return float4(p * float2(2,-2) + float2(-1,1), 0, 1);\n"
"}\n"
"float4 fog_ps(float4 p : SV_Position, uint sample : SV_SampleIndex) : SV_Target {\n"
"#if FOG_MSAA\n"
"  float z = Depth.Load(int2(p.xy), sample);\n"
"#else\n"
"  float z = Depth.Load(int3(int2(p.xy), 0));\n"
"#endif\n"
"  if (colour.a > 0) return float4(0,0,0,z >= 0.99999 ? 1 : 0);\n"
"  if (z >= 0.99999) return 0;\n"
"  float x = saturate((z * band.z - band.x) * band.y);\n"
"  return float4(colour.rgb, 1.0 - exp2(-6.643856 * x * x));\n"
"}\n";

static ID3D11VertexShader *s_native_fog_vs;
static ID3D11PixelShader *s_native_fog_ps[2];
static ID3D11Buffer *s_native_fog_cb;
static ID3D11BlendState *s_native_fog_blend, *s_native_fog_mask;
static ID3D11DepthStencilState *s_native_fog_depth;
static ID3D11RasterizerState *s_native_fog_raster;

static ID3DBlob *native_fog_compile(const char *entry, const char *target, int msaa)
{
    D3D_SHADER_MACRO macros[] = {{"FOG_MSAA", msaa ? "1" : "0"}, {NULL,NULL}};
    ID3DBlob *code = NULL, *error = NULL;
    HRESULT hr = nv_shader_cache_compile(s_native_fog_hlsl, sizeof s_native_fog_hlsl-1,
        "native_distance_fog", macros, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, &code, &error, NULL);
    if (FAILED(hr)) fprintf(stderr,"[FOG] %s: %s\n",entry,
                           error ? (const char *)ID3D10Blob_GetBufferPointer(error) : "compile failed");
    if (error) ID3D10Blob_Release(error);
    return code;
}

static int native_fog_init(void)
{
    ID3DBlob *code;
    D3D11_BUFFER_DESC cb = {32,D3D11_USAGE_DYNAMIC,D3D11_BIND_CONSTANT_BUFFER,D3D11_CPU_ACCESS_WRITE,0,0};
    D3D11_BLEND_DESC blend = {0};
    D3D11_DEPTH_STENCIL_DESC depth = {0};
    D3D11_RASTERIZER_DESC raster = {0};
    if (s_native_fog_vs) return 1;
    code = native_fog_compile("fog_vs","vs_5_0",0);
    if (!code) goto fail;
    HRESULT hr = ID3D11Device_CreateVertexShader(s_dev,ID3D10Blob_GetBufferPointer(code),
        ID3D10Blob_GetBufferSize(code),NULL,&s_native_fog_vs);
    ID3D10Blob_Release(code);
    if (FAILED(hr)) goto fail;
    for (int i = 0; i < 2; ++i) {
        code = native_fog_compile("fog_ps","ps_5_0",i);
        if (!code) goto fail;
        hr = ID3D11Device_CreatePixelShader(s_dev,ID3D10Blob_GetBufferPointer(code),
            ID3D10Blob_GetBufferSize(code),NULL,&s_native_fog_ps[i]);
        ID3D10Blob_Release(code);
        if (FAILED(hr)) goto fail;
    }
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.ScissorEnable = TRUE;
    raster.DepthClipEnable = TRUE;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev,&cb,NULL,&s_native_fog_cb)) ||
        FAILED(ID3D11Device_CreateBlendState(s_dev,&blend,&s_native_fog_blend)) ||
        FAILED(ID3D11Device_CreateDepthStencilState(s_dev,&depth,&s_native_fog_depth)) ||
        FAILED(ID3D11Device_CreateRasterizerState(s_dev,&raster,&s_native_fog_raster))) goto fail;
    blend.RenderTarget[0].BlendEnable = FALSE;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
    if (FAILED(ID3D11Device_CreateBlendState(s_dev,&blend,&s_native_fog_mask))) goto fail;
    return 1;
fail:
    if (s_native_fog_vs) { ID3D11VertexShader_Release(s_native_fog_vs); s_native_fog_vs = NULL; }
    for (int i = 0; i < 2; ++i)
        if (s_native_fog_ps[i]) { ID3D11PixelShader_Release(s_native_fog_ps[i]); s_native_fog_ps[i] = NULL; }
    if (s_native_fog_cb) { ID3D11Buffer_Release(s_native_fog_cb); s_native_fog_cb = NULL; }
    if (s_native_fog_blend) { ID3D11BlendState_Release(s_native_fog_blend); s_native_fog_blend = NULL; }
    if (s_native_fog_mask) { ID3D11BlendState_Release(s_native_fog_mask); s_native_fog_mask = NULL; }
    if (s_native_fog_depth) { ID3D11DepthStencilState_Release(s_native_fog_depth); s_native_fog_depth = NULL; }
    if (s_native_fog_raster) { ID3D11RasterizerState_Release(s_native_fog_raster); s_native_fog_raster = NULL; }
    return 0;
}

void nv2a_d3d_distance_fog(const NvD3DState *st, float depth_to_world, float far_clip, float start, float end, uint32_t argb, uint32_t flags)
{
    Surface *s = NULL; Depth *d = NULL; uint32_t gw, gh;
    if (!isfinite(depth_to_world) || !isfinite(far_clip) || !isfinite(start) || !isfinite(end) ||
        depth_to_world <= 0 || far_clip <= 0 || start < 0 || end <= start || end > far_clip ||
        !st || !bind_targets(st,1,&s,&d,&gw,&gh) ||
        !s || !d || !d->zsrv || s->iw != d->iw || s->ih != d->ih ||
        (!!s->ms != (d->samples > 1)) || !native_fog_init()) return;
    float params[8] = {start,1.f/(end-start),depth_to_world*(d->zf == 1 ? 65536.f : 16777216.f),0,
                      ((argb>>16)&255)/255.f,((argb>>8)&255)/255.f,(argb&255)/255.f,0};
    if (!isfinite(params[1]) || !isfinite(params[2]) ||
        !upload_cb(s_native_fog_cb,params,sizeof params)) return;
    D3D11_VIEWPORT vp = {0,0,(float)s->iw,(float)s->ih,0,1};
    D3D11_RECT sc = {0,0,(LONG)s->iw,(LONG)s->ih};
    unbind_textures();
    unbind_targets();
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx,1,&s->rtv,NULL);
    ID3D11DeviceContext_RSSetViewports(s_ctx,1,&vp);
    ID3D11DeviceContext_RSSetScissorRects(s_ctx,1,&sc);
    ID3D11DeviceContext_RSSetState(s_ctx,s_native_fog_raster);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx,s_native_fog_depth,0);
    ID3D11DeviceContext_OMSetBlendState(s_ctx,s_native_fog_blend,NULL,0xFFFFFFFFu);
    ID3D11DeviceContext_IASetInputLayout(s_ctx,NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    bind_vertex_shader(s_native_fog_vs);
    bind_pixel_shader(s_native_fog_ps[d->samples > 1]);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx,0,1,&s_native_fog_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx,0,1,&d->zsrv);
    ID3D11DeviceContext_Draw(s_ctx,3,0);
    /* Optional mask for subsequent destination-alpha composition. */
    params[7] = 1;
    if ((flags & NV2A_HOST_FOG_SKY_MASK) && upload_cb(s_native_fog_cb,params,sizeof params)) {
        ID3D11DeviceContext_OMSetBlendState(s_ctx,s_native_fog_mask,NULL,0xFFFFFFFFu);
        ID3D11DeviceContext_Draw(s_ctx,3,0);
    }
    unbind_textures();
    unbind_targets();
    s->gpu_dirty = 1; s->ms_dirty = !!s->ms; ++s->gen;
    static unsigned logged;
    if (!logged++) fprintf(stderr,"[FOG] native pass %ux%u depth-format=%u samples=%u\n",s->iw,s->ih,d->zf,d->samples);
}
