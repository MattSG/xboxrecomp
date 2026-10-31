#include "gpu_device.h"

#include <stdio.h>
#include <string.h>

#define RING_BYTES (32u * 1024u * 1024u)

int xgpu_init(XGpu *g, const char *title, int width, int height, int hidden)
{
    SDL_WindowFlags flags = hidden ? SDL_WINDOW_HIDDEN : 0;

    memset(g, 0, sizeof(*g));
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        fprintf(stderr, "[gpu] SDL video init failed: %s\n", SDL_GetError());
        return 0;
    }
    g->window = SDL_CreateWindow(title, width, height, flags);
    if (!g->window) {
        fprintf(stderr, "[gpu] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 0;
    }
    g->device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL |
                                    SDL_GPU_SHADERFORMAT_MSL, false, NULL);
    if (!g->device) {
        fprintf(stderr, "[gpu] SDL_CreateGPUDevice failed: %s\n", SDL_GetError());
        xgpu_shutdown(g);
        return 0;
    }
    if (!SDL_ClaimWindowForGPUDevice(g->device, g->window)) {
        fprintf(stderr, "[gpu] ClaimWindow failed: %s\n", SDL_GetError());
        xgpu_shutdown(g);
        return 0;
    }
    g->swap_format = SDL_GetGPUSwapchainTextureFormat(g->device, g->window);
    g->width = (Uint32)width;
    g->height = (Uint32)height;
    fprintf(stderr, "[gpu] %s device, %dx%d\n", SDL_GetGPUDeviceDriver(g->device), width, height);
    return 1;
}

void xgpu_shutdown(XGpu *g)
{
    if (g->cmd) {
        xgpu_end_passes(g);
        if (g->ring_ptr) SDL_UnmapGPUTransferBuffer(g->device, g->ring);
        SDL_CancelGPUCommandBuffer(g->cmd);
        g->cmd = NULL;
    }
    if (g->device) {
        SDL_WaitForGPUIdle(g->device);
        if (g->ring) SDL_ReleaseGPUTransferBuffer(g->device, g->ring);
        if (g->window) SDL_ReleaseWindowFromGPUDevice(g->device, g->window);
        SDL_DestroyGPUDevice(g->device);
    }
    if (g->window) SDL_DestroyWindow(g->window);
    memset(g, 0, sizeof(*g));
}

void xgpu_end_passes(XGpu *g)
{
    if (g->render) { SDL_EndGPURenderPass(g->render); g->render = NULL; }
    if (g->copy)   { SDL_EndGPUCopyPass(g->copy);     g->copy = NULL; }
}

SDL_GPUCopyPass *xgpu_copy_pass(XGpu *g)
{
    if (g->render) { SDL_EndGPURenderPass(g->render); g->render = NULL; }
    if (!g->copy && g->cmd)
        g->copy = SDL_BeginGPUCopyPass(g->cmd);
    return g->copy;
}

SDL_GPURenderPass *xgpu_render_pass(XGpu *g, XGpuTargets t, const XGpuClear *clear)
{
    SDL_GPUColorTargetInfo c;
    SDL_GPUDepthStencilTargetInfo d;
    SDL_GPUTexture *color = t.color ? t.color : g->backbuffer;

    if (!g->cmd)
        return NULL;
    if (g->render && !clear && g->targets.color == t.color && g->targets.depth == t.depth)
        return g->render;
    xgpu_end_passes(g);
    if (!color)
        return NULL;

    SDL_zero(c);
    c.texture = color;
    c.store_op = SDL_GPU_STOREOP_STORE;
    if (clear && clear->color) {
        c.load_op = SDL_GPU_LOADOP_CLEAR;
        c.clear_color.r = clear->rgba[0]; c.clear_color.g = clear->rgba[1];
        c.clear_color.b = clear->rgba[2]; c.clear_color.a = clear->rgba[3];
    } else {
        c.load_op = SDL_GPU_LOADOP_LOAD;
    }
    SDL_zero(d);
    if (t.depth) {
        d.texture = t.depth;
        d.store_op = SDL_GPU_STOREOP_STORE;
        d.stencil_store_op = SDL_GPU_STOREOP_STORE;
        if (clear && clear->depth) {
            d.load_op = SDL_GPU_LOADOP_CLEAR;
            d.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
            d.clear_depth = clear->z;
        } else {
            d.load_op = SDL_GPU_LOADOP_LOAD;
            d.stencil_load_op = SDL_GPU_LOADOP_LOAD;
        }
    }
    g->render = SDL_BeginGPURenderPass(g->cmd, &c, 1, t.depth ? &d : NULL);
    g->targets = t;
    return g->render;
}

void *xgpu_stage(XGpu *g, Uint32 size, SDL_GPUTransferBuffer **tb, Uint32 *offset)
{
    Uint32 at = (g->ring_used + 15u) & ~15u;
    SDL_GPUTransferBufferCreateInfo ci;
    SDL_GPUTransferBuffer *own;
    void *p;

    if (g->ring_ptr && at + size <= g->ring_size) {
        g->ring_used = at + size;
        *tb = g->ring;
        *offset = at;
        return g->ring_ptr + at;
    }
    /* Does not fit this frame's ring: give the upload its own buffer. Releasing
     * it right away is safe, SDL destroys it once the GPU is done with it. */
    SDL_zero(ci);
    ci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    ci.size = size;
    own = SDL_CreateGPUTransferBuffer(g->device, &ci);
    if (!own)
        return NULL;
    p = SDL_MapGPUTransferBuffer(g->device, own, false);
    SDL_ReleaseGPUTransferBuffer(g->device, own);
    *tb = own;
    *offset = 0;
    return p;
}

int xgpu_begin_frame(XGpu *g)
{
    Uint32 w = 0, h = 0;

    if (!g->ring) {
        SDL_GPUTransferBufferCreateInfo ci;
        SDL_zero(ci);
        ci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ci.size = RING_BYTES;
        g->ring = SDL_CreateGPUTransferBuffer(g->device, &ci);
        g->ring_size = RING_BYTES;
    }
    /* cycle: if the previous frame is still reading the ring, SDL hands back a
     * fresh backing buffer instead of stalling. */
    g->ring_ptr = g->ring ? (Uint8 *)SDL_MapGPUTransferBuffer(g->device, g->ring, true) : NULL;
    g->ring_used = 0;

    g->cmd = SDL_AcquireGPUCommandBuffer(g->device);
    g->backbuffer = NULL;
    if (!g->cmd)
        return 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(g->cmd, g->window, &g->backbuffer, &w, &h))
        return 0;
    if (!g->backbuffer)
        return 0;                       /* minimised: no image this frame */
    g->width = w;
    g->height = h;
    return 1;
}

void xgpu_clear_backbuffer(XGpu *g, float r, float g_, float b, float a)
{
    XGpuClear cl;
    XGpuTargets t;

    if (!g->cmd || !g->backbuffer)
        return;
    SDL_zero(cl); SDL_zero(t);
    cl.color = 1;
    cl.rgba[0] = r; cl.rgba[1] = g_; cl.rgba[2] = b; cl.rgba[3] = a;
    xgpu_render_pass(g, t, &cl);
    xgpu_end_passes(g);
}

void xgpu_present(XGpu *g)
{
    xgpu_end_passes(g);
    if (g->ring_ptr) {
        SDL_UnmapGPUTransferBuffer(g->device, g->ring);
        g->ring_ptr = NULL;
    }
    if (g->cmd) {
        SDL_SubmitGPUCommandBuffer(g->cmd);   /* presents the acquired image */
        g->cmd = NULL;
    }
    g->backbuffer = NULL;
}
