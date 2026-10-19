/**
 * D3D11 renderer for the NV2A pushbuffer executor.
 *
 * The executor (src/kernel/nv2a_pb_exec.c) decodes the title's GPU command
 * stream and supplies vertex arrays, indices and state for D3D11 draws. A
 * host vertex shader interprets the title's NV2A vertex programs.
 * Rasterisation, register combiners, texturing, blending, depth and stencil
 * all run on the host GPU. Render targets live in host textures keyed by the
 * physical address the title gave them. Render-to-texture passes sample these
 * host resources. CPU-assisted synchronization merges guest RAM writes into
 * surfaces and copies completed renderings back to guest RAM at flips.
 *
 * Everything here runs on the one thread that executes the pushbuffer.
 */
#ifndef XBOXRECOMP_NV2A_D3D11_H
#define XBOXRECOMP_NV2A_D3D11_H

#include <stdint.h>
#include "../kernel/nv2a_combiner.h"

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
    /* Window clip: SET_WINDOW_CLIP_TYPE (bit 0 exclusive) and the regions
     * the title set (mask bit n: region n), each min | max << 16. */
    uint32_t wclip_type, wclip_mask, wclip_h[8], wclip_v[8];
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
    uint32_t z_perspective;             /* SET_CONTROL0: w-buffering */
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

/* One vertex attribute array as the title set it up. */
typedef struct {
    uint32_t enabled;        /* fetched from an array (else the def value) */
    uint32_t type, size, stride;
    const uint8_t *data;     /* host address of element 0; NULL: unreadable */
} NvD3DAttrib;

/* Draw guest indices idx (all within [lo, hi]) entirely on the GPU: the
 * title's current vertex program (program != 0) or pre-transformed vertices
 * (position, diffuse 3, specular 4, texcoords 9-12). def holds the values of
 * attributes not fetched from an array. pos, when given, is each vertex's
 * screen position by index - lo, for widescreen placement of 2D overlays
 * (only consulted when the target is widened). */
void nv2a_d3d_draw_ub(const NvD3DState *st, int topology,
                      const NvD3DAttrib attr[16], const float def[16][4], int program,
                      const uint32_t *idx, uint32_t ni, uint32_t lo, uint32_t hi,
                      const float (*pos)[4]);
/* Whether display-sized surfaces are widened (pos is worth computing). */
int nv2a_d3d_widescreen(void);
/* Menu pointing: while on, each frame's textured screen-space quads are
 * recorded (pos is then worth computing too). */
typedef struct {
    float x0, y0, x1, y1;   /* guest pixels */
    uint32_t tex;           /* stage 0 texture address */
    uint32_t argb;          /* first vertex's colour out of the vertex stage */
} NvMenuRect;
void nv2a_d3d_collect_menu_rects(int on);
int nv2a_d3d_menu_rects_on(void);
/* The last shown frame's quads (up to max), and where it landed in the
 * window: x0, y0, w, h as fractions of the client area. */
int nv2a_d3d_menu_rects(NvMenuRect *out, int max, float present[4]);
/* Sample stage 0 with a cubic B-spline in depth-test-off draws blended with
 * this source/destination factor pair (a title's magnified sky); 0, 0: off. */
void nv2a_d3d_set_bspline_blend(uint32_t sfactor, uint32_t dfactor);
/* Each vertex's final colour (by index - lo) for the next draw, or NULL. */
void nv2a_d3d_set_menu_tints(const uint32_t *tints);

/* Create the device (no window needed). 0 if D3D11 is unavailable. */
int  nv2a_d3d_init(void);
int  nv2a_d3d_active(void);

/* Width / height the image is presented at (widescreen). */
float nv2a_d3d_display_aspect(void);
/* Screen-space (w = 1) 2D on a widened target spans the display. A title's
 * hooks can place it otherwise: place() may set *squeeze (the target's 4:3
 * width fraction keeps 4:3 proportions about the centre), *shift (moves it
 * that far, in squeezed widths, toward an edge), or fill dx (guest pixels
 * per vertex, indexed by vertex - lo; NULL when unavailable) and return 1. */
typedef struct {
    const float (*pos)[4];  /* screen position by vertex - lo */
    const uint32_t *idx;
    const NvD3DAttrib *attrib; /* 16 original guest arrays; read only. */
    uint32_t ni, lo, count; /* indices; vertices lo .. lo + count - 1 */
    uint32_t per;           /* indices per primitive: 3, 2 or 1 */
    float gw, gh;           /* target size in guest pixels */
    float narrow;           /* 4:3 width over the target's width */
    float x0, y0, x1, y1;   /* the draw's bounds */
    uint32_t tex;           /* stage 0 texture address */
    int textured, reads_wide;   /* samples a texture; samples a widened target */
} NvOverlayDraw;
typedef struct {
    void (*frame)(void);    /* at each flip, before the next frame's draws */
    int (*place)(const NvOverlayDraw *d, float *dx, float *squeeze, float *shift);
    int (*hold_aspect)(void);   /* nonzero: defer an aspect change */
    /* Optional title metadata cleanup on the private GPU copy. `copy` begins
     * at element lo of this attribute and follows attrib->stride. Called for
     * each readable, enabled array, even without screen positions or a wide
     * target. Do not change guest RAM or write outside the copied attribute. */
    void (*sanitize_vertex_upload)(uint8_t *copy, const NvD3DAttrib *attrib,
                                   unsigned attribute, uint32_t lo, uint32_t count);
} NvOverlayHooks;
void nv2a_d3d_set_overlay_hooks(const NvOverlayHooks *hooks);
/* Opt in lamp visibility sprites to scene depth, without depth writes:
 * `stage_program` is the flare shader's combiner stage program (its texture
 * modes), 0 to turn it off. */
void nv2a_d3d_set_flare_depth_test(uint32_t stage_program);
/* A title that multiplies its far plane by `scale` (w-buffered, so depth is
 * w / far) still indexes its authored depth lookups (fog ramps) with the
 * original fractions. Depth read back as a texture is reported in those:
 * unchanged in world distance up to `knee` of the original far plane, then
 * stretched so the rest of the authored ramp ends at the new far plane.
 * scale 1 (the default) leaves depth untouched. */
void nv2a_d3d_set_depth_view_remap(float scale, float knee);

/* Optional observer for consecutive NV097_NO_OPERATION parameters, called on
 * the executor thread in stream order. A NULL state ends an interrupted run.
 * Return nonzero only for consumed extension words. Other NO_OPERATION
 * commands retain native notification behavior, including with no hook. */
void nv2a_pb_set_nop_hook(int (*hook)(const NvD3DState *, uint32_t subchannel, uint32_t value));

/* Draw depth-based fog on the executor thread; the caller owns ordering,
 * distance bands, colours, and the optional world-up fade plane. */
#define NV_D3D_FOG_SKY_MASK 1u
#define NV_D3D_FOG_SKY_FILL 2u
#define NV_D3D_FOG_SKY_FADE 4u
void nv2a_d3d_distance_fog(const NvD3DState *st, float depth_to_world, float far_clip,
                         float start, float end, uint32_t argb, uint32_t flags, const float sky_fade[4]);

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
/* A flip's surfaces are still being read back into guest memory: DMA_GET and
 * fence releases must wait. Completes whatever the GPU has finished. */
int  nv2a_d3d_writeback_busy(void);
/* spike_sampler.c: the thread whose time a spike report breaks down. */
void recomp_sampler_watch_current_thread(void);
void recomp_sampler_report(double ms);
void nv2a_d3d_prof_add(int slot, int64_t ticks, uint32_t verts);

/* Called continuously by the pushbuffer thread. Presents what the display is
 * scanning when the title is not flipping (loading screens, movies). */
void nv2a_d3d_tick(void);

/* Shows the last flipped frame again, paced like a flip, and returns once it
 * is shown: a title thread's frame with nothing new to draw. */
void nv2a_d3d_repeat_frame(void);

/* A frame between the title's own: the executor's last recorded frame with
 * its vertex-program constants part way (t) from the frame before. need_new:
 * wait for the frame the title has just drawn. Returns once shown. */
void nv2a_d3d_interp_frame(float t, int need_new);

#endif
