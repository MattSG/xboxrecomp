/**
 * D3D11 renderer for the NV2A pushbuffer executor. See nv2a_d3d11.h.
 *
 * Pieces, in the order a frame meets them:
 *   surfaces  -- render targets and depth buffers, keyed by physical address
 *   textures  -- guest texture memory decoded to BGRA, cached by content hash
 *   draw      -- one uber pixel shader evaluates the register combiners and
 *                texture-stage modes from a constant buffer
 *   present   -- the finished surface (plus the PVIDEO overlay) scaled into
 *                the window's flip-model swap chain
 *
 * The CPU and the GPU share guest RAM on the Xbox; here the GPU's results stay
 * in host textures. Two consequences are handled explicitly:
 *   - a surface sampled as a texture is read from its render target;
 *   - a surface the CPU wrote (loading screens) is detected by hashing its
 *     guest memory and uploaded before it is drawn over or shown.
 */
#if defined(_WIN32)
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_d3d11.h"
#include "nv2a_shader_cache.h"
#include "nv2a_pacing.h"
#include "fb_present.h"
#include "platform/recomp_profile.h"
#include "../d3d/d3d8_swizzle.h"

/* Run under RenderDoc (renderdoccmd capture ...), creating rdc.flag in the
 * working directory captures the next frame of this renderer's device -- the
 * process has a second D3D device, so RenderDoc's own hotkey may pick the
 * wrong one. RENDERDOC_API_1_1_2 is a table of function pointers; the two
 * used are entries 19 (StartFrameCapture) and 21 (EndFrameCapture). */
static ID3D11Device *s_dev;
static void rdoc_poll(void)
{
    typedef int (__cdecl *GetApi)(int version, void **out);
    typedef void (__cdecl *Start)(void *dev, void *wnd);
    typedef unsigned (__cdecl *End)(void *dev, void *wnd);
    static void **api;
    static int tried, capturing;
    if (!tried) {
        HMODULE m = GetModuleHandleA("renderdoc.dll");
        GetApi get = m ? (GetApi)GetProcAddress(m, "RENDERDOC_GetAPI") : NULL;
        tried = 1;
        if (get && !get(10102, (void **)&api)) api = NULL;
    }
    if (!api) return;
    if (capturing) {
        capturing = 0;
        fprintf(stderr, "[D3D11] RenderDoc capture %s\n",
                ((End)api[21])(s_dev, NULL) ? "written" : "failed");
    } else if (GetFileAttributesA("rdc.flag") != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA("rdc.flag");
        ((Start)api[19])(s_dev, NULL);
        capturing = 1;
    }
}

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define CONTIG_BASE 0x80000000u
#define CONTIG_SIZE 0x04000000u

static ID3D11Device        *s_dev;
static ID3D11DeviceContext *s_ctx;
static IDXGISwapChain1     *s_swap;
static HWND                 s_swap_hwnd;
static UINT                 s_swap_w, s_swap_h;
static int                  s_init_state;      /* 0 untried, 1 ok, -1 failed */

/* RECOMP_D3D_PROFILE: per-frame cost of each stage, printed once a second. */
enum { PROF_VERTEX, PROF_TEXTURE, PROF_DRAW, PROF_SYNC, PROF_WRITEBACK,
       PROF_PRESENT, PROF_PACE, PROF_EXEC, PROF_COUNT };
static const char *const s_prof_name[PROF_COUNT] = {
    "vertex", "texture", "draw", "cpu-sync", "writeback", "present", "pace", "exec(all)" };
static int64_t s_prof[PROF_COUNT];
static uint64_t s_prof_verts, s_prof_draws;
static uint64_t s_prof_ps_uploads, s_prof_ps_reused;
static int s_prof_on = -1;

static int64_t qpc(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

/* Opt-in timing only. Keep the default path free of clocks, environment
 * lookups and output; the shader workers share this published configuration. */
static INIT_ONCE s_hitch_once = INIT_ONCE_STATIC_INIT;
static volatile LONG s_hitch_ready;
static int s_hitch_trace;
static LARGE_INTEGER s_hitch_freq;

static BOOL CALLBACK hitch_trace_initialize(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    const char *e = getenv("RECOMP_HITCH_TRACE");
    (void)once; (void)param; (void)ctx;
    s_hitch_trace = e && *e && *e != '0';
    if (s_hitch_trace && (!QueryPerformanceFrequency(&s_hitch_freq) ||
                         s_hitch_freq.QuadPart <= 0))
        s_hitch_trace = 0;
    InterlockedExchange(&s_hitch_ready, 1);
    return TRUE;
}

static void hitch_trace_init(void)
{
    if (!s_hitch_ready)
        InitOnceExecuteOnce(&s_hitch_once, hitch_trace_initialize, NULL, NULL);
}

static int64_t hitch_trace_begin(void)
{
    hitch_trace_init();
    return s_hitch_trace ? qpc() : 0;
}

/* Return the ending timestamp only for a phase that exceeded eight ms. */
static int64_t hitch_trace_elapsed(int64_t begin)
{
    int64_t now;
    if (!begin) return 0;
    now = qpc();
    return now - begin > s_hitch_freq.QuadPart / 125 ? now : 0;
}

static void hitch_trace_report(const char *phase, int64_t begin)
{
    int64_t end = hitch_trace_elapsed(begin);
    if (!end) return;
    fprintf(stderr, "[HITCH] phase=%s qpc_ms=%.3f start_qpc_ms=%.3f "
                    "duration_ms=%.3f thread=%lu\n", phase,
            end * 1000.0 / s_hitch_freq.QuadPart,
            begin * 1000.0 / s_hitch_freq.QuadPart,
            (end - begin) * 1000.0 / s_hitch_freq.QuadPart,
            GetCurrentThreadId());
}

void nv2a_d3d_prof_add(int slot, int64_t ticks, uint32_t verts)
{
    s_prof[slot] += ticks;
    s_prof_verts += verts;
}

/* The stage timers also run for RECOMP_SPIKE_CAPTURE, which reports each
 * spike's split; the once-a-second line is RECOMP_D3D_PROFILE's alone. */
int nv2a_d3d_prof_enabled(void)
{
    if (s_prof_on < 0) s_prof_on = getenv("RECOMP_D3D_PROFILE") != NULL || getenv("RECOMP_SPIKE_CAPTURE") != NULL;
    return s_prof_on;
}

static void prof_report(void)
{
    static int64_t last, frames, prev, worst;
    static LARGE_INTEGER freq;
    int64_t now = qpc();
    int i;
    static int print = -1;
    if (!nv2a_d3d_prof_enabled()) return;
    if (print < 0) print = getenv("RECOMP_D3D_PROFILE") != NULL;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    frames++;
    if (prev && now - prev > worst) worst = now - prev;
    prev = now;
    if (!last) { last = now; return; }
    if (now - last < freq.QuadPart) return;
    if (!print) goto reset;
    fprintf(stderr, "[D3D11_PROF] %.1f fps (worst %.1f ms) |",
            frames * (double)freq.QuadPart / (double)(now - last),
            worst * 1000.0 / (double)freq.QuadPart);
    for (i = 0; i < PROF_COUNT; i++)
        fprintf(stderr, " %s %.2f", s_prof_name[i],
                s_prof[i] * 1000.0 / (double)freq.QuadPart / (double)frames);
    fprintf(stderr, " ms/frame | %llu verts %llu draws/frame | ps-constants %llu uploaded %llu reused/frame\n",
            (unsigned long long)(s_prof_verts / frames), (unsigned long long)(s_prof_draws / frames),
            (unsigned long long)(s_prof_ps_uploads / frames), (unsigned long long)(s_prof_ps_reused / frames));
    fflush(stderr);
reset:
    memset(s_prof, 0, sizeof s_prof);
    s_prof_verts = s_prof_draws = 0;
    s_prof_ps_uploads = s_prof_ps_reused = 0;
    frames = 0;
    worst = 0;
    last = now;
}
static void spike_check(int64_t interval, int64_t pace);
/* RECOMP_FRAME_CSV=<path>: one row per flip. The original columns measure
 * guest flip completion; the appended columns measure successful host
 * Present returns for those guest flips. Movie-only tick presentations do
 * not contribute timestamps. These are submission, not displayed-frame,
 * timestamps. Keep the additional clock read opt-in with the CSV itself. */
static FILE *s_frame_csv_file;
static int s_frame_csv_tried;
static LARGE_INTEGER s_frame_csv_freq;
static int64_t s_frame_csv_present, s_frame_csv_present_prev;
static int64_t s_frame_csv_present_interval;

static void frame_csv_init(void)
{
    if (!s_frame_csv_tried) {
        const char *e = getenv("RECOMP_FRAME_CSV");
        s_frame_csv_tried = 1;
        if (e && (s_frame_csv_file = fopen(e, "w")) != NULL) {
            setvbuf(s_frame_csv_file, NULL, _IOFBF, 1 << 16);
            fprintf(s_frame_csv_file,
                    "qpc_ms,frame_ms,pace_ms,present_qpc_ms,present_ms\n");
            QueryPerformanceFrequency(&s_frame_csv_freq);
        }
    }
}

static void frame_csv_present(int64_t now, HRESULT hr)
{
    if (!SUCCEEDED(hr)) return;
    s_frame_csv_present_interval = s_frame_csv_present_prev
        ? now - s_frame_csv_present_prev : 0;
    s_frame_csv_present_prev = s_frame_csv_present = now;
}

static void frame_csv(int64_t pace)
{
    static int64_t prev;
    static unsigned rows;
    int64_t now;
    double ms;
    frame_csv_init();
    now = qpc();
    spike_check(prev ? now - prev : 0, pace);
    if (!s_frame_csv_file) { prev = now; return; }
    ms = 1000.0 / s_frame_csv_freq.QuadPart;
    if (prev) {
        fprintf(s_frame_csv_file, "%.3f,%.4f,%.4f,", now * ms,
                (now - prev) * ms, pace * ms);
        if (s_frame_csv_present) {
            fprintf(s_frame_csv_file, "%.3f,", s_frame_csv_present * ms);
            if (s_frame_csv_present_interval)
                fprintf(s_frame_csv_file, "%.4f", s_frame_csv_present_interval * ms);
        } else {
            fputc(',', s_frame_csv_file);
        }
        fputc('\n', s_frame_csv_file);
    }
    if (++rows % 120 == 0) fflush(s_frame_csv_file); /* harnesses kill the process */
    prev = now;
    s_frame_csv_present = 0;
}

static uint32_t             s_frame = 1;
/* Bumped at every pushbuffer kick: the CPU can only have written guest memory
 * between kicks, so a surface needs checking at most once per kick. */
static uint32_t             s_epoch = 1;

void nv2a_d3d_kick(void)
{
    s_epoch++;
}

/* Internal resolution relative to the guest's: s_scale vertically, and
 * horizontally too except on display-sized surfaces, which are also widened
 * to the display aspect (see nv2a_d3d_display_aspect). s_cur_sx/sy are the
 * bound target's. */
static float s_scale = 1.0f, s_cur_sx = 1.0f, s_cur_sy = 1.0f;
/* Host MSAA sample count for linear render targets and the anisotropy of
 * linearly filtered textures: RECOMP_MSAA (1/2/4/8), RECOMP_ANISO (1..16). */
static UINT s_msaa = 4, s_aniso = 16;
#define GUEST_W 640u
#define GUEST_H 480u

/* A title's placement of screen-space 2D on a widened target (header). */
static NvOverlayHooks s_overlay;

void nv2a_d3d_set_overlay_hooks(const NvOverlayHooks *hooks)
{
    memset(&s_overlay, 0, sizeof s_overlay);
    if (hooks) s_overlay = *hooks;
}

/* nv2a_d3d_set_depth_view_remap: x = scale, y = knee (see y16d_ps). */
static float s_zview_remap[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
static int s_depth_hazed;               /* depth read as Y16 this frame */

void nv2a_d3d_set_depth_view_remap(float scale, float knee)
{
    if (!(scale >= 1.0f) || !(knee > 0.0f && knee <= 1.0f)) { scale = 1.0f; knee = 1.0f; }
    s_zview_remap[1] = knee;
    s_zview_remap[0] = scale;            /* views rebuild on the next depth bind */
}

/* The aspect the image is rendered at, never narrower than the title's
 * 4:3: RECOMP_ASPECT (e.g. 16:9, 21:9, 1.6, 4:3), else the game window's
 * client area, following it as the window is resized (see display_config).
 * The title widens its projection to match. */
static volatile float s_aspect;     /* 0 until decided */

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

/* The window's client size; 0 if there is no window or it is minimised. */
static int client_size(UINT *w, UINT *h)
{
    HWND hw = xbox_FramebufferWindowHandle();
    RECT rc;
    if (!hw || !GetClientRect(hw, &rc) || rc.right <= rc.left || rc.bottom <= rc.top)
        return 0;
    *w = (UINT)(rc.right - rc.left);
    *h = (UINT)(rc.bottom - rc.top);
    return 1;
}

float nv2a_d3d_display_aspect(void)
{
    float a = s_aspect;
    UINT w, h;
    if (a) return a;
    if ((a = env_aspect()) != 0) return s_aspect = a;
    if (client_size(&w, &h)) return s_aspect = clamp_aspect((float)w / (float)h);
    {
        /* No window yet (sizing it): the monitor it will open on. */
        MONITORINFO mi = { sizeof mi };
        if (GetMonitorInfoA(MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY), &mi) &&
            mi.rcMonitor.bottom > mi.rcMonitor.top)
            return clamp_aspect((float)(mi.rcMonitor.right - mi.rcMonitor.left) /
                                (float)(mi.rcMonitor.bottom - mi.rcMonitor.top));
    }
    return 4.0f / 3.0f;
}

/* How much wider than 4:3 a display-sized surface is rendered. */
static float widen(void)
{
    return nv2a_d3d_display_aspect() * 3.0f / 4.0f;
}

static ID3D11VertexShader *s_ub_vs, *s_present_vs, *s_clear_vs;
static ID3D11PixelShader  *s_psv[4], *s_present_ps, *s_clear_ps;
static ID3D11VertexShader *s_y16_vs;
static ID3D11PixelShader  *s_y16_ps[2];      /* [msaa] */
static ID3D11PixelShader  *s_y16d_ps[2];     /* [msaa], from a depth buffer */
/* s_raw: the vertex-data ring ub_vs reads (raw view s_raw_srv) */
static ID3D11Buffer *s_raw, *s_ib, *s_cb_ub, *s_cb_prog, *s_cb_vconst, *s_cb_ps, *s_cb_present, *s_cb_clear;
static ID3D11ShaderResourceView *s_raw_srv;
static UINT s_raw_pos, s_ib_pos;
static int s_raw_nooverwrite;
#define VB_BYTES (16u << 20)
#define IB_BYTES (8u << 20)
static ID3D11SamplerState *s_linear_clamp, *s_point_clamp;
static ID3D11BlendState *s_blend_opaque;
static ID3D11RasterizerState *s_rs_plain;
static ID3D11DepthStencilState *s_ds_off;

/* This context has one owner. Every VS shader/b0 writer goes through these
 * helpers, including auxiliary blits and clears. Their objects live for the
 * renderer's lifetime; repeated draws need no driver call for the same bind. */
static ID3D11VertexShader *s_bound_vs;
static ID3D11Buffer *s_bound_vs_b0;
static int s_bound_vs_valid, s_bound_vs_b0_valid;
static ID3D11PixelShader *s_bound_ps;
static ID3D11SamplerState *s_bound_ps_samplers[4];
static int s_bound_ps_valid;
static uint32_t s_bound_ps_samplers_valid;

static void bind_vertex_shader(ID3D11VertexShader *shader)
{
    if (!s_bound_vs_valid || s_bound_vs != shader) {
        ID3D11DeviceContext_VSSetShader(s_ctx, shader, NULL, 0);
        s_bound_vs = shader;
        s_bound_vs_valid = 1;
    }
}

static void bind_vertex_constants(ID3D11Buffer *buffer)
{
    if (!s_bound_vs_b0_valid || s_bound_vs_b0 != buffer) {
        ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &buffer);
        s_bound_vs_b0 = buffer;
        s_bound_vs_b0_valid = 1;
    }
}

/* Every PS shader/sampler writer uses these helpers, including clears,
 * surface views, blits and presentation. These slots have no render-target
 * alias hazard: binding outputs can clear SRVs, but cannot change samplers or
 * shaders. A short sampler write preserves the untouched upper slots. */
static void bind_pixel_shader(ID3D11PixelShader *shader)
{
    if (!s_bound_ps_valid || s_bound_ps != shader) {
        ID3D11DeviceContext_PSSetShader(s_ctx, shader, NULL, 0);
        s_bound_ps = shader;
        s_bound_ps_valid = 1;
    }
}

static void bind_pixel_samplers(UINT count, ID3D11SamplerState *const *samplers)
{
    uint32_t valid;
    if (!count) return;
    if (count > 4) {
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, count, samplers);
        s_bound_ps_samplers_valid = 0;
        return;
    }
    valid = (1u << count) - 1u;
    if ((s_bound_ps_samplers_valid & valid) != valid ||
        memcmp(s_bound_ps_samplers, samplers, count * sizeof *samplers)) {
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, count, samplers);
        memcpy(s_bound_ps_samplers, samplers, count * sizeof *samplers);
        s_bound_ps_samplers_valid |= valid;
    }
}

/* No other pass writes VS t0/b1-b3 or PS b1. The raw buffer is input-only
 * and cannot overlap any texture output, so D3D11's output hazard handling
 * cannot unbind it. DISCARD changes its storage, not its resource binding.
 * Keep these slots bound after successful initialization. */
static void bind_fixed_resources(void)
{
    ID3D11Buffer *buffers[3] = { NULL, s_cb_prog, s_cb_vconst };
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 1, 3, buffers);
    ID3D11DeviceContext_VSSetShaderResources(s_ctx, 0, 1, &s_raw_srv);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 1, 1, &s_cb_ps);
    s_bound_vs_valid = s_bound_vs_b0_valid = 0;
    s_bound_ps_valid = 0;
    s_bound_ps_samplers_valid = 0;
}

static void fail(const char *what, HRESULT hr)
{
    fprintf(stderr, "[D3D11] %s failed: 0x%08lX\n", what, (unsigned long)hr);
    fflush(stderr);
}

static int guest_ok(uint32_t va, size_t bytes)
{
    if (va < 0x10000u) return 0;
    if (va < 0x08000000u) return (uint64_t)va + bytes <= 0x08000000u;
    if (va >= CONTIG_BASE && va < CONTIG_BASE + CONTIG_SIZE)
        return (uint64_t)va + bytes <= (uint64_t)CONTIG_BASE + CONTIG_SIZE;
    return 0;
}

static const uint8_t *guest(uint32_t va)
{
    return (const uint8_t *)xbox_GetMemoryOffset() + va;
}

/* The GPU's name for memory: the physical address, whichever window the
 * executor resolved it through. */
static uint32_t phys(uint32_t va)
{
    return va >= CONTIG_BASE ? va - CONTIG_BASE : va;
}

#include "nv2a_texture_hash.h"

/* ---------------------------------------------------------------- shaders */

static const char s_hlsl[] =
/* FLATQ: nointerpolation for the flat-shaded pixel shader variant. Under
 * MSAA the rest interpolate at the centroid: an edge pixel's centre can lie
 * outside its triangle, and texcoords extrapolated there read past the tile
 * edge -- bright seams between ground tiles. */
"#ifndef FLATQ\n#define FLATQ centroid\n#endif\n"
"struct PSIn { float4 pos:SV_Position; FLATQ float4 d0:COLOR0; FLATQ float4 d1:COLOR1;\n"
"  centroid float4 fog:FOG; centroid float4 t0:TEXCOORD0; centroid float4 t1:TEXCOORD1;\n"
"  centroid float4 t2:TEXCOORD2; centroid float4 t3:TEXCOORD3; };\n"
"cbuffer UBC : register(b0) { uint4 at[16]; float4 DEF[16]; uint4 ub_info; float4 ub_map;\n"
"  uint4 ub_fogi; float4 ub_fogf; float4 ub_place; };\n"
"cbuffer UBP : register(b2) { uint4 PROG[136]; };\n"
"cbuffer UBK : register(b3) { float4 C[192]; };\n"
"ByteAddressBuffer VB : register(t0);\n"
"uint ldw(uint a) {\n"
"  uint b = a & ~3u, s = (a & 3u) * 8u, l = VB.Load(b);\n"
"  return s ? (l >> s) | (VB.Load(b + 4u) << (32u - s)) : l;\n"
"}\n"
"float s16(uint a) { return (float)((int)(ldw(a) << 16) >> 16); }\n"
"float4 fetch(uint i, uint vid) {\n"
"  uint4 d = at[i];\n"
"  if (d.w == 0u) return DEF[i];\n"
"  uint a = d.x + vid * d.y;\n"
"  float4 r = float4(0, 0, 0, 1);\n"
"  if (d.z == 0u) { uint c = ldw(a); r = float4((c >> 16) & 255u, (c >> 8) & 255u, c & 255u, c >> 24) / 255.0; }\n"
"  else if (d.z == 6u) { int u = (int)ldw(a);\n"
"    r = float4((float)((u << 21) >> 21) / 1023.0, (float)((u << 10) >> 21) / 1023.0, (float)(u >> 22) / 511.0, 1.0); }\n"
"  else if (d.z == 2u) { [unroll] for (uint k = 0; k < 4; k++) if (k < d.w) r[k] = asfloat(ldw(a + 4u * k)); }\n"
"  else if (d.z == 4u) { uint c = ldw(a); [unroll] for (uint k = 0; k < 4; k++) if (k < d.w) r[k] = (float)((c >> (8u * k)) & 255u) / 255.0; }\n"
"  else if (d.z == 1u) { [unroll] for (uint k = 0; k < 4; k++) if (k < d.w) r[k] = s16(a + 2u * k) / 32767.0; }\n"
"  else if (d.z == 5u) { [unroll] for (uint k = 0; k < 4; k++) if (k < d.w) r[k] = s16(a + 2u * k); }\n"
"  return r;\n"
"}\n"
"static float4 R[13];\n"
"static float4 V[16];\n"
"static float4 O[13];\n"
"/* A component by dynamic index, by selection: indexing a vector compiles to\n"
" * a dot product with a one-hot vector, and inf * 0 there turns every\n"
" * component of a register holding an infinity (rcp of 0) into NaN. */\n"
"float cmp4(float4 s, uint i) { return i == 0u ? s.x : (i == 1u ? s.y : (i == 2u ? s.z : s.w)); }\n"
"float4 rd(uint swz, uint neg, uint mux, uint reg, uint vi, uint ci) {\n"
"  float4 s = mux == 1u ? R[reg < 13u ? reg : 0u] : (mux == 2u ? V[vi] : C[ci]);\n"
"  float4 r = float4(cmp4(s, (swz >> 6) & 3u), cmp4(s, (swz >> 4) & 3u), cmp4(s, (swz >> 2) & 3u), cmp4(s, swz & 3u));\n"
"  return neg ? -r : r;\n"
"}\n"
"float4 msk(float4 d, float4 v, uint m) {\n"
"  return float4((m & 8u) ? v.x : d.x, (m & 4u) ? v.y : d.y, (m & 2u) ? v.z : d.z, (m & 1u) ? v.w : d.w);\n"
"}\n"
"float ub_fog(float d) {\n"
"  if (!ub_fogi.x) return 1.0;\n"
"  float f, px = ub_fogf.x, py = ub_fogf.y; uint mode = ub_fogi.y;\n"
"  if (mode == 0x800u || mode == 0x802u) f = px + exp2(d * py * 16.0) - 1.5;\n"
"  else if (mode == 0x801u || mode == 0x803u) f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n"
"  else f = px + d * py - 1.0;\n"
"  if (mode == 0x802u || mode == 0x803u || mode == 0x804u) f = abs(f);\n"
"  if (isnan(f)) f = 1.0;\n"
"  return saturate(f);\n"
"}\n"
"PSIn ub_vs(uint vid : SV_VertexID) {\n"
"  PSIn o;\n"
"  float4 P, d0, d1, t0, t1, t2, t3;\n"
"  float fog = 1.0;\n"
"  if (ub_info.y) {\n"
"    [unroll] for (uint i = 0; i < 16; i++) V[i] = fetch(i, vid);\n"
"    [unroll] for (uint j = 0; j < 13; j++) { R[j] = float4(0, 0, 0, 0); O[j] = float4(0, 0, 0, 0); }\n"
"    O[3].w = 1; O[4].w = 1; O[9].w = 1; O[10].w = 1; O[11].w = 1; O[12].w = 1;\n"
"    int a0 = 0;\n"
"    [loop] for (uint s = ub_info.x; s < 136u; s++) {\n"
"      uint4 ins = PROG[s];\n"
"      uint mac = (ins.y >> 21) & 15u, ilu = (ins.y >> 25) & 7u, vi = (ins.y >> 9) & 15u;\n"
"      int ci = (int)((ins.y >> 13) & 255u) + (((ins.w >> 1) & 1u) ? a0 : 0);\n"
"      uint cu = (ci >= 0 && ci < 192) ? (uint)ci : 0u;\n"
"      float4 A = rd(ins.y & 255u, (ins.y >> 8) & 1u, (ins.z >> 26) & 3u, (ins.z >> 28) & 15u, vi, cu);\n"
"      float4 B = rd((ins.z >> 17) & 255u, (ins.z >> 25) & 1u, (ins.z >> 11) & 3u, (ins.z >> 13) & 15u, vi, cu);\n"
"      float4 Cc = rd((ins.z >> 2) & 255u, (ins.z >> 10) & 1u, (ins.w >> 28) & 3u, ((ins.z & 3u) << 2) | (ins.w >> 30), vi, cu);\n"
"      float4 mr = mac ? A : float4(0, 0, 0, 0), ir = ilu ? Cc : float4(0, 0, 0, 0);\n"
"      if (mac == 2u) mr = A * B;\n"
"      else if (mac == 3u) mr = A + Cc;\n"
"      else if (mac == 4u) mr = A * B + Cc;\n"
"      else if (mac == 5u) mr = dot(A.xyz, B.xyz).xxxx;\n"
"      else if (mac == 6u) mr = (dot(A.xyz, B.xyz) + B.w).xxxx;\n"
"      else if (mac == 7u) mr = dot(A, B).xxxx;\n"
"      else if (mac == 8u) mr = float4(1.0, A.y * B.y, A.z, B.w);\n"
"      else if (mac == 9u) mr = A < B ? A : B;\n"
"      else if (mac == 10u) mr = A >= B ? A : B;\n"
"      else if (mac == 11u) mr = A < B ? 1.0 : 0.0;\n"
"      else if (mac == 12u) mr = A >= B ? 1.0 : 0.0;\n"
"      else if (mac == 13u) a0 = (int)floor(A.x + 0.001);\n"
"      float x = Cc.x;\n"
"      if (ilu == 2u) ir = (x == 0.0 ? asfloat(0x7F800000u) : 1.0 / x).xxxx;\n"
"      else if (ilu == 3u) { float f = 1.0 / x; float af = clamp(abs(f), 5.42101e-20, 1.884467e19);\n"
"        ir = (f < 0 ? -af : af).xxxx; }\n"
"      else if (ilu == 4u) ir = (1.0 / sqrt(abs(x))).xxxx;\n"
"      else if (ilu == 5u) { float fl = floor(x); ir = float4(exp2(fl), x - fl, exp2(x), 1.0); }\n"
"      else if (ilu == 6u) { float ax = abs(x);\n"
"        if (ax == 0.0) ir = float4(-asfloat(0x7F800000u), 1.0, -asfloat(0x7F800000u), 1.0);\n"
"        else { float e = floor(log2(ax)); ir = float4(e, ax / exp2(e), log2(ax), 1.0); } }\n"
"      else if (ilu == 7u) { float nl = max(x, 0.0), nh = max(Cc.y, 0.0), p = clamp(Cc.w, -127.9961, 127.9961);\n"
"        ir = float4(1.0, nl, x > 0.0 ? (p == 0.0 ? 1.0 : pow(nh, p)) : 0.0, 1.0); }\n"
"      uint mm = (ins.w >> 24) & 15u, im = (ins.w >> 16) & 15u, td = (ins.w >> 20) & 15u;\n"
"      uint om = (ins.w >> 12) & 15u, oa = (ins.w >> 3) & 255u;\n"
"      /* As xemu: paired with an ILU op the MAC cannot write R1 (the ILU\n"
"       * owns it), and an output comes only from a unit that ran. */\n"
"      if (mac != 0u && mac != 13u && mm != 0u && td < 13u && !(ilu != 0u && td == 1u))\n"
"        R[td] = msk(R[td], mr, mm);\n"
"      if (ilu != 0u && im != 0u) { uint it = mac ? 1u : td; if (it < 13u) R[it] = msk(R[it], ir, im); }\n"
"      /* ponytail: writes to the constant file (out-to-c[]) are dropped;\n"
"       * a local copy of C[] would support them. */\n"
"      if (om != 0u && ((ins.w >> 11) & 1u) && (((ins.w >> 2) & 1u) ? ilu : mac) != 0u) {\n"
"        float4 ov = ((ins.w >> 2) & 1u) ? ir : mr;\n"
"        if (oa == 0u) R[12] = msk(R[12], ov, om);\n"
"        else if (oa < 13u) O[oa] = msk(O[oa], ov, om);\n"
"      }\n"
"      if (ins.w & 1u) break;\n"
"    }\n"
"    P = R[12]; d0 = O[3]; d1 = O[4]; fog = ub_fog(O[5].x);\n"
"    t0 = O[9]; t1 = O[10]; t2 = O[11]; t3 = O[12];\n"
"  } else {\n"
"    /* Pre-transformed: position (x, y, z, rhw), diffuse 3, specular 4,\n"
"     * texture coordinates 9-12. */\n"
"    float4 p0 = fetch(0, vid);\n"
"    P = float4(p0.xyz, at[0].w == 4u && p0.w > 0.0 ? 1.0 / p0.w : 1.0);\n"
"    d0 = fetch(3, vid); d1 = fetch(4, vid);\n"
"    t0 = fetch(9, vid); t1 = fetch(10, vid); t2 = fetch(11, vid); t3 = fetch(12, vid);\n"
"  }\n"
"  float px = P.x;\n"
"  if (ub_info.z != 0xFFFFFFFFu) px += asfloat(VB.Load(ub_info.z + vid * 4u));\n"
/* The NV2A rasterises with 4-bit sub-pixel precision, truncating (xemu
 * roundScreenCoords), so the Xbox D3D screen offset 0.53125 puts a
 * full-target quad's left/top edge exactly on the centre of pixel 0, and the
 * top-left rule covers the whole pixel. Upscaled, that edge splits pixel 0's
 * block of host pixels and leaves its first half unwritten -- a wrapped
 * water ripple map then showed the unwritten strip as seams across the
 * water. A coordinate that truncates to that first centre is placed on the
 * target edge, as the hardware covers it; everything else is unchanged. */
"  float py = P.y;\n"
"  if (ub_place.z != 0.0 && px >= 0.5 && px < 0.5625) px = 0.0;\n"
"  if (ub_place.z != 0.0 && py >= 0.5 && py < 0.5625) py = 0.0;\n"
"  if (ub_place.w != 0.0) {\n"
"    bool flat = abs(P.w - 1.0) < 1e-6;\n"
"    if (!flat || (frac(px) >= 0.5 && frac(px) < 0.5625)) px -= ub_place.w;\n"
"    if (!flat || (frac(py) >= 0.5 && frac(py) < 0.5625)) py -= ub_place.w;\n"
"  }\n"
"  /* NV2A clamps raster w away from zero and infinity (xemu clampAwayZeroInf),\n"
"   * keeping its sign so geometry behind the eye still clips. oPos is screen\n"
"   * space after the D3D epilogue's divide; multiplying back by w gives the\n"
"   * clip-space position, so the host clipper and perspective-correct\n"
"   * interpolation see what the NV2A saw. */\n"
"  float w = P.w;\n"
"  float aw = clamp(abs(w), 5.421011e-20, 1.8446744e19);\n"
"  w = w < 0 ? -aw : aw;\n"
"  o.pos = float4(((px * ub_map.x - 1.0) * ub_map.w + ub_place.x) * w,\n"
"                 (1.0 - py * ub_map.y) * ub_place.y * w, P.z * ub_map.z * w, w);\n"
"  o.d0 = saturate(d0); o.d1 = saturate(d1); o.fog = float4(fog, w, 0, 0);\n"
"  o.t0 = t0; o.t1 = t1; o.t2 = t2; o.t3 = t3;\n"
"  return o;\n"
"}\n"
/* SPEC: a variant specialised to one combiner configuration. The control
 * words below become constants (SPEC_DECL) so the compiler folds the
 * interpreter away; the cbuffer keeps its layout under other names. */
"#ifdef SPEC\n"
"#define c_icw cb_c_icw\n#define a_icw cb_a_icw\n#define c_ocw cb_c_ocw\n#define a_ocw cb_a_ocw\n"
"#define fin0 cb_fin0\n#define fin1 cb_fin1\n#define rc_control cb_rc_control\n#define stage_prog cb_stage_prog\n"
"#define alpha_func cb_alpha_func\n#define ps_flags cb_ps_flags\n#define clip_plane cb_clip_plane\n"
"#define other_input cb_other_input\n#define dot_map cb_dot_map\n#define tex_info cb_tex_info\n"
"#define RC_LOOP [unroll]\n"
"#else\n#define RC_LOOP [loop]\n#endif\n"
"cbuffer PSC : register(b1) {\n"
"  uint4 c_icw[2]; uint4 a_icw[2]; uint4 c_ocw[2]; uint4 a_ocw[2];\n"
"  float4 cf0[8]; float4 cf1[8]; float4 fin_c0; float4 fin_c1;\n"
"  uint fin0; uint fin1; uint rc_control; uint stage_prog;\n"
"  uint alpha_func; uint alpha_ref; uint ps_flags; uint clip_plane;\n"
"  uint other_input; uint dot_map; uint pad0; uint pad1;\n"
"  uint4 tex_info[4];\n"       /* x: bit0 alphakill, y: signed mask (filter>>28) */
"  float4 tex_scale[4];\n"     /* xy multiply the coordinate (1/size for linear) */
"  float4 fog_color;\n"
"  float4 bump_mat[4]; float4 bump_lum[4];\n"
"  float4 zp0; float4 zp1;\n"
"};\n"
"#ifdef SPEC\n"
"#undef c_icw\n#undef a_icw\n#undef c_ocw\n#undef a_ocw\n#undef fin0\n#undef fin1\n#undef rc_control\n"
"#undef stage_prog\n#undef alpha_func\n#undef ps_flags\n#undef clip_plane\n#undef other_input\n"
"#undef dot_map\n#undef tex_info\n"
"SPEC_DECL\n"
"#endif\n"
"Texture2D T2D0:register(t0); Texture2D T2D1:register(t1); Texture2D T2D2:register(t2); Texture2D T2D3:register(t3);\n"
"TextureCube TC0:register(t4); TextureCube TC1:register(t5); TextureCube TC2:register(t6); TextureCube TC3:register(t7);\n"
"Texture2D FBK:register(t8); static float2 g_pos;\n"
"SamplerState S0:register(s0); SamplerState S1:register(s1); SamplerState S2:register(s2); SamplerState S3:register(s3);\n"
"uint U8(uint4 a[2], uint s) { return a[s >> 2][s & 3]; }\n"
"float map_in(float x, uint b) {\n"
"  float p = max(x, 0.0);\n"
"  switch ((b >> 5) & 7) {\n"
"  case 0: return p; case 1: return 1.0 - saturate(x); case 2: return 2.0 * p - 1.0;\n"
"  case 3: return -2.0 * p + 1.0; case 4: return p - 0.5; case 5: return -p + 0.5;\n"
"  case 6: return x; default: return -x; }\n"
"}\n"
"float3 map_in3(float3 v, uint b) { return float3(map_in(v.x, b), map_in(v.y, b), map_in(v.z, b)); }\n"
"float3 map_out3(float3 x, uint m) {\n"
"  if (m == 0x08) return x - 0.5; if (m == 0x10) return x * 2.0; if (m == 0x18) return (x - 0.5) * 2.0;\n"
"  if (m == 0x20) return x * 4.0; if (m == 0x30) return x * 0.5; return x;\n"
"}\n"
"float signed_c(float c) { float s = c * 255.0; return clamp((s >= 127.5 ? s - 256.0 : s) / 127.0, -1.0, 1.0); }\n"
"float4 apply_signed(float4 t, uint m) {\n"
"  if (m & 1) t.a = signed_c(t.a); if (m & 2) t.r = signed_c(t.r);\n"
"  if (m & 4) t.g = signed_c(t.g); if (m & 8) t.b = signed_c(t.b); return t;\n"
"}\n"
/* SET_DOT_RGBMAPPING, as xemu's sign1/2/3: 1 MINUS_1_TO_1_D3D is the
 * biased (x - 128) / 127 that D3D's _bx2 on a texture read produces (255 is
 * +1), 2 the GL mapping, 3 two's complement. */
"float3 dotmap(float3 c, uint m) {\n"
"  float3 x = round(saturate(c) * 255.0);\n"
"  if (m == 1) return (x - 128.0) / 127.0;\n"
"  if (m == 2) return x >= 128.0 ? (x - 255.5) / 127.5 : (x + 0.5) / 127.5;\n"
"  if (m == 3) return (x >= 128.0 ? x - 256.0 : x) / 127.0;\n"
"  return c;\n"
"}\n"
"float4 bspline0(float2 uv, float lod) {\n"
"  float2 sz; float nl; T2D0.GetDimensions(0, sz.x, sz.y, nl); lod = min(lod, nl - 1.0); sz = max(floor(sz / exp2(lod)), 1.0);\n"
"  float2 p = uv * sz - 0.5, f = frac(p), f2 = f * f, f3 = f2 * f;\n"
"  float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0, w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;\n"
"  float2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0, w3 = f3 / 6.0;\n"
"  float2 s0 = w0 + w1, s1 = w2 + w3; p -= f;\n"
"  float2 c0 = (p - 0.5 + w1 / s0) / sz, c1 = (p + 1.5 + w3 / s1) / sz;\n"
"  return s0.y * (s0.x * T2D0.SampleLevel(S0, c0, lod) + s1.x * T2D0.SampleLevel(S0, float2(c1.x, c0.y), lod)) +\n"
"         s1.y * (s0.x * T2D0.SampleLevel(S0, float2(c0.x, c1.y), lod) + s1.x * T2D0.SampleLevel(S0, c1, lod));\n"
"}\n"
"float4 samp2(uint n, float2 uv) {\n"
"  if (n == 0 && (ps_flags & 4)) {\n"
"    float4 c = bspline0(uv, 0.0);\n"
"    float f = (float)((ps_flags >> 8) & 255u) / 100.0, dv = ddy(uv.y);\n"
"    if (f > 0.0 && abs(dv) > 1e-7) {\n"
"      float2 fs; FBK.GetDimensions(fs.x, fs.y);\n"
"      float y0 = clamp(g_pos.y - uv.y / dv - 3.0 * fs.y / 900.0, 0.0, fs.y - 1.0);\n"
"      float3 sky = 0;\n"
"      [unroll] for (int k = -4; k <= 4; k++) sky += FBK.Load(int3(clamp(g_pos.x + k * fs.x / 200.0, 0.0, fs.x - 1.0), y0, 0)).rgb;\n"
"      c.rgb = lerp(c.rgb, sky / 9.0, f);\n"
"    }\n"
"    return c; }\n"
"  if (n == 0) return T2D0.Sample(S0, uv); if (n == 1) return T2D1.Sample(S1, uv);\n"
"  if (n == 2) return T2D2.Sample(S2, uv); return T2D3.Sample(S3, uv);\n"
"}\n"
/* A point-sampled tap at a fixed spot of an upscaled render target (light
 * flares test their visibility with three such taps of the frame's
 * alpha). On the NV2A it reads one guest pixel; here that pixel is a block
 * of host pixels, and a single host texel inside it changes with every
 * sub-pixel move of the scene -- glows and tail lights flickered. The block
 * the guest pixel became is averaged instead. g: guest texel coordinates,
 * inv: 1 / guest size. */
"float4 ldn(uint n, int2 p) {\n"
"  if (n == 0) return T2D0.Load(int3(p, 0)); if (n == 1) return T2D1.Load(int3(p, 0));\n"
"  if (n == 2) return T2D2.Load(int3(p, 0)); return T2D3.Load(int3(p, 0));\n"
"}\n"
"float4 tapbox(uint n, float2 g, float2 inv) {\n"
"  uint w, h;\n"
"  if (n == 0) T2D0.GetDimensions(w, h); else if (n == 1) T2D1.GetDimensions(w, h);\n"
"  else if (n == 2) T2D2.GetDimensions(w, h); else T2D3.GetDimensions(w, h);\n"
"  float2 dim = float2(w, h), sc = dim * inv, gi = floor(g);\n"
"  int2 a = clamp(int2(floor(gi * sc + 0.5)), 0, int2(dim) - 1);\n"
"  int2 b = clamp(int2(floor((gi + 1.0) * sc + 0.5)), a + 1, int2(dim));\n"
"  float4 acc = 0; float cnt = 0;\n"
"  [loop] for (int y = a.y; y < b.y && y < a.y + 8; y++)\n"
"    [loop] for (int x = a.x; x < b.x && x < a.x + 8; x++) { acc += ldn(n, int2(x, y)); cnt += 1; }\n"
"  return acc / max(cnt, 1.0);\n"
"}\n"
"float4 sampc(uint n, float3 d) {\n"
"  if (n == 0) return TC0.Sample(S0, d); if (n == 1) return TC1.Sample(S1, d);\n"
"  if (n == 2) return TC2.Sample(S2, d); return TC3.Sample(S3, d);\n"
"}\n"
"float4 shade(PSIn i, out uint cov) {\n"
"  g_pos = i.pos.xy;\n"
"  float4 tc[4] = { i.t0, i.t1, i.t2, i.t3 };\n"
"  float4 T[4] = { float4(0,0,0,1), float4(0,0,0,1), float4(0,0,0,1), float4(0,0,0,1) }; float dots[4] = { 0, 0, 0, 0 };\n"
"  [unroll] for (uint n = 0; n < 4; n++) {\n"
"    uint mode = (stage_prog >> (n * 5)) & 31;\n"
"    uint inp = n == 2 ? (other_input >> 16) & 1 : (n == 3 ? (other_input >> 20) & 3 : 0);\n"
"    uint dm = n ? (dot_map >> ((n - 1) * 4)) & 7 : 0;\n"
"    float4 c = tc[n]; float4 t = float4(0, 0, 0, 1);\n"
"    float q = c.w != 0.0 ? c.w : 1.0;\n"
"    if (mode == 1 || mode == 2) {\n"
"      float2 g = c.xy / q, d = abs(ddx(g)) + abs(ddy(g));\n"
"      if (tex_info[n].z != 0u && max(d.x, d.y) < 1e-4) t = tapbox(n, g, tex_scale[n].xy);\n"
"      else t = samp2(n, g * tex_scale[n].xy);\n"
"    }\n"
"    else if (mode == 3) t = sampc(n, c.xyz);\n"
"    else if (mode == 4) t = saturate(c);\n"
"    else if (mode == 5) {\n"
"      [unroll] for (uint j = 0; j < 4; j++) {\n"
"        bool ge = (clip_plane >> (n * 4 + j)) & 1;\n"
"        if (ge ? c[j] >= 0.0 : c[j] < 0.0) discard; }\n"
"      t = float4(0, 0, 0, 0);\n"
"    } else if (mode == 6 || mode == 7) {\n"
"      float4 s = T[inp]; float4 m = bump_mat[n];\n"
"      float2 uv = c.xy / q * tex_scale[n].xy;\n"
"      uv += float2(m.x * s.r + m.z * s.g, m.y * s.r + m.w * s.g) * tex_scale[n].zw;\n"
"      t = samp2(n, uv);\n"
"      if (mode == 7) t.rgb *= saturate(s.b * bump_lum[n].x + bump_lum[n].y);\n"
"    } else if (mode == 17 || mode == 10) {\n"
"      dots[n] = dot(c.xyz, dotmap(T[inp].rgb, dm)); t = float4(0, 0, 0, 0);\n"
"    } else if (mode == 9) {\n"
"      dots[n] = dot(c.xyz, dotmap(T[inp].rgb, dm));\n"
"      t = samp2(n, float2(dots[n - 1], dots[n]) * tex_scale[n].xy);\n"
"    } else if (mode == 13 || mode == 14 || mode == 12 || mode == 18 || mode == 11) {\n"
"      dots[n] = dot(c.xyz, dotmap(T[inp].rgb, dm));\n"
"      float3 N = float3(dots[1], dots[2], dots[3]);\n"
"      if (mode == 12 || mode == 18) {\n"
"        float3 E = mode == 12 ? float3(tc[1].w, tc[2].w, tc[3].w) : float3(0, 0, 1);\n"
"        N = 2.0 * N * dot(N, E) / max(dot(N, N), 1e-20) - E;\n"
"      }\n"
"      if (mode == 13) t = samp2(n, N.xy * tex_scale[n].xy); else t = sampc(n, N);\n"
"    } else if (mode == 15) t = samp2(n, T[inp].ar);\n"
"    else if (mode == 16) t = samp2(n, T[inp].gb);\n"
"    if (mode != 0 && mode != 4 && mode != 5 && mode != 10 && mode != 17) {\n"
"      t = apply_signed(t, tex_info[n].y);\n"
"      if ((tex_info[n].x & 1) && t.a == 0.0) discard;\n"
"    }\n"
"    T[n] = t;\n"
"  }\n"
"  float4 R[16];\n"
"  [unroll] for (uint k = 0; k < 16; k++) R[k] = float4(0, 0, 0, 0);\n"
"  R[3] = float4(fog_color.rgb, i.fog.x); R[4] = i.d0; R[5] = i.d1;\n"
"  R[8] = T[0]; R[9] = T[1]; R[10] = T[2]; R[11] = T[3];\n"
"  float4 o;\n"
"  if (!(ps_flags & 1)) {\n"
/* No combiner program yet: texture 0 modulated by diffuse. */
"    o = (stage_prog & 31) ? T[0] * i.d0 : i.d0;\n"
"  } else {\n"
"    R[12].a = (stage_prog & 31) ? T[0].a : 1.0;\n"
"    uint nst = min(rc_control & 0xFF, 8); uint fl8 = rc_control >> 8;\n"
"    RC_LOOP for (uint s = 0; s < nst; s++) {\n"
"      uint icw = U8(c_icw, s), ocw = U8(c_ocw, s), aicw = U8(a_icw, s), aocw = U8(a_ocw, s);\n"
"      if (((ocw | aocw) & 0xFFF) == 0) continue;\n"
"      R[1] = cf0[(fl8 & 0x010) ? s : 0]; R[2] = cf1[(fl8 & 0x100) ? s : 0];\n"
"      bool mux = (fl8 & 1) ? R[12].a >= 0.5 : (((uint)(R[12].a * 255.0)) & 1) != 0;\n"
"      uint b;\n"
"      b = icw >> 24; float3 A = map_in3((b & 0x10) ? R[b & 15].aaa : R[b & 15].rgb, b);\n"
"      b = icw >> 16; float3 B = map_in3((b & 0x10) ? R[b & 15].aaa : R[b & 15].rgb, b);\n"
"      b = icw >> 8;  float3 C = map_in3((b & 0x10) ? R[b & 15].aaa : R[b & 15].rgb, b);\n"
"      b = icw;       float3 D = map_in3((b & 0x10) ? R[b & 15].aaa : R[b & 15].rgb, b);\n"
"      b = aicw >> 24; float aA = map_in((b & 0x10) ? R[b & 15].a : R[b & 15].b, b);\n"
"      b = aicw >> 16; float aB = map_in((b & 0x10) ? R[b & 15].a : R[b & 15].b, b);\n"
"      b = aicw >> 8;  float aC = map_in((b & 0x10) ? R[b & 15].a : R[b & 15].b, b);\n"
"      b = aicw;       float aD = map_in((b & 0x10) ? R[b & 15].a : R[b & 15].b, b);\n"
"      uint fl = ocw >> 12, afl = aocw >> 12;\n"
"      float3 pab = (fl & 2) ? dot(A, B).xxx : A * B;\n"
"      float3 pcd = (fl & 1) ? dot(C, D).xxx : C * D;\n"
"      float3 sum = (fl & 4) ? (mux ? pcd : pab) : pab + pcd;\n"
"      float3 ab = clamp(map_out3(pab, fl & 0x38), -1.0, 1.0);\n"
"      float3 cd = clamp(map_out3(pcd, fl & 0x38), -1.0, 1.0);\n"
"      float3 ms = clamp(map_out3(sum, fl & 0x38), -1.0, 1.0);\n"
"      float apab = aA * aB, apcd = aC * aD;\n"
"      float asum = (afl & 4) ? (mux ? apcd : apab) : apab + apcd;\n"
"      float aab = clamp(map_out3(apab.xxx, afl & 0x38).x, -1.0, 1.0);\n"
"      float acd = clamp(map_out3(apcd.xxx, afl & 0x38).x, -1.0, 1.0);\n"
"      float ams = clamp(map_out3(asum.xxx, afl & 0x38).x, -1.0, 1.0);\n"
"      uint d;\n"
"      d = (ocw >> 4) & 15; if (d) { R[d].rgb = ab; if (fl & 0x80) R[d].a = ab.b; }\n"
"      d = ocw & 15;        if (d) { R[d].rgb = cd; if (fl & 0x40) R[d].a = cd.b; }\n"
"      d = (ocw >> 8) & 15; if (d) R[d].rgb = ms;\n"
"      d = (aocw >> 4) & 15; if (d) R[d].a = aab;\n"
"      d = aocw & 15;        if (d) R[d].a = acd;\n"
"      d = (aocw >> 8) & 15; if (d) R[d].a = ams;\n"
"      R[0] = float4(0, 0, 0, 0);\n"
"    }\n"
"    if (fin0 | fin1) {\n"
"      uint ff = fin1 & 0xFF; uint b;\n"
"      R[1] = fin_c0; R[2] = fin_c1;\n"
"      float3 va = (ff & 0x40) ? 1.0 - R[5].rgb : R[5].rgb;\n"
"      float3 vb = (ff & 0x20) ? 1.0 - R[12].rgb : R[12].rgb;\n"
"      R[14] = float4((ff & 0x80) ? saturate(va + vb) : va + vb, 0);\n"
"      b = fin1 >> 24; float3 E = map_in3((b & 0x10) ? R[b & 15].aaa : R[b & 15].rgb, b);\n"
"      b = fin1 >> 16; float3 F = map_in3((b & 0x10) ? R[b & 15].aaa : R[b & 15].rgb, b);\n"
"      R[15] = float4(E * F, 0);\n"
"      b = fin0 >> 24; float3 A = map_in3((b & 0x10) ? R[b & 15].aaa : R[b & 15].rgb, b);\n"
"      b = fin0 >> 16; float3 B = map_in3((b & 0x10) ? R[b & 15].aaa : R[b & 15].rgb, b);\n"
"      b = fin0 >> 8;  float3 C = map_in3((b & 0x10) ? R[b & 15].aaa : R[b & 15].rgb, b);\n"
"      b = fin0;       float3 D = map_in3((b & 0x10) ? R[b & 15].aaa : R[b & 15].rgb, b);\n"
"      b = fin1 >> 8;  float G = map_in((b & 0x10) ? R[b & 15].a : R[b & 15].b, b);\n"
"      o = float4(D + C * (1.0 - A) + B * A, G);\n"
"    } else o = R[12];\n"
"  }\n"
"  o = saturate(o);\n"
"  cov = 0xFFFFFFFFu;\n"
/* Alpha test. On a multisampled target an inequality test becomes sample
 * coverage across the one-pixel band around its threshold (e = signed
 * distance past it, in the 8-bit units the test compares), so cut-out
 * edges -- foliage, fences -- are antialiased where they were before; the
 * colour and alpha written stay as they were. Single-sample: the plain test. */
"  float a8 = o.a * 255.0, fw = max(fwidth(a8), 1e-3);\n"
"  if (ps_flags & 2) {\n"
"    int v = (int)(a8 + 0.5), r = (int)(alpha_ref & 0xFF); bool ok = true; float e = 0;\n"
"    switch (alpha_func) {\n"
"    case 0x200: ok = false; break; case 0x201: ok = v < r; e = r - 0.5 - a8; break;\n"
"    case 0x202: ok = v == r; break; case 0x203: ok = v <= r; e = r + 0.5 - a8; break;\n"
"    case 0x204: ok = v > r; e = a8 - (r + 0.5); break; case 0x205: ok = v != r; break;\n"
"    case 0x206: ok = v >= r; e = a8 - (r - 0.5); break; }\n"
"    uint ns = GetRenderTargetSampleCount();\n"
"    bool ineq = alpha_func == 0x201 || alpha_func == 0x203 || alpha_func == 0x204 || alpha_func == 0x206;\n"
"    if (ns > 1 && ineq) {\n"
"      uint k = (uint)round(saturate(e / fw + 0.5) * ns);\n"
"      if (k == 0) discard;\n"
"      cov = k >= 32 ? 0xFFFFFFFFu : (1u << k) - 1u;\n"
"    } else if (!ok) discard;\n"
"  }\n"
"  return o;\n"
"}\n"
"float4 ps_main(PSIn i, out uint cov : SV_Coverage) : SV_Target { return shade(i, cov); }\n"
/* W-buffering (SET_CONTROL0 Z_PERSPECTIVE): the depth
 * buffer holds w, interpolated perspective-correctly (fog.y carries it), in
 * 24-bit fixed point -- linear precision, unlike z/w, so distant decals and
 * foliage do not fight. Polygon offset is applied here as xemu does: the
 * rasteriser's depth bias never reaches a shader-written depth. zp0 = offset
 * factor, units, host pixels per guest pixel x/y; zp1 = 1/(zmax+1), zmax,
 * view remap scale (0 = off), knee. Draws after the title has read the
 * depth buffer for its haze pass (foliage, in one title) miss that haze; with the
 * far plane extended they lose MSAA coverage by the fraction y16d_ps hazes
 * their depth, so the hazed scene shows through. The sample count is spread
 * over each 2x2 block of host pixels with a Bayer order (floor((c + i/4))
 * over i = 0..3 sums to floor(4c)), so at the default 2x supersample every
 * window pixel gets the exact fraction: a screen-noise dither left a visible
 * pattern on distant trees that shimmered white as they moved.
 * instead of them standing crisp in front of it. The sky dome, at the far
 * plane, is left alone. */
"void ps_wdepth(PSIn i, out float4 c : SV_Target, out float dep : SV_Depth, out uint cov : SV_Coverage) {\n"
"  c = shade(i, cov);\n"
"  float z = i.fog.y;\n"
"  z += zp0.y + zp0.x * max(abs(ddx(z)) * zp0.z, abs(ddy(z)) * zp0.w);\n"
"  z = clamp(z, 0.0, zp1.y);\n"
"  if (zp1.z > 0.0) {\n"
"    float x = z * zp1.x * zp1.z, k = zp1.w;\n"
"    if (x > k && x < 0.98 * zp1.z) {\n"
"      float b = 1.0 - exp((k - x) / (1.0 - k));\n"
"      uint2 q = uint2(i.pos.xy) & 1u;\n"
"      float r = (float)(q.x * 2u + (q.x ^ q.y)) * 0.25;\n"
"      uint ns = GetRenderTargetSampleCount(), kk = (uint)floor((1.0 - b) * ns + r);\n"
"      if (kk == 0) discard;\n"
"      if (ns > 1 && kk < ns) cov &= (1u << kk) - 1u;\n"
"    }\n"
"  }\n"
"  dep = asfloat(asuint(floor(z) * zp1.x) + 1u);\n"
"}\n"
/* Presentation and uploads: a textured rectangle, optionally with the PVIDEO
 * overlay composited the way the display hardware does. */
"cbuffer PC : register(b0) { float4 dst_rect; float4 src_uv; float4 src_size;\n"
"  float4 ovl_out; float4 ovl_walk; float4 ovl_in; uint4 ovl_flags; float4 force_alpha; };\n"
"Texture2D SrcTex : register(t0); Texture2D OvlTex : register(t1); SamplerState LinS : register(s0);\n"
"struct PO { float4 pos:SV_Position; float2 uv:TEXCOORD0; };\n"
"PO present_vs(uint id : SV_VertexID) {\n"
"  float2 c = float2(id & 1, id >> 1); PO o;\n"
"  o.pos = float4(lerp(dst_rect.x, dst_rect.z, c.x), lerp(dst_rect.y, dst_rect.w, c.y), 0, 1);\n"
"  o.uv = lerp(src_uv.xy, src_uv.zw, c); return o;\n"
"}\n"
"SamplerState PointS : register(s1);\n"
"float4 present_ps(PO i) : SV_Target {\n"
"  if (ovl_flags.w) {\n"
/* Masked upload: only pixels the CPU changed replace what the GPU drew. */
"    if (OvlTex.Sample(PointS, i.uv).r < 0.5) discard;\n"
"    return SrcTex.Sample(PointS, i.uv);\n"
"  }\n"
"  float4 c = SrcTex.Sample(LinS, i.uv);\n"
"  if (ovl_flags.x) {\n"
"    float2 g = i.uv * src_size.xy;\n"
/* On a widened image the movie stays 4:3, centred (ovl_in.z = widening). */
"    float2 d = float2(ovl_in.w + (g.x - ovl_in.w) * ovl_in.z, g.y) - ovl_out.xy;\n"
"    if (all(d >= 0) && all(d < ovl_out.zw)) {\n"
"      bool show = true;\n"
"      if (ovl_flags.y) {\n"
"        uint k = ovl_flags.z;\n"
"        float3 key = float3((k >> 16) & 255, (k >> 8) & 255, k & 255) / 255.0;\n"
"        float3 fb = SrcTex.Load(int3(floor(g) * src_size.zw / src_size.xy, 0)).rgb;\n"
"        show = all(abs(fb - key) < 1.5 / 255.0);\n"
"      }\n"
"      if (show) c = OvlTex.Sample(LinS, (ovl_walk.xy + d * ovl_walk.zw) / ovl_in.xy);\n"
"    }\n"
"  }\n"
/* Loading (force_alpha.y > 0, a phase): a turning arc in the bottom-right
 * corner while the title has stopped flipping; .zw is the target's size. */
"  if (force_alpha.y > 0) {\n"
"    float s = force_alpha.w / 720.0;\n"
"    float2 d = i.pos.xy - (force_alpha.zw - 40.0 * s);\n"
"    float ring = saturate(2.0 * s - abs(length(d) - 13.0 * s));\n"
"    float a = frac(atan2(d.y, d.x) / 6.2831853 - force_alpha.y);\n"
"    c.rgb = lerp(c.rgb, float3(0.91, 0.64, 0.24), ring * smoothstep(0.15, 1.0, a));\n"
"  }\n"
"  return force_alpha.x >= 0 ? float4(c.rgb, force_alpha.x) : c;\n"
"}\n"
"cbuffer CC : register(b0) { float4 ccol; float4 cz; };\n"
"float4 clear_vs(uint id : SV_VertexID) : SV_Position {\n"
"  float2 c = float2(id & 1, id >> 1); return float4(c.x * 2 - 1, 1 - c.y * 2, cz.x, 1);\n"
"}\n"
"float4 clear_ps() : SV_Target { return ccol; }\n"
/* A 32-bit colour surface read as LU_IMAGE_Y16: twice as wide, texel 2x the
 * low 16-bit word of pixel x (G:B), 2x+1 the high one (A:R), as R=G=B, A=1
 * (xemu's mapping). MSAA surfaces are read at one sample: averaging the bytes
 * of packed values (such as depth packed into G:B for fog) would corrupt them. */
"float4 y16_vs(uint id : SV_VertexID) : SV_Position {\n"
"  float2 c = float2((id << 1) & 2, id & 2); return float4(c * float2(2, -2) + float2(-1, 1), 0, 1);\n"
"}\n"
"#ifdef Y16_MS\nTexture2DMS<float4> Y16Src : register(t0);\n#else\nTexture2D<float4> Y16Src : register(t0);\n#endif\n"
"float4 y16_ps(float4 p : SV_Position) : SV_Target {\n"
"  uint2 q = uint2(p.xy);\n"
"#ifdef Y16_MS\n  float4 c = Y16Src.Load(int2(q.x >> 1, q.y), 0);\n"
"#else\n  float4 c = Y16Src.Load(int3(q.x >> 1, q.y, 0));\n#endif\n"
"  uint4 b = (uint4)round(saturate(c) * 255.0);\n"
"  float y = (float)((q.x & 1u) ? (b.a << 8 | b.r) : (b.g << 8 | b.b)) / 65535.0;\n"
"  return float4(y, y, y, 1);\n"
"}\n"
/* The same view of a Z24S8 depth buffer (fog passes read depth this way).
 * Guest memory holds z[7:0]:stencil in texel 2x and z[23:8] in 2x+1; a
 * pixel-centred guest tap lands on 2x+1 exactly, which host-resolution
 * rounding splits between the two (moire in the fog). Both texels hold
 * z[23:8]. ponytail: the low word (z[7:0]:stencil) is not readable this way;
 * keep it for even texels if a title ever reads stencil through Y16.
 * A high-word tap sits a quarter guest pixel to the right of the shaded
 * pixel. Shift the depth read left by ccol.z host pixels so a building's
 * edge does not accidentally use sky depth and acquire a bright haze rim.
 * Host depth is z / 0xFFFFFF (see map[2]). ccol = nv2a_d3d_set_depth_view_remap
 * (scale s, knee k): x = d*s is depth in units of the original far plane;
 * below k it is x, above it the band k..1 fills exponentially with the
 * original slope at k (63% at the old far plane, 99.6% at twice it), so
 * geometry the title never meant to show fades into the sky colour instead
 * of standing crisp in a stretched band. d = 1 (sky) stays 1. */
"#ifdef Y16_MS\nTexture2DMS<float> Y16Z : register(t0);\n#else\nTexture2D<float> Y16Z : register(t0);\n#endif\n"
"float4 y16d_ps(float4 p : SV_Position) : SV_Target {\n"
"  uint2 q = uint2(p.xy);\n"
"  int zx = max(0, (int)floor((q.x + 0.5) * 0.5 - ccol.z));\n"
"#ifdef Y16_MS\n  float d = Y16Z.Load(int2(zx, q.y), 0);\n"
"#else\n  float d = Y16Z.Load(int3(zx, q.y, 0));\n#endif\n"
"  float s = ccol.x, k = ccol.y;\n"
"  d = saturate(d);\n"
"  if (s > 1.0 && d < 1.0) { float x = d * s; d = x <= k ? x : k + (1.0 - k) * (1.0 - exp((k - x) / (1.0 - k))); }\n"
"  float y = (float)((uint)round(saturate(d) * 16777215.0) >> 8) / 65535.0;\n"
"  return float4(y, y, y, 1);\n"
"}\n";

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

static void display_config(int now);

static ID3DBlob *compile_def_status(const char *entry, const char *target,
                                   const D3D_SHADER_MACRO *def, int *cache_hit)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = nv_shader_cache_compile(s_hlsl, sizeof s_hlsl - 1, "nv2a_d3d11", def,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &code, &err, cache_hit);
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11] shader %s: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        fflush(stderr);
    }
    if (err) ID3D10Blob_Release(err);
    return SUCCEEDED(hr) ? code : NULL;
}

static ID3DBlob *compile_def(const char *entry, const char *target, const D3D_SHADER_MACRO *def)
{
    return compile_def_status(entry, target, def, NULL);
}

static ID3DBlob *compile(const char *entry, const char *target)
{
    return compile_def(entry, target, NULL);
}

/* ------------------------------------------------- specialised pixel shaders
 * The ubershader interprets the register combiners per pixel, with dynamic
 * register indexing the compiler cannot keep in registers -- most of the GPU
 * time at high resolutions. Each combiner configuration gets its own variant
 * with the control words as constants, compiled on a worker thread; draws
 * use the ubershader until it is ready, so output never changes. Only the
 * control words are keyed; factors and other floats stay in the cbuffer.
 * RECOMP_PS_SPEC=0 disables this. */
typedef struct {
    uint32_t c_icw[8], a_icw[8], c_ocw[8], a_ocw[8];
    uint32_t fin0, fin1, rc_control, stage_prog, alpha_func, ps_flags;
    uint32_t clip_plane, other_input, dot_map, variant;
    uint32_t tex_info[4][3];
} PsKey;

#include "nv2a_ps_manifest.h"
typedef char PsKeySizeCheck[sizeof(PsKey) == NV_PS_MANIFEST_WORDS * 4 ? 1 : -1];

typedef struct {
    PsKey key;
    volatile LONG state;            /* 0 empty, 1 queued, 2 ready, 3 failed, 4 claimed */
    ID3D11PixelShader *ps;          /* valid once state is 2 */
    int prewarm, promoted;          /* guarded by s_spec_cs */
} PsSpec;

#define PS_SPEC_SLOTS 4096          /* power of two */
#define PS_SPEC_QUEUE 1024
static PsSpec *s_spec;
static PsSpec *s_spec_q[PS_SPEC_QUEUE];
static unsigned s_spec_head, s_spec_tail;  /* guarded by s_spec_cs */
static PsSpec *s_spec_warm[NV_PS_MANIFEST_LIMIT];
static unsigned s_spec_warm_head, s_spec_warm_tail;
static CRITICAL_SECTION s_spec_cs;
static CONDITION_VARIABLE s_spec_cv;
static volatile LONG s_spec_ready;
static int s_spec_enabled = -1, s_spec_manifest;
static wchar_t s_spec_manifest_path[NV_SHADER_CACHE_PATH];
static uint8_t s_spec_family[32];
static PsKey s_spec_known[NV_PS_MANIFEST_LIMIT];
static unsigned s_spec_known_count, s_spec_dirty, s_spec_saved;
static int s_spec_saving;
static ULONGLONG s_spec_last_save;

static void spec_family(uint8_t family[32])
{
    /* Bump the manifest schema if PsKey field order or SPEC_DECL mapping changes. */
    static const D3D_SHADER_MACRO domain[] = {
        { "PS_KEY_LAYOUT", "c_icw8,a_icw8,c_ocw8,a_ocw8,fin0,fin1,rc_control,stage_prog,alpha_func,ps_flags,clip_plane,other_input,dot_map,variant,tex_info4x3:v1" },
        { "VARIANTS", "SPEC=1;FLATQ=nointerpolation|none;variant=0..3" },
        { NULL, NULL }
    };
    NvShaderHash hash;
    uint8_t shader[32];
    nv_shader_cache_key(s_hlsl, sizeof s_hlsl - 1, "nv2a_d3d11", domain,
                        "ps_main|ps_wdepth", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, shader);
    nv_shader_hash_init(&hash);
    nv_shader_hash_add(&hash, shader, sizeof shader);
    nv_shader_hash_number(&hash, NV_PS_MANIFEST_SCHEMA);
    nv_shader_hash_number(&hash, NV_PS_MANIFEST_WORDS);
    nv_shader_hash_finish(&hash, family);
}

static PsSpec *spec_find(const PsKey *k)
{
    static PsSpec *last, *owner;
    uint64_t hash;
    uint32_t h, i;
    /* Keys never change once published. Recheck state on each use so a
     * queued/claimed shader can become ready, or still promote to live work.
     * EMPTY slots are not cached: queue saturation must remain retryable. */
    if (owner != s_spec) { owner = s_spec; last = NULL; }
    if (last && InterlockedCompareExchange(&last->state, 0, 0) &&
        !memcmp(&last->key, k, sizeof *k)) return last;
    /* This is only a table index; complete exact-key comparisons resolve
     * collisions. Persistent manifest and bytecode identities stay separate. */
    hash = hash_mem((const uint8_t *)k, sizeof *k);
    h = (uint32_t)(hash ^ (hash >> 32));
    for (i = 0; i < PS_SPEC_SLOTS; i++) {
        PsSpec *e = &s_spec[(h + i) & (PS_SPEC_SLOTS - 1)];
        if (!InterlockedCompareExchange(&e->state, 0, 0)) return e;
        if (!memcmp(&e->key, k, sizeof *k)) { last = e; return e; }
    }
    return NULL;
}

/* Called only by the render thread, or during init before workers exist.
 * Worker threads never insert/change keys, so the lock-free lookup is safe. */
static ID3D11PixelShader *spec_request(const PsKey *k, int prewarm)
{
    PsSpec *e = spec_find(k);
    LONG state;
    if (!e) return NULL;
    state = InterlockedCompareExchange(&e->state, 0, 0);
    if (state == 2) return e->ps;
    if (state == 3 || state == 4) return NULL;
    EnterCriticalSection(&s_spec_cs);
    if (!state) {
        if ((prewarm && s_spec_warm_head < NV_PS_MANIFEST_LIMIT) ||
            (!prewarm && s_spec_head - s_spec_tail < PS_SPEC_QUEUE)) {
            e->key = *k;
            e->prewarm = prewarm;
            e->promoted = 0;
            InterlockedExchange(&e->state, 1);
            if (prewarm) s_spec_warm[s_spec_warm_head++] = e;
            else s_spec_q[s_spec_head++ % PS_SPEC_QUEUE] = e;
            WakeConditionVariable(&s_spec_cv);
        }
    } else if (!prewarm && e->prewarm && !e->promoted &&
               e->state == 1 && s_spec_head - s_spec_tail < PS_SPEC_QUEUE) {
        /* Keep the old low-priority pointer; claiming under the lock makes it
         * harmless when another worker later encounters it. */
        e->promoted = 1;
        s_spec_q[s_spec_head++ % PS_SPEC_QUEUE] = e;
        WakeConditionVariable(&s_spec_cv);
    }
    LeaveCriticalSection(&s_spec_cs);
    return NULL;
}

/* s_spec_cs is held. Live work always wins, and each entry is claimed once. */
static PsSpec *spec_claim(void)
{
    PsSpec *e;
    while (s_spec_tail != s_spec_head) {
        e = s_spec_q[s_spec_tail++ % PS_SPEC_QUEUE];
        if (e->state == 1) { InterlockedExchange(&e->state, 4); return e; }
    }
    while (s_spec_warm_tail != s_spec_warm_head) {
        e = s_spec_warm[s_spec_warm_tail++];
        if (e->state == 1 && !e->promoted) { InterlockedExchange(&e->state, 4); return e; }
    }
    return NULL;
}

static void spec_remember(const PsKey *k)
{
    unsigned i;
    if (!s_spec_manifest) return;
    EnterCriticalSection(&s_spec_cs);
    for (i = 0; i < s_spec_known_count; i++)
        if (!memcmp(&s_spec_known[i], k, sizeof *k)) break;
    if (i == s_spec_known_count && i < NV_PS_MANIFEST_LIMIT) {
        s_spec_known[s_spec_known_count++] = *k;
        s_spec_dirty++;
    }
    LeaveCriticalSection(&s_spec_cs);
}

static void spec_manifest_flush(void)
{
    uint32_t snapshot[NV_PS_MANIFEST_LIMIT][NV_PS_MANIFEST_WORDS];
    unsigned count, generation;
    ULONGLONG now;
    int ok;
    if (!s_spec_manifest) return;
    EnterCriticalSection(&s_spec_cs);
    now = GetTickCount64();
    if (s_spec_dirty == s_spec_saved || s_spec_saving || now - s_spec_last_save < 1000) {
        LeaveCriticalSection(&s_spec_cs); return;
    }
    s_spec_saving = 1;
    s_spec_last_save = now;
    generation = s_spec_dirty;
    count = s_spec_known_count;
    memcpy(snapshot, s_spec_known, count * sizeof(PsKey));
    LeaveCriticalSection(&s_spec_cs);
    /* Hashing and all file I/O stay on the existing low-priority workers. */
    ok = nv_ps_manifest_write(s_spec_manifest_path, s_spec_family, snapshot, count);
    EnterCriticalSection(&s_spec_cs);
    if (ok) s_spec_saved = generation;
    s_spec_saving = 0;
    LeaveCriticalSection(&s_spec_cs);
}

static void spec_hitch_report(const char *phase, int64_t start, uint32_t variant,
                              int cache_hit, HRESULT hr)
{
    int64_t end = hitch_trace_elapsed(start);
    double ms;
    if (!end) return;
    ms = (end - start) * 1000.0 / (double)s_hitch_freq.QuadPart;
    fprintf(stderr, "[HITCH] phase=%s qpc_ms=%.3f start_qpc_ms=%.3f duration_ms=%.3f thread=%lu variant=%u cache_hit=%d hr=0x%08lx\n",
            phase, end * 1000.0 / (double)s_hitch_freq.QuadPart,
            start * 1000.0 / (double)s_hitch_freq.QuadPart,
            ms, (unsigned long)GetCurrentThreadId(), variant, cache_hit, (unsigned long)hr);
}

static DWORD WINAPI spec_worker(LPVOID arg)
{
    (void)arg;
    for (;;) {
        PsSpec *e;
        const PsKey *k;
        char decl[2048];
        ID3DBlob *b;
        int flat, cache_hit;
        int64_t start;
        HRESULT hr;
        D3D_SHADER_MACRO m[4];
        EnterCriticalSection(&s_spec_cs);
        while (!(e = spec_claim())) {
            DWORD wait = s_spec_manifest && s_spec_dirty != s_spec_saved ? 1000 : INFINITE;
            if (!SleepConditionVariableCS(&s_spec_cv, &s_spec_cs, wait) && GetLastError() == ERROR_TIMEOUT) {
                LeaveCriticalSection(&s_spec_cs);
                spec_manifest_flush();
                EnterCriticalSection(&s_spec_cs);
            }
        }
        LeaveCriticalSection(&s_spec_cs);
        k = &e->key;
        snprintf(decl, sizeof decl,
            "static const uint4 c_icw[2] = { uint4(%uu,%uu,%uu,%uu), uint4(%uu,%uu,%uu,%uu) };"
            "static const uint4 a_icw[2] = { uint4(%uu,%uu,%uu,%uu), uint4(%uu,%uu,%uu,%uu) };"
            "static const uint4 c_ocw[2] = { uint4(%uu,%uu,%uu,%uu), uint4(%uu,%uu,%uu,%uu) };"
            "static const uint4 a_ocw[2] = { uint4(%uu,%uu,%uu,%uu), uint4(%uu,%uu,%uu,%uu) };"
            "static const uint fin0 = %uu; static const uint fin1 = %uu;"
            "static const uint rc_control = %uu; static const uint stage_prog = %uu;"
            "static const uint alpha_func = %uu; static const uint ps_flags = %uu;"
            "static const uint clip_plane = %uu; static const uint other_input = %uu;"
            "static const uint dot_map = %uu;"
            "static const uint4 tex_info[4] = { uint4(%uu,%uu,%uu,0), uint4(%uu,%uu,%uu,0),"
            " uint4(%uu,%uu,%uu,0), uint4(%uu,%uu,%uu,0) };",
            k->c_icw[0], k->c_icw[1], k->c_icw[2], k->c_icw[3],
            k->c_icw[4], k->c_icw[5], k->c_icw[6], k->c_icw[7],
            k->a_icw[0], k->a_icw[1], k->a_icw[2], k->a_icw[3],
            k->a_icw[4], k->a_icw[5], k->a_icw[6], k->a_icw[7],
            k->c_ocw[0], k->c_ocw[1], k->c_ocw[2], k->c_ocw[3],
            k->c_ocw[4], k->c_ocw[5], k->c_ocw[6], k->c_ocw[7],
            k->a_ocw[0], k->a_ocw[1], k->a_ocw[2], k->a_ocw[3],
            k->a_ocw[4], k->a_ocw[5], k->a_ocw[6], k->a_ocw[7],
            k->fin0, k->fin1, k->rc_control, k->stage_prog, k->alpha_func, k->ps_flags,
            k->clip_plane, k->other_input, k->dot_map,
            k->tex_info[0][0], k->tex_info[0][1], k->tex_info[0][2],
            k->tex_info[1][0], k->tex_info[1][1], k->tex_info[1][2],
            k->tex_info[2][0], k->tex_info[2][1], k->tex_info[2][2],
            k->tex_info[3][0], k->tex_info[3][1], k->tex_info[3][2]);
        flat = k->variant & 1;
        m[0].Name = "SPEC"; m[0].Definition = "1";
        m[1].Name = "SPEC_DECL"; m[1].Definition = decl;
        m[2].Name = flat ? "FLATQ" : NULL; m[2].Definition = flat ? "nointerpolation" : NULL;
        m[3].Name = NULL; m[3].Definition = NULL;
        start = hitch_trace_begin();
        b = compile_def_status(k->variant & 2 ? "ps_wdepth" : "ps_main", "ps_5_0", m, &cache_hit);
        spec_hitch_report("shader_compile_def", start, k->variant, cache_hit, b ? S_OK : E_FAIL);
        hr = E_FAIL;
        if (b) {
            start = hitch_trace_begin();
            hr = ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                              ID3D10Blob_GetBufferSize(b), NULL, &e->ps);
            spec_hitch_report("shader_create_pixel", start, k->variant, cache_hit, hr);
        }
        if (b && SUCCEEDED(hr)) {
            LONG n;
            InterlockedExchange(&e->state, 2);
            n = InterlockedIncrement(&s_spec_ready);
            if (!(n & (n - 1)))
                fprintf(stderr, "[D3D11] %ld specialised pixel shaders\n", (long)n);
            spec_remember(k);
        } else {
            InterlockedExchange(&e->state, 3);
        }
        if (b) ID3D10Blob_Release(b);
        spec_manifest_flush();
    }
}

static void spec_init(void)
{
    const char *v;
    unsigned workers = 0, i, count = 0;
    uint32_t records[NV_PS_MANIFEST_LIMIT][NV_PS_MANIFEST_WORDS];
    if (s_spec_enabled >= 0) return;
    v = getenv("RECOMP_PS_SPEC");
    s_spec_enabled = !(v && *v == '0');
    if (s_spec_enabled) s_spec = (PsSpec *)calloc(PS_SPEC_SLOTS, sizeof *s_spec);
    if (!s_spec) { s_spec_enabled = 0; return; }
    InitializeCriticalSection(&s_spec_cs);
    InitializeConditionVariable(&s_spec_cv);
    v = getenv("RECOMP_PS_PREWARM");
    if (!(v && *v == '0')) {
        spec_family(s_spec_family);
        s_spec_manifest = nv_ps_manifest_path(s_spec_family, s_spec_manifest_path);
        if (s_spec_manifest && nv_ps_manifest_read(s_spec_manifest_path, s_spec_family, records, &count)) {
            /* Variant controls entry-point and macro choice. Other words are
             * exact guest control words accepted by the existing lazy path. */
            for (i = 0; i < count; i++) if (records[i][41] > 3) break;
            if (i != count) count = 0;
            for (i = 0; i < count; i++) {
                PsKey k;
                memcpy(&k, records[i], sizeof k);
                spec_request(&k, 1);
                spec_remember(&k);
            }
            s_spec_saved = s_spec_dirty; /* Imported keys are already persistent. */
        }
    }
    /* Imports finish before threads or draw lookups can see the table. */
    for (i = 0; i < 2; i++) {
        HANDLE t = CreateThread(NULL, 0, spec_worker, NULL, CREATE_SUSPENDED, NULL);
        if (t) {
            SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
            if (ResumeThread(t) != (DWORD)-1) workers++;
            else TerminateThread(t, 0); /* It has never executed user code. */
            CloseHandle(t);
        }
    }
    if (!workers) {
        s_spec_enabled = 0;
        free(s_spec); s_spec = NULL;
        DeleteCriticalSection(&s_spec_cs);
    }
    if (workers && count)
        fprintf(stderr, "[D3D11] prewarming %u known pixel shader states\n", s_spec_known_count);
}

/* The specialised shader for this configuration, or NULL (not ready yet). */
static ID3D11PixelShader *spec_ps(const PSConsts *pc, uint32_t variant)
{
    PsKey k;
    uint32_t i;
    spec_init();
    if (!s_spec_enabled) return NULL;
    memset(&k, 0, sizeof k);
    memcpy(k.c_icw, pc->c_icw, sizeof k.c_icw);
    memcpy(k.a_icw, pc->a_icw, sizeof k.a_icw);
    memcpy(k.c_ocw, pc->c_ocw, sizeof k.c_ocw);
    memcpy(k.a_ocw, pc->a_ocw, sizeof k.a_ocw);
    k.fin0 = pc->fin0; k.fin1 = pc->fin1; k.rc_control = pc->rc_control;
    k.stage_prog = pc->stage_prog; k.alpha_func = pc->alpha_func; k.ps_flags = pc->ps_flags;
    k.clip_plane = pc->clip_plane; k.other_input = pc->other_input; k.dot_map = pc->dot_map;
    k.variant = variant;
    for (i = 0; i < 4; i++) {
        k.tex_info[i][0] = pc->tex_info[i][0]; k.tex_info[i][1] = pc->tex_info[i][1];
        k.tex_info[i][2] = pc->tex_info[i][2];
    }
    return spec_request(&k, 0);
}

static ID3D11Buffer *make_buffer(UINT bytes, UINT bind)
{
    D3D11_BUFFER_DESC d;
    ID3D11Buffer *b = NULL;
    memset(&d, 0, sizeof d);
    d.ByteWidth = (bytes + 15) & ~15u;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = bind;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &d, NULL, &b)))
        return NULL;
    return b;
}

static int upload_cb(ID3D11Buffer *b, const void *data, size_t bytes)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)b, 0,
                                         D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, data, bytes);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)b, 0);
        return 1;
    }
    return 0;
}

/* The pixel constant buffer has one writer and process-lifetime storage.
 * Repeated draws often keep exactly the same constants. Compare all bytes,
 * including dynamic factors/fog/alpha values, and remember only successful
 * uploads: a failed Map must retry even if the next draw has the same state. */
static PSConsts s_uploaded_pc;
static int s_uploaded_pc_valid;
static int upload_ps_constants(const PSConsts *pc)
{
    if (s_uploaded_pc_valid && !memcmp(&s_uploaded_pc, pc, sizeof *pc)) {
        if (s_prof_on > 0) s_prof_ps_reused++;
        return 1;
    }
    if (upload_cb(s_cb_ps, pc, sizeof *pc)) {
        memcpy(&s_uploaded_pc, pc, sizeof *pc);
        s_uploaded_pc_valid = 1;
        if (s_prof_on > 0) s_prof_ps_uploads++;
        return 1;
    }
    return 0;
}

int nv2a_d3d_init(void)
{
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr;
    ID3DBlob *b;
    const char *e;

    if (s_init_state)
        return s_init_state > 0;
    s_init_state = -1;
    nv2a_d3d_prof_enabled();
    /* Real pixels for the swap chain: a DPI-unaware thread sees the
     * window's client area scaled down and DWM stretches the result. */
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (getenv("RECOMP_D3D_DEBUG"))
        flags |= D3D11_CREATE_DEVICE_DEBUG;
    {
        /* The high-performance GPU, not the default adapter: on a hybrid
         * machine the default is the integrated one that drives the
         * display, and rendering there left a discrete GPU idle while each
         * frame took a hundred milliseconds. */
        IDXGIFactory6 *fac = NULL;
        IDXGIAdapter1 *ad = NULL;
        if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory6, (void **)&fac))) {
            if (FAILED(IDXGIFactory6_EnumAdapterByGpuPreference(fac, 0,
                    DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter1, (void **)&ad)))
                ad = NULL;
            IDXGIFactory6_Release(fac);
        }
        hr = D3D11CreateDevice((IDXGIAdapter *)ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                               NULL, flags, levels, 2, D3D11_SDK_VERSION, &s_dev, NULL, &s_ctx);
        if (ad) {
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(IDXGIAdapter1_GetDesc1(ad, &d)))
                fprintf(stderr, "[D3D11] adapter: %ls\n", d.Description);
            IDXGIAdapter1_Release(ad);
        }
    }
    if (FAILED(hr)) { fail("D3D11CreateDevice", hr); return 0; }

#define MAKE(entry, target, kind, out) \
    if (!(b = compile(entry, target))) return 0; \
    ID3D11Device_Create##kind(s_dev, ID3D10Blob_GetBufferPointer(b), \
                              ID3D10Blob_GetBufferSize(b), NULL, out); \
    ID3D10Blob_Release(b);
    {
        /* s_psv[flat | wdepth << 1]. Flat shading takes colours from one
         * vertex per primitive (see copy_indices for which one); wdepth
         * writes the w-buffer depth. */
        static const D3D_SHADER_MACRO flat[] = { { "FLATQ", "nointerpolation" }, { NULL, NULL } };
        int v;
        for (v = 0; v < 4; v++) {
            if (!(b = compile_def(v & 2 ? "ps_wdepth" : "ps_main", "ps_5_0", v & 1 ? flat : NULL)))
                return 0;
            ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                           ID3D10Blob_GetBufferSize(b), NULL, &s_psv[v]);
            ID3D10Blob_Release(b);
        }
    }
    MAKE("ub_vs", "vs_5_0", VertexShader, &s_ub_vs)
    MAKE("present_vs", "vs_5_0", VertexShader, &s_present_vs)
    MAKE("present_ps", "ps_5_0", PixelShader, &s_present_ps)
    MAKE("clear_vs", "vs_5_0", VertexShader, &s_clear_vs)
    MAKE("clear_ps", "ps_5_0", PixelShader, &s_clear_ps)
    MAKE("y16_vs", "vs_5_0", VertexShader, &s_y16_vs)
    MAKE("y16_ps", "ps_5_0", PixelShader, &s_y16_ps[0])
#undef MAKE
    {
        static const D3D_SHADER_MACRO ms[] = { { "Y16_MS", "1" }, { NULL, NULL } };
        int v;
        if (!(b = compile_def("y16_ps", "ps_5_0", ms))) return 0;
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                       ID3D10Blob_GetBufferSize(b), NULL, &s_y16_ps[1]);
        ID3D10Blob_Release(b);
        for (v = 0; v < 2; v++) {
            if (!(b = compile_def("y16d_ps", "ps_5_0", v ? ms : NULL))) return 0;
            ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                           ID3D10Blob_GetBufferSize(b), NULL, &s_y16d_ps[v]);
            ID3D10Blob_Release(b);
        }
    }

    {
        D3D11_BUFFER_DESC d;
        D3D11_SHADER_RESOURCE_VIEW_DESC v;
        D3D11_FEATURE_DATA_D3D11_OPTIONS o;
        memset(&d, 0, sizeof d);
        d.ByteWidth = VB_BYTES;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (SUCCEEDED(ID3D11Device_CreateBuffer(s_dev, &d, NULL, &s_raw))) {
            memset(&v, 0, sizeof v);
            v.Format = DXGI_FORMAT_R32_TYPELESS;
            v.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            v.BufferEx.NumElements = VB_BYTES / 4;
            v.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
            ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_raw, &v, &s_raw_srv);
        }
        /* Appending to a buffer a shader is reading needs D3D11.1's
         * no-overwrite on SRVs; without it every draw discards. */
        memset(&o, 0, sizeof o);
        if (SUCCEEDED(ID3D11Device_CheckFeatureSupport(s_dev, D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof o)))
            s_raw_nooverwrite = o.MapNoOverwriteOnDynamicBufferSRV;
    }
    s_ib = make_buffer(IB_BYTES, D3D11_BIND_INDEX_BUFFER);
    s_cb_ub = make_buffer(sizeof(UbConsts), D3D11_BIND_CONSTANT_BUFFER);
    s_cb_prog = make_buffer(136 * 16, D3D11_BIND_CONSTANT_BUFFER);
    s_cb_vconst = make_buffer(192 * 16, D3D11_BIND_CONSTANT_BUFFER);
    s_cb_ps = make_buffer(sizeof(PSConsts), D3D11_BIND_CONSTANT_BUFFER);
    s_cb_present = make_buffer(sizeof(PresentConsts), D3D11_BIND_CONSTANT_BUFFER);
    s_cb_clear = make_buffer(32, D3D11_BIND_CONSTANT_BUFFER);
    if (!s_raw_srv || !s_ib || !s_cb_ub || !s_cb_prog || !s_cb_vconst || !s_cb_ps || !s_cb_present || !s_cb_clear) {
        fail("buffer creation", E_FAIL);
        return 0;
    }
    {
        D3D11_SAMPLER_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(s_dev, &sd, &s_linear_clamp);
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        ID3D11Device_CreateSamplerState(s_dev, &sd, &s_point_clamp);
    }
    {
        D3D11_BLEND_DESC bd;
        D3D11_RASTERIZER_DESC rd;
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&bd, 0, sizeof bd);
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ID3D11Device_CreateBlendState(s_dev, &bd, &s_blend_opaque);
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.ScissorEnable = TRUE;
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs_plain);
        memset(&dd, 0, sizeof dd);
        ID3D11Device_CreateDepthStencilState(s_dev, &dd, &s_ds_off);
    }
    (void)e;
    display_config(1);
    if ((e = getenv("RECOMP_MSAA")) != NULL) s_msaa = (UINT)atoi(e);
    if ((e = getenv("RECOMP_ANISO")) != NULL) s_aniso = (UINT)atoi(e);
    if (s_aniso < 1) s_aniso = 1;
    if (s_aniso > 16) s_aniso = 16;
    if (s_msaa > 8) s_msaa = 8;
    for (; s_msaa > 1; s_msaa /= 2) {
        UINT qc = 0, qd = 0;
        ID3D11Device_CheckMultisampleQualityLevels(s_dev, DXGI_FORMAT_B8G8R8A8_UNORM, s_msaa, &qc);
        ID3D11Device_CheckMultisampleQualityLevels(s_dev, DXGI_FORMAT_D24_UNORM_S8_UINT, s_msaa, &qd);
        if (qc && qd) break;
    }
    if (s_msaa < 1) s_msaa = 1;
    spec_init();
    bind_fixed_resources();
    s_init_state = 1;
    fprintf(stderr, "[D3D11] renderer ready: %ux%u internal, display aspect %.3f, %ux MSAA, %ux AF\n",
            (uint32_t)(GUEST_W * s_scale * widen() + 0.5f), (uint32_t)(GUEST_H * s_scale + 0.5f),
            nv2a_d3d_display_aspect(), s_msaa, s_aniso);
    fflush(stderr);
    return 1;
}

int nv2a_d3d_active(void)
{
    return s_init_state > 0;
}

/* --------------------------------------------------------------- surfaces */

typedef struct {
    uint32_t phys, va, pitch, bpp, w, h, iw, ih;
    uint32_t color_fmt;             /* SURFACE_FORMAT colour code */
    float sx, sy;                   /* host pixels per guest pixel */
    int swizzled;
    ID3D11Texture2D *tex;           /* single-sample; what srv reads */
    ID3D11Texture2D *ms;            /* MSAA target rtv draws into, or NULL */
    int ms_dirty;                   /* ms drawn since the last resolve */
    ID3D11ShaderResourceView *ms_srv;
    uint32_t gen, y16_gen;          /* drawn count; when y16 was built */
    ID3D11Texture2D *y16;           /* LU_IMAGE_Y16 view (see y16_ps) */
    ID3D11Texture2D *mip;           /* mip-mapped copy for reductions */
    ID3D11ShaderResourceView *mip_srv;
    uint32_t mip_gen;
    ID3D11Texture2D *chain;         /* this target plus the levels rendered below it */
    ID3D11ShaderResourceView *chain_srv;
    int chain_levels;
    uint32_t chain_gen[16];
    ID3D11ShaderResourceView *y16_srv;
    ID3D11RenderTargetView *y16_rtv;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    uint8_t *shadow;                /* guest bytes as of the last sync */
    int gpu_dirty;                  /* drawn since guest memory was updated */
    int cpu_present_pending;        /* idle CPU update not yet shown successfully */
    uint32_t synced_frame, used_frame;
    uint32_t seq;                   /* bind order; see surface_find */
} Surface;

typedef struct {
    uint32_t phys, iw, ih, samples;
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
    ID3D11ShaderResourceView *zsrv;         /* depth as a texture (for y16) */
    ID3D11Texture2D *y16;                   /* LU_IMAGE_Y16 view (see y16d_ps) */
    ID3D11ShaderResourceView *y16_srv;
    ID3D11RenderTargetView *y16_rtv;
    uint32_t zf, w, h;                      /* zeta format code, guest size last bound with */
    uint32_t gen, y16_gen, seq;             /* as Surface */
    uint32_t used_frame;
} Depth;

#define MAX_SURFACES 32
#define MAX_DEPTHS 16
static Surface s_surf[MAX_SURFACES];
static Depth s_depth[MAX_DEPTHS];
static Surface *s_cur_rt;
/* Anti-aliased surfaces store 2x (CENTER_CORNER_2) or 2x2 (SQUARE_OFFSET_4)
 * samples per pixel of the coordinate space the title draws in. */
static uint32_t s_aa_x = 1, s_aa_y = 1;
static Depth *s_cur_ds;

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

static ID3D11Texture2D *make_texture_ms(UINT w, UINT h, UINT levels, UINT array,
                                        DXGI_FORMAT f, UINT bind, UINT misc, UINT samples)
{
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D *t = NULL;
    memset(&d, 0, sizeof d);
    d.Width = w ? w : 1;
    d.Height = h ? h : 1;
    d.MipLevels = levels;
    d.ArraySize = array;
    d.Format = f;
    d.SampleDesc.Count = samples;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    d.MiscFlags = misc;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &t)))
        return NULL;
    return t;
}

static ID3D11Texture2D *make_texture(UINT w, UINT h, UINT levels, UINT array,
                                     DXGI_FORMAT f, UINT bind, UINT misc)
{
    return make_texture_ms(w, h, levels, array, f, bind, misc, 1);
}

/* Bring a surface's single-sample texture up to date with its MSAA target
 * before anything reads tex/srv. */
static void surface_resolve(Surface *s)
{
    if (s->ms && s->ms_dirty) {
        ID3D11DeviceContext_ResolveSubresource(s_ctx, (ID3D11Resource *)s->tex, 0,
                                               (ID3D11Resource *)s->ms, 0,
                                               DXGI_FORMAT_B8G8R8A8_UNORM);
        s->ms_dirty = 0;
    }
}

static void unbind_targets(void)
{
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    s_cur_rt = NULL;
    s_cur_ds = NULL;
}

static void unbind_textures(void)
{
    ID3D11ShaderResourceView *none[8] = { 0 };
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 8, none);
}

/* Draw columns [u0, u1) of `srv` into columns [x0, x1) of `rtv` (fractions
 * of each width), full height. */
static int blit_span(ID3D11ShaderResourceView *srv, ID3D11ShaderResourceView *mask,
                      ID3D11RenderTargetView *rtv, UINT dst_w, UINT dst_h, float alpha,
                      float x0, float x1, float u0, float u1)
{
    PresentConsts pc;
    D3D11_VIEWPORT vp = { 0, 0, (float)dst_w, (float)dst_h, 0, 1 };
    D3D11_RECT sc = { 0, 0, (LONG)dst_w, (LONG)dst_h };
    memset(&pc, 0, sizeof pc);
    pc.dst_rect[0] = x0 * 2 - 1; pc.dst_rect[1] = 1; pc.dst_rect[2] = x1 * 2 - 1; pc.dst_rect[3] = -1;
    pc.src_uv[0] = u0; pc.src_uv[2] = u1; pc.src_uv[3] = 1;
    pc.src_size[0] = pc.src_size[1] = pc.src_size[2] = pc.src_size[3] = 1;
    pc.force_alpha[0] = alpha;
    pc.ovl_flags[3] = mask ? 1 : 0;
    if (!upload_cb(s_cb_present, &pc, sizeof pc)) return 0;
    unbind_textures();
    unbind_targets();
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sc);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs_plain);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, s_blend_opaque, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, s_ds_off, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    bind_vertex_shader(s_present_vs);
    bind_vertex_constants(s_cb_present);
    bind_pixel_shader(s_present_ps);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb_present);
    {
        ID3D11ShaderResourceView *views[2] = { srv, mask };
        ID3D11SamplerState *smp[2] = { s_linear_clamp, s_point_clamp };
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 2, views);
        bind_pixel_samplers(2, smp);
    }
    ID3D11DeviceContext_Draw(s_ctx, 4, 0);
    unbind_targets();
    unbind_textures();
    return 1;
}

static void blit(ID3D11ShaderResourceView *srv, ID3D11RenderTargetView *rtv,
                 UINT dst_w, UINT dst_h, float alpha)
{
    blit_span(srv, NULL, rtv, dst_w, dst_h, alpha, 0, 1, 0, 1);
}

/* Where a surface's guest pixels live in its host texture, as fractions of
 * the width: all of it, or on a widened surface the centred 4:3 region --
 * the same place 2D overlays are drawn -- so a CPU-drawn image keeps its
 * proportions and write-back reads back exactly what was uploaded. */
static void guest_span(const Surface *s, float *x0, float *x1)
{
    float f = s->sx > s->sy * 1.001f ? s->sy / s->sx : 1.0f;
    *x0 = (1.0f - f) * 0.5f;
    *x1 = (1.0f + f) * 0.5f;
}

/* Guest rows of a surface as BGRA, with the format's fixed alpha. */
static void surface_rows_to_bgra(const Surface *s, const uint8_t *src,
                                 uint32_t pitch, uint32_t *out)
{
    int amode = surface_alpha_mode(s->color_fmt);
    uint32_t x, y;
    for (y = 0; y < s->h; y++) {
        const uint8_t *row = src + (size_t)y * pitch;
        uint32_t *dst = out + (size_t)y * s->w;
        if (s->bpp == 4) {
            memcpy(dst, row, (size_t)s->w * 4);
            if (amode >= 0)
                for (x = 0; x < s->w; x++)
                    dst[x] = (dst[x] & 0xFFFFFFu) | (amode ? 0xFF000000u : 0);
        } else if (s->bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s->w; x++) {
                uint32_t v = p[x], r, g, b, a;
                if ((s->color_fmt & 0xF) == 0x3) {
                    r = d3d8_expand_channel((v >> 11) & 31, 5);
                    g = d3d8_expand_channel((v >> 5) & 63, 6);
                    b = d3d8_expand_channel(v & 31, 5);
                } else {
                    r = d3d8_expand_channel((v >> 10) & 31, 5);
                    g = d3d8_expand_channel((v >> 5) & 31, 5);
                    b = d3d8_expand_channel(v & 31, 5);
                }
                a = amode == 0 ? 0 : 255;
                dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s->w * 4);
        }
    }
}

/* Upload buffer for guest images, reused while the size holds. */
static ID3D11Texture2D *s_up_tex[2];
static ID3D11ShaderResourceView *s_up_srv[2];
static UINT s_up_w[2], s_up_h[2];
static uint32_t *s_up_pixels, *s_mask_pixels;
static size_t s_up_cap, s_mask_cap;
static uint8_t *s_swz_rows;
static size_t s_swz_cap;

/* slot 0: image, slot 1: change mask */
static ID3D11ShaderResourceView *upload_slot(int k, const uint32_t *pixels, UINT w, UINT h)
{
    if (!s_up_tex[k] || s_up_w[k] != w || s_up_h[k] != h) {
        if (s_up_srv[k]) ID3D11ShaderResourceView_Release(s_up_srv[k]);
        if (s_up_tex[k]) ID3D11Texture2D_Release(s_up_tex[k]);
        s_up_srv[k] = NULL;
        s_up_tex[k] = make_texture(w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM,
                                   D3D11_BIND_SHADER_RESOURCE, 0);
        if (!s_up_tex[k]) return NULL;
        s_up_w[k] = w; s_up_h[k] = h;
    }
    /* Resource creation can succeed while its view fails. Retry that view
     * without recreating the texture or publishing a failed upload. */
    if (!s_up_srv[k] && FAILED(ID3D11Device_CreateShaderResourceView(s_dev,
            (ID3D11Resource *)s_up_tex[k], NULL, &s_up_srv[k]))) return NULL;
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_up_tex[k], 0,
                                          NULL, pixels, w * 4, 0);
    return s_up_srv[k];
}

static ID3D11ShaderResourceView *upload_image(const uint32_t *pixels, UINT w, UINT h)
{
    return upload_slot(0, pixels, w, h);
}

static uint32_t *scratch_pixels(size_t count)
{
    if (count > s_up_cap) {
        free(s_up_pixels);
        s_up_pixels = (uint32_t *)malloc(count * 4);
        s_up_cap = s_up_pixels ? count : 0;
    }
    return s_up_pixels;
}

/* The shadow and change mask describe logical pixels. Swizzled targets
 * need the same CPU-write detection after undoing their Morton layout. */
static const uint8_t *surface_guest_rows(const Surface *s, uint32_t *pitch)
{
    size_t bytes;
    *pitch = s->pitch;
    if (!s->swizzled) return guest(s->va);
    *pitch = s->w * s->bpp;
    bytes = (size_t)*pitch * s->h;
    if (bytes > s_swz_cap) {
        free(s_swz_rows);
        s_swz_rows = (uint8_t *)malloc(bytes);
        s_swz_cap = s_swz_rows ? bytes : 0;
    }
    if (!s_swz_rows) return NULL;
    xbox_unswizzle_rect(s_swz_rows, guest(s->va), s->w, s->h, s->bpp);
    return s_swz_rows;
}

/* A single reusable pending shadow keeps CPU writes made during submission
 * visible to the next sync. Only successfully submitted bytes become the
 * surface shadow; swapping buffers avoids copying them again on commit. */
static uint8_t *s_sync_rows;
static size_t s_sync_cap;

/* Guest memory is the authority for what the CPU writes. A shadow copy of
 * each surface's guest bytes says which pixels the CPU changed since the last
 * look; only those replace what the GPU drew. A loading screen drawn by the
 * GPU and then animated by CPU writes keeps both. */
static void surface_sync_impl(Surface *s, int force)
{
    uint32_t rowb = s->w * s->bpp, y, x, x0 = 0xFFFFFFFFu, x1 = 0, y0 = 0xFFFFFFFFu, y1 = 0;
    uint32_t pitch;
    size_t bytes = (size_t)rowb * s->h, count = (size_t)s->w * s->h;
    uint32_t *px, *mask;
    const uint8_t *g;
    ID3D11ShaderResourceView *srv, *msk = NULL;
    float span0, span1;
    if (!s->bpp || !guest_ok(s->va, (size_t)s->pitch * s->h))
        return;
    if (!force && s->synced_frame == s_epoch)
        return;
    /* A forced sync can fail after an earlier successful sync in this epoch. */
    s->synced_frame = s_epoch - 1;
    g = surface_guest_rows(s, &pitch);
    if (!g) return;
    if (s->shadow) {
        /* The common unchanged path allocates and copies nothing. */
        for (y = 0; y < s->h; y++)
            if (memcmp(g + (size_t)y * pitch, s->shadow + (size_t)y * rowb, rowb))
                break;
        if (y == s->h) { s->synced_frame = s_epoch; return; }
    }
    if (bytes > s_sync_cap) {
        uint8_t *rows = (uint8_t *)realloc(s_sync_rows, bytes);
        if (!rows) return;
        s_sync_rows = rows;
        s_sync_cap = bytes;
    }
    for (y = 0; y < s->h; y++)
        memcpy(s_sync_rows + (size_t)y * rowb, g + (size_t)y * pitch, rowb);
    if (s->shadow) {
        if (s_mask_cap < count) {
            uint32_t *pixels = (uint32_t *)realloc(s_mask_pixels, count * 4);
            if (!pixels) return;
            s_mask_pixels = pixels;
            s_mask_cap = count;
        }
        memset(s_mask_pixels, 0, count * 4);
        for (y = 0; y < s->h; y++) {
            const uint8_t *row = s_sync_rows + (size_t)y * rowb;
            const uint8_t *sh = s->shadow + (size_t)y * rowb;
            if (!memcmp(row, sh, rowb)) continue;
            if (y0 == 0xFFFFFFFFu) y0 = y;
            y1 = y;
            mask = s_mask_pixels + (size_t)y * s->w;
            for (x = 0; x < s->w; x++)
                if (memcmp(row + x * s->bpp, sh + x * s->bpp, s->bpp)) {
                    mask[x] = 0xFFFFFFFFu;
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                }
        }
        if (y0 == 0xFFFFFFFFu) { s->synced_frame = s_epoch; return; }
        if (getenv("RECOMP_D3D_TRACE"))
            fprintf(stderr, "[D3D11] CPU wrote surface %08X (frame %u) %u,%u-%u,%u\n",
                    s->phys, s_frame, x0, y0, x1, y1);
    }
    px = scratch_pixels(count);
    if (!px) return;
    surface_rows_to_bgra(s, s_sync_rows, rowb, px);
    srv = upload_slot(0, px, s->w, s->h);
    if (!srv) return;
    if (s->shadow) {
        msk = upload_slot(1, s_mask_pixels, s->w, s->h);
        if (!msk) return;
    }
    guest_span(s, &span0, &span1);
    if (!blit_span(srv, msk, s->rtv, s->iw, s->ih, -1.0f, span0, span1, 0, 1))
        return;
    {
        uint8_t *old_shadow = s->shadow;
        s->shadow = s_sync_rows;
        s_sync_rows = old_shadow;
        s_sync_cap = old_shadow ? bytes : 0;
    }
    s->synced_frame = s_epoch;
    s->ms_dirty = 1;
    s->gen++;
}

static void surface_sync(Surface *s, int force)
{
    int64_t t0 = s_prof_on > 0 ? qpc() : 0;
    RECOMP_PROFILE_BEGIN("D3D11 surface sync");
    surface_sync_impl(s, force);
    RECOMP_PROFILE_END();
    if (s_prof_on > 0) s_prof[PROF_SYNC] += qpc() - t0;
}

static void writeback_drop_surface(Surface *s);

static void surface_release(Surface *s)
{
    writeback_drop_surface(s);
    if (s_cur_rt == s) unbind_targets();
    if (s->srv) ID3D11ShaderResourceView_Release(s->srv);
    if (s->rtv) ID3D11RenderTargetView_Release(s->rtv);
    if (s->ms) ID3D11Texture2D_Release(s->ms);
    if (s->ms_srv) ID3D11ShaderResourceView_Release(s->ms_srv);
    if (s->y16_srv) ID3D11ShaderResourceView_Release(s->y16_srv);
    if (s->y16_rtv) ID3D11RenderTargetView_Release(s->y16_rtv);
    if (s->y16) ID3D11Texture2D_Release(s->y16);
    if (s->mip_srv) ID3D11ShaderResourceView_Release(s->mip_srv);
    if (s->mip) ID3D11Texture2D_Release(s->mip);
    if (s->chain_srv) ID3D11ShaderResourceView_Release(s->chain_srv);
    if (s->chain) ID3D11Texture2D_Release(s->chain);
    if (s->tex) ID3D11Texture2D_Release(s->tex);
    free(s->shadow);
    memset(s, 0, sizeof *s);
}

/* Titles reuse one block of memory as targets of different shapes. Each shape keeps its own
 * host surface; the address resolves to the one bound most recently. */
static uint32_t s_surf_seq;

static Surface *surface_find(uint32_t p)
{
    Surface *best = NULL;
    int i;
    for (i = 0; i < MAX_SURFACES; i++)
        if (s_surf[i].tex && s_surf[i].phys == p && (!best || s_surf[i].seq > best->seq))
            best = &s_surf[i];
    return best;
}

static Surface *surface_get(uint32_t va, uint32_t pitch, uint32_t fmt,
                            uint32_t w, uint32_t h, int swizzled)
{
    uint32_t p = phys(va), bpp = surface_color_bpp(fmt);
    Surface *s = NULL, *victim = NULL;
    int i;
    for (i = 0; i < MAX_SURFACES; i++) {
        Surface *c = &s_surf[i];
        if (c->tex && c->phys == p && c->w >= w && c->h >= h && c->pitch == pitch &&
            c->bpp == bpp && c->swizzled == swizzled) {
            c->color_fmt = fmt;
            c->used_frame = s_frame;
            c->seq = ++s_surf_seq;
            return c;
        }
    }
    /* Same shape grown in place starts again from guest memory; another
     * shape at this address gets a surface of its own. */
    for (i = 0; i < MAX_SURFACES && !s; i++)
        if (s_surf[i].tex && s_surf[i].phys == p && s_surf[i].pitch == pitch &&
            s_surf[i].bpp == bpp && s_surf[i].swizzled == swizzled)
            s = &s_surf[i];
    if (s) {
        /* Grown or reformatted: start again from guest memory. */
        if (w < s->w && s->pitch == pitch) w = s->w;
        if (h < s->h && s->pitch == pitch) h = s->h;
        victim = s;
    } else {
        for (i = 0; i < MAX_SURFACES && !victim; i++)
            if (!s_surf[i].tex) victim = &s_surf[i];
        if (!victim)
            for (i = 0; i < MAX_SURFACES; i++)
                if (!victim || s_surf[i].used_frame < victim->used_frame)
                    victim = &s_surf[i];
        if (!victim) victim = &s_surf[0];
    }
    surface_release(victim);
    s = victim;
    s->phys = p; s->va = va; s->pitch = pitch; s->bpp = bpp;
    s->w = w; s->h = h; s->color_fmt = fmt; s->swizzled = swizzled;
    /* Display-sized surfaces carry the widescreen image; everything else
     * (environment maps, blur buffers, meters) keeps its aspect. */
    s->sy = s_scale;
    s->sx = (!swizzled && w == GUEST_W && h == GUEST_H) ? s_scale * widen() : s_scale;
    s->iw = (uint32_t)(w * s->sx + 0.5f);
    s->ih = (uint32_t)(h * s->sy + 0.5f);
    s->tex = make_texture(s->iw, s->ih, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM,
                          D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0);
    if (!s->tex) { fail("surface texture", E_FAIL); memset(s, 0, sizeof *s); return NULL; }
    /* Render-to-texture (swizzled) targets stay single-sample. */
    if (s_msaa > 1 && !swizzled)
        s->ms = make_texture_ms(s->iw, s->ih, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM,
                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, s_msaa);
    if (s->ms)
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s->ms, NULL, &s->ms_srv);
    ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)(s->ms ? s->ms : s->tex),
                                        NULL, &s->rtv);
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s->tex, NULL, &s->srv);
    s->used_frame = s_frame;
    s->seq = ++s_surf_seq;
    {
        float zero[4] = { 0, 0, 0, 0 };
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s->rtv, zero);
        s->ms_dirty = 1; s->gen++;
    }
    surface_sync(s, 1);
    if (getenv("RECOMP_D3D_TRACE"))
        fprintf(stderr, "[D3D11] surface %08X %ux%u pitch %u fmt %X%s\n", p, w, h,
                pitch, fmt & 0xF, swizzled ? " swizzled" : "");
    return s;
}

static void depth_release(Depth *d)
{
    if (d->y16_rtv) ID3D11RenderTargetView_Release(d->y16_rtv);
    if (d->y16_srv) ID3D11ShaderResourceView_Release(d->y16_srv);
    if (d->y16) ID3D11Texture2D_Release(d->y16);
    if (d->zsrv) ID3D11ShaderResourceView_Release(d->zsrv);
    if (d->dsv) ID3D11DepthStencilView_Release(d->dsv);
    if (d->tex) ID3D11Texture2D_Release(d->tex);
    memset(d, 0, sizeof *d);
}

/* The Z24S8 depth buffer most recently bound at p, or NULL. */
static Depth *depth_find(uint32_t p)
{
    Depth *best = NULL;
    int i;
    for (i = 0; i < MAX_DEPTHS; i++)
        if (s_depth[i].tex && s_depth[i].phys == p && s_depth[i].zf != 1 &&
            (!best || s_depth[i].seq > best->seq))
            best = &s_depth[i];
    return best;
}

static Depth *depth_get(uint32_t va, uint32_t iw, uint32_t ih, UINT samples)
{
    uint32_t p = phys(va);
    Depth *d, *victim = NULL;
    int i;
    for (i = 0; i < MAX_DEPTHS; i++) {
        d = &s_depth[i];
        if (d->tex && d->phys == p && d->iw == iw && d->ih == ih && d->samples == samples) {
            d->used_frame = s_frame;
            return d;
        }
    }
    for (i = 0; i < MAX_DEPTHS && !victim; i++)
        if (!s_depth[i].tex) victim = &s_depth[i];
    for (i = 0; i < MAX_DEPTHS; i++)
        if (!victim || s_depth[i].used_frame < victim->used_frame)
            victim = &s_depth[i];
    d = victim;
    if (s_cur_ds == d) unbind_targets();
    depth_release(d);
    /* Typeless so the title can read it back as a texture (depth_y16). */
    d->tex = make_texture_ms(iw, ih, 1, 1, DXGI_FORMAT_R24G8_TYPELESS,
                             D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 0, samples);
    if (!d->tex) return NULL;
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dv;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv;
        memset(&dv, 0, sizeof dv);
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = samples > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
        ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)d->tex, &dv, &d->dsv);
        memset(&sv, 0, sizeof sv);
        sv.ViewDimension = samples > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)d->tex, &sv, &d->zsrv);
    }
    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, d->dsv,
                                              D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                              1.0f, 0);
    d->phys = p; d->iw = iw; d->ih = ih; d->samples = samples; d->used_frame = s_frame;
    return d;
}

/* The colour surface and depth buffer a draw or clear writes, bound. Returns
 * the colour surface (may be NULL for a depth-only pass) via *out. */
static int bind_targets(const NvD3DState *st, int want_depth, Surface **out,
                        Depth **dout, uint32_t *gw, uint32_t *gh)
{
    uint32_t fmt = st->surface_format, w, h;
    int swizzled = ((fmt >> 8) & 0xF) == 2;
    Surface *s = NULL;
    Depth *d = NULL;
    uint32_t aa = (fmt >> 12) & 0xF;
    s_aa_x = aa ? 2 : 1;
    s_aa_y = aa == 2 ? 2 : 1;
    if (swizzled) {
        w = 1u << ((fmt >> 16) & 0xF);
        /* NV097 surface log-height is bits 24..31, after log-width 16..23.
         * Reading bit 20 collapsed a 0x07070228 surface to one row. */
        h = 1u << ((fmt >> 24) & 0xF);
        s_aa_x = s_aa_y = 1;
    } else {
        w = (st->clip_x + st->clip_w) * s_aa_x;
        h = (st->clip_y + st->clip_h) * s_aa_y;
    }
    if (!w || !h || w > 4096 || h > 4096)
        return 0;
    if (st->color_addr && surface_color_bpp(fmt) &&
        guest_ok(st->color_addr, (size_t)(swizzled ? w * surface_color_bpp(fmt) : st->color_pitch) * h)) {
        s = surface_get(st->color_addr, swizzled ? w * surface_color_bpp(fmt) : st->color_pitch,
                        fmt, w, h, swizzled);
        if (s) surface_sync(s, 0);
    }
    if (want_depth && st->zeta_addr && ((fmt >> 4) & 0xF)) {
        float wx = (w == GUEST_W && h == GUEST_H) ? s_scale * widen() : s_scale;
        uint32_t iw = s ? s->iw : (uint32_t)(w * wx + 0.5f);
        uint32_t ih = s ? s->ih : (uint32_t)(h * s_scale + 0.5f);
        UINT ns = s ? (s->ms ? s_msaa : 1) : (swizzled ? 1 : s_msaa);
        d = depth_get(st->zeta_addr, iw, ih, ns);
    }
    if (!s && !d)
        return 0;
    if (s && d && (d->iw != s->iw || d->ih != s->ih))
        d = NULL;
    if (s) { s->ms_dirty = 1; s->gen++; }
    if (d) { d->gen++; d->seq = ++s_surf_seq; d->zf = (fmt >> 4) & 0xF; d->w = w; d->h = h; }
    if (s != s_cur_rt || d != s_cur_ds) {
        ID3D11RenderTargetView *rtv = s ? s->rtv : NULL;
        unbind_textures();
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, s ? 1 : 0, s ? &rtv : NULL,
                                               d ? d->dsv : NULL);
        s_cur_rt = s;
        s_cur_ds = d;
    }
    {
        D3D11_VIEWPORT vp;
        vp.TopLeftX = 0; vp.TopLeftY = 0;
        vp.Width = (float)(s ? s->iw : d->iw);
        vp.Height = (float)(s ? s->ih : d->ih);
        vp.MinDepth = 0; vp.MaxDepth = 1;
        ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    }
    s_cur_sx = s ? s->sx : (float)d->iw / (float)w;
    s_cur_sy = s ? s->sy : (float)d->ih / (float)h;
    *out = s; *dout = d;
    *gw = (s ? s->w : w) / s_aa_x;
    *gh = (s ? s->h : h) / s_aa_y;
    return 1;
}

/* --------------------------------------------------------------- textures */

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
static uint32_t texel(uint32_t c, const uint8_t *p, const uint32_t *pal)
{
    uint32_t v;
    switch (c) {
    case 0x00: case 0x13: return argb(255, p[0], p[0], p[0]);           /* Y8  */
    case 0x01: case 0x1B: return argb(p[0], p[0], p[0], p[0]);          /* AY8 */
    case 0x19: case 0x1F: return argb(p[0], 255, 255, 255);             /* A8  */
    case 0x0B: return pal ? pal[p[0]] : argb(255, p[0], p[0], p[0]);    /* I8  */
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
    case 0x2C: case 0x2D: case 0x30: case 0x31: case 0x32: case 0x35:
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

typedef struct TexEntry {
    uint32_t phys, format, pitch, w, h, palette, palette_len, used;
    uint64_t hash, pal_hash;
    uint32_t checked_epoch;
    size_t bytes;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    struct TexEntry *next;
} TexEntry;

#define TEX_BUCKETS 1024
static TexEntry *s_tex_bucket[TEX_BUCKETS];
static size_t s_tex_bytes, s_tex_count;
#define TEX_BUDGET ((size_t)768 << 20)

/* Where each level/face lives, and the span to hash. */
typedef struct {
    int kind, bpp, levels, faces;
    uint32_t level_off[16], level_w[16], level_h[16];
    size_t face_stride, total;
} TexLayout;

/* Opt-in stream diagnostics: cache hits stay quiet, while real uploads,
 * rejected resources and allocation failures carry a common host timestamp.
 * Renew the budget each second so startup cannot consume a whole drive's log. */
static int s_tex_stream_trace = -1;
static uint64_t s_tex_trace_window, s_tex_trace_evictions;
static unsigned s_tex_trace_emitted, s_tex_trace_dropped;
static LARGE_INTEGER s_tex_trace_freq;

static int texture_trace_enabled(void)
{
    if (s_tex_stream_trace < 0) {
        const char *value = getenv("RECOMP_TEXTURE_STREAM_TRACE");
        s_tex_stream_trace = value && !strcmp(value, "1");
        if (s_tex_stream_trace) {
            QueryPerformanceFrequency(&s_tex_trace_freq);
            fprintf(stderr, "[TEXTURE_TRACE] enabled tick_ms=%llu budget=80/sec\n",
                    (unsigned long long)GetTickCount64());
            fflush(stderr);
        }
    }
    return s_tex_stream_trace;
}

static void texture_trace_event(const NvD3DTexture *t, const char *event,
                                size_t bytes, int levels, int host_mips,
                                uint64_t previous, uint64_t content, int64_t start)
{
    uint64_t now;
    double duration = 0;
    if (!texture_trace_enabled()) return;
    now = GetTickCount64();
    if (!s_tex_trace_window || now - s_tex_trace_window >= 1000) {
        if (s_tex_trace_window) {
            fprintf(stderr, "[TEXTURE_TRACE_SUMMARY] tick_ms=%llu emitted=%u dropped=%u cache_count=%zu cache_bytes=%zu evictions=%llu\n",
                    (unsigned long long)now, s_tex_trace_emitted, s_tex_trace_dropped,
                    s_tex_count, s_tex_bytes, (unsigned long long)s_tex_trace_evictions);
            fflush(stderr);
        }
        s_tex_trace_window = now;
        s_tex_trace_emitted = s_tex_trace_dropped = 0;
    }
    if (s_tex_trace_emitted >= 80) { ++s_tex_trace_dropped; return; }
    ++s_tex_trace_emitted;
    if (start && s_tex_trace_freq.QuadPart)
        duration = (double)(qpc() - start) * 1000.0 / (double)s_tex_trace_freq.QuadPart;
    fprintf(stderr, "[TEXTURE_STREAM] tick_ms=%llu frame=%u epoch=%u event=%s addr=%08X phys=%08X size=%ux%u format=%08X palette=%08X guest_bytes=%zu guest_mips=%d host_mips=%d previous=%016llX content=%016llX duration_ms=%.3f cache_count=%zu cache_bytes=%zu evictions=%llu\n",
            (unsigned long long)now, s_frame, s_epoch, event, t->addr, phys(t->addr),
            t->width, t->height, t->format, t->palette, bytes, levels, host_mips,
            (unsigned long long)previous, (unsigned long long)content, duration,
            s_tex_count, s_tex_bytes, (unsigned long long)s_tex_trace_evictions);
}

static int tex_layout(const NvD3DTexture *t, TexLayout *L)
{
    uint32_t w = t->width, h = t->height, lv, maxlv = 1, m;
    size_t off = 0;
    memset(L, 0, sizeof *L);
    L->bpp = tex_format(t->color, &L->kind);
    if (!L->bpp || !w || !h || w > 4096 || h > 4096)
        return 0;
    L->faces = t->cube ? 6 : 1;
    if (L->kind == K_LIN) {
        if (!t->pitch) return 0;
        L->levels = 1;
        L->level_w[0] = w; L->level_h[0] = h;
        L->total = L->face_stride = (size_t)t->pitch * h;
        L->faces = 1;
        return 1;
    }
    for (m = w > h ? w : h; m > 1; m >>= 1) maxlv++;
    L->levels = t->levels ? (int)t->levels : 1;
    if (L->levels > (int)maxlv) L->levels = (int)maxlv;
    for (lv = 0; lv < (uint32_t)L->levels; lv++) {
        uint32_t lw = w >> lv ? w >> lv : 1, lh = h >> lv ? h >> lv : 1;
        L->level_off[lv] = (uint32_t)off;
        L->level_w[lv] = lw; L->level_h[lv] = lh;
        off += L->kind == K_DXT ? (size_t)((lw + 3) / 4) * ((lh + 3) / 4) * L->bpp
                                : (size_t)lw * lh * L->bpp;
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
                    dst[x] = texel(t->color, row + (size_t)x * L->bpp, pal);
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
                dst[x] = texel(t->color, src + (size_t)(oy + ox) * L->bpp, pal);
                ox = (ox - mx) & mx;
            }
            oy = (oy - my) & my;
        }
    }
}

static void tex_free(TexEntry *e)
{
    if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
    if (e->tex) ID3D11Texture2D_Release(e->tex);
    s_tex_bytes -= e->bytes;
    s_tex_count--;
    free(e);
}

static void tex_evict(void)
{
    /* Least recently used, until under budget. */
    while (s_tex_bytes > TEX_BUDGET) {
        TexEntry **best = NULL, **pp;
        int b;
        for (b = 0; b < TEX_BUCKETS; b++)
            for (pp = &s_tex_bucket[b]; *pp; pp = &(*pp)->next)
                if ((*pp)->used != s_frame && (!best || (*pp)->used < (*best)->used))
                    best = pp;
        if (!best) return;
        {
            TexEntry *e = *best;
            *best = e->next;
            if (s_tex_stream_trace > 0) ++s_tex_trace_evictions;
            tex_free(e);
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
    if (L->levels < 2) return L->levels;
    while (m > 1) { m >>= 1; n++; }
    return n > L->levels ? n : L->levels;
}

/* Guest DXT blocks already have the BC1/2/3 layout. Keep short chains in BGRA
 * so their generated mips retain the same filtering; BC base dimensions must
 * be multiples of four, while the smaller mips still occupy complete blocks. */
static DXGI_FORMAT texture_host_format(const NvD3DTexture *t, const TexLayout *L, int hl)
{
    static const DXGI_FORMAT formats[3] = {
        DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC3_UNORM
    };
    static UINT support[3];
    static int checked[3];
    UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
    int index;
    if (L->kind != K_DXT || hl != L->levels ||
        (t->width & 3) || (t->height & 3))
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    index = t->color == 0x0C ? 0 : t->color == 0x0E ? 1 : 2;
    if (!checked[index]) {
        if (FAILED(ID3D11Device_CheckFormatSupport(s_dev, formats[index], &support[index])))
            support[index] = 0;
        checked[index] = 1;
    }
    if (t->cube) required |= D3D11_FORMAT_SUPPORT_TEXTURECUBE;
    return (support[index] & required) == required ? formats[index] : DXGI_FORMAT_B8G8R8A8_UNORM;
}

static ID3D11ShaderResourceView *texture_get_impl(const NvD3DTexture *t, int *guest_levels)
{
    TexLayout L;
    uint32_t p = phys(t->addr), pal_p = t->palette;
    uint32_t key = (p >> 7) ^ (t->format * 2654435761u) ^ (t->pitch << 3) ^ t->width;
    TexEntry *e, **head = &s_tex_bucket[key & (TEX_BUCKETS - 1)];
    uint64_t h, ph = 0, previous = 0;
    uint32_t *pal = NULL, *px = NULL;
    DXGI_FORMAT host_format;
    int hl, is_new, native;
    int64_t trace_start = 0;
    const char *failure = "cache_allocation_failed";

    if (guest_levels) *guest_levels = 1;
    if (!tex_layout(t, &L)) {
        texture_trace_event(t, "invalid_layout", 0, 0, 0, 0, 0, 0);
        return NULL;
    }
    if (guest_levels) *guest_levels = L.levels;
    if (!guest_ok(t->addr, L.total)) {
        texture_trace_event(t, "invalid_guest_span", L.total, L.levels, 0, 0, 0, 0);
        return NULL;
    }
    for (e = *head; e; e = e->next)
        if (e->phys == p && e->format == t->format && e->pitch == t->pitch &&
            e->w == t->width && e->h == t->height && e->palette == pal_p &&
            e->palette_len == t->palette_len)
            break;
    /* The CPU can refill streamed textures or palettes between submissions
     * before a flip. Recheck once per kick, as surface CPU synchronization
     * does; repeated draws in that submission still reuse the same check. */
    if (e && e->checked_epoch == s_epoch) {
        e->used = s_frame;
        return e->srv;
    }
    h = hash_mem(guest(t->addr), L.total);
    if (t->color == 0x0B && pal_p && guest_ok(pal_p, t->palette_len * 4)) {
        pal = (uint32_t *)guest(pal_p);
        ph = hash_mem((const uint8_t *)pal, t->palette_len * 4);
    }
    if (e && e->hash == h && e->pal_hash == ph) {
        e->checked_epoch = s_epoch;
        e->used = s_frame;
        return e->srv;
    }
    if (texture_trace_enabled()) trace_start = qpc();
    if (e) previous = e->hash;
    /* Prepare the whole upload before creating/publishing a cache entry or
     * changing an existing resource. Missing generated mips otherwise stay
     * uninitialized while the successful content hash suppresses retries. */
    hl = host_levels(&L, t);
    host_format = texture_host_format(t, &L, hl);
    native = host_format != DXGI_FORMAT_B8G8R8A8_UNORM;
    if (!native) {
        px = scratch_pixels((size_t)t->width * t->height);
        if (!px) {
            texture_trace_event(t, "scratch_allocation_failed", L.total, L.levels, hl,
                                previous, h, trace_start);
            if (e) e->used = s_frame;
            return e ? e->srv : NULL;
        }
    }
    is_new = !e;
    if (!e) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        HRESULT hr;
        e = (TexEntry *)calloc(1, sizeof *e);
        if (!e) goto upload_failed;
        failure = "texture_creation_failed";
        e->tex = make_texture(t->width, t->height, (UINT)hl, (UINT)L.faces,
                              host_format, D3D11_BIND_SHADER_RESOURCE,
                              L.faces == 6 ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0);
        if (!e->tex) goto upload_failed;
        memset(&sd, 0, sizeof sd);
        sd.Format = host_format;
        if (L.faces == 6) {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            sd.TextureCube.MipLevels = (UINT)hl;
        } else {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = (UINT)hl;
        }
        hr = ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)e->tex, &sd, &e->srv);
        if (FAILED(hr) || !e->srv) {
            failure = "texture_view_failed";
            fail("texture view", FAILED(hr) ? hr : E_FAIL);
            goto upload_failed;
        }
        e->phys = p; e->format = t->format; e->pitch = t->pitch;
        e->w = t->width; e->h = t->height; e->palette = pal_p;
        e->palette_len = t->palette_len;
        {
            uint32_t w = t->width, ht = t->height;
            int lv;
            for (lv = 0; lv < hl; lv++) {
                e->bytes += native ? (size_t)((w + 3) / 4) * ((ht + 3) / 4) * L.bpp * L.faces
                                   : (size_t)w * ht * 4 * L.faces;
                if (w > 1) w /= 2;
                if (ht > 1) ht /= 2;
            }
        }
    }
    {
        int f, lv;
        for (f = 0; f < L.faces; f++) {
            uint32_t w, h;
            for (lv = 0; lv < L.levels; lv++) {
                const uint8_t *src = guest(t->addr) + f * L.face_stride + L.level_off[lv];
                if (native) {
                    ID3D11DeviceContext_UpdateSubresource(
                        s_ctx, (ID3D11Resource *)e->tex,
                        (UINT)lv + (UINT)f * (UINT)hl, NULL,
                        src, ((L.level_w[lv] + 3) / 4) * L.bpp, 0);
                } else {
                    decode_level(t, &L, src, lv, pal, px);
                    ID3D11DeviceContext_UpdateSubresource(
                        s_ctx, (ID3D11Resource *)e->tex,
                        (UINT)lv + (UINT)f * (UINT)hl, NULL,
                        px, L.level_w[lv] * 4, 0);
                }
            }
            if (hl <= L.levels) continue;
            w = L.level_w[L.levels - 1]; h = L.level_h[L.levels - 1];
            /* UpdateSubresource has copied the supplied pixels on return.
             * Continue shrinking the decoded last guest mip in its existing
             * scratch space, avoiding two allocations and a copy per upload. */
            for (; lv < hl; lv++) {
                halve_bgra(px, w, h, px);
                if (w > 1) w /= 2;
                if (h > 1) h /= 2;
                ID3D11DeviceContext_UpdateSubresource(
                    s_ctx, (ID3D11Resource *)e->tex,
                    (UINT)lv + (UINT)f * (UINT)hl, NULL, px, w * 4, 0);
            }
        }
    }
    e->hash = h;
    e->pal_hash = ph;
    e->checked_epoch = s_epoch;
    e->used = s_frame;
    if (is_new) {
        e->next = *head;
        *head = e;
        s_tex_bytes += e->bytes;
        s_tex_count++;
    }
    tex_evict();
    texture_trace_event(t, is_new ? "new_upload" : "content_refresh", L.total,
                        L.levels, hl, previous, h, trace_start);
    return e->srv;

upload_failed:
    texture_trace_event(t, failure, L.total, L.levels, hl, previous, h, trace_start);
    if (e) {
        if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
        if (e->tex) ID3D11Texture2D_Release(e->tex);
        free(e);
    }
    return NULL;
}

static ID3D11ShaderResourceView *texture_get_with_levels(const NvD3DTexture *t, int *guest_levels)
{
    int64_t t0 = s_prof_on > 0 ? qpc() : 0;
    ID3D11ShaderResourceView *v;
    RECOMP_PROFILE_BEGIN("D3D11 texture");
    v = texture_get_impl(t, guest_levels);
    RECOMP_PROFILE_END();
    if (s_prof_on > 0) s_prof[PROF_TEXTURE] += qpc() - t0;
    return v;
}

static ID3D11ShaderResourceView *texture_get(const NvD3DTexture *t)
{
    return texture_get_with_levels(t, NULL);
}

/* -------------------------------------------------------- state objects */

typedef struct { uint64_t key; void *obj; } StateSlot;
#define STATE_SLOTS 512
static StateSlot s_blend_cache[STATE_SLOTS], s_ds_cache[STATE_SLOTS],
                 s_rs_cache[STATE_SLOTS], s_samp_cache[STATE_SLOTS];

static void **state_slot(StateSlot *tab, uint64_t key)
{
    uint32_t i = (uint32_t)(key ^ (key >> 29) ^ (key >> 47)) * 2654435761u;
    int n;
    for (n = 0; n < STATE_SLOTS; n++) {
        StateSlot *s = &tab[(i + n) & (STATE_SLOTS - 1)];
        if (!s->obj || s->key == key) {
            s->key = key;
            return &s->obj;
        }
    }
    /* Full: evict the home slot. A bound state keeps its own context ref. */
    {
        StateSlot *s = &tab[i & (STATE_SLOTS - 1)];
        IUnknown_Release((IUnknown *)s->obj);
        s->obj = NULL;
        s->key = key;
        return &s->obj;
    }
}

static D3D11_BLEND blend_factor(uint32_t f, int alpha)
{
    switch (f) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default: return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND_OP blend_op(uint32_t e)
{
    switch (e) {
    /* F005/F006 are the signed (reverse-subtract / add) equations; the
     * signed range isn't emulated, and their nominal ops wash out a title's
     * daytime scenes, so they keep the mapping that matches its output. */
    case 0x800A: case 0xF005: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: case 0xF006: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default: return D3D11_BLEND_OP_ADD;
    }
}

static ID3D11BlendState *blend_state(int enable, uint32_t sf, uint32_t df,
                                     uint32_t eq, uint32_t mask)
{
    uint64_t key = (uint64_t)(enable != 0) | ((uint64_t)(mask & 15) << 1) |
                   ((uint64_t)(sf & 0xFFFF) << 5) | ((uint64_t)(df & 0xFFFF) << 21) |
                   ((uint64_t)(eq & 0xFFFF) << 37);
    void **slot = state_slot(s_blend_cache, key);
    if (!*slot) {
        D3D11_BLEND_DESC d;
        D3D11_RENDER_TARGET_BLEND_DESC *r = &d.RenderTarget[0];
        memset(&d, 0, sizeof d);
        r->BlendEnable = enable ? TRUE : FALSE;
        r->SrcBlend = blend_factor(sf, 0);
        r->DestBlend = blend_factor(df, 0);
        r->SrcBlendAlpha = blend_factor(sf, 1);
        r->DestBlendAlpha = blend_factor(df, 1);
        r->BlendOp = r->BlendOpAlpha = blend_op(eq);
        r->RenderTargetWriteMask = (UINT8)mask;
        ID3D11Device_CreateBlendState(s_dev, &d, (ID3D11BlendState **)slot);
    }
    return (ID3D11BlendState *)*slot;
}

static D3D11_COMPARISON_FUNC cmp(uint32_t f)
{
    switch (f) {
    case 0x200: return D3D11_COMPARISON_NEVER;
    case 0x201: return D3D11_COMPARISON_LESS;
    case 0x202: return D3D11_COMPARISON_EQUAL;
    case 0x203: return D3D11_COMPARISON_LESS_EQUAL;
    case 0x204: return D3D11_COMPARISON_GREATER;
    case 0x205: return D3D11_COMPARISON_NOT_EQUAL;
    case 0x206: return D3D11_COMPARISON_GREATER_EQUAL;
    default: return D3D11_COMPARISON_ALWAYS;
    }
}

static D3D11_STENCIL_OP sop(uint32_t o)
{
    switch (o) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default: return D3D11_STENCIL_OP_KEEP;
    }
}

static ID3D11DepthStencilState *ds_state(int zt, uint32_t zf, int zw, int st,
                                         uint32_t sf, uint32_t rmask, uint32_t wmask,
                                         uint32_t fail_, uint32_t zfail, uint32_t zpass)
{
    uint64_t key = (uint64_t)(zt != 0) | ((uint64_t)(zw != 0) << 1) |
                   ((uint64_t)(st != 0) << 2) | ((uint64_t)(zf & 7) << 3) |
                   ((uint64_t)(sf & 7) << 6) | ((uint64_t)(rmask & 0xFF) << 9) |
                   ((uint64_t)(wmask & 0xFF) << 17) |
                   ((uint64_t)sop(fail_) << 25) | ((uint64_t)sop(zfail) << 29) |
                   ((uint64_t)sop(zpass) << 33);
    void **slot = state_slot(s_ds_cache, key);
    if (!*slot) {
        D3D11_DEPTH_STENCIL_DESC d;
        memset(&d, 0, sizeof d);
        d.DepthEnable = zt ? TRUE : FALSE;
        d.DepthWriteMask = zw ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc = cmp(0x200 | (zf & 7));
        d.StencilEnable = st ? TRUE : FALSE;
        d.StencilReadMask = (UINT8)rmask;
        d.StencilWriteMask = (UINT8)wmask;
        d.FrontFace.StencilFunc = cmp(0x200 | (sf & 7));
        d.FrontFace.StencilFailOp = sop(fail_);
        d.FrontFace.StencilDepthFailOp = sop(zfail);
        d.FrontFace.StencilPassOp = sop(zpass);
        d.BackFace = d.FrontFace;
        ID3D11Device_CreateDepthStencilState(s_dev, &d, (ID3D11DepthStencilState **)slot);
    }
    return (ID3D11DepthStencilState *)*slot;
}

static ID3D11RasterizerState *rs_state(int cull, int front_ccw, int bias, float slope)
{
    uint32_t sb;
    uint64_t key;
    void **slot;
    memcpy(&sb, &slope, 4);
    key = (uint64_t)(cull & 3) | ((uint64_t)(front_ccw != 0) << 2) |
          ((uint64_t)(uint32_t)bias << 3) ^ ((uint64_t)sb << 30);
    slot = state_slot(s_rs_cache, key);
    if (!*slot) {
        D3D11_RASTERIZER_DESC d;
        memset(&d, 0, sizeof d);
        d.FillMode = D3D11_FILL_SOLID;
        d.CullMode = cull == 1 ? D3D11_CULL_FRONT : cull == 2 ? D3D11_CULL_BACK : D3D11_CULL_NONE;
        d.FrontCounterClockwise = front_ccw ? TRUE : FALSE;
        d.DepthBias = bias;
        d.SlopeScaledDepthBias = slope;
        d.DepthClipEnable = FALSE;
        d.ScissorEnable = TRUE;
        ID3D11Device_CreateRasterizerState(s_dev, &d, (ID3D11RasterizerState **)slot);
    }
    return (ID3D11RasterizerState *)*slot;
}

static D3D11_TEXTURE_ADDRESS_MODE addr_mode(uint32_t m)
{
    switch (m) {
    case 1: return D3D11_TEXTURE_ADDRESS_WRAP;
    case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

typedef struct SamplerRequest {
    uint32_t filter, address, control0, border;
    int levels, reduce;
    uint64_t key;
    StateSlot *slot;
} SamplerRequest;
static SamplerRequest s_sampler_request[4];

static ID3D11SamplerState *sampler_request(const NvD3DTexture *t, int levels, int reduce,
                                         SamplerRequest *request)
{
    uint32_t minf = (t->filter >> 16) & 0xFF, magf = (t->filter >> 24) & 0xF;
    int bias13 = (int)(t->filter & 0x1FFF);
    uint64_t key;
    void **slot;
    /* Retain the table slot, rather than its COM pointer: a full state cache
     * can evict that object between two uses of this stage. Compare integer
     * inputs before constructing the key; descriptor floats stay unchanged. */
    if (request && request->slot && request->slot->obj &&
        request->slot->key == request->key && request->filter == t->filter &&
        request->address == t->address && request->control0 == t->control0 &&
        request->border == t->border && request->levels == levels && request->reduce == reduce)
        return (ID3D11SamplerState *)request->slot->obj;
    if (bias13 & 0x1000) bias13 -= 0x2000;
    key = (uint64_t)minf | ((uint64_t)magf << 8) | ((uint64_t)(bias13 & 0x1FFF) << 12) |
          ((uint64_t)(levels & 15) << 25) | ((uint64_t)(reduce != 0) << 29) |
          ((uint64_t)(t->address & 0xFFFFF) << 31);
    key ^= ((uint64_t)((t->control0 >> 6) & 0xFFFFFF) * 0xFF51AFD7ED558CCDull) ^
           ((uint64_t)t->border * 0xC4CEB9FE1A85EC53ull);
    slot = state_slot(s_samp_cache, key);
    if (!*slot) {
        D3D11_SAMPLER_DESC d;
        int min_lin = minf == 2 || minf == 4 || minf == 6;
        int mip_lin = minf == 5 || minf == 6;
        int mag_lin = magf != 1;
        memset(&d, 0, sizeof d);
        d.Filter = (D3D11_FILTER)((min_lin ? 0x10 : 0) | (mag_lin ? 0x4 : 0) | (mip_lin ? 0x1 : 0));
        d.AddressU = addr_mode(t->address & 0xF);
        d.AddressV = addr_mode((t->address >> 8) & 0xF);
        d.AddressW = addr_mode((t->address >> 16) & 0xF);
        if (min_lin && mag_lin && s_aniso > 1) d.Filter = D3D11_FILTER_ANISOTROPIC;
        d.MipLODBias = (float)bias13 / 256.0f;
        d.MaxAnisotropy = s_aniso;
        d.ComparisonFunc = D3D11_COMPARISON_NEVER;
        d.BorderColor[0] = ((t->border >> 16) & 0xFF) / 255.0f;
        d.BorderColor[1] = ((t->border >> 8) & 0xFF) / 255.0f;
        d.BorderColor[2] = (t->border & 0xFF) / 255.0f;
        d.BorderColor[3] = (t->border >> 24) / 255.0f;
        if (minf <= 2 || minf == 7 || levels <= 1) {
            d.MinLOD = d.MaxLOD = 0;
        } else {
            d.MinLOD = (float)((t->control0 >> 18) & 0xFFF) / 256.0f;
            d.MaxLOD = (float)((t->control0 >> 6) & 0xFFF) / 256.0f;
            if (d.MaxLOD < d.MinLOD) d.MaxLOD = D3D11_FLOAT32_MAX;
        }
        if (reduce) {
            /* Over surface_mips: the title's min/mag filter within a level,
             * levels picked and blended by each tap's footprint. */
            d.Filter = (D3D11_FILTER)((min_lin ? 0x10 : 0) | (mag_lin ? 0x4 : 0) | 0x1);
            d.MinLOD = 0;
            d.MaxLOD = D3D11_FLOAT32_MAX;
        }
        ID3D11Device_CreateSamplerState(s_dev, &d, (ID3D11SamplerState **)slot);
    }
    if (request) {
        request->filter = t->filter; request->address = t->address;
        request->control0 = t->control0; request->border = t->border;
        request->levels = levels; request->reduce = reduce;
        request->key = key;
        request->slot = (StateSlot *)((char *)slot - offsetof(StateSlot, obj));
    }
    return (ID3D11SamplerState *)*slot;
}

static ID3D11SamplerState *sampler(const NvD3DTexture *t, int levels, int reduce)
{
    return sampler_request(t, levels, reduce, NULL);
}

static ID3D11SamplerState *sampler_stage(unsigned stage, const NvD3DTexture *t, int levels, int reduce)
{
    return sampler_request(t, levels, reduce, stage < 4 ? &s_sampler_request[stage] : NULL);
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

/* Scratch copy for render-to-texture reads of the target being drawn. */
static ID3D11Texture2D *s_feedback_tex;
static ID3D11ShaderResourceView *s_feedback_srv;
static UINT s_feedback_w, s_feedback_h;

static ID3D11ShaderResourceView *feedback_copy(Surface *s)
{
    if (!s_feedback_tex || s_feedback_w != s->iw || s_feedback_h != s->ih) {
        if (s_feedback_srv) ID3D11ShaderResourceView_Release(s_feedback_srv);
        if (s_feedback_tex) ID3D11Texture2D_Release(s_feedback_tex);
        s_feedback_srv = NULL;
        s_feedback_tex = make_texture(s->iw, s->ih, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM,
                                      D3D11_BIND_SHADER_RESOURCE, 0);
        if (!s_feedback_tex) return NULL;
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_feedback_tex,
                                              NULL, &s_feedback_srv);
        s_feedback_w = s->iw; s_feedback_h = s->ih;
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_feedback_tex,
                                     (ID3D11Resource *)s->tex);
    return s_feedback_srv;
}

static void apply_scissor(const NvD3DState *st, uint32_t gw, uint32_t gh)
{
    D3D11_RECT r;
    uint32_t x0 = st->clip_x, y0 = st->clip_y;
    uint32_t x1 = st->clip_x + st->clip_w, y1 = st->clip_y + st->clip_h;
    if (x0 < st->wclip_x0) x0 = st->wclip_x0;
    if (y0 < st->wclip_y0) y0 = st->wclip_y0;
    if (x1 > st->wclip_x1) x1 = st->wclip_x1;
    if (y1 > st->wclip_y1) y1 = st->wclip_y1;
    if (x1 > gw) x1 = gw;
    if (y1 > gh) y1 = gh;
    if (x0 > x1) x0 = x1;
    if (y0 > y1) y0 = y1;
    r.left = (LONG)(x0 * s_aa_x * s_cur_sx + 0.5f);
    r.top = (LONG)(y0 * s_aa_y * s_cur_sy + 0.5f);
    r.right = (LONG)(x1 * s_aa_x * s_cur_sx + 0.5f);
    r.bottom = (LONG)(y1 * s_aa_y * s_cur_sy + 0.5f);
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &r);
}

static uint32_t write_mask(const NvD3DState *st, Surface *s)
{
    uint32_t m = 0, cm = st->color_mask;
    if (cm & 0x00010000u) m |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (cm & 0x00000100u) m |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (cm & 0x00000001u) m |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if ((cm & 0x01000000u) && (!s || surface_alpha_mode(s->color_fmt) < 0))
        m |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    return m;
}

/* Everything a draw needs apart from its vertices: targets, textures, the
 * combiner constants, raster and output-merger state, topology. Returns 0 if
 * nothing is to be drawn. */
/* What the last setup_pipeline bound: any texture, and any render target
 * stored widened (its image already spans the wide screen). */
static int s_draw_textured, s_draw_reads_wide;
static uint32_t s_draw_tex_addr;
static int s_flare_depth_test;
static int s_draw_bspline;
static uint32_t s_bspline_sf, s_bspline_df;

extern const uint32_t (*nv2a_vsh_program(uint32_t *start, uint32_t *version))[4];
extern const float (*nv2a_vsh_constants(uint32_t *version))[4];

void nv2a_d3d_set_flare_depth_test(int on)
{
    s_flare_depth_test = on != 0;
}

static int flare_needs_depth(const NvD3DState *st)
{
    static int noted;
    static unsigned traced;
    Surface *mask;
    uint32_t address, reject = 0;
    unsigned n;
    /* tex_used describes only the fixed-function fallback in the pushbuffer
     * executor. Register-combiner draws select sampled stages in stage_program;
     * 0x8421 is the native Flares program's four projective 2D stages. */
    if (!s_flare_depth_test || !st->rc_seen || st->rc.stage_program != 0x8421u)
        return 0;
    address = phys(st->tex[0].addr);
    if (!address || phys(st->tex[3].addr) == address) return 0;
    mask = surface_find(address);
    if (st->depth_test || st->depth_mask || !st->zeta_addr) reject |= 1u;
    if (!st->blend_enable || st->blend_dfactor != 1u || st->blend_equation != 0x8006u)
        reject |= 2u;
    if (!mask || mask->w != st->clip_w || mask->h != st->clip_h) reject |= 4u;
    /* MM3 Flares/Flares.psh: t0..t2 sample the screen visibility mask;
     * t3 samples the lamp texture. Four framebuffer taps are bloom, and
     * one/two textures can be HUD or road light passes: none qualify. */
    for (n = 0; n < 3; ++n) {
        const NvD3DTexture *t = &st->tex[n];
        if (phys(t->addr) != address || t->cube || t->color != 0x12u ||
            ((t->filter >> 24) & 15u) != 2u)
            reject |= 8u;
    }
    if (traced < 4 && getenv("MM3_FLARE_TRACE") &&
        phys(st->tex[1].addr) == address && phys(st->tex[2].addr) == address) {
        fprintf(stderr, "[D3D11] lamp pass reject=%X clip=%ux%u mask=%ux%u depth=%u write=%u zeta=%08X wbuffer=%u tex_used=%X filter=%08X/%08X/%08X\n",
                reject, st->clip_w, st->clip_h, mask ? mask->w : 0, mask ? mask->h : 0,
                st->depth_test, st->depth_mask, st->zeta_addr, st->z_perspective,
                st->tex_used, st->tex[0].filter, st->tex[1].filter, st->tex[2].filter);
        if (!traced) {
            uint32_t start = 0, version = 0, cver = 0, k;
            const uint32_t (*prog)[4] = nv2a_vsh_program(&start, &version);
            const float (*consts)[4] = nv2a_vsh_constants(&cver);
            if (prog && consts) {
                for (k = start; k < start + 16 && k < 136u; ++k) {
                    fprintf(stderr, "[D3D11] lamp vp %u %08X %08X %08X %08X\n",
                            k - start, prog[k][0], prog[k][1], prog[k][2], prog[k][3]);
                    if (prog[k][3] & 1u) break;
                }
                for (k = 0; k < 2; ++k)
                    fprintf(stderr, "[D3D11] lamp c%u %.9g %.9g %.9g %.9g\n", k,
                            consts[k][0], consts[k][1], consts[k][2], consts[k][3]);
            }
        }
        ++traced;
    }
    if (reject) return 0;
    if (!noted) {
        fprintf(stderr, "[D3D11] lamp visibility sprites use scene depth\n");
        noted = 1;
    }
    return 1;
}

/* A guest pass that reduces a render target (glare, luminance
 * or reflection downsamples) takes texel-exact taps: at the guest's
 * resolution four taps cover a 2x2 block, at a host resolution several times
 * higher they hit isolated texels, and which ones moves with the camera --
 * lit windows and lamps then flicker through the bloom. Reductions read this
 * mip-mapped copy, so each tap averages the footprint it stands for. */
/* A swizzled render target sampled as a mipmapped texture: titles render
 * each level of such a texture themselves, one surface per level at the
 * offsets the texture's layout puts them. Reading only the top
 * surface sampled a full-size normal map at every distance -- shimmer and
 * moire out across the water. The levels found are copied into one host
 * texture, each at its own (scaled) size, as the title's chain; returns the
 * view and sets *levels, or NULL to fall back to the single surface. */
static ID3D11ShaderResourceView *surface_chain(Surface *rt, const NvD3DTexture *t, int *levels)
{
    Surface *lv[16];
    uint32_t off = 0, k, n = 0, bpp = rt->bpp;
    for (k = 0; k < t->levels && k < 16; k++) {
        uint32_t w = t->width >> k, h = t->height >> k;
        Surface *c;
        if (!w) w = 1;
        if (!h) h = 1;
        c = k ? surface_find(phys(t->addr) + off) : rt;
        if (!c || !c->swizzled || c->w != w || c->h != h || c->bpp != bpp || c == s_cur_rt)
            break;
        lv[n++] = c;
        off += w * h * bpp;
    }
    if (n < 2) return NULL;
    if (!rt->chain || rt->chain_levels != (int)n) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        if (rt->chain_srv) ID3D11ShaderResourceView_Release(rt->chain_srv);
        if (rt->chain) ID3D11Texture2D_Release(rt->chain);
        rt->chain_srv = NULL;
        rt->chain = make_texture(rt->iw, rt->ih, n, 1, DXGI_FORMAT_B8G8R8A8_UNORM,
                                 D3D11_BIND_SHADER_RESOURCE, 0);
        if (!rt->chain) return NULL;
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = n;
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)rt->chain, &sd, &rt->chain_srv);
        if (!rt->chain_srv) return NULL;
        rt->chain_levels = (int)n;
        for (k = 0; k < n; k++) rt->chain_gen[k] = lv[k]->gen - 1;
    }
    for (k = 0; k < n; k++) {
        uint32_t dw = rt->iw >> k, dh = rt->ih >> k;
        D3D11_BOX box;
        if (!dw) dw = 1;
        if (!dh) dh = 1;
        surface_sync(lv[k], 0);
        if (rt->chain_gen[k] == lv[k]->gen) continue;
        box.left = box.top = box.front = 0;
        box.right = lv[k]->iw < dw ? lv[k]->iw : dw;
        box.bottom = lv[k]->ih < dh ? lv[k]->ih : dh;
        box.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)rt->chain, k, 0, 0, 0,
                                                  (ID3D11Resource *)lv[k]->tex, 0, &box);
        rt->chain_gen[k] = lv[k]->gen;
    }
    *levels = (int)n;
    return rt->chain_srv;
}

static ID3D11ShaderResourceView *surface_mips(Surface *s)
{
    if (!s->mip) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        s->mip = make_texture(s->iw, s->ih, 0, 1, DXGI_FORMAT_B8G8R8A8_UNORM,
                              D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                              D3D11_RESOURCE_MISC_GENERATE_MIPS);
        if (!s->mip) return NULL;
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = (UINT)-1;
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s->mip, &sd, &s->mip_srv);
        if (!s->mip_srv) return NULL;
        s->mip_gen = s->gen - 1;
    }
    if (s->mip_gen != s->gen) {
        surface_resolve(s);
        ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)s->mip, 0, 0, 0, 0,
                                                  (ID3D11Resource *)s->tex, 0, NULL);
        ID3D11DeviceContext_GenerateMips(s_ctx, s->mip_srv);
        s->mip_gen = s->gen;
    }
    return s->mip_srv;
}

/* Bring s's LU_IMAGE_Y16 view up to date (2 x iw by ih); NULL on failure.
 * Unbinds the current targets: call before a draw binds its own. */
static ID3D11ShaderResourceView *surface_y16(Surface *s)
{
    if (!s->y16) {
        s->y16 = make_texture(2 * s->iw, s->ih, 1, 1, DXGI_FORMAT_R16G16B16A16_UNORM,
                              D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0);
        if (!s->y16) return NULL;
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s->y16, NULL, &s->y16_srv);
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s->y16, NULL, &s->y16_rtv);
        s->y16_gen = s->gen - 1;
    }
    if (s->y16_gen != s->gen) {
        ID3D11ShaderResourceView *src = s->ms ? s->ms_srv : s->srv;
        D3D11_VIEWPORT vp = { 0, 0, (float)(2 * s->iw), (float)s->ih, 0, 1 };
        D3D11_RECT sc = { 0, 0, (LONG)(2 * s->iw), (LONG)s->ih };
        unbind_textures();
        unbind_targets();
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s->y16_rtv, NULL);
        ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
        ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sc);
        ID3D11DeviceContext_RSSetState(s_ctx, s_rs_plain);
        ID3D11DeviceContext_OMSetBlendState(s_ctx, s_blend_opaque, NULL, 0xFFFFFFFF);
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, s_ds_off, 0);
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        bind_vertex_shader(s_y16_vs);
        bind_pixel_shader(s_y16_ps[s->ms ? 1 : 0]);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &src);
        ID3D11DeviceContext_Draw(s_ctx, 3, 0);
        unbind_targets();
        unbind_textures();
        s->y16_gen = s->gen;
    }
    return s->y16_srv;
}

/* The same for a depth buffer the title reads as LU_IMAGE_Y16 (see y16d_ps). */
static ID3D11ShaderResourceView *depth_y16(Depth *d)
{
    if (!d->zsrv) return NULL;
    s_depth_hazed = 1;
    if (!d->y16) {
        d->y16 = make_texture(2 * d->iw, d->ih, 1, 1, DXGI_FORMAT_R16G16B16A16_UNORM,
                              D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0);
        if (!d->y16) return NULL;
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)d->y16, NULL, &d->y16_srv);
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)d->y16, NULL, &d->y16_rtv);
        d->y16_gen = d->gen - 1;
    }
    if (d->y16_gen != d->gen) {
        ID3D11ShaderResourceView *src[1] = { d->zsrv };
        D3D11_VIEWPORT vp = { 0, 0, (float)(2 * d->iw), (float)d->ih, 0, 1 };
        D3D11_RECT sc = { 0, 0, (LONG)(2 * d->iw), (LONG)d->ih };
        unbind_textures();
        unbind_targets();
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &d->y16_rtv, NULL);
        ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
        ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sc);
        ID3D11DeviceContext_RSSetState(s_ctx, s_rs_plain);
        ID3D11DeviceContext_OMSetBlendState(s_ctx, s_blend_opaque, NULL, 0xFFFFFFFF);
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, s_ds_off, 0);
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        bind_vertex_shader(s_y16_vs);
        bind_pixel_shader(s_y16d_ps[d->samples > 1 ? 1 : 0]);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, src);
        {
            float cc[8] = { 0 };
            memcpy(cc, s_zview_remap, sizeof s_zview_remap);
            cc[2] = d->w ? 0.25f * (float)d->iw / (float)d->w : 0.0f;
            upload_cb(s_cb_clear, cc, sizeof cc);
            ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb_clear);
        }
        ID3D11DeviceContext_Draw(s_ctx, 3, 0);
        unbind_targets();
        unbind_textures();
        d->y16_gen = d->gen;
    }
    return d->y16_srv;
}

static int setup_pipeline(const NvD3DState *st, int topology, Surface **out_s,
                          uint32_t *out_gw, uint32_t *out_gh, uint32_t *out_zmax)
{
    Surface *s;
    Depth *d;
    uint32_t gw, gh, n, zf;
    PSConsts pc;
    ID3D11ShaderResourceView *srv[8] = { 0 };
    ID3D11SamplerState *smp[4] = { 0 };
    int cull = 0, bias = 0, flare_depth = flare_needs_depth(st);
    float slope = 0, bf[4];
    s_draw_textured = s_draw_reads_wide = 0;
    s_draw_tex_addr = 0;
    s_draw_bspline = 0;

    {
        /* Bring-up switches: RECOMP_D3D_SKIP_CMASK / _SKIP_SFACTOR (hex) drop
         * draws with that colour mask / source blend factor. */
        static int init;
        static long skip_cmask = -1, skip_sf = -1;
        if (!init) {
            const char *e;
            init = 1;
            if ((e = getenv("RECOMP_D3D_SKIP_CMASK")) != NULL) skip_cmask = strtol(e, NULL, 16);
            if ((e = getenv("RECOMP_D3D_SKIP_SFACTOR")) != NULL) skip_sf = strtol(e, NULL, 16);
        }
        if ((skip_cmask >= 0 && st->color_mask == (uint32_t)skip_cmask) ||
            (skip_sf >= 0 && st->blend_enable && st->blend_sfactor == (uint32_t)skip_sf))
            return 0;
    }
    if (st->cull_enable)
        cull = st->cull_face == 0x404 ? 1 : st->cull_face == 0x405 ? 2 : 3;
    if (cull == 3 && topology == NV_D3D_TRIANGLES)
        return 0;                                /* front and back culled */
    /* Y16 views of 32-bit targets are drawn before this draw's targets go
     * on; see y16_ps. */
    for (n = 0; n < 4; n++) {
        const NvD3DTexture *t = &st->tex[n];
        Surface *rt;
        Depth *zt;
        if (t->color != 0x35 || t->cube || !t->addr) continue;
        /* A depth buffer wins over a colour surface at the same address:
         * titles bind one there for colour-masked stencil passes. */
        if ((zt = depth_find(phys(t->addr))) != NULL) { depth_y16(zt); continue; }
        rt = surface_find(phys(t->addr));
        if (rt && rt->bpp == 4) { surface_sync(rt, 0); surface_y16(rt); }
    }
    if (!bind_targets(st, st->depth_test || st->stencil_test || flare_depth, &s, &d, &gw, &gh))
        return 0;
    zf = (st->surface_format >> 4) & 0xF;
    if (s && write_mask(st, s)) s->gpu_dirty = 1;

    /* Textures first: a surface sampled here may need a copy. */
    memset(&pc, 0, sizeof pc);
    for (n = 0; n < 4; n++) {
        const NvD3DTexture *t = &st->tex[n];
        uint32_t mode = (st->rc.stage_program >> (n * 5)) & 31;
        ID3D11ShaderResourceView *view = NULL;
        Surface *rt;
        Depth *zt;
        int kind = K_SWZ, levels = 1, reduce = 0;
        pc.tex_scale[n][0] = pc.tex_scale[n][1] = 1.0f;
        pc.tex_scale[n][2] = pc.tex_scale[n][3] = 1.0f;
        if (!st->rc_seen && n == 0)
            mode = (st->tex_used & 1) ? 1 : 0;
        if (!mode_needs_texture(mode) || !t->addr || !t->width || !t->height)
            continue;
        tex_format(t->color, &kind);
        /* A recycled target address can now hold a streamed compressed or
         * paletted image. Those layouts cannot alias a colour render target;
         * decode their guest bytes instead of sampling the cached target. */
        rt = !t->cube && kind != K_DXT && t->color != 0x0B
             ? surface_find(phys(t->addr)) : NULL;
        s_draw_textured = 1;
        if (!s_draw_tex_addr) s_draw_tex_addr = t->addr;
        if (rt && rt->sx > rt->sy * 1.001f) s_draw_reads_wide = 1;
        zt = t->color == 0x35 && !t->cube ? depth_find(phys(t->addr)) : NULL;
        if (zt && zt->y16_srv) {
            view = zt->y16_srv;                 /* each pixel is two texels */
            pc.tex_scale[n][0] = 1.0f / (float)(2 * zt->w);
            pc.tex_scale[n][1] = 1.0f / (float)zt->h;
        } else if (rt) {
            surface_sync(rt, 0);
            surface_resolve(rt);
            /* An MSAA target draws into ms, so its resolved tex is free to read. */
            view = rt == s && !rt->ms ? feedback_copy(rt) : rt->srv;
            if (t->color == 0x35 && rt->bpp == 4 && rt->y16_srv) {
                view = rt->y16_srv;             /* each pixel is two texels */
                pc.tex_scale[n][0] = 1.0f / (float)(2 * rt->w);
                pc.tex_scale[n][1] = 1.0f / (float)rt->h;
            } else if (kind == K_LIN) {
                pc.tex_scale[n][0] = 1.0f / (float)rt->w;
                pc.tex_scale[n][1] = 1.0f / (float)rt->h;
                /* A pass into a smaller target is a reduction; see surface_mips. */
                if (s && s != rt && s->iw < rt->iw && s->ih < rt->ih) {
                    ID3D11ShaderResourceView *m = surface_mips(rt);
                    if (m) { view = m; reduce = 1; }
                }
            } else {
                pc.tex_scale[n][0] = (float)t->width / (float)rt->w;
                pc.tex_scale[n][1] = (float)t->height / (float)rt->h;
                if (t->levels > 1 && rt->swizzled && rt != s_cur_rt) {
                    ID3D11ShaderResourceView *c = surface_chain(rt, t, &levels);
                    if (c) view = c;
                }
            }
        } else {
            view = texture_get_with_levels(t, &levels);
            if (kind == K_LIN) {
                pc.tex_scale[n][0] = 1.0f / (float)t->width;
                pc.tex_scale[n][1] = 1.0f / (float)t->height;
            }
        }
        if (kind == K_LIN) {
            pc.tex_scale[n][2] = pc.tex_scale[n][0];
            pc.tex_scale[n][3] = pc.tex_scale[n][1];
        } else {
            pc.tex_scale[n][2] = pc.tex_scale[n][3] = 1.0f;
        }
        if (!view) continue;
        if (t->cube && !rt) srv[4 + n] = view; else srv[n] = view;
        smp[n] = sampler_stage(n, t, levels, reduce);
        /* The title opts in its 256x256 DXT5 horizon panorama, magnified
         * across the skyline through destination alpha. Keep ordinary sky,
         * clouds, UI, cubemaps and render-target copies on their samplers. */
        if (n == 0 && !rt && !t->cube && kind == K_DXT && t->color == 0x0F &&
            t->width == 256 && t->height == 256 && levels > 4 &&
            (((t->filter >> 16) & 0xFF) == 2 || ((t->filter >> 16) & 0xFF) == 4 ||
             ((t->filter >> 16) & 0xFF) == 6) && ((t->filter >> 24) & 0xF) != 1 &&
            s_bspline_sf && !st->depth_test && st->blend_enable &&
            st->blend_sfactor == s_bspline_sf && st->blend_dfactor == s_bspline_df)
            s_draw_bspline = 1;
        pc.tex_info[n][0] = (t->control0 >> 2) & 1;
        pc.tex_info[n][1] = t->filter >> 28;
        /* Point taps of an upscaled linear target average the guest pixel's
         * host block; see tapbox. */
        if (rt && kind == K_LIN && t->color != 0x35 && (rt->iw > rt->w || rt->ih > rt->h) &&
            ((t->filter >> 24) & 0xF) == 1 && !(((t->filter >> 16) & 0xFF) == 2 ||
            ((t->filter >> 16) & 0xFF) == 4 || ((t->filter >> 16) & 0xFF) == 6))
            pc.tex_info[n][2] = 1;
        memcpy(pc.bump_mat[n], t->bump_mat, sizeof pc.bump_mat[n]);
        pc.bump_lum[n][0] = t->bump_scale;
        pc.bump_lum[n][1] = t->bump_offset;
    }
    for (n = 0; n < 4; n++)
        if (!smp[n]) smp[n] = s_linear_clamp;

    /* A sampled surface's CPU upload can blit and unbind this draw's
     * outputs. Restore targets and viewport before binding its textures. */
    if ((s != s_cur_rt || d != s_cur_ds) &&
        !bind_targets(st, st->depth_test || st->stencil_test || flare_depth, &s, &d, &gw, &gh))
        return 0;

    /* Combiners. */
    for (n = 0; n < 8; n++) {
        pc.c_icw[n] = st->rc.color_icw[n];
        pc.a_icw[n] = st->rc.alpha_icw[n];
        pc.c_ocw[n] = st->rc.color_ocw[n];
        pc.a_ocw[n] = st->rc.alpha_ocw[n];
        unpack4(st->rc.factor0[n], pc.cf0[n]);
        unpack4(st->rc.factor1[n], pc.cf1[n]);
    }
    unpack4(st->rc.final_c0, pc.fin_c0);
    unpack4(st->rc.final_c1, pc.fin_c1);
    pc.fin0 = st->rc.final0;
    pc.fin1 = st->rc.final1;
    pc.rc_control = st->rc.control;
    pc.stage_prog = st->rc_seen ? st->rc.stage_program : (st->tex_used & 1);
    pc.alpha_func = st->alpha_func;
    pc.alpha_ref = st->alpha_ref;
    pc.ps_flags = (st->rc_seen ? 1u : 0u) | (st->alpha_test ? 2u : 0u) |
                  (s_draw_bspline ? 4u : 0u);
    pc.clip_plane = st->clip_plane_mode;
    pc.other_input = st->other_stage_input;
    pc.dot_map = st->dot_rgb_mapping;
    /* SET_FOG_COLOR keeps red in the low byte. */
    pc.fog_color[0] = (st->fog_color & 0xFF) / 255.0f;
    pc.fog_color[1] = ((st->fog_color >> 8) & 0xFF) / 255.0f;
    pc.fog_color[2] = ((st->fog_color >> 16) & 0xFF) / 255.0f;
    pc.fog_color[3] = (st->fog_color >> 24) / 255.0f;
    if (st->z_perspective && d) {
        uint32_t zm = zf == 1 ? 0xFFFFu : 0xFFFFFFu;
        if (st->poly_offset_fill) {
            pc.zp0[0] = st->poly_offset_factor;
            pc.zp0[1] = st->poly_offset_units;
        }
        pc.zp0[2] = s_cur_sx;
        pc.zp0[3] = s_cur_sy;
        pc.zp1[0] = 1.0f / ((float)zm + 1.0f);
        pc.zp1[1] = (float)zm;
        if (s_depth_hazed && st->depth_test && st->depth_mask && s_zview_remap[0] > 1.0f) {
            pc.zp1[2] = s_zview_remap[0];
            pc.zp1[3] = s_zview_remap[1];
        }
    }
    if (!upload_ps_constants(&pc)) return 0;

    if (st->poly_offset_fill && !(st->z_perspective && d)) {
        /* The slope term multiplies the depth change per pixel. At an
         * internal resolution s times the guest's a pixel spans 1/s as much
         * depth, so the factor scales by s to offset decals as far as the NV2A does; unscaled they fight. */
        bias = (int)st->poly_offset_units;
        slope = st->poly_offset_factor * s_cur_sy;
    }
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx,
        topology == NV_D3D_LINES ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST :
        topology == NV_D3D_POINTS ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST :
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    {
        uint32_t v = (st->flat_shade ? 1u : 0u) | (st->z_perspective && d ? 2u : 0u);
        ID3D11PixelShader *ps = spec_ps(&pc, v);
        bind_pixel_shader(ps ? ps : s_psv[v]);
    }
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 8, srv);
    {   /* The horizon panorama reads the frame drawn so far (see bspline0). */
        ID3D11ShaderResourceView *fb = NULL;
        if (s_draw_bspline && s) {
            if (s->ms) { surface_resolve(s); fb = s->srv; } else fb = feedback_copy(s);
        }
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 8, 1, &fb);
    }
    bind_pixel_samplers(4, smp);
    ID3D11DeviceContext_RSSetState(s_ctx, rs_state(cull == 3 ? 0 : cull,
                                                   st->front_face == 0x901, bias, slope));
    apply_scissor(st, gw, gh);
    if ((st->blend_sfactor | st->blend_dfactor) & 0x8000) {
        float c[4];
        unpack4(st->blend_color, c);
        if (st->blend_sfactor == 0x8003 || st->blend_sfactor == 0x8004 ||
            st->blend_dfactor == 0x8003 || st->blend_dfactor == 0x8004)
            bf[0] = bf[1] = bf[2] = bf[3] = c[3];
        else
            memcpy(bf, c, sizeof bf);
    } else {
        bf[0] = bf[1] = bf[2] = bf[3] = 0;
    }
    ID3D11DeviceContext_OMSetBlendState(s_ctx,
        blend_state(st->blend_enable, st->blend_sfactor, st->blend_dfactor,
                    st->blend_equation, s ? write_mask(st, s) : 0), bf, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx,
        d ? ds_state(st->depth_test || flare_depth, flare_depth ? 0x203u : st->depth_func,
                     flare_depth ? 0u : st->depth_mask,
                     st->stencil_test, st->stencil_func, st->stencil_func_mask,
                     st->stencil_write_mask, st->stencil_fail,
                     st->stencil_zfail, st->stencil_zpass) : s_ds_off,
        st->stencil_ref & 0xFF);
    *out_s = s;
    *out_gw = gw;
    *out_gh = gh;
    *out_zmax = zf == 1 ? 0xFFFFu : 0xFFFFFFu;
    return 1;
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

/* Bytes from the vertex and index rings; NULL if the request cannot fit. */
static uint8_t *ring_map(ID3D11Buffer *b, UINT *pos, UINT cap, UINT bytes,
                         D3D11_MAPPED_SUBRESOURCE *m)
{
    if (bytes > cap) return NULL;
    if (*pos + bytes > cap) *pos = 0;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)b, 0,
            *pos ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, 0, m)))
        return NULL;
    return (uint8_t *)m->pData + *pos;
}

/* ---------------------------------------------- vertex processing on the GPU
 *
 * Every draw runs ub_vs: it pulls the title's raw vertex arrays from a byte
 * buffer (any format, any alignment) and either interprets the current NV2A
 * vertex program from a constant buffer or passes pre-transformed vertices
 * through. One shader for everything means every pass over an object rounds
 * its depth the same way (no z-fighting between passes), nothing waits on a
 * shader compile, and no vertex is drawn from a CPU transform. */

#define NV_VSH_SLOTS 136
extern const uint32_t (*nv2a_vsh_program(uint32_t *start, uint32_t *version))[4];
extern const float (*nv2a_vsh_constants(uint32_t *version))[4];

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

int nv2a_d3d_widescreen(void)
{
    return widen() > 1.001f;
}

/* Menu pointing (RECOMP_MENU_KEYS): each frame's textured screen-space quads,
 * in guest pixels, with the colour their vertices leave the vertex stage
 * with, and where the last frame landed in the window. What is a button and
 * which one has the focus is the title's business (nv2a_d3d_menu_rects). */
typedef NvMenuRect MenuRect;
#define MENU_RECTS 1024
static MenuRect s_mr_frame[MENU_RECTS], s_mr_shown[MENU_RECTS];
static int s_mr_frame_n, s_mr_shown_n;
static float s_present_rect[4];                  /* x0, y0, w, h as client fractions */
static SRWLOCK s_mr_lock = SRWLOCK_INIT;
static volatile LONG s_mr_on;
static const uint32_t *s_mr_tints;               /* the current draw's, see header */

void nv2a_d3d_collect_menu_rects(int on)
{
    InterlockedExchange(&s_mr_on, on != 0);
}

int nv2a_d3d_menu_rects_on(void)
{
    return InterlockedCompareExchange(&s_mr_on, 0, 0) != 0;
}

void nv2a_d3d_set_menu_tints(const uint32_t *tints)
{
    s_mr_tints = tints;
}

int nv2a_d3d_menu_rects(NvMenuRect *out, int max, float present[4])
{
    int n;
    AcquireSRWLockShared(&s_mr_lock);
    n = s_mr_shown_n < max ? s_mr_shown_n : max;
    memcpy(out, s_mr_shown, (size_t)n * sizeof *out);
    memcpy(present, s_present_rect, sizeof s_present_rect);
    ReleaseSRWLockShared(&s_mr_lock);
    return n;
}

void nv2a_d3d_set_bspline_blend(uint32_t sfactor, uint32_t dfactor)
{
    s_bspline_sf = sfactor;
    s_bspline_df = dfactor;
}

/* One rect per triangle: either half of an axis-aligned quad spans the
 * whole quad, so batched quads come apart without knowing how they pair. */
static void note_menu_rects(const float (*pos)[4], const uint32_t *idx, uint32_t ni, uint32_t lo)
{
    uint32_t group = ni % 3 == 0 ? 3 : ni, k, j;
    for (k = 0; k + group <= ni && s_mr_frame_n < MENU_RECTS; k += group) {
        MenuRect r = { 1e30f, 1e30f, -1e30f, -1e30f, s_draw_tex_addr,
                       s_mr_tints ? s_mr_tints[idx[k] - lo] : 0xFFFFFFFFu };
        for (j = k; j < k + group; j++) {
            const float *v = pos[idx[j] - lo];
            if (v[3] != 1.0f) return;
            if (v[0] < r.x0) r.x0 = v[0];
            if (v[0] > r.x1) r.x1 = v[0];
            if (v[1] < r.y0) r.y0 = v[1];
            if (v[1] > r.y1) r.y1 = v[1];
        }
        if (s_mr_frame_n && !memcmp(&s_mr_frame[s_mr_frame_n - 1], &r, sizeof r))
            continue;                            /* the quad's other half */
        s_mr_frame[s_mr_frame_n++] = r;
    }
}

void nv2a_d3d_draw_ub(const NvD3DState *st, int topology, const NvD3DAttrib attr[16],
                      const float def[16][4], int program, const uint32_t *idx, uint32_t ni,
                      uint32_t lo, uint32_t hi, const float (*pos)[4])
{
    static uint32_t s_prog_version, s_vconst_version;
    static int s_prog_valid, s_vconst_valid;
    static float *dx;
    static uint32_t dx_cap;
    struct { const uint8_t *base; uint32_t stride, span; } grp[16];
    uint32_t ngrp = 0, i, count = hi - lo + 1, gw, gh, zmax, gof[16];
    UbConsts uc;
    Surface *s;
    D3D11_MAPPED_SUBRESOURCE m;
    uint8_t *p;
    UINT total = 0, at = 0, ib_bytes = ni * 4;
    float squeeze = 1.0f, shift = 0.0f, yscale = 1.0f;
    int use_dx = 0;
    int64_t t_draw;

    if (!nv2a_d3d_init() || !ni || hi < lo || ib_bytes > IB_BYTES)
        return;
    if (getenv("TMP_NOPANO") && !st->depth_test && st->blend_enable && st->blend_sfactor == 0x304 &&
        st->blend_dfactor == 0x305 && st->tex[0].width == 256 && st->tex[0].height == 256) /* TMPPANO */
        return;
    memset(&uc, 0, sizeof uc);
    memcpy(uc.def, def, sizeof uc.def);
    /* Arrays: interleaved ones share one copied span. */
    for (i = 0; i < 16; i++) {
        uint32_t g, bytes = attr_bytes(attr[i].type, attr[i].size);
        if (!attr[i].enabled) continue;
        uc.at[i][2] = attr[i].type;
        uc.at[i][3] = attr[i].size;
        if (!attr[i].data || !bytes) { uc.at[i][2] = 0xFF; continue; }   /* reads (0,0,0,1) */
        for (g = 0; g < ngrp; g++)
            if (grp[g].stride == attr[i].stride && attr[i].data >= grp[g].base &&
                attr[i].data + bytes <= grp[g].base + (attr[i].stride ? attr[i].stride : 64))
                break;
        if (g == ngrp) {
            grp[g].base = attr[i].data;
            grp[g].stride = attr[i].stride;
            grp[g].span = 0;
            ngrp++;
        }
        if ((uint32_t)(attr[i].data - grp[g].base) + bytes > grp[g].span)
            grp[g].span = (uint32_t)(attr[i].data - grp[g].base) + bytes;
        gof[i] = g;
        uc.at[i][0] = (uint32_t)(attr[i].data - grp[g].base);   /* + group offset below */
        uc.at[i][1] = attr[i].stride;
    }
    for (i = 0; i < ngrp; i++)
        total += (((grp[i].stride ? (count - 1) * grp[i].stride : 0) + grp[i].span) + 15) & ~15u;
    if (total + count * 4 > VB_BYTES)
        return;

    RECOMP_PROFILE_BEGIN("D3D11 draw");
    t_draw = s_prof_on > 0 ? qpc() : 0;
    s_prof_draws++;
    {
        int64_t trace_begin = hitch_trace_begin();
        int setup_ok = setup_pipeline(st, topology, &s, &gw, &gh, &zmax);
        hitch_trace_report("draw_setup", trace_begin);
        if (!setup_ok) {
            RECOMP_PROFILE_END();
            return;
        }
    }
    if (pos && s_draw_textured && nv2a_d3d_menu_rects_on())
        note_menu_rects(pos, idx, ni, lo);
    if (pos && s && s->sx > s->sy * 1.001f && s_overlay.place) {
        /* Screen-space 2D on a widened target spans the display unless the
         * title's overlay hooks place it otherwise. */
        float xlo = 1e30f, xhi = -1e30f, ylo = 1e30f, yhi = -1e30f;
        uint32_t k;
        int flat = 1;
        for (k = 0; k < ni && flat; k++) {
            const float *v = pos[idx[k] - lo];
            if (v[3] != 1.0f) flat = 0;
            if (v[0] < xlo) xlo = v[0];
            if (v[0] > xhi) xhi = v[0];
            if (v[1] < ylo) ylo = v[1];
            if (v[1] > yhi) yhi = v[1];
        }
        if (flat) {
            NvOverlayDraw d;
            if (count > dx_cap) {
                float *nd = realloc(dx, (size_t)count * sizeof *nd);
                if (nd) { dx = nd; dx_cap = count; }
            }
            d.pos = pos; d.idx = idx; d.attrib = attr; d.ni = ni; d.lo = lo; d.count = count;
            d.per = topology == NV_D3D_TRIANGLES ? 3 : topology == NV_D3D_LINES ? 2 : 1;
            d.gw = (float)gw; d.gh = (float)gh; d.narrow = s->sy / s->sx;
            d.x0 = xlo; d.y0 = ylo; d.x1 = xhi; d.y1 = yhi;
            d.tex = s_draw_tex_addr; d.textured = s_draw_textured; d.reads_wide = s_draw_reads_wide;
            use_dx = s_overlay.place(&d, count <= dx_cap ? dx : NULL, &squeeze, &shift);
        }
    }

    /* Vertex data for [lo, hi], one span per group, then any x shifts. */
    if (!s_raw_nooverwrite) s_raw_pos = 0;
    if (!(p = ring_map(s_raw, &s_raw_pos, VB_BYTES, total + (use_dx ? count * 4 : 0), &m))) {
        RECOMP_PROFILE_END();
        return;
    }
    {
        UINT goff[16];
        for (i = 0; i < ngrp; i++) {
            UINT bytes = (grp[i].stride ? (count - 1) * grp[i].stride : 0) + grp[i].span;
            memcpy(p + at, grp[i].base + (size_t)lo * grp[i].stride, bytes);
            goff[i] = s_raw_pos + at;
            at += (bytes + 15) & ~15u;
        }
        for (i = 0; i < 16; i++)
            if (attr[i].enabled && uc.at[i][2] != 0xFF)
                uc.at[i][0] += goff[gof[i]];
        if (s_overlay.sanitize_vertex_upload)
            for (i = 0; i < 16; ++i)
                if (attr[i].enabled && attr[i].data && uc.at[i][2] != 0xFF)
                    s_overlay.sanitize_vertex_upload(p + uc.at[i][0] - s_raw_pos,
                                                      &attr[i], i, lo, count);
    }
    uc.info[2] = 0xFFFFFFFFu;
    if (use_dx) {
        memcpy(p + at, dx, count * 4);
        uc.info[2] = s_raw_pos + at;
        at += count * 4;
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_raw, 0);
    if (!(p = ring_map(s_ib, &s_ib_pos, IB_BYTES, ib_bytes, &m))) {
        RECOMP_PROFILE_END();
        return;
    }
    copy_indices((uint32_t *)p, idx, ni, lo, topology, st->flat_shade);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ib, 0);

    uc.info[1] = program ? 1u : 0u;
    if (program) {
        uint32_t start, pver, cver;
        const uint32_t (*prog)[4] = nv2a_vsh_program(&start, &pver);
        const float (*consts)[4] = nv2a_vsh_constants(&cver);
        uc.info[0] = start;
        if (!prog || !consts) {
            RECOMP_PROFILE_END();
            return;
        }
        if (!s_prog_valid || pver != s_prog_version) {
            if (!upload_cb(s_cb_prog, prog, NV_VSH_SLOTS * 16)) {
                RECOMP_PROFILE_END();
                return;
            }
            s_prog_version = pver;
            s_prog_valid = 1;
        }
        if (!s_vconst_valid || cver != s_vconst_version) {
            if (!upload_cb(s_cb_vconst, consts, 192 * 16)) {
                RECOMP_PROFILE_END();
                return;
            }
            s_vconst_version = cver;
            s_vconst_valid = 1;
        }
    }
    uc.map[0] = 2.0f / (float)gw;
    uc.map[1] = 2.0f / (float)gh;
    uc.map[2] = 1.0f / (float)zmax;
    uc.map[3] = squeeze;
    uc.fogi[0] = st->fog_enable;
    uc.fogi[1] = st->fog_mode;
    uc.fogf[0] = st->fog_param[0];
    uc.fogf[1] = st->fog_param[1];
    uc.place[0] = shift;
    uc.place[1] = yscale;
    /* Display passes must translate position and texture interpolation
     * together. Snapping only the first edge slid glare reads half a guest
     * pixel and left bright silhouette rims; retaining the Xbox offset
     * instead left the first host row/column uncovered. Whole-pixel menu
     * vertices keep their placement; offscreen ripple maps keep the snap. */
    uc.place[2] = s && s->w == GUEST_W && s->h == GUEST_H ? 0.0f : 1.0f;
    uc.place[3] = uc.place[2] == 0.0f ? 0.53125f : 0.0f;
    if (!upload_cb(s_cb_ub, &uc, sizeof uc)) {
        RECOMP_PROFILE_END();
        return;
    }
    {
        ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ib, DXGI_FORMAT_R32_UINT, s_ib_pos);
        bind_vertex_shader(s_ub_vs);
        bind_vertex_constants(s_cb_ub);
        ID3D11DeviceContext_DrawIndexed(s_ctx, ni, 0, 0);
    }
    s_raw_pos += (at + 255) & ~255u;
    s_ib_pos += (ib_bytes + 255) & ~255u;
    s_prof_verts += count;
    if (s_prof_on > 0) s_prof[PROF_DRAW] += qpc() - t_draw;
    RECOMP_PROFILE_END();
}

void nv2a_d3d_clear(const NvD3DState *st, uint32_t flags, uint32_t color,
                    uint32_t zstencil, uint32_t x0, uint32_t y0,
                    uint32_t x1, uint32_t y1)
{
    Surface *s;
    Depth *d;
    uint32_t gw, gh, zf, mask = 0;
    float cc[8];
    D3D11_RECT r;
    int zclear = (flags & 1) != 0, sclear = (flags & 2) != 0;
    int64_t trace_begin = hitch_trace_begin();

    if (!nv2a_d3d_init()) {
        hitch_trace_report("clear_setup", trace_begin);
        return;
    }
    if (!bind_targets(st, zclear || sclear, &s, &d, &gw, &gh)) {
        hitch_trace_report("clear_setup", trace_begin);
        return;
    }
    hitch_trace_report("clear_setup", trace_begin);
    trace_begin = hitch_trace_begin();
    if (s) {
        if (flags & 0x10) mask |= D3D11_COLOR_WRITE_ENABLE_RED;
        if (flags & 0x20) mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
        if (flags & 0x40) mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
        if ((flags & 0x80) && surface_alpha_mode(s->color_fmt) < 0)
            mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    }
    if (!d) { zclear = sclear = 0; }
    if (!mask && !zclear && !sclear) {
        hitch_trace_report("clear_submit", trace_begin);
        return;
    }
    if (s && mask) s->gpu_dirty = 1;
    zf = (st->surface_format >> 4) & 0xF;
    unpack4(color, cc);
    if (s && (s->color_fmt & 0xF) == 0x3) {
        /* Quantise to what a 5:6:5 surface stores. */
        cc[0] = (float)(((color >> 19) & 31) * 255 / 31) / 255.0f;
        cc[1] = (float)(((color >> 10) & 63) * 255 / 63) / 255.0f;
        cc[2] = (float)(((color >> 3) & 31) * 255 / 31) / 255.0f;
    }
    memset(cc + 4, 0, 16);
    cc[4] = zf == 1 ? (float)(zstencil & 0xFFFF) / 65535.0f
                    : (float)(zstencil >> 8) / 16777215.0f;
    upload_cb(s_cb_clear, cc, sizeof cc);
    if (x1 >= gw) x1 = gw - 1;
    if (y1 >= gh) y1 = gh - 1;
    if (x0 > x1 || y0 > y1) {
        hitch_trace_report("clear_submit", trace_begin);
        return;
    }
    r.left = (LONG)(x0 * s_aa_x * s_cur_sx + 0.5f);
    r.top = (LONG)(y0 * s_aa_y * s_cur_sy + 0.5f);
    r.right = (LONG)((x1 + 1) * s_aa_x * s_cur_sx + 0.5f);
    r.bottom = (LONG)((y1 + 1) * s_aa_y * s_cur_sy + 0.5f);
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &r);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs_plain);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, blend_state(0, 1, 0, 0x8006, mask), NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx,
        ds_state(zclear, 7, zclear, sclear, 7, 0xFF, 0xFF, 0x1E01, 0x1E01, 0x1E01),
        zf == 1 ? 0 : (zstencil & 0xFF));
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    bind_vertex_shader(s_clear_vs);
    bind_vertex_constants(s_cb_clear);
    bind_pixel_shader(s_clear_ps);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb_clear);
    ID3D11DeviceContext_Draw(s_ctx, 4, 0);
    hitch_trace_report("clear_submit", trace_begin);
}

/* -------------------------------------------------------------- write-back */

/* What the GPU rendered goes back to guest memory when the title flips, the
 * way the NV2A wrote it before signalling the frame done. The CPU reads and
 * rewrites framebuffers (loading screens persist the display, copy it, draw
 * over it), and without this it sees whatever was there before the GPU drew:
 * a loading image rewritten with the bytes left over from boot would not even
 * register as a change. One stall per flip; the copies are batched first. */
typedef struct {
    UINT w, h;
    int pending;                    /* copied, not yet read back */
    ID3D11Texture2D *stage, *down;
    ID3D11RenderTargetView *down_rtv;
} WbSlot;
static WbSlot s_wb[MAX_SURFACES];
static int s_wb_pending;
static uint32_t s_wb_mask;
static ID3D11Query *s_wb_event;
static int s_wb_event_pending;
static int64_t s_wb_hitch_pending, s_wb_hitch_last, s_wb_hitch_gap;
static uint64_t s_wb_hitch_polls;

/* A readback belongs to the surface that queued it, not the reusable cache
 * slot. A replacement of the same size must not inherit old GPU pixels. */
static void writeback_drop_surface(Surface *s)
{
    unsigned slot = (unsigned)(s - s_surf);
    s_wb[slot].pending = 0;
    s_wb_mask &= ~(1u << slot);
    s_wb_pending = s_wb_mask != 0;
    if (!s_wb_pending) {
        s_wb_event_pending = 0;
        s_wb_hitch_pending = s_wb_hitch_last = s_wb_hitch_gap = 0;
        s_wb_hitch_polls = 0;
    }
}

static int wb_slot_ready(WbSlot *k, UINT w, UINT h, int need_down)
{
    if (k->stage && (k->w != w || k->h != h)) {
        ID3D11Texture2D_Release(k->stage);
        if (k->down_rtv) ID3D11RenderTargetView_Release(k->down_rtv);
        if (k->down) ID3D11Texture2D_Release(k->down);
        memset(k, 0, sizeof *k);
    }
    if (!k->stage) {
        D3D11_TEXTURE2D_DESC d;
        memset(&d, 0, sizeof d);
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &k->stage)))
            return 0;
        k->w = w; k->h = h;
    }
    if (need_down) {
        if (!k->down) {
            k->down = make_texture(w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D3D11_BIND_RENDER_TARGET, 0);
            if (!k->down) return 0;
        }
        if (!k->down_rtv && FAILED(ID3D11Device_CreateRenderTargetView(s_dev,
                (ID3D11Resource *)k->down, NULL, &k->down_rtv))) return 0;
    }
    return 1;
}

/* The copies are queued at the flip; reading them back waited for the GPU to
 * finish the frame, a stall of every flip. Now the executor goes on with the
 * next frame and the ack thread completes the read-back once the GPU is done
 * (nv2a_d3d_writeback_busy), holding back DMA_GET and fence releases until
 * then -- the title cannot learn the frame is finished before its pixels are
 * in guest memory. */
static int wb_surface_ok(const Surface *s)
{
    return s->tex && !s->swizzled && (s->bpp == 2 || s->bpp == 4) &&
           surface_find(s->phys) == s && s->shadow && guest_ok(s->va, (size_t)s->pitch * s->h);
}

static void writeback_trace_finish(int wait, int left, int64_t begin)
{
    int64_t end;
    hitch_trace_report(wait ? "readback_blocking" : "readback_poll", begin);
    if (!begin) return; /* No extra clocks or counters on the default path. */
    if (s_wb_hitch_pending) {
        int64_t gap = begin - s_wb_hitch_last;
        if (gap > s_wb_hitch_gap) s_wb_hitch_gap = gap;
        s_wb_hitch_last = begin;
        if (!wait) s_wb_hitch_polls++;
    } else if (left) {
        /* Begin at the first unfinished poll, excluding intentional vblank. */
        s_wb_hitch_pending = s_wb_hitch_last = begin;
        s_wb_hitch_gap = 0;
        s_wb_hitch_polls = wait ? 0 : 1;
    }
    if (left) return;
    end = hitch_trace_elapsed(s_wb_hitch_pending);
    if (end) fprintf(stderr, "[HITCH] phase=readback_pending qpc_ms=%.3f start_qpc_ms=%.3f "
                    "duration_ms=%.3f thread=%lu poll_count=%llu max_poll_gap_ms=%.3f\n",
                    end * 1000.0 / s_hitch_freq.QuadPart,
                    s_wb_hitch_pending * 1000.0 / s_hitch_freq.QuadPart,
                    (end - s_wb_hitch_pending) * 1000.0 / s_hitch_freq.QuadPart,
                    GetCurrentThreadId(), (unsigned long long)s_wb_hitch_polls,
                    s_wb_hitch_gap * 1000.0 / s_hitch_freq.QuadPart);
    s_wb_hitch_pending = s_wb_hitch_last = s_wb_hitch_gap = 0;
    s_wb_hitch_polls = 0;
}

/* Account inside an individual slow poll. Buffer the measurements until it
 * finishes, so diagnostic I/O cannot be mistaken for a slow driver call. */
typedef struct {
    int64_t query, map, copy, unmap;
    unsigned surfaces, copied_rows, cpu_rows;
    HRESULT query_hr;
    BOOL query_done;
} WbTraceParts;

static void writeback_trace_parts(int wait, int64_t begin, const WbTraceParts *p)
{
    int64_t end = hitch_trace_elapsed(begin);
    double ms;
    if (!end) return;
    ms = 1000.0 / s_hitch_freq.QuadPart;
    fprintf(stderr, "[HITCH] phase=readback_parts qpc_ms=%.3f start_qpc_ms=%.3f "
                    "duration_ms=%.3f thread=%lu wait=%d query_ms=%.3f map_ms=%.3f "
                    "copy_ms=%.3f unmap_ms=%.3f surfaces=%u copied_rows=%u cpu_rows=%u "
                    "query_hr=0x%08lX query_done=%d\n",
            end * ms, begin * ms, (end - begin) * ms, GetCurrentThreadId(), wait,
            p->query * ms, p->map * ms, p->copy * ms, p->unmap * ms,
            p->surfaces, p->copied_rows, p->cpu_rows,
            (unsigned long)p->query_hr, (int)p->query_done);
}

/* Read back what is ready; wait: block until all of it is. 1 when none left. */
static int writeback_finish(int wait)
{
    int i, left = 0;
    int64_t trace_begin;
    WbTraceParts parts = { 0 };
    if (!s_wb_pending)
        return 1;
    trace_begin = hitch_trace_begin();
    RECOMP_PROFILE_BEGIN("D3D11 write-back");
    if (!wait && s_wb_event_pending) {
        BOOL done = FALSE;
        int64_t part_begin = trace_begin ? qpc() : 0;
        HRESULT hr = ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)s_wb_event,
                                               &done, sizeof done, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (trace_begin) {
            parts.query = qpc() - part_begin;
            parts.query_hr = hr;
            parts.query_done = done;
        }
        if (hr == S_FALSE || (hr == S_OK && !done)) {
            RECOMP_PROFILE_END();
            writeback_trace_parts(0, trace_begin, &parts);
            writeback_trace_finish(0, 1, trace_begin);
            return 0;
        }
        /* EVENT completion covers every preceding staging copy. A query
         * failure keeps the original per-resource Map fallback available. */
        s_wb_event_pending = 0;
    }
    for (i = 0; i < MAX_SURFACES; i++) {
        Surface *s = &s_surf[i];
        WbSlot *k = &s_wb[i];
        D3D11_MAPPED_SUBRESOURCE m;
        uint8_t *dst;
        uint32_t x, y, rowb;
        int amode, r565;
        HRESULT hr;
        int64_t part_begin;
        if (!(s_wb_mask & (1u << i)))
            continue;
        if (!wb_surface_ok(s) || k->w != s->w || k->h != s->h) {
            k->pending = 0;                       /* dropped (resize) */
            s_wb_mask &= ~(1u << i);
            continue;
        }
        part_begin = trace_begin ? qpc() : 0;
        hr = ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)k->stage, 0, D3D11_MAP_READ,
                                     wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (trace_begin) parts.map += qpc() - part_begin;
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { left = 1; continue; }
        k->pending = 0;
        s_wb_mask &= ~(1u << i);
        if (FAILED(hr))
            continue;
        if (trace_begin) ++parts.surfaces;
        part_begin = trace_begin ? qpc() : 0;
        dst = (uint8_t *)guest(s->va);
        rowb = s->w * s->bpp;
        amode = surface_alpha_mode(s->color_fmt);
        r565 = (s->color_fmt & 0xF) == 0x3;
        for (y = 0; y < s->h; y++) {
            const uint32_t *src = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
            uint8_t *row = dst + (size_t)y * s->pitch;
            /* A row the CPU changed since the executor last looked holds
             * writes made after this frame was submitted (the title runs
             * ahead): they win, and the next sync uploads them. */
            if (memcmp(row, s->shadow + (size_t)y * rowb, rowb)) {
                if (trace_begin) ++parts.cpu_rows;
                continue;
            }
            if (trace_begin) ++parts.copied_rows;
            if (s->bpp == 4) {
                if (amode < 0) {
                    memcpy(row, src, rowb);
                } else {
                    uint32_t a = amode ? 0xFF000000u : 0, *o = (uint32_t *)row;
                    for (x = 0; x < s->w; x++) o[x] = (src[x] & 0xFFFFFFu) | a;
                }
            } else {
                uint16_t *o = (uint16_t *)row;
                for (x = 0; x < s->w; x++) {
                    uint32_t c = src[x];
                    o[x] = r565 ? (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x1F))
                                : (uint16_t)((amode == 0 ? 0 : 0x8000) | ((c >> 9) & 0x7C00) |
                                             ((c >> 6) & 0x03E0) | ((c >> 3) & 0x1F));
                }
            }
            memcpy(s->shadow + (size_t)y * rowb, row, rowb);
        }
        if (trace_begin) parts.copy += qpc() - part_begin;
        part_begin = trace_begin ? qpc() : 0;
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)k->stage, 0);
        if (trace_begin) parts.unmap += qpc() - part_begin;
    }
    RECOMP_PROFILE_END();
    s_wb_pending = left;
    if (!left) s_wb_event_pending = 0;
    writeback_trace_parts(wait, trace_begin, &parts);
    writeback_trace_finish(wait, left, trace_begin);
    return !left;
}

int nv2a_d3d_writeback_busy(void)
{
    if (!s_wb_pending || writeback_finish(0))
        return 0;
    /* writeback_surfaces already submitted the entire copy batch. Repeated
     * Flush calls while the GPU completes it only add driver work. */
    return 1;
}

static void writeback_surfaces(void)
{
    static int enabled = -1;
    int i;
    if (enabled < 0)
        enabled = !getenv("RECOMP_D3D_NO_WRITEBACK");
    if (!enabled)
        return;
    writeback_finish(1);                          /* the staging slots are reused */
    RECOMP_PROFILE_BEGIN("D3D11 write-back issue");
    for (i = 0; i < MAX_SURFACES; i++) {
        Surface *s = &s_surf[i];
        WbSlot *k = &s_wb[i];
        int scaled;
        if (!s->gpu_dirty || !wb_surface_ok(s))
            continue;
        scaled = s->iw != s->w || s->ih != s->h;
        if (!wb_slot_ready(k, s->w, s->h, scaled))
            continue;
        surface_resolve(s);
        if (scaled) {
            float x0, x1;
            guest_span(s, &x0, &x1);
            blit_span(s->srv, NULL, k->down_rtv, s->w, s->h, -1.0f, 0, 1, x0, x1);
            ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)k->stage,
                                             (ID3D11Resource *)k->down);
        } else {
            ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)k->stage,
                                             (ID3D11Resource *)s->tex);
        }
        s->gpu_dirty = 0;                        /* later draws dirty it again */
        k->pending = 1;
        s_wb_mask |= 1u << i;
        s_wb_pending = 1;
    }
    if (s_wb_pending) {
        if (!s_wb_event) {
            D3D11_QUERY_DESC d = { D3D11_QUERY_EVENT, 0 };
            ID3D11Device_CreateQuery(s_dev, &d, &s_wb_event);
        }
        if (s_wb_event) {
            ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)s_wb_event);
            s_wb_event_pending = 1;
        }
        ID3D11DeviceContext_Flush(s_ctx);
    }
    RECOMP_PROFILE_END();
}

/* ----------------------------------------------------- visibility tests */

/* NV097_SET_ZPASS_PIXEL_COUNT_ENABLE / CLEAR_REPORT_VALUE / GET_REPORT, as a
 * D3D11 occlusion query. Results are read back immediately: a title asks for
 * a handful per frame and polls the report until it is written. */
static ID3D11Query *s_query;
static int s_query_active;
static uint64_t s_zpass;

static void zpass_collect(void)
{
    UINT64 n = 0;
    int64_t trace_begin;
    if (!s_query_active) return;
    trace_begin = hitch_trace_begin();
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)s_query);
    while (ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)s_query, &n,
                                       sizeof n, 0) == S_FALSE)
        SwitchToThread();
    s_zpass += n;
    s_query_active = 0;
    hitch_trace_report("zpass_wait", trace_begin);
}

void nv2a_d3d_zpass_enable(int on)
{
    if (!nv2a_d3d_init()) return;
    if (!s_query) {
        D3D11_QUERY_DESC qd = { D3D11_QUERY_OCCLUSION, 0 };
        if (FAILED(ID3D11Device_CreateQuery(s_dev, &qd, &s_query))) return;
    }
    zpass_collect();
    if (on) {
        ID3D11DeviceContext_Begin(s_ctx, (ID3D11Asynchronous *)s_query);
        s_query_active = 1;
    }
}

void nv2a_d3d_zpass_clear(void)
{
    int was = s_query_active;
    zpass_collect();
    s_zpass = 0;
    if (was) nv2a_d3d_zpass_enable(1);
}

uint32_t nv2a_d3d_zpass_read(void)
{
    int was = s_query_active;
    double px;
    zpass_collect();
    if (was) nv2a_d3d_zpass_enable(1);
    /* In guest pixels: the host target has scale^2 as many, and an MSAA
     * target counts samples. */
    px = (double)s_zpass / ((double)s_cur_sx * s_cur_sy *
                            (s_cur_rt && s_cur_rt->ms ? (double)s_msaa : 1.0));
    return px > 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)px;
}

/* ---------------------------------------------------------------- present */

static ID3D11Texture2D *s_ovl_tex;
static ID3D11ShaderResourceView *s_ovl_srv;
static UINT s_ovl_w, s_ovl_h;
static uint32_t s_ovl_serial = 0xFFFFFFFFu;
static DWORD s_last_present, s_last_flip;
static Surface *s_shown;

/* Display aspect: 4:3 unless the title renders widescreen. */

static int ensure_swapchain(void)
{
    HWND h = xbox_FramebufferWindowHandle();
    RECT rc;
    UINT w, hh;
    if (!h) return 0;
    if (!GetClientRect(h, &rc)) return 0;
    w = (UINT)(rc.right - rc.left);
    hh = (UINT)(rc.bottom - rc.top);
    if (!w || !hh) return 0;                    /* minimised */
    if (s_swap && s_swap_hwnd != h) {
        IDXGISwapChain1_Release(s_swap);
        s_swap = NULL;
    }
    if (!s_swap) {
        IDXGIDevice *dxdev = NULL;
        IDXGIAdapter *ad = NULL;
        IDXGIFactory2 *fac = NULL;
        DXGI_SWAP_CHAIN_DESC1 d;
        HRESULT hr;
        ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice, (void **)&dxdev);
        if (dxdev) IDXGIDevice_GetAdapter(dxdev, &ad);
        if (ad) IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&fac);
        memset(&d, 0, sizeof d);
        d.Width = w; d.Height = hh;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = fac ? IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown *)s_dev, h, &d,
                                                        NULL, NULL, &s_swap) : E_FAIL;
        if (fac) {
            IDXGIFactory2_MakeWindowAssociation(fac, h, DXGI_MWA_NO_ALT_ENTER);
            IDXGIFactory2_Release(fac);
        }
        if (ad) IDXGIAdapter_Release(ad);
        if (dxdev) IDXGIDevice_Release(dxdev);
        if (FAILED(hr)) { fail("CreateSwapChainForHwnd", hr); return 0; }
        s_swap_hwnd = h; s_swap_w = w; s_swap_h = hh;
        fprintf(stderr, "[D3D11] swap chain %ux%u\n", w, hh);
    } else if (w != s_swap_w || hh != s_swap_h) {
        unbind_targets();
        if (FAILED(IDXGISwapChain1_ResizeBuffers(s_swap, 0, w, hh, DXGI_FORMAT_UNKNOWN, 0)))
            return 0;
        s_swap_w = w; s_swap_h = hh;
    }
    return 1;
}

/* RECOMP_PRESENT_CAPTURE=<prefix>; create <prefix>.flag to capture the next
 * presented images (as many as the flag file's number, default 1). */
/* Read back a presentable texture and write it as a 24-bit BMP. */
static void write_bmp(ID3D11Texture2D *bb, const char *path)
{
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    FILE *f;
    ID3D11Texture2D_GetDesc(bb, &d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &st))) return;
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, (ID3D11Resource *)bb);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0,
                                         D3D11_MAP_READ, 0, &m))) {
        uint32_t row = (d.Width * 3 + 3) & ~3u, img = row * d.Height, tot = 54 + img, y, x;
        uint8_t hdr[54], *line = (uint8_t *)calloc(1, row);
        f = fopen(path, "wb");
        if (f && line) {
            memset(hdr, 0, sizeof hdr);
            hdr[0] = 'B'; hdr[1] = 'M';
            memcpy(hdr + 2, &tot, 4);
            hdr[10] = 54; hdr[14] = 40;
            memcpy(hdr + 18, &d.Width, 4);
            memcpy(hdr + 22, &d.Height, 4);
            hdr[26] = 1; hdr[28] = 24;
            memcpy(hdr + 34, &img, 4);
            fwrite(hdr, 1, 54, f);
            for (y = 0; y < d.Height; y++) {
                const uint8_t *src = (const uint8_t *)m.pData + (size_t)(d.Height - 1 - y) * m.RowPitch;
                for (x = 0; x < d.Width; x++) {
                    line[x * 3] = src[x * 4];
                    line[x * 3 + 1] = src[x * 4 + 1];
                    line[x * 3 + 2] = src[x * 4 + 2];
                }
                fwrite(line, 1, row, f);
            }
            fprintf(stderr, "[D3D11] captured %s\n", path);
        }
        if (f) fclose(f);
        free(line);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
}

/* RECOMP_SPIKE_CAPTURE=<prefix>[,ms]: a flip interval over ms (default 50)
 * is logged as [SPIKE] with how much of it was work rather than pacing, and
 * the next two presented frames are saved as <prefix>-<spike>-<n>.bmp --
 * what was on screen as the hitch ended. At most one capture every two
 * seconds and 200 in a session; every spike is still logged. */
static const char *s_spike_prefix = (const char *)-1;
static double s_spike_ms = 50.0;
static unsigned s_spike_pending, s_spike_id, s_spike_shot;
static int64_t s_spike_split[PROF_COUNT];
static LONG s_spec_recent;

static void spike_check(int64_t interval, int64_t pace)
{
    static LARGE_INTEGER freq;
    static DWORD last_capture;
    double ms, work;
    if (s_spike_prefix == (const char *)-1) {
        const char *e = getenv("RECOMP_SPIKE_CAPTURE"), *c;
        s_spike_prefix = NULL;
        if (e && *e) {
            static char buf[512];
            snprintf(buf, sizeof buf, "%s", e);
            if ((c = strrchr(buf, ',')) != NULL && atof(c + 1) > 0.0) {
                s_spike_ms = atof(c + 1);
                buf[c - buf] = 0;
            }
            s_spike_prefix = buf;
            fprintf(stderr, "[SPIKE] capturing frames after flips over %.1f ms to %s-*\n",
                    s_spike_ms, s_spike_prefix);
        }
        QueryPerformanceFrequency(&freq);
    }
    {   /* stage time since the previous flip (the 1 s profile reset aside) */
        static int64_t snap[PROF_COUNT];
        int i;
        for (i = 0; i < PROF_COUNT; i++) {
            int64_t d = s_prof[i] - snap[i];
            s_spike_split[i] = d >= 0 ? d : s_prof[i];
            snap[i] = s_prof[i];
        }
    }
    {   /* specialised shaders that became ready over the last second */
        static LONG ring[64]; static unsigned at;
        LONG now_ready = s_spec_ready;
        s_spec_recent = now_ready - ring[at % 64];
        ring[at++ % 64] = now_ready;
    }
    if (!s_spike_prefix || !interval) return;
    ms = interval * 1000.0 / freq.QuadPart;
    if (ms <= s_spike_ms) return;
    work = ms - pace * 1000.0 / freq.QuadPart;
    s_spike_id++;
    fprintf(stderr, "[SPIKE] #%u frame %u: %.1f ms between flips (%.1f ms not pacing)%s\n",
            s_spike_id, s_frame, ms, work,
            s_spike_id <= 200 && GetTickCount() - last_capture >= 2000 ? ", capturing" : "");
    {
        int i;
        fprintf(stderr, "[SPIKE]   ms:");
        for (i = 0; i < PROF_COUNT; i++)
            fprintf(stderr, " %s %.1f", s_prof_name[i], s_spike_split[i] * 1000.0 / freq.QuadPart);
        fprintf(stderr, " | %ld new shaders in 1 s\n", (long)s_spec_recent);
        recomp_sampler_report(ms);
    }
    if (s_spike_id <= 200 && GetTickCount() - last_capture >= 2000) {
        last_capture = GetTickCount();
        s_spike_pending = 2;
        s_spike_shot = 0;
    }
}

static void capture_backbuffer(ID3D11Texture2D *bb)
{
    static const char *prefix = (const char *)-1;
    static unsigned remaining, frame;
    char path[1024];
    FILE *f;
    if (s_spike_pending && s_spike_prefix && s_spike_prefix != (const char *)-1) {
        snprintf(path, sizeof path, "%s-%04u-%u.bmp", s_spike_prefix, s_spike_id, s_spike_shot++);
        write_bmp(bb, path);
        s_spike_pending--;
    }
    if (prefix == (const char *)-1) prefix = getenv("RECOMP_PRESENT_CAPTURE");
    if (!prefix) return;
    if (!remaining) {
        int count = 0;
        snprintf(path, sizeof path, "%s.flag", prefix);
        f = fopen(path, "rb");
        if (!f) return;
        if (fscanf(f, "%d", &count) != 1 || count < 1) count = 1;
        fclose(f);
        remove(path);
        remaining = (unsigned)count;
    }
    snprintf(path, sizeof path, "%s-%03u.bmp", prefix, frame++);
    write_bmp(bb, path);
    remaining--;
}

/* Above 0 while a load keeps the last frame up: the spinner's phase. */
static float s_loading;

/* Draw surface `s` (with the overlay described in *pc) into a w x h target,
 * letterboxed to the display aspect when asked. */
static void composite(ID3D11RenderTargetView *rtv, UINT w, UINT h, Surface *s,
                      PresentConsts *pc, int letterbox)
{
    float target_w = (float)w, target_h = (float)h, x0 = 0, y0 = 0;
    float black[4] = { 0, 0, 0, 1 };
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;
    if (letterbox) {
        target_h = target_w / nv2a_d3d_display_aspect();
        if (target_h > (float)h) {
            target_h = (float)h;
            target_w = target_h * nv2a_d3d_display_aspect();
        }
        x0 = ((float)w - target_w) * 0.5f;
        y0 = ((float)h - target_h) * 0.5f;
    }
    if (letterbox) {
        AcquireSRWLockExclusive(&s_mr_lock);
        s_present_rect[0] = x0 / w; s_present_rect[1] = y0 / h;
        s_present_rect[2] = target_w / w; s_present_rect[3] = target_h / h;
        ReleaseSRWLockExclusive(&s_mr_lock);
    }
    pc->dst_rect[0] = x0 / w * 2 - 1;
    pc->dst_rect[1] = 1 - y0 / h * 2;
    pc->dst_rect[2] = (x0 + target_w) / w * 2 - 1;
    pc->dst_rect[3] = 1 - (y0 + target_h) / h * 2;
    pc->src_uv[0] = pc->src_uv[1] = 0;
    pc->src_uv[2] = 1; pc->src_uv[3] = 1;
    pc->src_size[0] = (float)s->w; pc->src_size[1] = (float)s->h;
    pc->src_size[2] = (float)s->iw; pc->src_size[3] = (float)s->ih;
    pc->force_alpha[0] = 1.0f;
    pc->force_alpha[1] = letterbox ? s_loading : 0.0f;   /* the window only */
    pc->force_alpha[2] = (float)w;
    pc->force_alpha[3] = (float)h;
    if (pc->ovl_flags[0] && s->sx > s->sy * 1.001f) {
        pc->ovl_in[2] = s->sx / s->sy;
        pc->ovl_in[3] = (float)s->w * 0.5f;
    }
    upload_cb(s_cb_present, pc, sizeof *pc);
    unbind_textures();
    unbind_targets();
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, rtv, black);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rtv, NULL);
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)w; vp.Height = (float)h;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    sc.left = 0; sc.top = 0; sc.right = (LONG)w; sc.bottom = (LONG)h;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sc);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs_plain);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, s_blend_opaque, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, s_ds_off, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    bind_vertex_shader(s_present_vs);
    bind_vertex_constants(s_cb_present);
    bind_pixel_shader(s_present_ps);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb_present);
    surface_resolve(s);
    {
        ID3D11ShaderResourceView *views[2] = { s->srv, pc->ovl_flags[0] ? s_ovl_srv : NULL };
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 2, views);
    }
    bind_pixel_samplers(1, &s_linear_clamp);
    ID3D11DeviceContext_Draw(s_ctx, 4, 0);
    unbind_targets();
    unbind_textures();
}

/* The overlay, uploaded when a new frame arrived, as present constants. */
static void prepare_overlay(PresentConsts *pc)
{
    XboxOverlay ovl;
    if (!xbox_FramebufferOverlay(&ovl))
        return;
    if (ovl.serial != s_ovl_serial || !s_ovl_tex) {
        uint32_t *px = scratch_pixels((size_t)ovl.in_w * ovl.in_h), x, y;
        if (px) {
            for (y = 0; y < ovl.in_h; y++)
                for (x = 0; x + 1 < ovl.in_w; x += 2)
                    yuv_pair(ovl.yuy2 + (size_t)y * ovl.pitch + x * 2, 0,
                             px + (size_t)y * ovl.in_w + x);
            if (!s_ovl_tex || s_ovl_w != ovl.in_w || s_ovl_h != ovl.in_h) {
                if (s_ovl_srv) ID3D11ShaderResourceView_Release(s_ovl_srv);
                if (s_ovl_tex) ID3D11Texture2D_Release(s_ovl_tex);
                s_ovl_srv = NULL;
                s_ovl_tex = make_texture(ovl.in_w, ovl.in_h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM,
                                         D3D11_BIND_SHADER_RESOURCE, 0);
                if (s_ovl_tex)
                    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_ovl_tex,
                                                          NULL, &s_ovl_srv);
                s_ovl_w = ovl.in_w; s_ovl_h = ovl.in_h;
            }
            if (s_ovl_tex)
                ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_ovl_tex,
                                                      0, NULL, px, ovl.in_w * 4, 0);
        }
        s_ovl_serial = ovl.serial;
    }
    if (!s_ovl_srv)
        return;
    pc->ovl_flags[0] = 1;
    pc->ovl_flags[1] = ovl.color_key_enabled;
    pc->ovl_flags[2] = ovl.color_key;
    pc->ovl_out[0] = (float)ovl.out_x; pc->ovl_out[1] = (float)ovl.out_y;
    pc->ovl_out[2] = (float)ovl.out_w; pc->ovl_out[3] = (float)ovl.out_h;
    pc->ovl_walk[0] = ovl.start_s / 1048576.0f;
    pc->ovl_walk[1] = ovl.start_t / 1048576.0f;
    pc->ovl_walk[2] = ovl.ds_dx / 1048576.0f;
    pc->ovl_walk[3] = ovl.dt_dy / 1048576.0f;
    pc->ovl_in[0] = (float)ovl.in_w; pc->ovl_in[1] = (float)ovl.in_h;
    pc->ovl_in[2] = 1.0f;
}

/* RECOMP_CAPTURE_SURFACE: the composited guest image at internal resolution
 * (no window scaling or letterbox), for pixel comparisons. */
static ID3D11Texture2D *s_cap_tex;
static ID3D11RenderTargetView *s_cap_rtv;
static UINT s_cap_w, s_cap_h;

/* A copy of the last image presented, for nv2a_d3d_repeat_frame: the guest
 * may already be drawing the next frame into the surface it came from. */
static ID3D11Texture2D *s_kept;

static void keep_image(ID3D11Texture2D *bb)
{
    D3D11_TEXTURE2D_DESC d, k;
    ID3D11Texture2D_GetDesc(bb, &d);
    if (s_kept) {
        ID3D11Texture2D_GetDesc(s_kept, &k);
        if (k.Width != d.Width || k.Height != d.Height || k.Format != d.Format) {
            ID3D11Texture2D_Release(s_kept);
            s_kept = NULL;
        }
    }
    if (!s_kept) {
        d.BindFlags = 0;
        d.MiscFlags = 0;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.CPUAccessFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &s_kept)))
            s_kept = NULL;
    }
    if (s_kept)
        ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_kept, (ID3D11Resource *)bb);
}

/* The kept image presented again; 0 if there is none to show. */
static int present_kept(void)
{
    ID3D11Texture2D *bb = NULL;
    D3D11_TEXTURE2D_DESC d, k;
    static int vsync = -1;
    if (!s_kept || !s_swap ||
        FAILED(IDXGISwapChain1_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&bb)))
        return 0;
    ID3D11Texture2D_GetDesc(bb, &d);
    ID3D11Texture2D_GetDesc(s_kept, &k);
    if (k.Width != d.Width || k.Height != d.Height || k.Format != d.Format) {
        ID3D11Texture2D_Release(bb);
        return 0;                           /* resized since: draw afresh */
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)bb, (ID3D11Resource *)s_kept);
    capture_backbuffer(bb);
    if (vsync < 0) vsync = getenv("RECOMP_VSYNC") ? atoi(getenv("RECOMP_VSYNC")) : 0;
    IDXGISwapChain1_Present(s_swap, (UINT)vsync, 0);
    ID3D11Texture2D_Release(bb);
    s_last_present = GetTickCount();
    return 1;
}

/* One wait per guest flip; tick/movie presentation passes no pacing state. */
typedef struct FlipPacing {
    int done;
    int64_t ticks;
} FlipPacing;

static void pace_vblank(void);

static int pace_after_present(void)
{
    static int after = -1;
    if (after < 0) {
        const char *e = getenv("RECOMP_PACE_AFTER_PRESENT");
        after = e && atoi(e) != 0;
    }
    return after;
}

static void pace_flip(FlipPacing *pacing)
{
    int64_t begin;
    if (!pacing || pacing->done) return;
    pacing->done = 1;
    begin = qpc();
    pace_vblank();
    pacing->ticks = qpc() - begin;
    if (s_prof_on > 0) s_prof[PROF_PACE] += pacing->ticks;
}

static void present_surface(Surface *s, FlipPacing *pacing)
{
    PresentConsts pc;
    s_last_present = GetTickCount();
    if (!s)
        return;
    RECOMP_PROFILE_BEGIN("D3D11 present");
    memset(&pc, 0, sizeof pc);
    prepare_overlay(&pc);
    if (getenv("RECOMP_CAPTURE_SURFACE")) {
        if (!s_cap_tex || s_cap_w != s->iw || s_cap_h != s->ih) {
            if (s_cap_rtv) ID3D11RenderTargetView_Release(s_cap_rtv);
            if (s_cap_tex) ID3D11Texture2D_Release(s_cap_tex);
            s_cap_rtv = NULL;
            s_cap_tex = make_texture(s->iw, s->ih, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM,
                                     D3D11_BIND_RENDER_TARGET, 0);
            if (s_cap_tex)
                ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s_cap_tex,
                                                    NULL, &s_cap_rtv);
            s_cap_w = s->iw; s_cap_h = s->ih;
        }
        if (s_cap_rtv) {
            composite(s_cap_rtv, s->iw, s->ih, s, &pc, 0);
            capture_backbuffer(s_cap_tex);
        }
    }
    if (ensure_swapchain()) {
        ID3D11Texture2D *bb = NULL;
        ID3D11RenderTargetView *rtv = NULL;
        if (SUCCEEDED(IDXGISwapChain1_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&bb))) {
            static int vsync = -1;
            HRESULT present_hr;
            int64_t present_trace;
            if (FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)bb,
                                                         NULL, &rtv)) || !rtv) {
                ID3D11Texture2D_Release(bb);
                RECOMP_PROFILE_END();
                return; /* The guest flip still receives its fallback wait. */
            }
            composite(rtv, s_swap_w, s_swap_h, s, &pc, 1);
            keep_image(bb);
            if (!getenv("RECOMP_CAPTURE_SURFACE"))
                capture_backbuffer(bb);
            if (vsync < 0) vsync = getenv("RECOMP_VSYNC") ? atoi(getenv("RECOMP_VSYNC")) : 0;
            frame_csv_init();
            /* Composite and capture are complete; align the actual host
             * submission, without pacing tick/movie-only presentations. */
            if (pacing && !pace_after_present()) pace_flip(pacing);
            present_trace = hitch_trace_begin();
            present_hr = IDXGISwapChain1_Present(s_swap, (UINT)vsync, 0);
            if (present_hr == S_OK)
                s->cpu_present_pending = 0;
            if (pacing && s_frame_csv_file)
                frame_csv_present(qpc(), present_hr);
            hitch_trace_report("present", present_trace);
            ID3D11RenderTargetView_Release(rtv);
            ID3D11Texture2D_Release(bb);
        }
    }
    RECOMP_PROFILE_END();
}

/* The flip completes at the display's next vertical blank, as it does on the
 * NV2A: FLIP_STALL holds the GPU until then, and the title, waiting on the
 * GPU, is paced by it. Completing it at once let menus run at hundreds of
 * frames a second on a busy core. RECOMP_FPS_LIMIT=N sets the rate (default
 * 60, 0 = unpaced). The wait is a high-resolution timer, then a short spin. */
static void pace_vblank(void)
{
    static int limit = -1;
    static LARGE_INTEGER freq;
    static int64_t next;
    static HANDLE timer;
    int64_t now;
    extern int xbox_FramebufferPaused(void);
    while (xbox_FramebufferPaused()) {   /* in the background: hold the vblank */
        Sleep(50);
        next = 0;                        /* and start a new cadence after */
    }
    if (limit < 0) {
        const char *e = getenv("RECOMP_FPS_LIMIT");
        limit = e ? atoi(e) : 60;
        QueryPerformanceFrequency(&freq);
        timer = nv_pacing_create_timer();
    }
    if (limit <= 0)
        return;
    now = qpc();
    /* A frame that ran late is shown at once and starts a new cadence;
     * waiting for the next boundary would turn every slow frame into two. */
    if (!nv_pacing_deadline(&next, now, freq.QuadPart, limit))
        return;
    {
        RECOMP_PROFILE_BEGIN("D3D11 pace");
        nv_pacing_wait(timer, freq.QuadPart, next, now);
        RECOMP_PROFILE_END();
    }
}

/* Internal resolution and aspect follow the window: RECOMP_RENDER_SCALE=N
 * and RECOMP_ASPECT pin them, else the window's client area -- the guest's
 * 480 lines become as many as the window has, at its aspect (F11 fullscreen:
 * the monitor's native resolution and shape). A resize applies once it has
 * settled for RESIZE_SETTLE_MS, by dropping every host surface; they are
 * rebuilt at the new size from guest memory, which the flip just updated.
 * A title whose projection is built once per scene can ask that an aspect
 * change wait (NvOverlayHooks.hold_aspect) rather than stretch the view. */
#define RESIZE_SETTLE_MS 300
static void display_config(int now)
{
    static UINT seen_w, seen_h, done_w, done_h;
    static DWORD seen_at;
    const char *e = getenv("RECOMP_RENDER_SCALE");
    float scale, aspect = env_aspect();
    UINT w, h;
    int i, deferred;
    if (!client_size(&w, &h)) {
        if (!now) return;
        w = (UINT)(GUEST_H * nv2a_d3d_display_aspect()); h = GUEST_H;
    }
    if (!now) {
        if (w != seen_w || h != seen_h) { seen_w = w; seen_h = h; seen_at = GetTickCount(); return; }
        if ((w == done_w && h == done_h) || GetTickCount() - seen_at < RESIZE_SETTLE_MS) return;
    }
    if (!aspect) aspect = clamp_aspect((float)w / (float)h);
    deferred = !now && s_aspect && fabsf(aspect - s_aspect) > 1e-4f &&
               s_overlay.hold_aspect && s_overlay.hold_aspect();
    if (deferred) aspect = s_aspect;             /* retried every flip */
    if (e && atof(e) >= 0.5 && atof(e) <= 8.0) {
        scale = (float)atof(e);
    } else {
        /* RECOMP_SUPERSAMPLE=N (1..4) renders N x N host pixels per window
         * pixel; presentation's bilinear minification averages them back
         * (an exact 2x2 box at the default N=2). */
        const char *ss = getenv("RECOMP_SUPERSAMPLE");
        int n = ss ? atoi(ss) : 2;
        float fit_h = (float)h < (float)w / aspect ? (float)h : (float)w / aspect;
        if (n < 1) n = 1;
        if (n > 4) n = 4;
        scale = fit_h / (float)GUEST_H * (float)n;
        if (scale < 1.0f) scale = 1.0f;
        if (scale > 8.0f) scale = 8.0f;
    }
    if (!deferred) { done_w = w; done_h = h; }
    if (!now && fabsf(scale - s_scale) < 1e-3f && fabsf(aspect - s_aspect) < 1e-4f)
        return;
    if (!now) {
        unbind_targets();
        unbind_textures();
        for (i = 0; i < MAX_SURFACES; i++)
            if (s_surf[i].tex) surface_release(&s_surf[i]);
        for (i = 0; i < MAX_DEPTHS; i++)
            depth_release(&s_depth[i]);
        s_shown = NULL;
    }
    s_scale = scale;
    s_aspect = aspect;
    fprintf(stderr, "[D3D11] rendering %ux%u (window %ux%u, aspect %.3f)\n",
            (uint32_t)(GUEST_W * s_scale * widen() + 0.5f), (uint32_t)(GUEST_H * s_scale + 0.5f),
            w, h, aspect);
    fflush(stderr);
}

/* A display buffer read from guest memory: the surface at that address,
 * brought up to date with whatever the CPU wrote there. */
static Surface *scan_surface(uint32_t va, uint32_t pitch)
{
    uint32_t bpp = pitch >= 640 * 4 ? 4 : 2;
    uint32_t w = pitch / bpp, h = 480;
    Surface *s = surface_find(phys(va));
    if (w > 640) w = 640;
    if (!s || s->pitch != pitch)
        s = surface_get(va, pitch, bpp == 4 ? 0x5 : 0x3, w, h, 0);
    if (s) surface_sync(s, 1);
    return s;
}

void nv2a_d3d_flip(uint32_t surface_addr, uint32_t pitch)
{
    Surface *s;
    FlipPacing pacing = { 0 };
    if (!nv2a_d3d_init())
        return;
    if (s_overlay.frame) s_overlay.frame();
    if (s_mr_frame_n || s_mr_shown_n) {
        AcquireSRWLockExclusive(&s_mr_lock);
        memcpy(s_mr_shown, s_mr_frame, (size_t)s_mr_frame_n * sizeof *s_mr_frame);
        s_mr_shown_n = s_mr_frame_n;
        ReleaseSRWLockExclusive(&s_mr_lock);
        s_mr_frame_n = 0;
    }
    s_depth_hazed = 0;
    s = surface_find(phys(surface_addr));
    if (!s && pitch)
        s = scan_surface(surface_addr, pitch);
    s_shown = s;
    s_last_flip = GetTickCount();
    {
        int64_t t0 = s_prof_on > 0 ? qpc() : 0, t1;
        writeback_surfaces();
        t1 = s_prof_on > 0 ? qpc() : 0;
        present_surface(s, &pacing);
        if (s_prof_on > 0) {
            s_prof[PROF_WRITEBACK] += t1 - t0;
            s_prof[PROF_PRESENT] += qpc() - t1 - pacing.ticks;
        }
    }
    /* Legacy A/B mode, or a missing surface/swap-chain/GetBuffer path,
     * still completes exactly one paced guest flip. A failed Present has
     * already waited before the attempt and must not wait a second time. */
    pace_flip(&pacing);
    frame_csv(pacing.ticks);
    RECOMP_PROFILE_FRAME();
    prof_report();
    display_config(0);
    rdoc_poll();
    s_frame++;
}

/* The last flipped frame shown again, paced like a flip: for a title that
 * has nothing new to draw this frame (MM3's console-rate mod between world
 * updates). Asked from the title's thread, done on this one, which owns the
 * device; the asker waits for it as it would for a flip. */
static volatile LONG s_repeat;

void nv2a_d3d_repeat_frame(void)
{
    if (!s_shown)
        return;
    InterlockedExchange(&s_repeat, 1);
    while (InterlockedCompareExchange(&s_repeat, 1, 1))
        SwitchToThread();
}

void nv2a_d3d_tick(void)
{
    DWORD now;
    uint32_t va, pitch;
    /* CPU scanout and PVIDEO can precede the first pushbuffer flip. */
    if (s_init_state <= 0 && !nv2a_d3d_init())
        return;
    if (s_repeat) {
        int64_t t0;
        if (!present_kept())
            present_surface(s_shown, NULL);
        t0 = qpc();
        pace_vblank();
        frame_csv(qpc() - t0);
        s_frame++;
        InterlockedExchange(&s_repeat, 0);
        return;
    }
    now = GetTickCount();
    if (now - s_last_present < 16)
        return;
    /* A stop in flipping after the title has started, with no movie, is a
     * load (a level takes ~5 s, shown by CPU scanout or not at all): the
     * frame gets a turning arc so it does not look hung. With
     * RECOMP_LOADING_SPINNER=0 it stays still, as on the console. */
    static int spinner = -1;
    XboxOverlay ovl;
    if (spinner < 0)
        spinner = !getenv("RECOMP_LOADING_SPINNER") || atoi(getenv("RECOMP_LOADING_SPINNER")) != 0;
    int movie = xbox_FramebufferOverlay(&ovl);
    s_loading = spinner && !movie && s_last_flip && now - s_last_flip > 700
              ? 1.0f + (float)(now % 1000) / 1000.0f : 0.0f;
    if (xbox_FramebufferScanSource(&va, &pitch)) {
        Surface *s = scan_surface(va, pitch);
        present_surface(s, NULL);
        s_loading = 0.0f;
        s_frame++;
        return;
    }
    /* A title can keep drawing the displayed buffer with the CPU after its
     * last GPU flip, without another AvSetDisplayMode. The explicit scanout
     * above may already have handed control back to flips at that point. */
    if (s_shown && now - s_last_flip > 50) {
        uint32_t generation = s_shown->gen;
        /* Do not commit a new shadow while an older GPU readback is pending:
         * readback uses that shadow to identify and preserve later CPU writes.
         * Force the check because the submission epoch stops with the flips. */
        if (!s_wb_pending)
            surface_sync(s_shown, 1);
        if (s_shown->gen != generation)
            s_shown->cpu_present_pending = 1;
        /* Keep the update pending across swap-chain failures or occlusion;
         * a successful Present clears it. Unchanged idle buffers need no blit. */
        if (s_shown->cpu_present_pending || movie ||
            (s_loading > 0.0f && now - s_last_present >= 33))
            present_surface(s_shown, NULL);
        else
            s_last_present = now;
    }
    s_loading = 0.0f;
}

#else
#include "nv2a_d3d11.h"
int  nv2a_d3d_init(void) { return 0; }
int  nv2a_d3d_active(void) { return 0; }
int nv2a_d3d_writeback_busy(void) { return 0; }
int nv2a_d3d_prof_enabled(void) { return 0; }
void nv2a_d3d_prof_add(int slot, int64_t ticks, uint32_t verts)
{ (void)slot; (void)ticks; (void)verts; }
void nv2a_d3d_clear(const NvD3DState *st, uint32_t f, uint32_t c, uint32_t z,
                    uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{ (void)st; (void)f; (void)c; (void)z; (void)x0; (void)y0; (void)x1; (void)y1; }
void nv2a_d3d_flip(uint32_t a, uint32_t p) { (void)a; (void)p; }
void nv2a_d3d_tick(void) {}
void nv2a_d3d_repeat_frame(void) {}
void nv2a_d3d_kick(void) {}
void nv2a_d3d_draw_ub(const NvD3DState *st, int t, const NvD3DAttrib a[16], const float d[16][4],
                      int pr, const uint32_t *i, uint32_t n, uint32_t lo, uint32_t hi,
                      const float (*pos)[4])
{ (void)st; (void)t; (void)a; (void)d; (void)pr; (void)i; (void)n; (void)lo; (void)hi; (void)pos; }
int nv2a_d3d_widescreen(void) { return 0; }
void nv2a_d3d_collect_menu_rects(int on) { (void)on; }
int nv2a_d3d_menu_rects_on(void) { return 0; }
void nv2a_d3d_set_menu_tints(const uint32_t *tints) { (void)tints; }
int nv2a_d3d_menu_rects(NvMenuRect *out, int max, float present[4])
{ (void)out; (void)max; present[0] = present[1] = present[2] = present[3] = 0; return 0; }
void nv2a_d3d_set_bspline_blend(uint32_t sfactor, uint32_t dfactor) { (void)sfactor; (void)dfactor; }
void nv2a_d3d_set_overlay_hooks(const NvOverlayHooks *hooks) { (void)hooks; }
void nv2a_d3d_zpass_enable(int on) { (void)on; }
void nv2a_d3d_zpass_clear(void) {}
uint32_t nv2a_d3d_zpass_read(void) { return 0; }
float nv2a_d3d_display_aspect(void) { return 4.0f / 3.0f; }
void nv2a_d3d_set_flare_depth_test(int on) { (void)on; }
void nv2a_d3d_set_depth_view_remap(float scale, float knee) { (void)scale; (void)knee; }
#endif
