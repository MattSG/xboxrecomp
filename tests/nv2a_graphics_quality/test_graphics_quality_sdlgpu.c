/* SDL3 GPU counterpart of test_graphics_quality.c: the panorama gate and its
 * cubic sampling (the shared combiner HLSL), and the packed depth view, with
 * the D3D11 test's expectations. Pixels are read back from the GPU.
 *
 * Not ported: the placement check. It reads the vertex stage's output through
 * stream output, which the SDL GPU API does not have; the SDL vertex placement
 * runs in its own shader and is not captured. */
#include <SDL3/SDL.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/video/nv2a_sdlgpu.c"
#include "../nv2a_sdlgpu_test_host.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static uint8_t *ram;

static NvD3DState state_for(unsigned width, unsigned height)
{
    NvD3DState st = {0};
    st.color_addr = 0x10000; st.color_pitch = width * 4;
    st.surface_format = 0x28; st.clip_w = width; st.clip_h = height;
    st.color_mask = 0x01010101; return st;
}

/* The panorama flag the draw would take: the renderer's gate for the 256x256
 * DXT5 horizon (s_draw_bspline) is decided while the draw state is set up. */
static void assert_panorama_gate(NvD3DState *st, int expected)
{
    Surface *target; uint32_t w, h, z;
    CHECK(setup_pipeline(st, NV_D3D_TRIANGLES, &target, &w, &h, &z, 1));
    CHECK((s_draw_bspline != 0) == expected);
}

static NvD3DTexture panorama_texture(void)
{
    NvD3DTexture t = {0};
    t.addr = 0x100000; t.color = 0x0F;
    t.format = 0x09000F00; t.levels = 9;
    t.width = t.height = 256;
    t.filter = (6u << 16) | (2u << 24);
    t.control0 = 8u * 256u << 6; t.address = 0x030101;
    return t;
}

/* Same cases as test_gates in test_graphics_quality.c. SDL has the gate, so
 * every expectation is the same as on D3D11. */
static int test_gates(void)
{
    NvD3DState base = state_for(64, 64), st;
    unsigned i;
    base.tex_used = 1; base.blend_enable = 1;
    base.blend_sfactor = 0x304; base.blend_dfactor = 0x305;
    base.tex[0] = panorama_texture();
    memset(ram + 0x100000, 0x55, 0x90000);
    nv2a_d3d_set_bspline_blend(0, 0); assert_panorama_gate(&base, 0);
    nv2a_d3d_set_bspline_blend(0x304, 0x305); assert_panorama_gate(&base, 1);
    for (i = 0; i < 10; ++i) {
        st = base;
        switch (i) {
        case 0: st.depth_test = 1; st.zeta_addr = 0x20000; st.zeta_pitch = 64 * 4; break;
        case 1: st.blend_enable = 0; break;
        case 2: st.blend_sfactor = 0x302; break;
        case 3: st.blend_dfactor = 0x303; break;
        case 4: st.tex[0].width = 128; st.tex[0].format++; break;
        case 5: st.tex[0].height = 128; st.tex[0].format++; break;
        case 6: st.tex[0].color = 0x0C; st.tex[0].format++; break;
        case 7: st.tex[0].levels = 4; st.tex[0].format++; break;
        case 8: st.tex[0].filter = (5u << 16) | (2u << 24); break;
        case 9: st.tex[0].filter = (6u << 16) | (1u << 24); break;
        }
        assert_panorama_gate(&st, 0);
    }
    st = base; st.tex[0].cube = 1; st.tex[0].format++;
    assert_panorama_gate(&st, 0);
    st = base; st.rc_seen = 1; st.rc.stage_program = 1u << 5;
    st.tex[1] = st.tex[0]; memset(&st.tex[0], 0, sizeof st.tex[0]);
    assert_panorama_gate(&st, 0);
    nv2a_d3d_set_bspline_blend(0, 0); assert_panorama_gate(&base, 0);
    puts("PASS: panorama gate selects only title-enabled stage-zero 256x256 linearly-filtered DXT5 no-depth draws; twelve ordinary/cube/stage/mip/blend/filter cases remain unchanged");
    return 0;
}

/* The panorama's samples: the gate-enabled draw samples the 256x256 DXT5
 * horizon with the shared cubic kernel (the combiner's bspline path); the
 * gate-off draw samples bilinear. Both are compared with independent
 * references over the CPU-decoded texture. Each sample is a 2x2 pixel block,
 * so the hardware derivatives are zero and the bilinear draw reads level 0 as
 * the reference does. Alpha endpoints are 255, so the blend leaves the sample
 * colour as written. */
#define PANO_W 256
#define PANO_TAPS 128
#define PANO_TOL (8.0 / 255.0)   /* BC3 decode and filter precision (see native_bc) */

static double cubic_kernel(double distance)
{
    double a = fabs(distance);
    if (a < 1.0) return 2.0 / 3.0 - a * a + a * a * a / 2.0;
    if (a < 2.0) return pow(2.0 - a, 3.0) / 6.0;
    return 0;
}

static int wrap_index(int value, int size)
{
    int result = value % size;
    return result < 0 ? result + size : result;
}

/* Wrapped sample at normalised (u, v) over texels in [0,1], cubic or bilinear. */
static void panorama_reference(const float (*pixels)[4], double u, double v, int cubic, double result[4])
{
    double px = u * PANO_W - 0.5, py = v * PANO_W - 0.5;
    int bx = (int)floor(px), by = (int)floor(py), x, y, c;
    memset(result, 0, 4 * sizeof(double));
    for (y = cubic ? -1 : 0; y <= (cubic ? 2 : 1); ++y)
        for (x = cubic ? -1 : 0; x <= (cubic ? 2 : 1); ++x) {
            double wx = cubic ? cubic_kernel(px - (bx + x)) : x ? px - bx : 1.0 - (px - bx);
            double wy = cubic ? cubic_kernel(py - (by + y)) : y ? py - by : 1.0 - (py - by);
            unsigned index = wrap_index(by + y, PANO_W) * PANO_W + wrap_index(bx + x, PANO_W);
            for (c = 0; c < 4; ++c) result[c] += pixels[index][c] * wx * wy;
        }
}

static int test_panorama(void)
{
    NvD3DTexture texture = panorama_texture();
    TexLayout layout;
    static uint32_t decoded[PANO_W * PANO_W];
    static float texels[PANO_W * PANO_W][4];
    static float pos[PANO_TAPS * 4][4], uv[PANO_TAPS * 4][2];
    static uint32_t indices[PANO_TAPS * 6];
    static uint32_t cubic_out[PANO_TAPS], bilinear_out[PANO_TAPS];
    uint32_t state = 0x7A11E5u, i;
    unsigned tap, changed = 0;
    NvD3DState st = {0};
    double max_error = 0;
    CHECK(tex_layout(&texture, &layout));
    for (i = 0; i < layout.total; i += 4) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        memcpy(ram + texture.addr + i, &state, 4);
    }
    /* Opaque alpha: both endpoints of every block's alpha ramp are 255. */
    for (i = 0; i < layout.total; i += 16) ram[texture.addr + i] = ram[texture.addr + i + 1] = 0xFF;
    decode_level(&texture, &layout, ram + texture.addr, 0, NULL, decoded);
    for (i = 0; i < PANO_W * PANO_W; i++) {
        texels[i][0] = ((decoded[i] >> 16) & 255) / 255.0f;
        texels[i][1] = ((decoded[i] >> 8) & 255) / 255.0f;
        texels[i][2] = (decoded[i] & 255) / 255.0f;
        texels[i][3] = (decoded[i] >> 24) / 255.0f;
    }
    /* Texel centres of the first row, bilinear at lod 0: the GPU's native BC3
     * decode must reproduce the CPU decode of the same bytes to within BC
     * precision (D3D12 differs by up to 3/255 here; see native_bc). */
    {
        static float rpos[PANO_W * 4][4], ruv[PANO_W * 4][2];
        static uint32_t ridx[PANO_W * 6], row[PANO_W];
        static NvD3DState rs;
        NvD3DAttrib ra[16] = {{0}};
        float rdef[16][4] = {{0}};
        unsigned x, k;
        for (x = 0; x < PANO_W; x++) {
            for (k = 0; k < 4; k++) {
                rpos[x * 4 + k][0] = (float)(2 * x + 2 * (k & 1));
                rpos[x * 4 + k][1] = (float)(2 * (k >> 1));
                rpos[x * 4 + k][2] = .5f; rpos[x * 4 + k][3] = 1;
                ruv[x * 4 + k][0] = (x + .5f) / PANO_W; ruv[x * 4 + k][1] = .5f / PANO_W;
            }
            ridx[x * 6 + 0] = x * 4; ridx[x * 6 + 1] = x * 4 + 1; ridx[x * 6 + 2] = x * 4 + 2;
            ridx[x * 6 + 3] = x * 4 + 2; ridx[x * 6 + 4] = x * 4 + 1; ridx[x * 6 + 5] = x * 4 + 3;
        }
        rs = st;
        rs.color_addr = 0x280000; rs.color_pitch = PANO_W * 2 * 4; rs.clip_w = PANO_W * 2; rs.clip_h = 2;
        rs.surface_format = 0x28; rs.color_mask = 0x01010101; rs.tex_used = 1;
        rs.blend_enable = 0; rs.tex[0] = texture;
        rdef[3][0] = rdef[3][1] = rdef[3][2] = rdef[3][3] = 1;
        ra[0].enabled = 1; ra[0].type = 2; ra[0].size = 4; ra[0].stride = 16; ra[0].data = (const uint8_t *)rpos;
        ra[9].enabled = 1; ra[9].type = 2; ra[9].size = 2; ra[9].stride = 8; ra[9].data = (const uint8_t *)ruv;
        nv2a_d3d_set_bspline_blend(0, 0);
        nv2a_d3d_clear(&rs, 0xF0, 0xFF000000u, 0, 0, 0, 0xFFFF, 0xFFFF);
        nv2a_d3d_draw_ub(&rs, NV_D3D_TRIANGLES, ra, rdef, 0, ridx, PANO_W * 6, 0, PANO_W * 4 - 1, NULL);
        for (x = 0; x < PANO_W; x++) row[x] = nv2a_test_target_pixel(rs.color_addr, 2 * x, 0);
        for (x = 0; x < PANO_W; x++) {
            for (k = 0; k < 3; k++) {
                unsigned sh = k == 0 ? 16 : k == 1 ? 8 : 0;
                int cpu = (decoded[x] >> sh) & 255, gpu = (row[x] >> sh) & 255;
                if (abs(cpu - gpu) > 8)
                    fprintf(stderr, "texel %u channel %u: cpu %d gpu %d\n", x, k, cpu, gpu);
                CHECK(abs(cpu - gpu) <= 8);
            }
        }
    }
    for (tap = 0; tap < PANO_TAPS; tap++) {
        float u = -.125f + (tap % 16) * .0875f, v = -.25f + (tap / 16) * .19375f;
        unsigned k;
        for (k = 0; k < 4; k++) {
            pos[tap * 4 + k][0] = (float)(2 * tap + 2 * (k & 1));
            pos[tap * 4 + k][1] = (float)(2 * (k >> 1));
            pos[tap * 4 + k][2] = .5f; pos[tap * 4 + k][3] = 1;
            uv[tap * 4 + k][0] = u; uv[tap * 4 + k][1] = v;
        }
        indices[tap * 6 + 0] = tap * 4; indices[tap * 6 + 1] = tap * 4 + 1; indices[tap * 6 + 2] = tap * 4 + 2;
        indices[tap * 6 + 3] = tap * 4 + 2; indices[tap * 6 + 4] = tap * 4 + 1; indices[tap * 6 + 5] = tap * 4 + 3;
    }
    st.color_addr = 0x200000; st.color_pitch = PANO_TAPS * 2 * 4;
    st.surface_format = 0x28; st.clip_w = PANO_TAPS * 2; st.clip_h = 2;
    st.color_mask = 0x01010101;
    st.tex_used = 1;
    st.blend_enable = 1; st.blend_sfactor = 0x304; st.blend_dfactor = 0x305;
    st.blend_equation = 0x8006;
    st.tex[0] = texture;
    for (i = 0; i < 2; i++) {
        float defaults[16][4] = {{0}};
        NvD3DAttrib attributes[16] = {{0}};
        uint32_t *out = i ? bilinear_out : cubic_out;
        unsigned n;
        /* White diffuse: the sample is the output colour. */
        defaults[3][0] = defaults[3][1] = defaults[3][2] = defaults[3][3] = 1;
        attributes[0].enabled = 1; attributes[0].type = 2; attributes[0].size = 4;
        attributes[0].stride = sizeof pos[0]; attributes[0].data = (const uint8_t *)pos;
        attributes[9].enabled = 1; attributes[9].type = 2; attributes[9].size = 2;
        attributes[9].stride = sizeof uv[0]; attributes[9].data = (const uint8_t *)uv;
        nv2a_d3d_set_bspline_blend(i ? 0 : 0x304, i ? 0 : 0x305);
        nv2a_d3d_clear(&st, 0xF0, 0xFF000000u, 0, 0, 0, 0xFFFF, 0xFFFF);
        nv2a_d3d_draw_ub(&st, NV_D3D_TRIANGLES, attributes, defaults, 0, indices, PANO_TAPS * 6,
                         0, PANO_TAPS * 4 - 1, NULL);
        CHECK(s_draw_bspline == !i);
        for (n = 0; n < PANO_TAPS; n++) out[n] = nv2a_test_target_pixel(st.color_addr, 2 * n, 0);
    }
    for (tap = 0; tap < PANO_TAPS; tap++) {
        float u = -.125f + (tap % 16) * .0875f, v = -.25f + (tap / 16) * .19375f;
        double cubic_ref[4], bilinear_ref[4];
        unsigned c;
        panorama_reference((const float (*)[4])texels, u, v, 1, cubic_ref);
        panorama_reference((const float (*)[4])texels, u, v, 0, bilinear_ref);
        for (c = 0; c < 3; ++c) {
            unsigned shift = c == 0 ? 16 : c == 1 ? 8 : 0;
            double gpu_cubic = ((cubic_out[tap] >> shift) & 255) / 255.0;
            double gpu_bilinear = ((bilinear_out[tap] >> shift) & 255) / 255.0;
            double e1 = fabs(gpu_cubic - cubic_ref[c]), e2 = fabs(gpu_bilinear - bilinear_ref[c]);
            if (e1 > max_error) max_error = e1;
            if (e2 > max_error) max_error = e2;
            if (e1 > PANO_TOL || e2 > PANO_TOL)
                fprintf(stderr, "panorama tap %u channel %u: cubic %.4f (ref %.4f) bilinear %.4f (ref %.4f)\n",
                        tap, c, gpu_cubic, cubic_ref[c], gpu_bilinear, bilinear_ref[c]);
            CHECK(e1 <= PANO_TOL);
            CHECK(e2 <= PANO_TOL);
            if (fabs(bilinear_ref[c] - cubic_ref[c]) > .03) changed++;
        }
    }
    CHECK(changed > 100);
    printf("PASS: 384 panorama samples on the GPU match the shared cubic kernel (gate on) and bilinear (gate off); max error %.6f\n", max_error);
    return 0;
}

/* Packed depth view at scales 1..8, two variants (the D3D11 test's
 * single-sample and 4x MSAA; the SDL renderer keeps one sample for both). */
static int test_depth(void)
{
    unsigned variant, scale, x, y;
    float depth[16];
    for (variant = 0; variant < 2; ++variant) {
        uint32_t zeta = 0x300000 + variant * 0x10000;
        Depth *d = depth_get(zeta, 16, 2);
        NvD3DState clear = {0};
        CHECK(d);
        clear.zeta_addr = zeta; clear.zeta_pitch = 16 * 4;
        clear.surface_format = 0x28; clear.clip_w = 16; clear.clip_h = 2;
        for (x = 0; x < 16; ++x) {
            /* Columns get distinct depths, cleared through the guest clear path. */
            uint32_t z;
            depth[x] = ((x * 7) % 16 + 0.5f) / 16.0f;
            z = (uint32_t)(depth[x] * 16777215.0f) << 8;
            nv2a_d3d_clear(&clear, 1, 0, z, x, 0, x, 1);
        }
        for (scale = 1; scale <= 8; scale *= 2) {
            uint32_t *texels = malloc(32 * 2 * 8);
            const uint16_t *pixels = (const uint16_t *)texels;
            CHECK(texels);
            d->w = 16 / scale; d->h = 2; ++d->gen;
            CHECK(depth_y16(d));
            CHECK(nv2a_test_read_texels(d->y16, 32, 2, 8, 0, 0, texels));
            for (y = 0; y < 2; ++y) for (x = 0; x < 32; ++x) {
                /* A high-word tap is centered a quarter guest pixel right.
                 * The view must compensate in host coordinates, including
                 * the left boundary where the coordinate clamps to zero. */
                int column = (int)floor((x + .5) / 2.0 - .25 * scale);
                unsigned expected;
                const uint16_t *pixel;
                if (column < 0) column = 0;
                expected = (unsigned)floor(depth[column] * 16777215.0 + .5) >> 8;
                pixel = pixels + (y * 32 + x) * 4;
                CHECK(abs((int)pixel[0] - (int)expected) <= 1);
                CHECK(pixel[0] == pixel[1] && pixel[1] == pixel[2] && pixel[3] == 65535);
            }
            free(texels);
        }
    }
    puts("PASS: 512 depth-view pixels at scales 1/2/4/8 on both variants; quarter-pixel compensation and left clamping preserve packed depth");
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    ram = calloc(1, 0x400000);
    CHECK(ram);
    nv2a_test_ram = ram;
    nv2a_test_default_env("RECOMP_ASPECT", "4:3");
    /* Lavapipe (Linux) blurs the panorama's samples under the default 16x
     * anisotropy, even over 2x2 constant-UV blocks: texel centres then differ
     * from the CPU decode by up to 40/255. D3D12 does not (it passes at 16x),
     * so 1x is pinned for both, keeping the lod-0 references exact. */
    nv2a_test_default_env("RECOMP_ANISO", "1");
    nv2a_test_default_env("RECOMP_PS_PREWARM", "0");
    nv2a_test_default_env("RECOMP_SHADER_CACHE", "0");
    CHECK(nv2a_test_init());
    if (!strcmp(argv[1], "panorama")) return test_panorama();
    if (!strcmp(argv[1], "gates")) return test_gates();
    if (!strcmp(argv[1], "depth")) return test_depth();
    CHECK(0);
    return 1;
}
