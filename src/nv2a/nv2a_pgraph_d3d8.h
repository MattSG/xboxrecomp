/*
 * NV2A PGRAPH → D3D8 Translator
 *
 * Intercepts NV2A push buffer method calls and translates them into
 * D3D8 on SDL_GPU rendering commands. This is the core of the GPU translation
 * layer for Xbox static recompilation.
 *
 * The push buffer contains NV2A Kelvin (NV097) methods:
 *   - Surface/viewport setup → D3D8 render target + viewport
 *   - Render state (blend, depth, cull) → D3D8 render states
 *   - Begin/End draw + Inline vertex data → D3D8 DrawPrimitiveUP
 *   - Texture binding → D3D8 SetTexture
 *   - Clear commands → D3D8 Clear
 *
 * Vertex formats observed in menus:
 *   5 dwords per vertex: float X, float Y, float U, float V, D3DCOLOR
 *   Drawn as TRIANGLE_STRIP (mode 6)
 *
 * This module is designed to be reusable across Xbox recompilation projects.
 * See: https://github.com/sp00nznet/xboxrecomp
 */

#ifndef NV2A_PGRAPH_D3D8_H
#define NV2A_PGRAPH_D3D8_H

#include <stdint.h>

/* Initialize the PGRAPH→D3D8 translator. Call after GPU device is created. */
void pgraph_d3d8_init(void);

/* Shut down and release resources. */
void pgraph_d3d8_shutdown(void);

/* Process an NV2A PGRAPH method call. Called from push buffer parser.
 * Returns 1 if handled, 0 if unhandled (caller should log/ignore). */
int pgraph_d3d8_method(int subchannel, uint32_t method, uint32_t param);

/* Flush any pending draw commands (call at end of frame). */
void pgraph_d3d8_flush(void);

/* Set chyron scroll: pass frame counter to animate, 0 to disable.
 * Applies horizontal scroll offset to vertices in the chyron Y band. */
void pgraph_d3d8_set_chyron_scroll(uint32_t frame);

/* Statistics */
typedef struct {
    uint32_t frames;
    uint32_t draw_calls;
    uint32_t vertices_submitted;
    uint32_t methods_handled;
    uint32_t methods_ignored;
    uint32_t clears;
} PgraphD3D8Stats;

void pgraph_d3d8_get_stats(PgraphD3D8Stats *out);

#endif /* NV2A_PGRAPH_D3D8_H */
