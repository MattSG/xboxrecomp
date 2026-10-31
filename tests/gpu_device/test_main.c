/* Window + device + swapchain: three frames of clear and present. */
#include <stdio.h>
#include "gpu_device.h"

int main(void)
{
    XGpu g;
    int i, drawn = 0;

    if (!xgpu_init(&g, "gpu_device test", 320, 240, 1))
        return 1;
    for (i = 0; i < 3; i++) {
        if (xgpu_begin_frame(&g)) {
            xgpu_clear_backbuffer(&g, 0.1f * i, 0.2f, 0.3f, 1.0f);
            drawn++;
        }
        xgpu_present(&g);
    }
    xgpu_shutdown(&g);
    if (!drawn) { puts("FAIL no frame got a swapchain image"); return 1; }
    printf("PASS: %d/3 frames cleared and presented\n", drawn);
    return 0;
}
