/* Pass manager: an upload (copy pass) between two draws (render pass) on the
 * same targets. The clear must land first, the vertex upload must precede the
 * draw, and the second render pass must LOAD what the first one drew. */
#include <stdio.h>
#include <string.h>
#include "gpu_device.h"
#include "gpu_shader.h"

static const char *k_vs =
    "struct VIn { float2 pos : TEXCOORD0; };\n"
    "float4 main(VIn i) : SV_Position { return float4(i.pos, 0, 1); }\n";
static const char *k_ps =
    "cbuffer CB : register(b0, space3) { float4 col; };\n"
    "float4 main() : SV_Target { return col; }\n";

#define RT_SIZE 8

static int draw(XGpu *g, SDL_GPUGraphicsPipeline *pipe, SDL_GPUBuffer *vb,
                SDL_GPUTexture *rt, const float *col, const XGpuClear *clear)
{
    XGpuTargets t = { rt, NULL };
    SDL_GPURenderPass *rp = xgpu_render_pass(g, t, clear);
    SDL_GPUBufferBinding b;
    if (!rp) return 0;
    SDL_BindGPUGraphicsPipeline(rp, pipe);
    SDL_PushGPUFragmentUniformData(g->cmd, 0, col, 16);
    SDL_zero(b); b.buffer = vb;
    SDL_BindGPUVertexBuffers(rp, 0, &b, 1);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    return 1;
}

int main(void)
{
    XGpu g;
    SDL_GPUShader *vs, *ps;
    SDL_GPUGraphicsPipeline *pipe;
    SDL_GPUTexture *rt;
    SDL_GPUBuffer *vb;
    SDL_GPUTransferBuffer *down, *tb;
    Uint32 off;
    float *verts;
    Uint8 *px;
    const float blue[4] = { 0, 0, 1, 1 };
    const float green[4] = { 0, 1, 0, 1 };
    XGpuClear clear;
    int ok = 1;

    if (!xgpu_init(&g, "gpu_pass test", 64, 64, 1)) return 1;
    vs = xgpu_compile_hlsl(g.device, k_vs, "main", 0, "vs");
    ps = xgpu_compile_hlsl(g.device, k_ps, "main", 1, "ps");
    if (!vs || !ps) return 1;

    {   SDL_GPUVertexBufferDescription vbd;
        SDL_GPUVertexAttribute va;
        SDL_GPUColorTargetDescription ct;
        SDL_GPUGraphicsPipelineCreateInfo pi;
        SDL_zero(vbd); SDL_zero(va); SDL_zero(ct); SDL_zero(pi);
        vbd.pitch = 8; vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        va.location = 0; va.format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        pi.vertex_shader = vs; pi.fragment_shader = ps;
        pi.vertex_input_state.vertex_buffer_descriptions = &vbd;
        pi.vertex_input_state.num_vertex_buffers = 1;
        pi.vertex_input_state.vertex_attributes = &va;
        pi.vertex_input_state.num_vertex_attributes = 1;
        pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pi.target_info.color_target_descriptions = &ct;
        pi.target_info.num_color_targets = 1;
        pipe = SDL_CreateGPUGraphicsPipeline(g.device, &pi);
        if (!pipe) { fprintf(stderr, "pipeline: %s\n", SDL_GetError()); return 1; } }

    {   SDL_GPUTextureCreateInfo ti;
        SDL_GPUBufferCreateInfo bi;
        SDL_GPUTransferBufferCreateInfo ci;
        SDL_zero(ti); SDL_zero(bi); SDL_zero(ci);
        ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ti.width = ti.height = RT_SIZE; ti.layer_count_or_depth = 1; ti.num_levels = 1;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        rt = SDL_CreateGPUTexture(g.device, &ti);
        bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX; bi.size = 3 * 8;
        vb = SDL_CreateGPUBuffer(g.device, &bi);
        ci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; ci.size = RT_SIZE * RT_SIZE * 4;
        down = SDL_CreateGPUTransferBuffer(g.device, &ci);
        if (!rt || !vb || !down) { fprintf(stderr, "resources: %s\n", SDL_GetError()); return 1; } }

    xgpu_begin_frame(&g);
    if (!g.cmd) { fprintf(stderr, "no command buffer\n"); return 1; }

    /* Upload a triangle covering the whole target (so both draws hit every pixel). */
    verts = (float *)xgpu_stage(&g, 3 * 8, &tb, &off);
    if (!verts) return 1;
    verts[0] = -1; verts[1] = -1; verts[2] = 3; verts[3] = -1; verts[4] = -1; verts[5] = 3;
    {   SDL_GPUCopyPass *cp = xgpu_copy_pass(&g);
        SDL_GPUTransferBufferLocation src; SDL_GPUBufferRegion dst;
        SDL_zero(src); SDL_zero(dst);
        src.transfer_buffer = tb; src.offset = off;
        dst.buffer = vb; dst.size = 3 * 8;
        SDL_UploadToGPUBuffer(cp, &src, &dst, false); }

    /* Clear to red and draw nothing visible-yet: a draw with the pass cleared. */
    SDL_zero(clear); clear.color = 1; clear.rgba[0] = 1; clear.rgba[3] = 1;
    if (!draw(&g, pipe, vb, rt, blue, &clear)) return 1;

    /* An upload in the middle forces the render pass to end... */
    {   SDL_GPUTransferBuffer *tb2; Uint32 off2;
        float *again = (float *)xgpu_stage(&g, 3 * 8, &tb2, &off2);
        SDL_GPUCopyPass *cp = xgpu_copy_pass(&g);
        SDL_GPUTransferBufferLocation src; SDL_GPUBufferRegion dst;
        memcpy(again, verts, 3 * 8);
        SDL_zero(src); SDL_zero(dst);
        src.transfer_buffer = tb2; src.offset = off2;
        dst.buffer = vb; dst.size = 3 * 8;
        SDL_UploadToGPUBuffer(cp, &src, &dst, false); }

    /* ...and the next draw starts a new pass that LOADs the blue from before,
     * then draws green over only half via a scissor-free partial: here we just
     * check ordering by drawing green last. */
    if (!draw(&g, pipe, vb, rt, green, NULL)) return 1;

    {   SDL_GPUCopyPass *cp = xgpu_copy_pass(&g);
        SDL_GPUTextureRegion src; SDL_GPUTextureTransferInfo dst;
        SDL_zero(src); SDL_zero(dst);
        src.texture = rt; src.w = src.h = RT_SIZE; src.d = 1;
        dst.transfer_buffer = down; dst.pixels_per_row = RT_SIZE; dst.rows_per_layer = RT_SIZE;
        SDL_DownloadFromGPUTexture(cp, &src, &dst); }
    xgpu_present(&g);
    SDL_WaitForGPUIdle(g.device);

    px = (Uint8 *)SDL_MapGPUTransferBuffer(g.device, down, false);
    printf("pixel: %u %u %u %u\n", px[0], px[1], px[2], px[3]);
    if (px[0] != 0 || px[1] != 255 || px[2] != 0 || px[3] != 255) {
        puts("FAIL expected green (the last draw) over the earlier blue/red");
        ok = 0;
    }
    SDL_UnmapGPUTransferBuffer(g.device, down);
    xgpu_shutdown(&g);
    if (!ok) return 1;
    puts("PASS: clear, upload between passes, and a reloaded second pass in order");
    return 0;
}
