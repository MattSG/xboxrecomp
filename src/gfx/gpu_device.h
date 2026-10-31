/**
 * SDL_GPU device, window and swapchain for the D3D8 layer.
 *
 * One window, one device. A frame is: xgpu_begin_frame() acquires a command
 * buffer and the swapchain image, draws go through that command buffer, and
 * xgpu_present() submits and presents it. D3D8 has no frame boundary until
 * Present, so this is the only place that decides when a command buffer ends.
 */
#ifndef XBOX_GPU_DEVICE_H
#define XBOX_GPU_DEVICE_H

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

typedef struct XGpu {
    SDL_Window            *window;
    SDL_GPUDevice         *device;
    SDL_GPUCommandBuffer  *cmd;          /* current frame, NULL between frames */
    SDL_GPUTexture        *backbuffer;   /* swapchain image for this frame */
    SDL_GPUTextureFormat   swap_format;
    Uint32                 width, height;
} XGpu;

/* hidden != 0 creates the window without showing it (tests, headless runs).
 * Returns 0 on failure with the reason on stderr. */
int  xgpu_init(XGpu *g, const char *title, int width, int height, int hidden);
void xgpu_shutdown(XGpu *g);

/* Starts a frame. Returns 0 if the swapchain image is not available this
 * frame (minimised window): the caller should skip drawing but still present. */
int  xgpu_begin_frame(XGpu *g);

/* Clears the swapchain image in its own render pass. */
void xgpu_clear_backbuffer(XGpu *g, float r, float g_, float b, float a);

/* Submits the frame and presents. Safe to call after a failed begin. */
void xgpu_present(XGpu *g);

#endif
