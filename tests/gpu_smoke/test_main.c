/* Proves the chain the SDL_GPU backend depends on: run-time HLSL -> SDL_GPU
 * shaders with a vertex uniform buffer and a fragment texture+sampler, a
 * pipeline, an offscreen render and a read-back. No window, no game assets. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include "gpu_shader.h"

static const char *k_vs =
    "cbuffer CB : register(b0, space1) { float4 tint; };\n"
    "struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 col : COLOR0; };\n"
    "VOut main(uint id : SV_VertexID) {\n"
    "    float2 p[3] = { float2(-1,-1), float2(3,-1), float2(-1,3) };\n"
    "    VOut o; o.pos = float4(p[id], 0, 1); o.uv = float2(0.5, 0.5); o.col = tint; return o; }\n";

static const char *k_ps =
    "Texture2D tex0 : register(t0, space2);\n"
    "SamplerState samp0 : register(s0, space2);\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0, float4 col : COLOR0) : SV_Target {\n"
    "    return tex0.Sample(samp0, uv) * col; }\n";

#define FAIL(msg) do { fprintf(stderr, "FAIL %s: %s\n", msg, SDL_GetError()); return 1; } while (0)

int main(void)
{
    SDL_GPUDevice *dev;
    SDL_GPUShader *vs, *ps;
    SDL_GPUGraphicsPipeline *pipe;
    SDL_GPUTexture *rt, *tex;
    SDL_GPUSampler *samp;
    SDL_GPUTransferBuffer *up, *down;
    SDL_GPUCommandBuffer *cb;
    SDL_GPURenderPass *rp;
    SDL_GPUCopyPass *cp;
    SDL_GPUFence *fence;
    const float tint[4] = { 1.0f, 0.5f, 0.25f, 1.0f };
    Uint8 *p;

    if (!SDL_Init(SDL_INIT_VIDEO)) FAIL("SDL_Init");
    dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL |
                              SDL_GPU_SHADERFORMAT_MSL, true, NULL);
    if (!dev) FAIL("SDL_CreateGPUDevice");
    printf("driver: %s\n", SDL_GetGPUDeviceDriver(dev));

    vs = xgpu_compile_hlsl(dev, k_vs, "main", 0, "vs");
    ps = xgpu_compile_hlsl(dev, k_ps, "main", 1, "ps");
    if (!vs || !ps) return 1;

    {   SDL_GPUColorTargetDescription ct;
        SDL_GPUGraphicsPipelineCreateInfo pi;
        SDL_zero(ct); SDL_zero(pi);
        ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        pi.vertex_shader = vs; pi.fragment_shader = ps;
        pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pi.target_info.color_target_descriptions = &ct;
        pi.target_info.num_color_targets = 1;
        pipe = SDL_CreateGPUGraphicsPipeline(dev, &pi);
        if (!pipe) FAIL("pipeline"); }

    {   SDL_GPUTextureCreateInfo ti;
        SDL_zero(ti);
        ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ti.width = ti.height = 4; ti.layer_count_or_depth = 1; ti.num_levels = 1;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        rt = SDL_CreateGPUTexture(dev, &ti);
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        tex = SDL_CreateGPUTexture(dev, &ti);
        if (!rt || !tex) FAIL("textures"); }

    {   SDL_GPUSamplerCreateInfo si;
        SDL_zero(si);
        samp = SDL_CreateGPUSampler(dev, &si);
        if (!samp) FAIL("sampler"); }

    {   SDL_GPUTransferBufferCreateInfo ti;
        SDL_zero(ti);
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; ti.size = 4 * 4 * 4;
        up = SDL_CreateGPUTransferBuffer(dev, &ti);
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        down = SDL_CreateGPUTransferBuffer(dev, &ti);
        if (!up || !down) FAIL("transfer buffers"); }

    /* Texture of constant (200, 100, 50, 255). */
    p = SDL_MapGPUTransferBuffer(dev, up, false);
    for (int i = 0; i < 16; i++) { p[i*4] = 200; p[i*4+1] = 100; p[i*4+2] = 50; p[i*4+3] = 255; }
    SDL_UnmapGPUTransferBuffer(dev, up);

    cb = SDL_AcquireGPUCommandBuffer(dev);
    cp = SDL_BeginGPUCopyPass(cb);
    {   SDL_GPUTextureTransferInfo src; SDL_GPUTextureRegion dst;
        SDL_zero(src); SDL_zero(dst);
        src.transfer_buffer = up; src.pixels_per_row = 4; src.rows_per_layer = 4;
        dst.texture = tex; dst.w = dst.h = 4; dst.d = 1;
        SDL_UploadToGPUTexture(cp, &src, &dst, false); }
    SDL_EndGPUCopyPass(cp);

    {   SDL_GPUColorTargetInfo c;
        SDL_zero(c);
        c.texture = rt; c.load_op = SDL_GPU_LOADOP_CLEAR; c.store_op = SDL_GPU_STOREOP_STORE;
        rp = SDL_BeginGPURenderPass(cb, &c, 1, NULL); }
    SDL_BindGPUGraphicsPipeline(rp, pipe);
    SDL_PushGPUVertexUniformData(cb, 0, tint, sizeof tint);
    {   SDL_GPUTextureSamplerBinding b; b.texture = tex; b.sampler = samp;
        SDL_BindGPUFragmentSamplers(rp, 0, &b, 1); }
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp);

    cp = SDL_BeginGPUCopyPass(cb);
    {   SDL_GPUTextureRegion src; SDL_GPUTextureTransferInfo dst;
        SDL_zero(src); SDL_zero(dst);
        src.texture = rt; src.w = src.h = 4; src.d = 1;
        dst.transfer_buffer = down; dst.pixels_per_row = 4; dst.rows_per_layer = 4;
        SDL_DownloadFromGPUTexture(cp, &src, &dst); }
    SDL_EndGPUCopyPass(cp);
    fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    SDL_WaitForGPUFences(dev, true, &fence, 1);
    SDL_ReleaseGPUFence(dev, fence);

    p = SDL_MapGPUTransferBuffer(dev, down, false);
    printf("pixel: %u %u %u %u\n", p[0], p[1], p[2], p[3]);
    /* (200,100,50) * (1, .5, .25) = (200, 50, 12.5); allow rounding. */
    if (p[0] < 198 || p[0] > 202 || p[1] < 48 || p[1] > 52 || p[2] < 11 || p[2] > 14 || p[3] != 255) {
        puts("FAIL pixel mismatch");
        return 1;
    }
    SDL_UnmapGPUTransferBuffer(dev, down);
    puts("PASS: HLSL -> SDL_GPU with uniform buffer, texture sampling and read-back");
    return 0;
}
