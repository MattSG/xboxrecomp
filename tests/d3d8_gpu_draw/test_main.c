/* End-to-end through the D3D8 layer on SDL_GPU: create the device, render a
 * pre-transformed, vertex-coloured triangle into an offscreen render target
 * with the fixed-function shaders, and read the pixel back. Covers shader
 * generation + compilation, the pipeline cache, targets, Clear and LockRect. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3d8_internal.h"

typedef struct { float x, y, z, rhw; DWORD color; } Vtx;

#define FAIL(msg) do { fprintf(stderr, "FAIL: %s\n", msg); return 1; } while (0)

int main(void)
{
    IDirect3D8 *d3d;
    IDirect3DDevice8 *dev;
    IDirect3DSurface8 *rt = NULL;
    D3DPRESENT_PARAMETERS pp;
    D3DLOCKED_RECT lr;
    HRESULT hr;
    /* One big triangle that covers the whole 32x32 target. */
    Vtx v[3] = {
        { -100.f, -100.f, 0.5f, 1.f, 0xFF3366CC },
        {  300.f, -100.f, 0.5f, 1.f, 0xFF3366CC },
        { -100.f,  300.f, 0.5f, 1.f, 0xFF3366CC },
    };
    const BYTE *px;

    SDL_setenv_unsafe("RECOMP_HIDE_WINDOW", "1", 1);

    d3d = xbox_Direct3DCreate8(220);
    if (!d3d) FAIL("Direct3DCreate8");
    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.Windowed = TRUE;
    hr = d3d->lpVtbl->CreateDevice(d3d, 0, 0, NULL, 0, &pp, &dev);
    if (FAILED(hr)) FAIL("CreateDevice");

    hr = dev->lpVtbl->CreateRenderTarget(dev, 32, 32, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, TRUE, &rt);
    if (FAILED(hr)) FAIL("CreateRenderTarget");
    dev->lpVtbl->SetRenderTarget(dev, rt, NULL);

    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

    dev->lpVtbl->BeginScene(dev);
    dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    hr = dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, v, sizeof(Vtx));
    if (FAILED(hr)) FAIL("DrawPrimitiveUP");
    dev->lpVtbl->EndScene(dev);

    hr = rt->lpVtbl->LockRect(rt, &lr, NULL, D3DLOCK_READONLY);
    if (FAILED(hr)) FAIL("LockRect");
    px = (const BYTE *)lr.pBits + 16 * lr.Pitch + 16 * 4;   /* centre; BGRA */
    printf("centre pixel BGRA: %u %u %u %u\n", px[0], px[1], px[2], px[3]);
    /* 0xFF3366CC: A=FF R=33 G=66 B=CC */
    if (abs(px[2] - 0x33) > 2 || abs(px[1] - 0x66) > 2 || abs(px[0] - 0xCC) > 2) {
        rt->lpVtbl->UnlockRect(rt);
        FAIL("pixel is not the vertex colour");
    }
    rt->lpVtbl->UnlockRect(rt);

    /* A second frame: present, then clear and draw again (pass reuse, cache hit). */
    dev->lpVtbl->Present(dev, NULL, NULL, NULL, NULL);
    dev->lpVtbl->BeginScene(dev);
    dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    hr = dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, v, sizeof(Vtx));
    if (FAILED(hr)) FAIL("second DrawPrimitiveUP");
    dev->lpVtbl->EndScene(dev);
    hr = rt->lpVtbl->LockRect(rt, &lr, NULL, D3DLOCK_READONLY);
    if (FAILED(hr)) FAIL("second LockRect");
    px = (const BYTE *)lr.pBits + 16 * lr.Pitch + 16 * 4;
    if (abs(px[2] - 0x33) > 2) { rt->lpVtbl->UnlockRect(rt); FAIL("second frame pixel wrong"); }
    rt->lpVtbl->UnlockRect(rt);

    puts("PASS: fixed-function draw through the SDL_GPU D3D8 layer");
    return 0;
}
