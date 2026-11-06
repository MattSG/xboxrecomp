/* IDirect3DDevice8::Clear: rectangles, the viewport limit, and partial depth
 * clears (checked by what a depth-tested draw then does). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3d8_internal.h"

typedef struct { float x, y, z, rhw; DWORD color; } Vtx;

#define FAIL(msg) do { fprintf(stderr, "FAIL: %s\n", msg); return 1; } while (0)

static IDirect3DDevice8 *dev;
static IDirect3DSurface8 *rt;

/* BGRA bytes of one pixel of the render target. */
static int pixel(int x, int y, unsigned char out[4])
{
    D3DLOCKED_RECT lr;
    if (FAILED(rt->lpVtbl->LockRect(rt, &lr, NULL, D3DLOCK_READONLY))) return 0;
    memcpy(out, (const BYTE *)lr.pBits + (size_t)y * lr.Pitch + (size_t)x * 4, 4);
    rt->lpVtbl->UnlockRect(rt);
    return 1;
}

static int is(int x, int y, unsigned r, unsigned g, unsigned b)
{
    unsigned char p[4];
    if (!pixel(x, y, p)) return 0;
    return abs(p[2] - (int)r) <= 2 && abs(p[1] - (int)g) <= 2 && abs(p[0] - (int)b) <= 2;
}

static void quad(DWORD color, float z)
{
    Vtx v[3] = { { -100.f, -100.f, z, 1.f, color }, { 300.f, -100.f, z, 1.f, color },
                 { -100.f, 300.f, z, 1.f, color } };
    dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, v, sizeof(Vtx));
}

int main(void)
{
    IDirect3D8 *d3d;
    IDirect3DSurface8 *ds = NULL;
    D3DPRESENT_PARAMETERS pp;
    D3DRECT rect;
    D3DVIEWPORT8 vp;

    SDL_setenv_unsafe("RECOMP_HIDE_WINDOW", "1", 1);
    d3d = xbox_Direct3DCreate8(220);
    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = 64; pp.BackBufferHeight = 64; pp.Windowed = TRUE;
    if (!d3d || FAILED(d3d->lpVtbl->CreateDevice(d3d, 0, 0, NULL, 0, &pp, &dev))) FAIL("CreateDevice");
    if (FAILED(dev->lpVtbl->CreateRenderTarget(dev, 32, 32, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, TRUE, &rt))) FAIL("rt");
    if (FAILED(dev->lpVtbl->CreateDepthStencilSurface(dev, 32, 32, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, &ds))) FAIL("ds");
    dev->lpVtbl->SetRenderTarget(dev, rt, ds);
    vp.X = 0; vp.Y = 0; vp.Width = 32; vp.Height = 32; vp.MinZ = 0; vp.MaxZ = 1;
    dev->lpVtbl->SetViewport(dev, &vp);
    dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    dev->lpVtbl->BeginScene(dev);

    /* 1. A rectangle: only that region changes. */
    dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);
    rect.x1 = 8; rect.y1 = 8; rect.x2 = 24; rect.y2 = 24;
    dev->lpVtbl->Clear(dev, 1, &rect, D3DCLEAR_TARGET, 0xFFFF0000, 1.0f, 0);
    if (!is(16, 16, 255, 0, 0)) FAIL("rectangle interior not cleared");
    if (!is(4, 4, 0, 0, 0) || !is(28, 28, 0, 0, 0) || !is(24, 16, 0, 0, 0)) FAIL("rectangle clear leaked outside");

    /* 2. The viewport limits a clear without rectangles. */
    dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    vp.Width = 16;
    dev->lpVtbl->SetViewport(dev, &vp);
    dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF00FF00, 1.0f, 0);
    if (!is(8, 16, 0, 255, 0)) FAIL("viewport interior not cleared");
    if (!is(24, 16, 0, 0, 0)) FAIL("viewport clear leaked outside the viewport");
    vp.Width = 32;
    dev->lpVtbl->SetViewport(dev, &vp);

    /* 3. A partial depth clear: depth 0 on the left half, 1 on the right.
     * A depth-tested draw at z=0.5 then lands only on the right. */
    dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);
    rect.x1 = 0; rect.y1 = 0; rect.x2 = 16; rect.y2 = 32;
    dev->lpVtbl->Clear(dev, 1, &rect, D3DCLEAR_ZBUFFER, 0, 0.0f, 0);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, TRUE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    quad(0xFF0000FF, 0.5f);
    if (!is(24, 16, 0, 0, 255)) FAIL("draw missing where depth was 1");
    if (!is(8, 16, 0, 0, 0)) FAIL("draw landed where depth was cleared to 0");

    dev->lpVtbl->EndScene(dev);
    puts("PASS: Clear honours rectangles, the viewport and partial depth");
    return 0;
}
