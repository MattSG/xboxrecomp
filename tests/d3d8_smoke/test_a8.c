/* A8 textures through the fixed-function path: Xbox A8 samples as white RGB
 * with the texel in alpha (DXGI-style A8 supplies zero RGB), so a diffuse
 * vertex colour must come through unchanged and the alpha must be the texel's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3d8_internal.h"

typedef struct { float x, y, z, rhw; DWORD color; float u, v; } Vtx;

#define FAIL(msg) do { fprintf(stderr, "FAIL: %s\n", msg); return 1; } while (0)

int main(void)
{
    IDirect3D8 *d3d;
    IDirect3DDevice8 *dev;
    IDirect3DSurface8 *rt = NULL;
    IDirect3DTexture8 *tex = NULL;
    D3DPRESENT_PARAMETERS pp;
    D3DLOCKED_RECT lr;
    HRESULT hr;
    UINT y;
    Vtx v[3] = {
        { -100.f, -100.f, 0.5f, 1.f, 0xFF3366CC, 0.5f, 0.5f },
        {  300.f, -100.f, 0.5f, 1.f, 0xFF3366CC, 0.5f, 0.5f },
        { -100.f,  300.f, 0.5f, 1.f, 0xFF3366CC, 0.5f, 0.5f },
    };
    const BYTE *px;

    SDL_setenv_unsafe("RECOMP_HIDE_WINDOW", "1", 1);
    d3d = xbox_Direct3DCreate8(220);
    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth = 64; pp.BackBufferHeight = 64; pp.Windowed = TRUE;
    if (!d3d || FAILED(d3d->lpVtbl->CreateDevice(d3d, 0, 0, NULL, 0, &pp, &dev))) FAIL("CreateDevice");

    if (FAILED(dev->lpVtbl->CreateRenderTarget(dev, 32, 32, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, TRUE, &rt))) FAIL("CreateRenderTarget");
    dev->lpVtbl->SetRenderTarget(dev, rt, NULL);

    hr = dev->lpVtbl->CreateTexture(dev, 4, 4, 1, 0, D3DFMT_A8, D3DPOOL_MANAGED, &tex);
    if (FAILED(hr)) FAIL("CreateTexture A8");
    if (FAILED(tex->lpVtbl->LockRect(tex, 0, &lr, NULL, 0))) FAIL("texture LockRect");
    for (y = 0; y < 4; y++) memset((BYTE *)lr.pBits + y * lr.Pitch, 0x80, 4);
    tex->lpVtbl->UnlockRect(tex, 0);

    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture8 *)tex);
    dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);

    dev->lpVtbl->BeginScene(dev);
    dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
    if (FAILED(dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, v, sizeof(Vtx)))) FAIL("DrawPrimitiveUP");
    dev->lpVtbl->EndScene(dev);

    if (FAILED(rt->lpVtbl->LockRect(rt, &lr, NULL, D3DLOCK_READONLY))) FAIL("LockRect");
    px = (const BYTE *)lr.pBits + 16 * lr.Pitch + 16 * 4;     /* BGRA */
    printf("centre pixel BGRA: %u %u %u %u\n", px[0], px[1], px[2], px[3]);
    if (abs(px[2] - 0x33) > 2 || abs(px[1] - 0x66) > 2 || abs(px[0] - 0xCC) > 2) FAIL("A8 darkened the vertex colour");
    if (abs(px[3] - 0x80) > 2) FAIL("alpha is not the A8 texel");
    rt->lpVtbl->UnlockRect(rt);

    puts("PASS: A8 samples as white RGB with the texel in alpha");
    return 0;
}
