/**
 * SDL_GPU device, window and swapchain for the D3D8 layer.
 *
 * One window, one device. A frame is: xgpu_begin_frame() acquires a command
 * buffer and the swapchain image, draws go through that command buffer, and
 * xgpu_present() submits and presents it. D3D8 has no frame boundary until
 * Present, so this is the only place that decides when a command buffer ends.
 *
 * SDL_GPU does not allow copy passes inside render passes (or the reverse), but
 * D3D8 interleaves uploads and draws freely. Everything therefore goes through
 * xgpu_copy_pass() / xgpu_render_pass(), which end the other kind first.
 */
#ifndef XBOX_GPU_DEVICE_H
#define XBOX_GPU_DEVICE_H

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

/* The attachments a render pass draws into. Passes are reused while the
 * targets stay the same; any change ends the pass and starts a new one. */
typedef struct XGpuTargets {
    SDL_GPUTexture *color;               /* NULL = the swapchain image */
    SDL_GPUTexture *depth;               /* may be NULL */
} XGpuTargets;

/* Which attachments a new render pass clears instead of loading. */
typedef struct XGpuClear {
    int   color, depth;
    float rgba[4];
    float z;
} XGpuClear;

typedef struct XGpu {
    SDL_Window            *window;
    SDL_GPUDevice         *device;
    SDL_GPUCommandBuffer  *cmd;          /* current frame, NULL between frames */
    SDL_GPUTexture        *backbuffer;   /* swapchain image for this frame */
    SDL_GPUTextureFormat   swap_format;
    Uint32                 width, height;

    SDL_GPURenderPass     *render;
    SDL_GPUCopyPass       *copy;
    XGpuTargets            targets;      /* of the open render pass */

    /* Per-frame upload ring, persistently mapped between begin and present. */
    SDL_GPUTransferBuffer *ring;
    Uint8                 *ring_ptr;
    Uint32                 ring_size, ring_used;
} XGpu;

/* hidden != 0 creates the window without showing it (tests, headless runs).
 * Returns 0 on failure with the reason on stderr. */
int  xgpu_init(XGpu *g, const char *title, int width, int height, int hidden);
void xgpu_shutdown(XGpu *g);

/* Starts a frame. Returns 0 if the swapchain image is not available this
 * frame (minimised window): the caller should skip drawing but still present. */
int  xgpu_begin_frame(XGpu *g);

/* Ends whichever pass is open. */
void xgpu_end_passes(XGpu *g);

/* The open copy pass, ending any render pass first. */
SDL_GPUCopyPass *xgpu_copy_pass(XGpu *g);

/* The render pass for `t`. Reuses the open pass when the targets match and no
 * clear is asked for; otherwise ends it and starts one, loading existing
 * contents except where `clear` says otherwise. */
SDL_GPURenderPass *xgpu_render_pass(XGpu *g, XGpuTargets t, const XGpuClear *clear);

/* Space in the upload ring for `size` bytes (16-byte aligned). Write through
 * the returned pointer; (*tb, *offset) is where the GPU reads it from. A request
 * that does not fit gets its own transfer buffer, freed once the GPU is done.
 * Returns NULL on failure. */
void *xgpu_stage(XGpu *g, Uint32 size, SDL_GPUTransferBuffer **tb, Uint32 *offset);

/* Clears the swapchain image. */
void xgpu_clear_backbuffer(XGpu *g, float r, float g_, float b, float a);

/* Submits the frame and presents. Safe to call after a failed begin. */
void xgpu_present(XGpu *g);

#endif
