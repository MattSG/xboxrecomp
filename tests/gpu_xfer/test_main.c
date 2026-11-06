/* Uploads and readbacks, with and without a frame in progress, and a
 * mid-frame readback of the backbuffer followed by more drawing. */
#include <stdio.h>
#include <string.h>
#include "gpu_device.h"

#define FAIL(m) do { fprintf(stderr, "FAIL %s\n", m); return 1; } while (0)

int main(void)
{
    XGpu g;
    SDL_GPUTexture *tex;
    SDL_GPUTextureCreateInfo ti;
    Uint8 src[4 * 4 * 4], out[4 * 4 * 4], px[4];
    int i;

    if (!xgpu_init(&g, "gpu_xfer test", 16, 16, 1)) return 1;

    SDL_zero(ti);
    ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.width = ti.height = 4; ti.layer_count_or_depth = 1; ti.num_levels = 1;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tex = SDL_CreateGPUTexture(g.device, &ti);
    if (!tex) FAIL("texture");
    for (i = 0; i < 64; i++) src[i] = (Uint8)(i * 3);

    /* Outside a frame: the upload must submit itself. */
    if (!xgpu_upload_texture(&g, tex, 0, 0, 0, 0, 0, 4, 4, 1, src, 16, 4)) FAIL("upload");
    memset(out, 0, sizeof out);
    if (!xgpu_download_texture(&g, tex, 0, 0, 0, 0, 4, 4, out, 16)) FAIL("download");
    if (memcmp(src, out, sizeof src)) FAIL("round trip differs");

    /* A sub-rectangle round trip. */
    {   Uint8 patch[2 * 2 * 4], back[2 * 2 * 4];
        for (i = 0; i < 16; i++) patch[i] = (Uint8)(200 + i);
        if (!xgpu_upload_texture(&g, tex, 0, 0, 1, 1, 0, 2, 2, 1, patch, 8, 2)) FAIL("sub upload");
        if (!xgpu_download_texture(&g, tex, 0, 0, 1, 1, 2, 2, back, 8)) FAIL("sub download");
        if (memcmp(patch, back, sizeof patch)) FAIL("sub rect differs"); }

    /* In a frame: clear red, read it back mid-frame, clear green, present. */
    xgpu_begin_frame(&g);
    xgpu_clear_backbuffer(&g, 1, 0, 0, 1);
    if (!xgpu_download_texture(&g, g.backbuffer, 0, 0, 0, 0, 1, 1, px, 4)) FAIL("backbuffer read");
    printf("backbuffer BGRA: %u %u %u %u\n", px[0], px[1], px[2], px[3]);
    if (px[2] != 255 || px[1] != 0 || px[0] != 0) FAIL("backbuffer not red");
    xgpu_clear_backbuffer(&g, 0, 1, 0, 1);       /* the frame goes on after the flush */
    if (!xgpu_download_texture(&g, g.backbuffer, 0, 0, 0, 0, 1, 1, px, 4)) FAIL("second read");
    if (px[1] != 255 || px[2] != 0) FAIL("backbuffer not green");
    xgpu_present(&g);

    xgpu_shutdown(&g);
    puts("PASS: uploads/readbacks inside and outside a frame, mid-frame flush");
    return 0;
}
