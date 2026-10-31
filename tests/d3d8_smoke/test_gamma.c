/* Gamma ramp: get/set round trip, and the present-time pass. The swapchain
 * image cannot be read back, so this checks the pass runs (compiles its
 * shaders, builds its pipeline, submits) without failing across frames. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3d8_internal.h"

#define FAIL(msg) do { fprintf(stderr, "FAIL: %s\n", msg); return 1; } while (0)

int main(void)
{
    IDirect3D8 *d3d;
    IDirect3DDevice8 *dev;
    D3DPRESENT_PARAMETERS pp;
    D3DGAMMARAMP ramp, got;
    UINT i;

    SDL_setenv_unsafe("RECOMP_HIDE_WINDOW", "1", 1);
    d3d = xbox_Direct3DCreate8(220);
    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth = 64; pp.BackBufferHeight = 64; pp.Windowed = TRUE;
    if (!d3d || FAILED(d3d->lpVtbl->CreateDevice(d3d, 0, 0, NULL, 0, &pp, &dev))) FAIL("CreateDevice");

    /* Default is the identity ramp. */
    dev->lpVtbl->GetGammaRamp(dev, &got);
    for (i = 0; i < 256; i++)
        if (got.red[i] != i * 257 || got.green[i] != i * 257 || got.blue[i] != i * 257) FAIL("default ramp is not identity");

    /* An inverting ramp round-trips. */
    for (i = 0; i < 256; i++)
        ramp.red[i] = ramp.green[i] = ramp.blue[i] = (WORD)(65535 - i * 257);
    dev->lpVtbl->SetGammaRamp(dev, 0, &ramp);
    dev->lpVtbl->GetGammaRamp(dev, &got);
    if (memcmp(&ramp, &got, sizeof ramp)) FAIL("ramp did not round trip");

    /* Present through the gamma pass for a few frames. */
    for (i = 0; i < 3; i++) {
        dev->lpVtbl->BeginScene(dev);
        dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF808080, 1.0f, 0);
        dev->lpVtbl->EndScene(dev);
        if (FAILED(dev->lpVtbl->Present(dev, NULL, NULL, NULL, NULL))) FAIL("Present with gamma");
    }

    /* And back to identity (the plain blit path). */
    for (i = 0; i < 256; i++) ramp.red[i] = ramp.green[i] = ramp.blue[i] = (WORD)(i * 257);
    dev->lpVtbl->SetGammaRamp(dev, 0, &ramp);
    if (FAILED(dev->lpVtbl->Present(dev, NULL, NULL, NULL, NULL))) FAIL("Present after identity");

    puts("PASS: gamma ramp round trip and present-time pass");
    return 0;
}
