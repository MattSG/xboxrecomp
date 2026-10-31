#include "gpu_device.h"

#include <stdio.h>
#include <string.h>

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
    if (g->cmd) { SDL_CancelGPUCommandBuffer(g->cmd); g->cmd = NULL; }
    if (g->device) {
        SDL_WaitForGPUIdle(g->device);
        if (g->window) SDL_ReleaseWindowFromGPUDevice(g->device, g->window);
        SDL_DestroyGPUDevice(g->device);
    }
    if (g->window) SDL_DestroyWindow(g->window);
    memset(g, 0, sizeof(*g));
}

int xgpu_begin_frame(XGpu *g)
{
    Uint32 w = 0, h = 0;

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
    SDL_GPUColorTargetInfo c;
    SDL_GPURenderPass *rp;

    if (!g->cmd || !g->backbuffer)
        return;
    SDL_zero(c);
    c.texture = g->backbuffer;
    c.clear_color.r = r; c.clear_color.g = g_; c.clear_color.b = b; c.clear_color.a = a;
    c.load_op = SDL_GPU_LOADOP_CLEAR;
    c.store_op = SDL_GPU_STOREOP_STORE;
    rp = SDL_BeginGPURenderPass(g->cmd, &c, 1, NULL);
    if (rp)
        SDL_EndGPURenderPass(rp);
}

void xgpu_present(XGpu *g)
{
    if (g->cmd) {
        SDL_SubmitGPUCommandBuffer(g->cmd);   /* presents the acquired image */
        g->cmd = NULL;
    }
    g->backbuffer = NULL;
}
