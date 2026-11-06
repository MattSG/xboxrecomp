/* The functions an embedding project provides when it builds the runtime with
 * XBOXRECOMP_RENDERER=EXTERNAL and XBOXRECOMP_APU_OUTPUT=EXTERNAL, as no-ops.
 * Their prototypes come from the contract headers, so a signature that
 * drifts breaks this file's compile rather than a downstream link. */
#include <stddef.h>
#include <stdint.h>

#include "apu_xaudio2.h"   /* xa2_*: the audio device */
#include "fb_window.h"     /* fbw_*, xbox_Framebuffer*: the window */
#include "nv2a_d3d11.h"    /* nv2a_d3d_*: the renderer */

/* The recompiled title's indirect-call tables (generated code in a real
 * project). */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

/* Audio device. */
int xa2_init(void) { return 0; }
void xa2_shutdown(void) {}
int xa2_is_active(void) { return 0; }
int xa2_submit_samples(const int16_t *s, int n) { (void)s; (void)n; return 0; }
int xa2_get_buffer_size(void) { return 1024; }
int xa2_get_buffer_count(void) { return 8; }
int xa2_queued_samples(void) { return 0; }
void xa2_expect_drain(void) {}
unsigned xa2_underruns(void) { return 0; }
uint64_t xa2_samples_played(void) { return 0; }

/* Window. */
void *fbw_native_window(void) { return NULL; }
void xbox_FramebufferWindowStart(void) {}

/* Renderer. */
void nv2a_d3d_draw_ub(const NvD3DState *st, int topology, const NvD3DAttrib attr[16],
                      const float def[16][4], int program, const uint32_t *idx,
                      uint32_t ni, uint32_t lo, uint32_t hi, const float (*pos)[4])
{ (void)st; (void)topology; (void)attr; (void)def; (void)program; (void)idx;
  (void)ni; (void)lo; (void)hi; (void)pos; }
int nv2a_d3d_widescreen(void) { return 0; }
int nv2a_d3d_init(void) { return 0; }
float nv2a_d3d_display_aspect(void) { return 4.0f / 3.0f; }
void nv2a_d3d_clear(const NvD3DState *st, uint32_t flags, uint32_t color, uint32_t zs,
                    uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{ (void)st; (void)flags; (void)color; (void)zs; (void)x0; (void)y0; (void)x1; (void)y1; }
void nv2a_d3d_flip_present(uint32_t a, uint32_t p, int show) { (void)a; (void)p; (void)show; }
void nv2a_d3d_kick(void) {}
void nv2a_d3d_zpass_enable(int on) { (void)on; }
void nv2a_d3d_zpass_clear(void) {}
uint32_t nv2a_d3d_zpass_read(void) { return 0; }
int nv2a_d3d_prof_enabled(void) { return 0; }
void nv2a_d3d_prof_add(int slot, int64_t ticks, uint32_t verts) { (void)slot; (void)ticks; (void)verts; }
int nv2a_d3d_writeback_busy(void) { return 0; }
void nv2a_d3d_tick(void) {}
