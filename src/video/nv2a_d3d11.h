/**
 * D3D11 renderer for the NV2A pushbuffer executor.
 *
 * The executor (src/kernel/nv2a_pb_exec.c) decodes the title's GPU command
 * stream and runs its vertex programs; this turns the result into D3D11 draws.
 * Rasterisation, register combiners, texturing, blending, depth and stencil
 * all run on the host GPU. Render targets live in host textures keyed by the
 * physical address the title gave them, so a surface the title later samples
 * as a texture (its own render-to-texture passes) is read back from the GPU
 * rather than from guest RAM, which the GPU never writes.
 *
 * Everything here runs on the one thread that executes the pushbuffer.
 */
#ifndef XBOXRECOMP_NV2A_D3D11_H
#define XBOXRECOMP_NV2A_D3D11_H

#include <stdint.h>
#include "../kernel/nv2a_combiner.h"

/* One vertex as the GPU sees it: oPos already in screen space (x, y in guest
 * pixels, z in depth-buffer units) with the clip-space w kept in pos[3], as
 * the Xbox D3D epilogue leaves it. fog is the evaluated fog factor. */
typedef struct {
    float pos[4];
    float d0[4], d1[4];
    float fog[4];
    float tex[4][4];
} NvD3DVertex;

typedef struct {
    uint32_t addr;          /* guest VA of texel (0,0), resolved */
    uint32_t format;        /* raw SET_TEXTURE_FORMAT */
    uint32_t color;         /* colour-format code */
    uint32_t width, height; /* texels */
    uint32_t pitch;         /* linear formats */
    uint32_t levels;
    uint32_t cube;
    uint32_t address;       /* SET_TEXTURE_ADDRESS */
    uint32_t control0;      /* SET_TEXTURE_CONTROL0 */
    uint32_t filter;        /* SET_TEXTURE_FILTER */
    uint32_t palette;       /* guest VA of the CLUT, 0 if none */
    uint32_t palette_len;   /* entries */
    uint32_t border;        /* SET_TEXTURE_BORDER_COLOR */
    float    bump_mat[4];   /* SET_TEXTURE_SET_BUMP_ENV_MAT */
    float    bump_scale, bump_offset;
} NvD3DTexture;

typedef struct {
    /* Surface. Addresses are resolved guest VAs; 0 = none. */
    uint32_t color_addr, zeta_addr;
    uint32_t color_pitch, zeta_pitch;
    uint32_t surface_format;            /* raw SET_SURFACE_FORMAT */
    uint32_t clip_x, clip_y, clip_w, clip_h;
    /* Output merger. */
    uint32_t blend_enable, blend_sfactor, blend_dfactor, blend_equation;
    uint32_t blend_color, color_mask;
    uint32_t depth_test, depth_func, depth_mask;
    uint32_t stencil_test, stencil_func, stencil_ref, stencil_func_mask;
    uint32_t stencil_write_mask, stencil_fail, stencil_zfail, stencil_zpass;
    /* Rasteriser. */
    uint32_t cull_enable, cull_face, front_face;
    uint32_t poly_offset_fill;
    float    poly_offset_factor, poly_offset_units;
    uint32_t flat_shade;
    /* Pixel pipeline. */
    Nv2aCombiner rc;
    uint32_t rc_seen;
    uint32_t clip_plane_mode, other_stage_input, dot_rgb_mapping;
    uint32_t alpha_test, alpha_func, alpha_ref;
    uint32_t fog_color;                 /* SET_FOG_COLOR: R in bits 0-7 */
    uint32_t fog_enable, fog_mode;
    float    fog_param[2];
    uint32_t tex_used;                  /* stages the program samples */
    NvD3DTexture tex[4];
} NvD3DState;

enum { NV_D3D_TRIANGLES = 0, NV_D3D_LINES = 1, NV_D3D_POINTS = 2 };

/* One vertex attribute array, for the GPU vertex-program path. */
typedef struct {
    uint32_t enabled;        /* fetched from an array (else DEF value) */
    uint32_t type, size, stride;
    const uint8_t *data;     /* host address of element 0 */
} NvD3DAttrib;

/* Run the title's current vertex program on the GPU over guest indices idx
 * (all within [lo, hi]). Returns 0 if this path cannot take the batch, and
 * the caller transforms it on the CPU instead. */
int nv2a_d3d_draw_vsh(const NvD3DState *st, int topology,
                      const NvD3DAttrib attr[16], const float def[16][4],
                      const uint32_t *idx, uint32_t ni, uint32_t lo, uint32_t hi);

/* Create the device (no window needed). 0 if D3D11 is unavailable. */
int  nv2a_d3d_init(void);
int  nv2a_d3d_active(void);

/* Width / height the image is presented at (widescreen). */
float nv2a_d3d_display_aspect(void);
/* Called by the title as its race camera, or a frontend camera, sets up its
 * projection; the race HUD is placed at the screen edges in between. */
void nv2a_d3d_note_race_camera(void);
void nv2a_d3d_note_frontend_camera(void);
/* The race HUD's first element, as its 4:3 screen rectangle; 2D drawn ahead
 * of it in a race frame is world-anchored (name tags over cars). */
void nv2a_d3d_set_hud_start(float x0, float y0, float x1, float y1);

void nv2a_d3d_draw(const NvD3DState *st, int topology,
                   const NvD3DVertex *v, uint32_t nv,
                   const uint32_t *idx, uint32_t ni);

/* NV097_CLEAR_SURFACE: flags as the method's parameter, rect inclusive. */
void nv2a_d3d_clear(const NvD3DState *st, uint32_t flags, uint32_t color,
                    uint32_t zstencil, uint32_t x0, uint32_t y0,
                    uint32_t x1, uint32_t y1);

/* The title finished a frame in this surface: put it on screen. */
void nv2a_d3d_flip(uint32_t surface_addr, uint32_t pitch);

/* A new pushbuffer segment is about to execute. */
void     nv2a_d3d_kick(void);

/* Visibility tests (NV097 zpass pixel counting). */
void     nv2a_d3d_zpass_enable(int on);
void     nv2a_d3d_zpass_clear(void);
uint32_t nv2a_d3d_zpass_read(void);

/* RECOMP_D3D_PROFILE stage accounting (slot 0 = vertex processing). */
int  nv2a_d3d_prof_enabled(void);
void nv2a_d3d_prof_add(int slot, int64_t ticks, uint32_t verts);

/* Called continuously by the pushbuffer thread. Presents what the display is
 * scanning when the title is not flipping (loading screens, movies). */
void nv2a_d3d_tick(void);

#endif
