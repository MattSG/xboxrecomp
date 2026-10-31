/* Render-state translation: the D3D8 render states become a memcmp-comparable
 * pipeline key (blend, depth/stencil, raster). No GPU device is involved. */
#include "d3d8_internal.h"
#include <stdio.h>
#include <string.h>

static DWORD rs[256];
static int failures;

XGpu *d3d8_GetGpu(void) { static XGpu g; return &g; }
const DWORD *d3d8_GetRenderStates(void) { return rs; }
const DWORD *d3d8_GetTSS(DWORD stage) { (void)stage; return NULL; }

#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", name); failures++; } } while (0)

static D3D8StateKey key(void)
{
    D3D8StateKey k;
    d3d8_states_get_key(&k);
    return k;
}

static void check_blend_factors(void)
{
    static const struct { DWORD guest; Uint8 rgb, alpha; } cases[] = {
        {D3DBLEND_SRCALPHA,    SDL_GPU_BLENDFACTOR_SRC_ALPHA,           SDL_GPU_BLENDFACTOR_SRC_ALPHA},
        {D3DBLEND_SRCCOLOR,    SDL_GPU_BLENDFACTOR_SRC_COLOR,           SDL_GPU_BLENDFACTOR_SRC_ALPHA},
        {D3DBLEND_INVSRCCOLOR, SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR, SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA},
        {D3DBLEND_DESTCOLOR,   SDL_GPU_BLENDFACTOR_DST_COLOR,           SDL_GPU_BLENDFACTOR_DST_ALPHA},
        {D3DBLEND_INVDESTCOLOR,SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR, SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA},
        {D3DBLEND_ONE,         SDL_GPU_BLENDFACTOR_ONE,                 SDL_GPU_BLENDFACTOR_ONE},
        {D3DBLEND_ZERO,        SDL_GPU_BLENDFACTOR_ZERO,                SDL_GPU_BLENDFACTOR_ZERO}
    };
    unsigned side, i;
    rs[D3DRS_ALPHABLENDENABLE] = TRUE;
    for (side = 0; side < 2; side++) {
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            D3D8StateKey k;
            rs[D3DRS_SRCBLEND] = side ? D3DBLEND_ONE : cases[i].guest;
            rs[D3DRS_DESTBLEND] = side ? cases[i].guest : D3DBLEND_ZERO;
            k = key();
            /* Colour factors are replaced by their alpha components in the alpha channel. */
            if (!k.blend_enable ||
                k.src_color != (side ? SDL_GPU_BLENDFACTOR_ONE : cases[i].rgb) ||
                k.dst_color != (side ? cases[i].rgb : SDL_GPU_BLENDFACTOR_ZERO) ||
                k.src_alpha != (side ? SDL_GPU_BLENDFACTOR_ONE : cases[i].alpha) ||
                k.dst_alpha != (side ? cases[i].alpha : SDL_GPU_BLENDFACTOR_ZERO)) {
                printf("FAIL: blend side=%u factor=%lu\n", side, (unsigned long)cases[i].guest);
                failures++;
            }
        }
    }
}

int main(void)
{
    D3D8StateKey k, k2;

    rs[D3DRS_ZENABLE] = TRUE;
    rs[D3DRS_ZWRITEENABLE] = TRUE;
    rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    rs[D3DRS_STENCILENABLE] = TRUE;
    rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
    rs[D3DRS_STENCILMASK] = 0xFF;
    rs[D3DRS_STENCILWRITEMASK] = 0xFF;
    rs[D3DRS_STENCILFAIL] = 1;
    rs[D3DRS_STENCILZFAIL] = 1;
    rs[D3DRS_STENCILPASS] = 1;
    rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    rs[D3DRS_COLORWRITEENABLE] = 0xF;
    rs[D3DRS_CULLMODE] = D3DCULL_NONE;
    rs[D3DRS_FILLMODE] = D3DFILL_SOLID;

    k = key();
    CHECK("initial write mask", k.stencil_write == 0xFF);
    CHECK("cull none", k.cull == SDL_GPU_CULLMODE_NONE);
    CHECK("solid fill", k.fill == SDL_GPU_FILLMODE_FILL);

    rs[D3DRS_STENCILWRITEMASK] = 0x5A;
    CHECK("STENCILWRITEMASK", key().stencil_write == 0x5A);
    rs[D3DRS_STENCILFAIL] = 2;
    CHECK("STENCILFAIL", key().stencil_fail == SDL_GPU_STENCILOP_ZERO);
    rs[D3DRS_STENCILZFAIL] = 3;
    CHECK("STENCILZFAIL", key().stencil_zfail == SDL_GPU_STENCILOP_REPLACE);
    rs[D3DRS_STENCILPASS] = 6;
    CHECK("STENCILPASS", key().stencil_pass == SDL_GPU_STENCILOP_INVERT);

    /* The old hash collided on these: (16 << 14) == (1 << 18). The key has no hash. */
    rs[D3DRS_STENCILREF] = 16;
    rs[D3DRS_STENCILMASK] = 0xFE;
    CHECK("reference/read-mask collision", key().stencil_read == 0xFE);
    rs[D3DRS_STENCILREF] = 17;
    k2 = key();
    CHECK("reference does not enter the key", k2.stencil_read == 0xFE);

    rs[D3DRS_ZWRITEENABLE] = FALSE;
    CHECK("ZWRITEENABLE", key().depth_write == 0);
    rs[D3DRS_ZFUNC] = D3DCMP_GREATER;
    CHECK("ZFUNC", key().depth_func == SDL_GPU_COMPAREOP_GREATER);
    rs[D3DRS_ZENABLE] = FALSE;
    CHECK("ZENABLE", key().depth_test == 0);
    rs[D3DRS_STENCILFUNC] = D3DCMP_EQUAL;
    CHECK("STENCILFUNC", key().stencil_func == SDL_GPU_COMPAREOP_EQUAL);
    rs[D3DRS_STENCILENABLE] = FALSE;
    CHECK("STENCILENABLE", key().stencil_enable == 0);

    rs[D3DRS_CULLMODE] = D3DCULL_CW;
    CHECK("cull CW", key().cull == SDL_GPU_CULLMODE_FRONT);
    rs[D3DRS_CULLMODE] = D3DCULL_CCW;
    CHECK("cull CCW", key().cull == SDL_GPU_CULLMODE_BACK);
    rs[D3DRS_FILLMODE] = D3DFILL_WIREFRAME;
    CHECK("wireframe", key().fill == SDL_GPU_FILLMODE_LINE);

    /* Equal states give identical keys, so the pipeline cache can memcmp them. */
    k = key(); k2 = key();
    CHECK("key is stable", memcmp(&k, &k2, sizeof k) == 0);

    check_blend_factors();
    printf("d3d8_states_smoke: %d FAILURE(S)\n", failures);
    return failures ? 1 : 0;
}
