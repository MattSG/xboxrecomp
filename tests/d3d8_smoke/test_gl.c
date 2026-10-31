/*
 * d3d8_gl_smoke - POSIX smoke test for the OpenGL D3D8 backend (d3d8_gl.c).
 *
 * Needs a real GL 3.3 core context (WSLg llvmpipe on the dev box). The
 * backend is included directly so its window and GL state are reachable;
 * the window is hidden after device creation. Covers device creation, a
 * clear, texture create/lock/unlock in several formats, and present.
 */

#include "../../src/d3d/d3d8_gl.c"

#include <stdio.h>

static int failures;
#define CHECK(label, cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", label); failures++; } } while (0)

static void check_gl(const char *label)
{
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        fprintf(stderr, "FAIL: %s: GL error 0x%04x\n", label, (unsigned)e);
        failures++;
    }
}

/* Lock, write every staged row, unlock. min_pitch is the smallest row size
 * that holds one row of texels, and rows is the number of rows the app
 * writes (4x4 blocks for compressed formats). */
static void texture_round_trip(IDirect3DDevice8 *dev, const char *name,
                               D3DFORMAT fmt, UINT w, UINT h,
                               UINT min_pitch, UINT rows)
{
    IDirect3DTexture8 *tex = NULL;
    D3DLOCKED_RECT lr;
    HRESULT hr;
    char label[128];

    snprintf(label, sizeof label, "%s create", name);
    hr = dev->lpVtbl->CreateTexture(dev, w, h, 1, 0, fmt, D3DPOOL_MANAGED, &tex);
    CHECK(label, hr == D3D_OK && tex != NULL);
    if (!tex) return;

    snprintf(label, sizeof label, "%s lock", name);
    memset(&lr, 0, sizeof lr);
    hr = tex->lpVtbl->LockRect(tex, 0, &lr, NULL, 0);
    CHECK(label, hr == D3D_OK && lr.pBits != NULL);
    if (hr == D3D_OK && lr.pBits) {
        snprintf(label, sizeof label, "%s pitch %d >= %u", name, lr.Pitch, min_pitch);
        CHECK(label, (UINT)lr.Pitch >= min_pitch);
        /* Writing the whole surface must stay inside the staging buffer. */
        if ((UINT)lr.Pitch >= min_pitch)
            memset(lr.pBits, 0x5A, (size_t)lr.Pitch * rows);
    }

    snprintf(label, sizeof label, "%s unlock", name);
    hr = tex->lpVtbl->UnlockRect(tex, 0);
    CHECK(label, hr == D3D_OK);
    check_gl(label);

    tex->lpVtbl->Release(tex);
}

int main(void)
{
    IDirect3D8 *d3d;
    IDirect3DDevice8 *dev = NULL;
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;
    const char *renderer;

    d3d = xbox_Direct3DCreate8(220);
    CHECK("Direct3DCreate8", d3d != NULL);
    if (!d3d) return 1;

    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth  = 320;
    pp.BackBufferHeight = 240;
    hr = d3d->lpVtbl->CreateDevice(d3d, 0, 0, NULL, 0, &pp, &dev);
    CHECK("CreateDevice", hr == D3D_OK && dev != NULL);
    if (!dev) {
        fprintf(stderr, "d3d8_gl_smoke: no device (is a display available?)\n");
        return 1;
    }
    SDL_HideWindow(g.window);
    renderer = (const char *)glGetString(GL_RENDERER);
    CHECK("GL renderer string", renderer != NULL && renderer[0] != '\0');
    check_gl("after CreateDevice");

    /* Clear: return code and GL error state only (no readback). */
    hr = dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF336699, 1.0f, 0);
    CHECK("Clear", hr == D3D_OK);
    check_gl("Clear");

    /* Textures: 32-bit (uploaded), 16-bit, 8-bit, 64-bit float, DXT1. */
    texture_round_trip(dev, "A8R8G8B8", D3DFMT_A8R8G8B8, 64, 32, 64 * 4, 32);
    texture_round_trip(dev, "R5G6B5", D3DFMT_R5G6B5, 64, 32, 64 * 2, 32);
    texture_round_trip(dev, "L8", D3DFMT_L8, 64, 32, 64, 32);
    texture_round_trip(dev, "A16B16G16R16F", D3DFMT_A16B16G16R16F, 64, 32, 64 * 8, 32);
    texture_round_trip(dev, "DXT1", D3DFMT_DXT1, 64, 32, (64 / 4) * 8, 32 / 4);

    hr = dev->lpVtbl->Present(dev, NULL, NULL, NULL, NULL);
    CHECK("Present", hr == D3D_OK);
    check_gl("Present");

    if (failures) {
        fprintf(stderr, "d3d8_gl_smoke: %d failure(s)\n", failures);
        return 1;
    }
    printf("d3d8_gl_smoke: all checks passed (%s)\n", renderer);
    return 0;
}
