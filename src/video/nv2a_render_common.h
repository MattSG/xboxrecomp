/**
 * What both NV2A renderers (nv2a_d3d11.c, nv2a_sdlgpu.c) share and no graphics
 * API touches: the constant-buffer layouts their shaders read, guest texture
 * and surface formats and their decoding to BGRA, vertex array sizes, index
 * rotation for flat shading, and the display aspect setting.
 *
 * Included once by each renderer, after nv2a_d3d11.h.
 */
#ifndef XBOXRECOMP_NV2A_RENDER_COMMON_H
#define XBOXRECOMP_NV2A_RENDER_COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../d3d/d3d8_swizzle.h"

/* 0 until decided */

static float clamp_aspect(float a)
{
    return a < 4.0f / 3.0f ? 4.0f / 3.0f : a > 4.0f ? 4.0f : a;
}

static float env_aspect(void)
{
    const char *e = getenv("RECOMP_ASPECT");
    float w, h;
    if (e && sscanf(e, "%f:%f", &w, &h) == 2 && h > 0 && w > 0) return clamp_aspect(w / h);
    if (e && sscanf(e, "%f", &w) == 1 && w > 0) return clamp_aspect(w);
    return 0;
}

/* Apply caller policy to the existing vertex alignment shader parameters. */
static void vertex_alignment(const NvOverlayHooks *hooks, uint32_t width,
                             uint32_t height, float place[4])
{
    place[2] = 1.0f;
    place[3] = 0.0f;
    if (hooks->vertex_alignment)
        hooks->vertex_alignment(width, height, &place[2], &place[3]);
}

typedef struct {
    uint32_t c_icw[8], a_icw[8], c_ocw[8], a_ocw[8];
    float cf0[8][4], cf1[8][4], fin_c0[4], fin_c1[4];
    uint32_t fin0, fin1, rc_control, stage_prog;
    uint32_t alpha_func, alpha_ref, ps_flags, clip_plane;
    uint32_t other_input, dot_map, pad0, pad1;
    uint32_t tex_info[4][4];
    float tex_scale[4][4];
    float fog_color[4];
    float bump_mat[4][4], bump_lum[4][4];
    float zp0[4], zp1[4];
    float wclip[8][4];
    uint32_t wclip_mode, wclip_pad[3];
} PSConsts;

typedef struct {
    uint32_t at[16][4];          /* byte offset, stride, type, size (0: DEF) */
    float def[16][4];
    uint32_t info[4];            /* start slot, program, dx offset, - */
    float map[4];                /* 2/w, 2/h, 1/zmax, squeeze */
    uint32_t fogi[4];
    float fogf[4];
    float place[4];              /* x shift (NDC), y scale, pixel-0 snap, screen offset */
} UbConsts;

typedef struct {
    float dst_rect[4], src_uv[4], src_size[4];
    float ovl_out[4], ovl_walk[4], ovl_in[4];
    uint32_t ovl_flags[4];
    float force_alpha[4];
} PresentConsts;

static uint32_t surface_color_bpp(uint32_t fmt)
{
    switch (fmt & 0xF) {
    case 0x1: case 0x2: case 0x3: case 0xA: return 2;
    case 0x4: case 0x5: case 0x6: case 0x7: case 0x8: return 4;
    case 0x9: return 1;
    default: return 0;
    }
}

/* Formats with no stored alpha hold a fixed value there: Z..=0, O..=1. */
static int surface_alpha_mode(uint32_t fmt)
{
    switch (fmt & 0xF) {
    case 0x1: case 0x4: case 0x6: return 0;     /* zero */
    case 0x2: case 0x3: case 0x5: case 0x7: return 1;   /* one */
    default: return -1;                         /* real alpha */
    }
}

enum { K_SWZ, K_LIN, K_DXT };

/* Bytes per texel and layout of an NV097 texture colour format; 0 if the
 * format is not one this decodes. */
static int tex_format(uint32_t c, int *kind)
{
    switch (c) {
    case 0x00: case 0x01: case 0x0B: case 0x19: *kind = K_SWZ; return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x1A: case 0x27:
    case 0x28: case 0x29: case 0x2C: case 0x2D: case 0x32: case 0x38: case 0x39:
        *kind = K_SWZ; return 2;
    case 0x06: case 0x07: case 0x2A: case 0x2B: case 0x3A: case 0x3B: case 0x3C:
        *kind = K_SWZ; return 4;
    case 0x0C: *kind = K_DXT; return 8;
    case 0x0E: case 0x0F: *kind = K_DXT; return 16;
    case 0x13: case 0x1B: case 0x1F: *kind = K_LIN; return 1;
    case 0x10: case 0x11: case 0x16: case 0x17: case 0x1C: case 0x1D: case 0x20:
    case 0x24: case 0x25: case 0x30: case 0x31: case 0x35: case 0x37:
    case 0x3D: case 0x3E:
        *kind = K_LIN; return 2;
    case 0x12: case 0x1E: case 0x2E: case 0x2F: case 0x3F: case 0x40: case 0x41:
        *kind = K_LIN; return 4;
    default: return 0;
    }
}

static uint32_t x5(uint32_t v) { return d3d8_expand_channel(v & 31, 5); }

static uint32_t x6(uint32_t v) { return d3d8_expand_channel(v & 63, 6); }

static uint32_t x4(uint32_t v) { return d3d8_expand_channel(v & 15, 4); }

static uint32_t argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b)
{
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* One texel, as ARGB (= BGRA bytes). */

static unsigned s_palette_refusals;
static uint32_t palette_index_refused(uint32_t index, uint32_t pal_len)
{
    unsigned n = ++s_palette_refusals;
    if (n <= 8 || !(n & (n - 1)))
        fprintf(stderr, "[PALETTE_REFUSED] count=%u index=%u palette_len=%u (texel is transparent black)\n",
                n, index, pal_len);
    return 0;
}
static unsigned s_tex_refusals;
static void texture_refused(const NvD3DTexture *t, const char *reason, size_t bytes)
{
    unsigned n = ++s_tex_refusals;
    if (n > 8 && (n & (n - 1))) return;
    fprintf(stderr, "[TEXTURE_REFUSED] count=%u addr=%08X size=%ux%u color=%02X format=%08X pitch=%u levels=%u bytes=%zu reason=%s\n",
            n, t->addr, t->width, t->height, t->color, t->format,
            t->pitch, t->levels, bytes, reason);
    fflush(stderr);
}
static uint32_t texel(uint32_t c, const uint8_t *p, const uint32_t *pal, uint32_t pal_len)
{
    uint32_t v;
    switch (c) {
    case 0x00: case 0x13: return argb(255, p[0], p[0], p[0]);           /* Y8  */
    case 0x01: case 0x1B: return argb(p[0], p[0], p[0], p[0]);          /* AY8 */
    case 0x19: case 0x1F: return argb(p[0], 255, 255, 255);             /* A8  */
    case 0x0B: return p[0] < pal_len ? pal[p[0]] : palette_index_refused(p[0], pal_len); /* I8 */
    case 0x1A: case 0x20: return argb(p[1], p[0], p[0], p[0]);          /* A8Y8 */
    case 0x06: case 0x12: memcpy(&v, p, 4); return v;
    case 0x07: case 0x1E: memcpy(&v, p, 4); return v | 0xFF000000u;
    case 0x3A: case 0x3F: memcpy(&v, p, 4);                              /* A8B8G8R8 */
        return (v & 0xFF00FF00u) | ((v & 0xFF) << 16) | ((v >> 16) & 0xFF);
    case 0x3B: case 0x40: memcpy(&v, p, 4);                              /* B8G8R8A8 */
        return argb(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24);
    case 0x3C: case 0x41: memcpy(&v, p, 4);                              /* R8G8B8A8 */
        return argb(v & 0xFF, v >> 24, (v >> 16) & 0xFF, (v >> 8) & 0xFF);
    case 0x2A: case 0x2B: case 0x2E: case 0x2F: memcpy(&v, p, 4);        /* depth */
        return argb(255, v >> 24, (v >> 16) & 0xFF, (v >> 8) & 0xFF);
    }
    v = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    switch (c) {
    case 0x02: case 0x10: return argb(v & 0x8000 ? 255 : 0, x5(v >> 10), x5(v >> 5), x5(v));
    case 0x03: case 0x1C: return argb(255, x5(v >> 10), x5(v >> 5), x5(v));
    case 0x04: case 0x1D: return argb(x4(v >> 12), x4(v >> 8), x4(v >> 4), x4(v));
    case 0x05: case 0x11: return argb(255, x5(v >> 11), x6(v >> 5), x5(v));
    case 0x27: case 0x37: return argb(255, x6(v >> 10), x5(v >> 5), x5(v));  /* R6G5B5 */
    case 0x28: case 0x17: return argb(255, 0, v >> 8, v & 0xFF);              /* G8B8 */
    case 0x29: case 0x16: return argb(255, v >> 8, 0, v & 0xFF);              /* R8B8 */
    case 0x38: case 0x3D: return argb(v & 1 ? 255 : 0, x5(v >> 11), x5(v >> 6), x5(v >> 1));
    case 0x39: case 0x3E: return argb(x4(v), x4(v >> 12), x4(v >> 8), x4(v >> 4));
    case 0x32: case 0x35: return argb(255, 255, v >> 8, v & 0xFF); /* Y16: one, high byte, low byte, one */
    case 0x2C: case 0x2D: case 0x30: case 0x31:
        return argb(255, v >> 8, v >> 8, v >> 8);
    }
    return 0xFFFF00FFu;
}

static void yuv_pair(const uint8_t *g, int uyvy, uint32_t out[2])
{
    int yo = uyvy ? 1 : 0, k;
    int cu = (int)g[1 - yo] - 128, cv = (int)g[3 - yo] - 128;
    for (k = 0; k < 2; k++) {
        int c = (int)g[k * 2 + yo] - 16;
        int r = (298 * c + 409 * cv + 128) >> 8;
        int gg = (298 * c - 100 * cu - 208 * cv + 128) >> 8;
        int b = (298 * c + 516 * cu + 128) >> 8;
        r = r < 0 ? 0 : r > 255 ? 255 : r;
        gg = gg < 0 ? 0 : gg > 255 ? 255 : gg;
        b = b < 0 ? 0 : b > 255 ? 255 : b;
        out[k] = argb(255, (uint32_t)r, (uint32_t)gg, (uint32_t)b);
    }
}

/* Where each level/face lives, and the span to hash. */
static int tex_volume(const NvD3DTexture *t)
{
    return !t->cube && ((t->format >> 4) & 15u) == 3u;
}

typedef struct {
    int kind, bpp, levels, faces;
    uint32_t level_off[16], level_w[16], level_h[16], level_d[16];
    size_t face_stride, total;
} TexLayout;

static int tex_layout(const NvD3DTexture *t, TexLayout *L)
{
    uint32_t w = t->width, h = t->height, lv, maxlv = 1, m;
    uint32_t d = tex_volume(t) ? 1u << (t->format >> 28) : 1u;
    size_t off = 0;
    memset(L, 0, sizeof *L);
    L->bpp = tex_format(t->color, &L->kind);
    if (!L->bpp) {
        texture_refused(t, "unknown_format", 0);
        return 0;
    }
    if (!w || !h || w > 4096 || h > 4096 || (tex_volume(t) && (w > 2048 || h > 2048 || d > 2048 || L->kind == K_LIN))) {
        texture_refused(t, "size_out_of_range", 0);
        return 0;
    }
    L->faces = t->cube ? 6 : 1;
    if (L->kind == K_LIN) {
        if (!t->pitch) {
            texture_refused(t, "linear_pitch_zero", 0);
            return 0;
        }
        L->levels = 1;
        L->level_w[0] = w; L->level_h[0] = h; L->level_d[0] = 1;
        L->total = L->face_stride = (size_t)t->pitch * h;
        L->faces = 1;
        return 1;
    }
    m = w > h ? w : h;
    if (d > m) m = d;
    for (; m > 1; m >>= 1) maxlv++;
    L->levels = t->levels ? (int)t->levels : 1;
    /* Not a refusal: the guest's mip count is a field that can exceed the
     * full chain for its base size (log2 + 1). Levels past 1x1 have no texels,
     * and the offsets below would otherwise run past the real chain into
     * the next face or resource. Trim to the chain the size allows. */
    if (L->levels > (int)maxlv) L->levels = (int)maxlv;
    for (lv = 0; lv < (uint32_t)L->levels; lv++) {
        uint32_t lw = w >> lv ? w >> lv : 1, lh = h >> lv ? h >> lv : 1;
        L->level_off[lv] = (uint32_t)off;
        L->level_w[lv] = lw; L->level_h[lv] = lh;
        L->level_d[lv] = d >> lv ? d >> lv : 1;
        off += (L->kind == K_DXT ? (size_t)((lw + 3) / 4) * ((lh + 3) / 4) * L->bpp
                                 : (size_t)lw * lh * L->bpp) * L->level_d[lv];
    }
    L->face_stride = L->faces > 1 ? (off + 127) & ~(size_t)127 : off;
    L->total = L->face_stride * (L->faces - 1) + off;
    return 1;
}

/* A 4x4 Morton tile is contiguous in guest memory. Copy pairs into its four
 * destination rows, keeping source cache lines hot instead of visiting each
 * image row separately. The masks advance by four texels after removing the
 * two low coordinate bits of each dimension. */
static void decode_swizzled_argb32(const uint8_t *src, uint32_t w, uint32_t h,
                                 uint32_t *out, int opaque)
{
    uint32_t mx, my, ox, oy = 0, x, y, row;
    uint64_t alpha = opaque ? 0xFF000000FF000000ull : 0;
    xbox_swizzle_masks(w, h, &mx, &my);
    mx &= ~15u; my &= ~15u;
    for (y = 0; y < h; y += 4) {
        ox = 0;
        for (x = 0; x < w; x += 4) {
            const uint8_t *tile = src + (size_t)(oy + ox) * 4;
            uint32_t *dst = out + (size_t)y * w + x;
            for (row = 0; row < 4; row++) {
                uint64_t left, right;
                uint32_t offset = (row & 1) * 8 + (row & 2) * 16;
                memcpy(&left, tile + offset, 8);
                memcpy(&right, tile + offset + 16, 8);
                left |= alpha; right |= alpha;
                memcpy(dst + (size_t)row * w, &left, 8);
                memcpy(dst + (size_t)row * w + 2, &right, 8);
            }
            ox = (ox - mx) & mx;
        }
        oy = (oy - my) & my;
    }
}

static void decode_level(const NvD3DTexture *t, const TexLayout *L, const uint8_t *src,
                         int lv, const uint32_t *pal, uint32_t *out)
{
    uint32_t w = L->level_w[lv], h = L->level_h[lv], x, y;
    uint32_t d = L->level_d[lv];
    if (d > 1) {
        uint32_t z;
        if (L->kind == K_DXT) {
            uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bz, bx, by, dz, pixels[16];
            size_t block = 0;
            /* Xbox DXT volumes group four 4x4 slices into each 4x4x4 block.
             * See xemu hw/xbox/nv2a/pgraph/s3tc.c (MIT), s3tc_decompress_3d. */
            for (bz = 0; bz < d; bz += 4)
                for (by = 0; by < bh; ++by)
                    for (bx = 0; bx < bw; ++bx)
                        for (dz = 0; dz < 4 && bz + dz < d; ++dz) {
                            d3d8_dxt_decode_block(src + block++ * L->bpp, t->color, pixels);
                            for (y = 0; y < 4 && by * 4 + y < h; ++y)
                                for (x = 0; x < 4 && bx * 4 + x < w; ++x)
                                    out[((size_t)(bz + dz) * h + by * 4 + y) * w + bx * 4 + x] = pixels[y * 4 + x];
                        }
        } else {
            /* Morton order interleaves only axes which still have a bit. */
            uint32_t masks[3] = {0}, bit, dstbit = 1, axis, sizes[3] = {w,h,d};
            for (bit = 1; bit < w || bit < h || bit < d; bit <<= 1)
                for (axis = 0; axis < 3; ++axis)
                    if (bit < sizes[axis]) { masks[axis] |= dstbit; dstbit <<= 1; }
            for (z = 0; z < d; ++z)
                for (y = 0; y < h; ++y)
                    for (x = 0; x < w; ++x) {
                        uint32_t coords[3] = {x,y,z}, offset = 0;
                        for (axis = 0; axis < 3; ++axis) {
                            uint32_t value = coords[axis];
                            for (bit = 1; bit && bit <= masks[axis]; bit <<= 1)
                                if (masks[axis] & bit) { if (value & 1) offset |= bit; value >>= 1; }
                        }
                        out[((size_t)z * h + y) * w + x] = texel(t->color, src + (size_t)offset * L->bpp, pal, t->palette_len);
                    }
        }
        return;
    }
    if (L->kind == K_DXT) {
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bx, by, px[16];
        if (!(w & 3) && !(h & 3)) {
            for (by = 0; by < bh; by++)
                for (bx = 0; bx < bw; bx++) {
                    uint32_t *dst = out + (size_t)by * 4 * w + bx * 4;
                    d3d8_dxt_decode_block(src + ((size_t)by * bw + bx) * L->bpp, t->color, px);
                    memcpy(dst, px, 16);
                    memcpy(dst + w, px + 4, 16);
                    memcpy(dst + 2 * w, px + 8, 16);
                    memcpy(dst + 3 * w, px + 12, 16);
                }
            return;
        }
        for (by = 0; by < bh; by++)
            for (bx = 0; bx < bw; bx++) {
                d3d8_dxt_decode_block(src + ((size_t)by * bw + bx) * L->bpp, t->color, px);
                for (y = 0; y < 4 && by * 4 + y < h; y++)
                    for (x = 0; x < 4 && bx * 4 + x < w; x++)
                        out[(size_t)(by * 4 + y) * w + bx * 4 + x] = px[y * 4 + x];
            }
        return;
    }
    if ((t->color == 0x06 || t->color == 0x07) && w >= 4 && h >= 4 &&
        !(w & (w - 1)) && !(h & (h - 1))) {
        decode_swizzled_argb32(src, w, h, out, t->color == 0x07);
        return;
    }
    if (L->kind == K_LIN) {
        for (y = 0; y < h; y++) {
            const uint8_t *row = src + (size_t)y * t->pitch;
            uint32_t *dst = out + (size_t)y * w;
            if (t->color == 0x24 || t->color == 0x25) {
                for (x = 0; x + 1 < w; x += 2)
                    yuv_pair(row + x * 2, t->color == 0x25, dst + x);
            } else if (t->color == 0x12) {
                memcpy(dst, row, (size_t)w * 4);
            } else {
                for (x = 0; x < w; x++)
                    dst[x] = texel(t->color, row + (size_t)x * L->bpp, pal, t->palette_len);
            }
        }
        return;
    }
    {
        /* Swizzled: undo the Morton order a texel at a time. */
        uint32_t mx, my, oy = 0;
        xbox_swizzle_masks(w, h, &mx, &my);
        for (y = 0; y < h; y++) {
            uint32_t ox = 0, *dst = out + (size_t)y * w;
            for (x = 0; x < w; x++) {
                dst[x] = texel(t->color, src + (size_t)(oy + ox) * L->bpp, pal, t->palette_len);
                ox = (ox - mx) & mx;
            }
            oy = (oy - my) & my;
        }
    }
}

/* One mip level below src: a 2x2 box (a dimension already 1 stays 1).
 * src == dst is safe: each output is written only after reading its complete
 * source footprint, and precedes the next footprint in forward row order. */
static void halve_bgra(const uint32_t *src, uint32_t w, uint32_t h, uint32_t *dst)
{
    uint32_t nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1, x, y, c;
    for (y = 0; y < nh; y++)
        for (x = 0; x < nw; x++) {
            uint32_t x0 = w > 1 ? 2 * x : x, y0 = h > 1 ? 2 * y : y;
            uint32_t x1 = w > 1 ? x0 + 1 : x0, y1 = h > 1 ? y0 + 1 : y0, out = 0;
            for (c = 0; c < 32; c += 8)
                out |= ((((src[y0 * w + x0] >> c) & 255) + ((src[y0 * w + x1] >> c) & 255) +
                         ((src[y1 * w + x0] >> c) & 255) + ((src[y1 * w + x1] >> c) & 255) + 2) / 4) << c;
            dst[y * nw + x] = out;
        }
}

/* Mip levels the host texture gets. Titles shipped short chains to save
 * memory (for example, 1024x1024 with 4 levels), so distant surfaces kept sampling
 * a level far finer than their footprint -- grain and shimmer, with a band
 * where the chain runs out. A chain the title started is completed down to
 * 1x1 from its own smallest level; a single level stays single. */
static int host_levels(const TexLayout *L, const NvD3DTexture *t)
{
    int n = 1;
    uint32_t m = t->width > t->height ? t->width : t->height;
    if (tex_volume(t) || L->levels < 2) return L->levels;
    while (m > 1) { m >>= 1; n++; }
    return n > L->levels ? n : L->levels;
}

/* ------------------------------------------------------------------- draw */

static uint32_t mode_needs_texture(uint32_t mode)
{
    return mode == 1 || mode == 2 || mode == 3 || mode == 6 || mode == 7 ||
           mode == 9 || mode == 11 || mode == 12 || mode == 13 || mode == 14 ||
           mode == 15 || mode == 16 || mode == 18;
}

static void unpack4(uint32_t c, float o[4])
{
    o[0] = ((c >> 16) & 0xFF) / 255.0f;
    o[1] = ((c >> 8) & 0xFF) / 255.0f;
    o[2] = (c & 0xFF) / 255.0f;
    o[3] = (c >> 24) / 255.0f;
}

/* An index list into the ring, rebased by sub. Flat shading takes colours
 * from a primitive's last vertex on the NV2A (GL's provoking vertex) but its
 * first on D3D11, so flat primitives are rotated to lead with their last
 * vertex; rotating a triangle keeps its winding. */
static void copy_indices(uint32_t *o, const uint32_t *idx, uint32_t ni, uint32_t sub,
                         int topology, int flat)
{
    uint32_t i;
    if (flat && topology == NV_D3D_TRIANGLES) {
        for (i = 0; i + 2 < ni; i += 3) {
            o[i] = idx[i + 2] - sub; o[i + 1] = idx[i] - sub; o[i + 2] = idx[i + 1] - sub;
        }
    } else if (flat && topology == NV_D3D_LINES) {
        for (i = 0; i + 1 < ni; i += 2) {
            o[i] = idx[i + 1] - sub; o[i + 1] = idx[i] - sub;
        }
    } else {
        for (i = 0; i < ni; i++) o[i] = idx[i] - sub;
    }
}

/* Bytes one element of an NV2A array format occupies; 0 if unknown. */
static uint32_t attr_bytes(uint32_t type, uint32_t size)
{
    if (!size || size > 4) return 0;
    switch (type) {
    case 0: case 6: return 4;                        /* D3DCOLOR, 11:11:10 */
    case 2: return 4 * size;                         /* float */
    case 4: return size;                             /* ubyte */
    case 1: case 5: return 2 * size;                 /* short */
    default: return 0;
    }
}

#endif
