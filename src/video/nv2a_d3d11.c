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
static int s_prof_on = -1;

static int64_t qpc(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void nv2a_d3d_prof_add(int slot, int64_t ticks, uint32_t verts)
{
    s_prof[slot] += ticks;
    s_prof_verts += verts;
}

int nv2a_d3d_prof_enabled(void)
{
    if (s_prof_on < 0) s_prof_on = getenv("RECOMP_D3D_PROFILE") != NULL;
    return s_prof_on;
}

static void prof_report(void)
{
    static int64_t last, frames, prev, worst;
    static LARGE_INTEGER freq;
    int64_t now = qpc();
    int i;
    if (!nv2a_d3d_prof_enabled()) return;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    frames++;
    if (prev && now - prev > worst) worst = now - prev;
    prev = now;
    if (!last) { last = now; return; }
    if (now - last < freq.QuadPart) return;
    fprintf(stderr, "[D3D11_PROF] %.1f fps (worst %.1f ms) |",
            frames * (double)freq.QuadPart / (double)(now - last),
            worst * 1000.0 / (double)freq.QuadPart);
    for (i = 0; i < PROF_COUNT; i++)
        fprintf(stderr, " %s %.2f", s_prof_name[i],
                s_prof[i] * 1000.0 / (double)freq.QuadPart / (double)frames);
    fprintf(stderr, " ms/frame | %llu verts %llu draws/frame\n",
            (unsigned long long)(s_prof_verts / frames), (unsigned long long)(s_prof_draws / frames));
    fflush(stderr);
    memset(s_prof, 0, sizeof s_prof);
    s_prof_verts = s_prof_draws = 0;
    frames = 0;
    worst = 0;
    last = now;
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

/* Race HUD edge placement is latched: the title builds its race camera's
 * projection once as a race starts (src/mm3_widescreen.c reports it), and
 * car select's camera ends it. Frames that draw a full-screen background
 * image are menus or loading screens and never get it, which keeps menus
 * reached from a race (and the race's own loading screen) in 4:3 layout. */
static volatile LONG s_race_hud;
static uint32_t s_frame_bg_tex;     /* this frame's background image; reset at flip */
static int s_frame_tag_window;      /* still in the frame's leading name tags */

/* Name tags over cars (world-anchored 2D) are the first 2D a race frame
 * draws, ahead of the HUD; the title names the HUD's first element by its
 * 4:3 rectangle (nv2a_d3d_set_hud_start). Textures drawn from there on are
 * learned as HUD textures. In each race frame the leading draws keep their
 * projected position (hud_split's follow mode) until the HUD start or a
 * known HUD texture -- so frames without the minimap (zoomed map, pause)
 * still end the tags at their first HUD element. */
static float s_hud_start[4];
static uint32_t s_hud_tex[32];
static int s_hud_tex_n;

void nv2a_d3d_set_hud_start(float x0, float y0, float x1, float y1)
{
    s_hud_start[0] = x0; s_hud_start[1] = y0; s_hud_start[2] = x1; s_hud_start[3] = y1;
}

static int world_tag_draw(float lo, float hi, float ylo, float yhi, uint32_t tex)
{
    int i;
    if (!s_hud_start[2])
        return 0;
    if (s_frame_tag_window &&
        fabsf(lo - s_hud_start[0]) < 2.0f && fabsf(ylo - s_hud_start[1]) < 2.0f &&
        fabsf(hi - s_hud_start[2]) < 2.0f && fabsf(yhi - s_hud_start[3]) < 2.0f)
        s_frame_tag_window = 0;
    for (i = 0; i < s_hud_tex_n && s_hud_tex[i] != tex; i++) {}
    if (s_frame_tag_window && i == s_hud_tex_n)
        return 1;
    s_frame_tag_window = 0;
    if (i == s_hud_tex_n && s_hud_tex_n < 32)
        s_hud_tex[s_hud_tex_n++] = tex;
    return 0;
}

void nv2a_d3d_note_race_camera(void)
{
    if (!InterlockedExchange(&s_race_hud, 1))
        s_hud_tex_n = 0;                 /* a new race may load other textures */
}
void nv2a_d3d_note_frontend_camera(void) { InterlockedExchange(&s_race_hud, 0); }

static int nv2a_d3d_hud_active(void)
{
    return !s_frame_bg_tex && InterlockedCompareExchange(&s_race_hud, 0, 0) != 0;
}

/* The aspect the image is rendered at, never narrower than the title's
 * 4:3: RECOMP_ASPECT (e.g. 16:9, 21:9, 1.6, 4:3), else the game window's
 * client area, following it as the window is resized (see display_config).
 * The title's projection is widened to match (src/mm3_widescreen.c). */
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

/* Four independent lanes so the multiply chain does not serialise. */
static uint64_t hash_mem(const uint8_t *p, size_t n)
{
    uint64_t a = 0x9E3779B97F4A7C15ull, b = 0xC2B2AE3D27D4EB4Full,
             c = 0x165667B19E3779F9ull, d = 0x27D4EB2F165667C5ull ^ n;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t w[4];
        memcpy(w, p + i, 32);
        a = (a ^ w[0]) * 0x100000001B3ull; a ^= a >> 31;
        b = (b ^ w[1]) * 0x100000001B3ull; b ^= b >> 31;
        c = (c ^ w[2]) * 0x100000001B3ull; c ^= c >> 31;
        d = (d ^ w[3]) * 0x100000001B3ull; d ^= d >> 31;
    }
    for (; i < n; i++)
        a = (a ^ p[i]) * 0x100000001B3ull;
    return a ^ (b * 3) ^ (c * 5) ^ (d * 7);
}

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
"      /* ponytail: writes to the constant file (out-to-c[]) are dropped; no\n"
"       * MM3 program does it. A local copy of C[] would support it. */\n"
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
 * block of host pixels and leaves its first half unwritten -- MM3's wrapped
 * water ripple map then showed the unwritten strip as seams across the
 * water. A coordinate that truncates to that first centre is placed on the
 * target edge, as the hardware covers it; everything else is unchanged. */
"  float py = P.y;\n"
"  if (px >= 0.5 && px < 0.5625) px = 0.0;\n"
"  if (py >= 0.5 && py < 0.5625) py = 0.0;\n"
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
"float4 samp2(uint n, float2 uv) {\n"
"  if (n == 0) return T2D0.Sample(S0, uv); if (n == 1) return T2D1.Sample(S1, uv);\n"
"  if (n == 2) return T2D2.Sample(S2, uv); return T2D3.Sample(S3, uv);\n"
"}\n"
"float4 sampc(uint n, float3 d) {\n"
"  if (n == 0) return TC0.Sample(S0, d); if (n == 1) return TC1.Sample(S1, d);\n"
"  if (n == 2) return TC2.Sample(S2, d); return TC3.Sample(S3, d);\n"
"}\n"
"float4 shade(PSIn i, out uint cov) {\n"
"  float4 tc[4] = { i.t0, i.t1, i.t2, i.t3 };\n"
"  float4 T[4] = { float4(0,0,0,1), float4(0,0,0,1), float4(0,0,0,1), float4(0,0,0,1) }; float dots[4] = { 0, 0, 0, 0 };\n"
"  [unroll] for (uint n = 0; n < 4; n++) {\n"
"    uint mode = (stage_prog >> (n * 5)) & 31;\n"
"    uint inp = n == 2 ? (other_input >> 16) & 1 : (n == 3 ? (other_input >> 20) & 3 : 0);\n"
"    uint dm = n ? (dot_map >> ((n - 1) * 4)) & 7 : 0;\n"
"    float4 c = tc[n]; float4 t = float4(0, 0, 0, 1);\n"
"    float q = c.w != 0.0 ? c.w : 1.0;\n"
"    if (mode == 1 || mode == 2) t = samp2(n, c.xy / q * tex_scale[n].xy);\n"
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
/* W-buffering (SET_CONTROL0 Z_PERSPECTIVE, which MM3 uses): the depth
 * buffer holds w, interpolated perspective-correctly (fog.y carries it), in
 * 24-bit fixed point -- linear precision, unlike z/w, so distant decals and
 * foliage do not fight. Polygon offset is applied here as xemu does: the
 * rasteriser's depth bias never reaches a shader-written depth. zp0 = offset
 * factor, units, host pixels per guest pixel x/y; zp1 = 1/(zmax+1), zmax. */
"void ps_wdepth(PSIn i, out float4 c : SV_Target, out float dep : SV_Depth, out uint cov : SV_Coverage) {\n"
"  c = shade(i, cov);\n"
"  float z = i.fog.y;\n"
"  z += zp0.y + zp0.x * max(abs(ddx(z)) * zp0.z, abs(ddy(z)) * zp0.w);\n"
"  z = clamp(z, 0.0, zp1.y);\n"
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
 * of packed values (MM3 packs depth into G:B for its fog) would corrupt them. */
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
    float place[4];              /* x shift (NDC), y scale */
} UbConsts;

typedef struct {
    float dst_rect[4], src_uv[4], src_size[4];
    float ovl_out[4], ovl_walk[4], ovl_in[4];
    uint32_t ovl_flags[4];
    float force_alpha[4];
} PresentConsts;

static void display_config(int now);

static ID3DBlob *compile_def(const char *entry, const char *target, const D3D_SHADER_MACRO *def)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "nv2a_d3d11", def, NULL,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11] shader %s: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        fflush(stderr);
    }
    if (err) ID3D10Blob_Release(err);
    return SUCCEEDED(hr) ? code : NULL;
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
    uint32_t tex_info[4][2];
} PsKey;

typedef struct {
    PsKey key;
    volatile LONG state;            /* 0 empty, 1 queued, 2 ready, 3 failed */
    ID3D11PixelShader *ps;          /* valid once state is 2 */
} PsSpec;

#define PS_SPEC_SLOTS 4096          /* power of two */
#define PS_SPEC_QUEUE 1024
static PsSpec *s_spec;
static PsSpec *s_spec_q[PS_SPEC_QUEUE];
static unsigned s_spec_head, s_spec_tail;  /* guarded by s_spec_cs */
static CRITICAL_SECTION s_spec_cs;
static CONDITION_VARIABLE s_spec_cv;
static volatile LONG s_spec_ready;

static DWORD WINAPI spec_worker(LPVOID arg)
{
    (void)arg;
    for (;;) {
        PsSpec *e;
        const PsKey *k;
        char decl[2048];
        ID3DBlob *b;
        int flat;
        D3D_SHADER_MACRO m[4];
        EnterCriticalSection(&s_spec_cs);
        while (s_spec_head == s_spec_tail)
            SleepConditionVariableCS(&s_spec_cv, &s_spec_cs, INFINITE);
        e = s_spec_q[s_spec_tail++ % PS_SPEC_QUEUE];
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
            "static const uint4 tex_info[4] = { uint4(%uu,%uu,0,0), uint4(%uu,%uu,0,0),"
            " uint4(%uu,%uu,0,0), uint4(%uu,%uu,0,0) };",
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
            k->tex_info[0][0], k->tex_info[0][1], k->tex_info[1][0], k->tex_info[1][1],
            k->tex_info[2][0], k->tex_info[2][1], k->tex_info[3][0], k->tex_info[3][1]);
        flat = k->variant & 1;
        m[0].Name = "SPEC"; m[0].Definition = "1";
        m[1].Name = "SPEC_DECL"; m[1].Definition = decl;
        m[2].Name = flat ? "FLATQ" : NULL; m[2].Definition = flat ? "nointerpolation" : NULL;
        m[3].Name = NULL; m[3].Definition = NULL;
        b = compile_def(k->variant & 2 ? "ps_wdepth" : "ps_main", "ps_5_0", m);
        if (b && SUCCEEDED(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                                          ID3D10Blob_GetBufferSize(b), NULL, &e->ps))) {
            LONG n;
            InterlockedExchange(&e->state, 2);
            n = InterlockedIncrement(&s_spec_ready);
            if (!(n & (n - 1)))
                fprintf(stderr, "[D3D11] %ld specialised pixel shaders\n", (long)n);
        } else {
            InterlockedExchange(&e->state, 3);
        }
        if (b) ID3D10Blob_Release(b);
    }
}

/* The specialised shader for this configuration, or NULL (not ready yet). */
static ID3D11PixelShader *spec_ps(const PSConsts *pc, uint32_t variant)
{
    static int enabled = -1;
    PsKey k;
    uint32_t h = 2166136261u, i;
    const uint8_t *p = (const uint8_t *)&k;
    PsSpec *e;
    if (enabled < 0) {
        const char *v = getenv("RECOMP_PS_SPEC");
        int threads = 2;
        enabled = !(v && *v == '0');
        if (enabled) s_spec = (PsSpec *)calloc(PS_SPEC_SLOTS, sizeof *s_spec);
        if (!s_spec) enabled = 0;
        if (enabled) {
            InitializeCriticalSection(&s_spec_cs);
            InitializeConditionVariable(&s_spec_cv);
            while (threads--) {
                HANDLE t = CreateThread(NULL, 0, spec_worker, NULL, 0, NULL);
                if (t) { SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); }
            }
        }
    }
    if (!enabled) return NULL;
    memset(&k, 0, sizeof k);
    memcpy(k.c_icw, pc->c_icw, sizeof k.c_icw);
    memcpy(k.a_icw, pc->a_icw, sizeof k.a_icw);
    memcpy(k.c_ocw, pc->c_ocw, sizeof k.c_ocw);
    memcpy(k.a_ocw, pc->a_ocw, sizeof k.a_ocw);
    k.fin0 = pc->fin0; k.fin1 = pc->fin1; k.rc_control = pc->rc_control;
    k.stage_prog = pc->stage_prog; k.alpha_func = pc->alpha_func; k.ps_flags = pc->ps_flags;
    k.clip_plane = pc->clip_plane; k.other_input = pc->other_input; k.dot_map = pc->dot_map;
    k.variant = variant;
    for (i = 0; i < 4; i++) { k.tex_info[i][0] = pc->tex_info[i][0]; k.tex_info[i][1] = pc->tex_info[i][1]; }
    for (i = 0; i < sizeof k; i++) h = (h ^ p[i]) * 16777619u;
    for (i = 0; i < PS_SPEC_SLOTS; i++) {
        e = &s_spec[(h + i) & (PS_SPEC_SLOTS - 1)];
        if (!e->state) break;
        if (!memcmp(&e->key, &k, sizeof k))
            return InterlockedCompareExchange(&e->state, 0, 0) == 2 ? e->ps : NULL;
    }
    if (i == PS_SPEC_SLOTS) return NULL;            /* table full: ubershader */
    EnterCriticalSection(&s_spec_cs);
    if (s_spec_head - s_spec_tail < PS_SPEC_QUEUE) {
        e->key = k;
        e->state = 1;
        s_spec_q[s_spec_head++ % PS_SPEC_QUEUE] = e;
        WakeConditionVariable(&s_spec_cv);
    }
    LeaveCriticalSection(&s_spec_cs);
    return NULL;
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

static void upload_cb(ID3D11Buffer *b, const void *data, size_t bytes)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)b, 0,
                                         D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, data, bytes);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)b, 0);
    }
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
        if (!(b = compile_def("y16_ps", "ps_5_0", ms))) return 0;
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                       ID3D10Blob_GetBufferSize(b), NULL, &s_y16_ps[1]);
        ID3D10Blob_Release(b);
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
    uint32_t synced_frame, used_frame;
    uint32_t seq;                   /* bind order; see surface_find */
} Surface;

typedef struct {
    uint32_t phys, iw, ih, samples;
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
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
static void blit_span(ID3D11ShaderResourceView *srv, ID3D11ShaderResourceView *mask,
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
    upload_cb(s_cb_present, &pc, sizeof pc);
    unbind_textures();
    unbind_targets();
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sc);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs_plain);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, s_blend_opaque, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, s_ds_off, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_present_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb_present);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_present_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb_present);
    {
        ID3D11ShaderResourceView *views[2] = { srv, mask };
        ID3D11SamplerState *smp[2] = { s_linear_clamp, s_point_clamp };
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 2, views);
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 2, smp);
    }
    ID3D11DeviceContext_Draw(s_ctx, 4, 0);
    unbind_targets();
    unbind_textures();
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
static void surface_rows_to_bgra(const Surface *s, uint32_t *out)
{
    const uint8_t *src = guest(s->va);
    int amode = surface_alpha_mode(s->color_fmt);
    uint32_t x, y;
    for (y = 0; y < s->h; y++) {
        const uint8_t *row = src + (size_t)y * s->pitch;
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
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_up_tex[k],
                                              NULL, &s_up_srv[k]);
        s_up_w[k] = w; s_up_h[k] = h;
    }
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

/* Guest memory is the authority for what the CPU writes. A shadow copy of
 * each surface's guest bytes says which pixels the CPU changed since the last
 * look; only those replace what the GPU drew. A loading screen drawn by the
 * GPU and then animated by CPU writes keeps both. */
static void surface_sync_impl(Surface *s, int force)
{
    uint32_t rowb = s->w * s->bpp, y, x, x0 = 0xFFFFFFFFu, x1 = 0, y0 = 0xFFFFFFFFu, y1 = 0;
    uint32_t *px, *mask;
    const uint8_t *g;
    if (!s->bpp || s->swizzled || !guest_ok(s->va, (size_t)s->pitch * s->h))
        return;
    if (!force && s->synced_frame == s_epoch)
        return;
    s->synced_frame = s_epoch;
    g = guest(s->va);
    if (!s->shadow) {
        s->shadow = (uint8_t *)malloc((size_t)rowb * s->h);
        if (!s->shadow) return;
        for (y = 0; y < s->h; y++)
            memcpy(s->shadow + (size_t)y * rowb, g + (size_t)y * s->pitch, rowb);
        px = scratch_pixels((size_t)s->w * s->h);
        if (!px) return;
        surface_rows_to_bgra(s, px);
        {
            ID3D11ShaderResourceView *srv = upload_image(px, s->w, s->h);
            float x0, x1;
            guest_span(s, &x0, &x1);
            if (srv) blit_span(srv, NULL, s->rtv, s->iw, s->ih, -1.0f, x0, x1, 0, 1);
            s->ms_dirty = 1; s->gen++;
        }
        return;
    }
    for (y = 0; y < s->h; y++) {
        const uint8_t *row = g + (size_t)y * s->pitch;
        uint8_t *sh = s->shadow + (size_t)y * rowb;
        if (!memcmp(row, sh, rowb)) continue;
        if (y0 == 0xFFFFFFFFu) {
            if (s_mask_cap < (size_t)s->w * s->h) {
                free(s_mask_pixels);
                s_mask_pixels = (uint32_t *)malloc((size_t)s->w * s->h * 4);
                s_mask_cap = s_mask_pixels ? (size_t)s->w * s->h : 0;
                if (!s_mask_pixels) return;
            }
            memset(s_mask_pixels, 0, (size_t)s->w * s->h * 4);
            y0 = y;
        }
        y1 = y;
        mask = s_mask_pixels + (size_t)y * s->w;
        for (x = 0; x < s->w; x++)
            if (memcmp(row + x * s->bpp, sh + x * s->bpp, s->bpp)) {
                mask[x] = 0xFFFFFFFFu;
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
            }
        memcpy(sh, row, rowb);
    }
    if (y0 == 0xFFFFFFFFu)
        return;
    if (getenv("RECOMP_D3D_TRACE"))
        fprintf(stderr, "[D3D11] CPU wrote surface %08X (frame %u) %u,%u-%u,%u\n",
                s->phys, s_frame, x0, y0, x1, y1);
    px = scratch_pixels((size_t)s->w * s->h);
    if (!px) return;
    surface_rows_to_bgra(s, px);
    {
        ID3D11ShaderResourceView *srv = upload_slot(0, px, s->w, s->h);
        ID3D11ShaderResourceView *msk = upload_slot(1, s_mask_pixels, s->w, s->h);
        float x0, x1;
        guest_span(s, &x0, &x1);
        if (srv && msk) blit_span(srv, msk, s->rtv, s->iw, s->ih, -1.0f, x0, x1, 0, 1);
        s->ms_dirty = 1; s->gen++;
    }
}

static void surface_sync(Surface *s, int force)
{
    int64_t t0 = s_prof_on > 0 ? qpc() : 0;
    RECOMP_PROFILE_BEGIN("D3D11 surface sync");
    surface_sync_impl(s, force);
    RECOMP_PROFILE_END();
    if (s_prof_on > 0) s_prof[PROF_SYNC] += qpc() - t0;
}

static void surface_release(Surface *s)
{
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

/* Titles reuse one block of memory as targets of different shapes (MM3's
 * glare buffer is 640x480 and 320x240 every frame). Each shape keeps its own
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
        for (i = 0; i < MAX_SURFACES && !victim; i++)
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
    if (d->dsv) ID3D11DepthStencilView_Release(d->dsv);
    if (d->tex) ID3D11Texture2D_Release(d->tex);
    memset(d, 0, sizeof *d);
    d->tex = make_texture_ms(iw, ih, 1, 1, DXGI_FORMAT_D24_UNORM_S8_UINT,
                             D3D11_BIND_DEPTH_STENCIL, 0, samples);
    if (!d->tex) return NULL;
    ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)d->tex, NULL, &d->dsv);
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
         * Reading bit 20 collapsed MM3's 0x07070228 water target to one row. */
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
    uint32_t phys, format, pitch, w, h, palette, used;
    uint64_t hash, pal_hash;
    uint32_t checked_frame;
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

static void decode_level(const NvD3DTexture *t, const TexLayout *L, const uint8_t *src,
                         int lv, const uint32_t *pal, uint32_t *out)
{
    uint32_t w = L->level_w[lv], h = L->level_h[lv], x, y;
    if (L->kind == K_DXT) {
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bx, by, px[16];
        for (by = 0; by < bh; by++)
            for (bx = 0; bx < bw; bx++) {
                d3d8_dxt_decode_block(src + ((size_t)by * bw + bx) * L->bpp, t->color, px);
                for (y = 0; y < 4 && by * 4 + y < h; y++)
                    for (x = 0; x < 4 && bx * 4 + x < w; x++)
                        out[(size_t)(by * 4 + y) * w + bx * 4 + x] = px[y * 4 + x];
            }
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
            tex_free(e);
        }
    }
}

/* One mip level below src: a 2x2 box (a dimension already 1 stays 1). */
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
 * memory (MM3: 1024x1024 with 4 levels), so distant surfaces kept sampling
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

static ID3D11ShaderResourceView *texture_get_impl(const NvD3DTexture *t)
{
    TexLayout L;
    uint32_t p = phys(t->addr), pal_p = t->palette;
    uint32_t key = (p >> 7) ^ (t->format * 2654435761u) ^ (t->pitch << 3) ^ t->width;
    TexEntry *e, **head = &s_tex_bucket[key & (TEX_BUCKETS - 1)];
    uint64_t h, ph = 0;
    uint32_t *pal = NULL;

    if (!tex_layout(t, &L) || !guest_ok(t->addr, L.total))
        return NULL;
    for (e = *head; e; e = e->next)
        if (e->phys == p && e->format == t->format && e->pitch == t->pitch &&
            e->w == t->width && e->h == t->height && e->palette == pal_p)
            break;
    if (e && e->checked_frame == s_frame) {
        e->used = s_frame;
        return e->srv;
    }
    h = hash_mem(guest(t->addr), L.total);
    if (t->color == 0x0B && pal_p && guest_ok(pal_p, t->palette_len * 4)) {
        pal = (uint32_t *)guest(pal_p);
        ph = hash_mem((const uint8_t *)pal, t->palette_len * 4);
    }
    if (e && e->hash == h && e->pal_hash == ph) {
        e->checked_frame = s_frame;
        e->used = s_frame;
        return e->srv;
    }
    if (!e) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        int hl = host_levels(&L, t);
        e = (TexEntry *)calloc(1, sizeof *e);
        if (!e) return NULL;
        e->tex = make_texture(t->width, t->height, (UINT)hl, (UINT)L.faces,
                              DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE,
                              L.faces == 6 ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0);
        if (!e->tex) { free(e); return NULL; }
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (L.faces == 6) {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            sd.TextureCube.MipLevels = (UINT)hl;
        } else {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = (UINT)hl;
        }
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)e->tex, &sd, &e->srv);
        e->phys = p; e->format = t->format; e->pitch = t->pitch;
        e->w = t->width; e->h = t->height; e->palette = pal_p;
        e->bytes = (size_t)t->width * t->height * 4 * L.faces * (L.levels > 1 ? 4 : 3) / 3;
        e->next = *head;
        *head = e;
        s_tex_bytes += e->bytes;
        s_tex_count++;
    }
    {
        int f, lv, hl = host_levels(&L, t);
        uint32_t *px = scratch_pixels((size_t)t->width * t->height), *a = NULL, *b = NULL;
        if (!px) return NULL;
        if (hl > L.levels) {
            size_t n = (size_t)L.level_w[L.levels - 1] * L.level_h[L.levels - 1];
            a = (uint32_t *)malloc(n * 4);
            b = (uint32_t *)malloc(n * 4);
            if (!a || !b) { free(a); free(b); a = b = NULL; }
        }
        for (f = 0; f < L.faces; f++) {
            uint32_t w, h;
            for (lv = 0; lv < L.levels; lv++) {
                decode_level(t, &L, guest(t->addr) + f * L.face_stride + L.level_off[lv],
                             lv, pal, px);
                ID3D11DeviceContext_UpdateSubresource(
                    s_ctx, (ID3D11Resource *)e->tex,
                    (UINT)lv + (UINT)f * (UINT)hl, NULL,
                    px, L.level_w[lv] * 4, 0);
            }
            if (hl <= L.levels || !a) continue;
            w = L.level_w[L.levels - 1]; h = L.level_h[L.levels - 1];
            memcpy(a, px, (size_t)w * h * 4);
            for (; lv < hl; lv++) {
                uint32_t *tmp;
                halve_bgra(a, w, h, b);
                if (w > 1) w /= 2;
                if (h > 1) h /= 2;
                ID3D11DeviceContext_UpdateSubresource(
                    s_ctx, (ID3D11Resource *)e->tex,
                    (UINT)lv + (UINT)f * (UINT)hl, NULL, b, w * 4, 0);
                tmp = a; a = b; b = tmp;
            }
        }
        free(a);
        free(b);
    }
    e->hash = h;
    e->pal_hash = ph;
    e->checked_frame = s_frame;
    e->used = s_frame;
    tex_evict();
    return e->srv;
}

static ID3D11ShaderResourceView *texture_get(const NvD3DTexture *t)
{
    int64_t t0 = s_prof_on > 0 ? qpc() : 0;
    ID3D11ShaderResourceView *v;
    RECOMP_PROFILE_BEGIN("D3D11 texture");
    v = texture_get_impl(t);
    RECOMP_PROFILE_END();
    if (s_prof_on > 0) s_prof[PROF_TEXTURE] += qpc() - t0;
    return v;
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
    return &tab[i & (STATE_SLOTS - 1)].obj;   /* full: reuse, leaks one object */
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

static ID3D11SamplerState *sampler(const NvD3DTexture *t, int levels, int reduce)
{
    uint32_t minf = (t->filter >> 16) & 0xFF, magf = (t->filter >> 24) & 0xF;
    int bias13 = (int)(t->filter & 0x1FFF);
    uint64_t key;
    void **slot;
    if (bias13 & 0x1000) bias13 -= 0x2000;
    key = (uint64_t)minf | ((uint64_t)magf << 8) | ((uint64_t)(t->address & 0xFFF) << 12) |
          ((uint64_t)(bias13 & 0x1FFF) << 24) | ((uint64_t)(levels & 15) << 37) |
          ((uint64_t)((t->control0 >> 6) & 0xFFFFFF) << 41) ^ ((uint64_t)t->border * 0x9E3779B1ull);
    key ^= (uint64_t)(reduce != 0) << 63;
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
    return (ID3D11SamplerState *)*slot;
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
    if (x1 > gw) x1 = gw;
    if (y1 > gh) y1 = gh;
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

/* A guest pass that reduces a render target (MM3's glare and luminance
 * downsamples, its reflection map) takes texel-exact taps: at the guest's
 * resolution four taps cover a 2x2 block, at a host resolution several times
 * higher they hit isolated texels, and which ones moves with the camera --
 * lit windows and lamps then flicker through the bloom. Reductions read this
 * mip-mapped copy, so each tap averages the footprint it stands for. */
/* A swizzled render target sampled as a mipmapped texture: titles render
 * each level of such a texture themselves, one surface per level at the
 * offsets the texture's layout puts them (MM3's water ripple map: 128, 64,
 * 32, 16 at 0x682000, 0x692000, 0x696000, 0x697000). Reading only the top
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
        ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
        ID3D11DeviceContext_VSSetShader(s_ctx, s_y16_vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(s_ctx, s_y16_ps[s->ms ? 1 : 0], NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &src);
        ID3D11DeviceContext_Draw(s_ctx, 3, 0);
        unbind_targets();
        unbind_textures();
        s->y16_gen = s->gen;
    }
    return s->y16_srv;
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
    int cull = 0, bias = 0;
    float slope = 0, bf[4];
    s_draw_textured = s_draw_reads_wide = 0;
    s_draw_tex_addr = 0;

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
        if (t->color != 0x35 || t->cube || !t->addr) continue;
        rt = surface_find(phys(t->addr));
        if (rt && rt->bpp == 4) { surface_sync(rt, 0); surface_y16(rt); }
    }
    if (!bind_targets(st, st->depth_test || st->stencil_test, &s, &d, &gw, &gh))
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
        int kind = K_SWZ, levels = 1, reduce = 0;
        pc.tex_scale[n][0] = pc.tex_scale[n][1] = 1.0f;
        pc.tex_scale[n][2] = pc.tex_scale[n][3] = 1.0f;
        if (!st->rc_seen && n == 0)
            mode = (st->tex_used & 1) ? 1 : 0;
        if (!mode_needs_texture(mode) || !t->addr || !t->width || !t->height)
            continue;
        tex_format(t->color, &kind);
        rt = !t->cube ? surface_find(phys(t->addr)) : NULL;
        s_draw_textured = 1;
        if (!s_draw_tex_addr) s_draw_tex_addr = t->addr;
        if (rt && rt->sx > rt->sy * 1.001f) s_draw_reads_wide = 1;
        if (rt) {
            surface_sync(rt, 0);
            surface_resolve(rt);
            /* An MSAA target draws into ms, so its resolved tex is free to read. */
            view = rt == s_cur_rt && !rt->ms ? feedback_copy(rt) : rt->srv;
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
            TexLayout L;
            view = texture_get(t);
            if (tex_layout(t, &L)) levels = L.levels;
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
        smp[n] = sampler(t, levels, reduce);
        pc.tex_info[n][0] = (t->control0 >> 2) & 1;
        pc.tex_info[n][1] = t->filter >> 28;
        memcpy(pc.bump_mat[n], t->bump_mat, sizeof pc.bump_mat[n]);
        pc.bump_lum[n][0] = t->bump_scale;
        pc.bump_lum[n][1] = t->bump_offset;
    }
    for (n = 0; n < 4; n++)
        if (!smp[n]) smp[n] = s_linear_clamp;

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
    pc.ps_flags = (st->rc_seen ? 1u : 0u) | (st->alpha_test ? 2u : 0u);    pc.clip_plane = st->clip_plane_mode;
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
    }
    upload_cb(s_cb_ps, &pc, sizeof pc);

    if (st->poly_offset_fill && !(st->z_perspective && d)) {
        /* The slope term multiplies the depth change per pixel. At an
         * internal resolution s times the guest's a pixel spans 1/s as much
         * depth, so the factor scales by s to offset decals (MM3's windows,
         * factor -0.25) as far as the NV2A does; unscaled they fight. */
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
        ID3D11DeviceContext_PSSetShader(s_ctx, ps ? ps : s_psv[v], NULL, 0);
    }
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 1, 1, &s_cb_ps);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 8, srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, smp);
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
        d ? ds_state(st->depth_test, st->depth_func, st->depth_mask,
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

/* The race HUD batches sprites from one texture atlas into a single draw
 * (MM3's speedometer needle shares one with the timer icon on the far
 * side), so edge placement is decided per cluster: connected primitives form
 * a piece, pieces on the same line less than HUD_GAP apart join a cluster
 * (the glyphs of a centred message stay together), and each cluster moves by
 * its own half of the 4:3 layout. pos holds the screen positions by vertex
 * (index - sub); fills dx with each vertex's x shift in guest pixels and
 * returns 0 when nothing moves, leaving only the shader's 4:3 squeeze. */
#define HUD_GAP 24.0f
static int hud_split(const float (*pos)[4], uint32_t nv, const uint32_t *idx, uint32_t sub,
                     uint32_t ni, int topology, float gw, float narrow, int follow, float *dx)
{
    static uint32_t *root, *piece, cap;
    static float (*box)[4];             /* per piece: x0 x1 y0 y1 */
    uint32_t i, k, np = 0, per = topology == NV_D3D_TRIANGLES ? 3 : topology == NV_D3D_LINES ? 2 : 1;
    float delta = (1.0f - narrow) * gw / (2.0f * narrow), half = gw * 0.5f;
    int moved = 0;
    if (nv > cap) {
        uint32_t *r = realloc(root, (size_t)nv * sizeof *r);
        uint32_t *pc = r ? realloc(piece, (size_t)nv * sizeof *pc) : NULL;
        float (*b)[4] = pc ? realloc(box, (size_t)nv * sizeof *b) : NULL;
        if (r) root = r;
        if (pc) piece = pc;
        if (!b) return 0;
        box = b;
        cap = nv;
    }
#define FIND(x) do { while (root[x] != x) x = root[x] = root[root[x]]; } while (0)
    /* pieces: vertices joined by primitives */
    for (i = 0; i < nv; i++) root[i] = i;
    for (i = 0; i + per <= ni; i += per)
        for (k = 1; k < per; k++) {
            uint32_t a = idx[i] - sub, b = idx[i + k] - sub;
            if (a >= nv || b >= nv) return 0;
            FIND(a); FIND(b);
            if (a != b) root[b] = a;
        }
    for (i = 0; i < nv; i++) {
        uint32_t r = i;
        FIND(r);
        if (r == i) {
            piece[i] = np;
            box[np][0] = box[np][2] = 1e30f;
            box[np][1] = box[np][3] = -1e30f;
            np++;
        }
    }
    for (i = 0; i < nv; i++) {
        uint32_t r = i, n;
        FIND(r);
        n = piece[r];
        piece[i] = n;
        if (pos[i][0] < box[n][0]) box[n][0] = pos[i][0];
        if (pos[i][0] > box[n][1]) box[n][1] = pos[i][0];
        if (pos[i][1] < box[n][2]) box[n][2] = pos[i][1];
        if (pos[i][1] > box[n][3]) box[n][3] = pos[i][1];
    }
    /* clusters: pieces on one line within HUD_GAP (root[] reused per piece);
     * ponytail: O(pieces^2), fine for HUD batches of a few hundred glyphs */
    for (i = 0; i < np; i++) root[i] = i;
    for (i = 0; i < np; i++)
        for (k = i + 1; k < np; k++) {
            uint32_t a = i, b = k;
            if (box[i][2] > box[k][3] || box[k][2] > box[i][3] ||
                box[i][0] > box[k][1] + HUD_GAP || box[k][0] > box[i][1] + HUD_GAP)
                continue;
            FIND(a); FIND(b);
            if (a == b) continue;
            root[b] = a;
            if (box[b][0] < box[a][0]) box[a][0] = box[b][0];
            if (box[b][1] > box[a][1]) box[a][1] = box[b][1];
            if (box[b][2] < box[a][2]) box[a][2] = box[b][2];
            if (box[b][3] > box[a][3]) box[a][3] = box[b][3];
        }
    for (i = 0; i < nv; i++) {
        uint32_t c = piece[i];
        FIND(c);
        dx[i] = 0.0f;
        if (follow) {
            /* keep the cluster's centre where the title projected it on
             * the full width; the shader squeezes it to 4:3 about there */
            dx[i] = ((box[c][0] + box[c][1]) * 0.5f - half) * (1.0f / narrow - 1.0f);
            moved = 1;
        } else if (box[c][1] <= half) { dx[i] = -delta; moved = 1; }
        else if (box[c][0] >= half) { dx[i] = delta; moved = 1; }
    }
#undef FIND
    return moved;
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

void nv2a_d3d_draw_ub(const NvD3DState *st, int topology, const NvD3DAttrib attr[16],
                      const float def[16][4], int program, const uint32_t *idx, uint32_t ni,
                      uint32_t lo, uint32_t hi, const float (*pos)[4])
{
    static uint32_t s_prog_version, s_vconst_version;
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
    if (!setup_pipeline(st, topology, &s, &gw, &gh, &zmax)) {
        RECOMP_PROFILE_END();
        return;
    }
    if (pos && s && s->sx > s->sy * 1.001f) {
        /* Widescreen placement of overlays drawn straight in screen space
         * (w = 1). Nothing is stretched:
         *  - draws with no image to distort (fades, fills) and copies of a
         *    widened render target span the wide surface 1:1;
         *  - full-screen image backgrounds are zoomed uniformly to cover it,
         *    cropping a little top and bottom;
         *  - the race HUD keeps its 4:3 shape and moves to the screen edge
         *    its half of the 4:3 layout belongs to;
         *  - everything else keeps its 4:3 proportions, centred. */
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
            float narrow = s->sy / s->sx;
            int wide_x = xlo <= 0.5f && xhi >= (float)gw - 0.5f;
            int tall = ylo <= 0.5f && yhi >= (float)gh - 0.5f;
            if (count > dx_cap) {
                float *d = realloc(dx, (size_t)count * sizeof *d);
                if (d) { dx = d; dx_cap = count; }
            }
            if (wide_x && (!s_draw_textured || s_draw_reads_wide)) {
                /* spans as it is */
            } else if (tall && (!s_frame_bg_tex || s_draw_tex_addr == s_frame_bg_tex) &&
                       (wide_x || s_draw_tex_addr == s_frame_bg_tex)) {
                /* The frame's background image (and anything else drawn
                 * with it, like the menus' animated overlay mesh). A
                 * later full-screen image is content -- the title's logo
                 * picture -- and stays 4:3 over this background. */
                s_frame_bg_tex = s_draw_tex_addr;
                yscale = 1.0f / narrow;
            } else {
                squeeze = narrow;
                if (nv2a_d3d_hud_active() && world_tag_draw(xlo, xhi, ylo, yhi, s_draw_tex_addr)) {
                    /* a name tag over a car: the title projected it
                     * through the widened camera, keep that position */
                    use_dx = count <= dx_cap &&
                             hud_split(pos, count, idx, lo, ni, topology, (float)gw, narrow, 1, dx);
                } else if (nv2a_d3d_hud_active()) {
                    if (xhi <= (float)gw * 0.5f) shift = narrow - 1.0f;
                    else if (xlo >= (float)gw * 0.5f) shift = 1.0f - narrow;
                    else use_dx = count <= dx_cap &&
                                  hud_split(pos, count, idx, lo, ni, topology, (float)gw, narrow, 0, dx);
                }
            }
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
        if (pver != s_prog_version) {
            upload_cb(s_cb_prog, prog, NV_VSH_SLOTS * 16);
            s_prog_version = pver;
        }
        if (cver != s_vconst_version) {
            upload_cb(s_cb_vconst, consts, 192 * 16);
            s_vconst_version = cver;
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
    upload_cb(s_cb_ub, &uc, sizeof uc);
    {
        ID3D11Buffer *vcb[4] = { s_cb_ub, NULL, s_cb_prog, s_cb_vconst };
        ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
        ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ib, DXGI_FORMAT_R32_UINT, s_ib_pos);
        ID3D11DeviceContext_VSSetShader(s_ctx, s_ub_vs, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 4, vcb);
        ID3D11DeviceContext_VSSetShaderResources(s_ctx, 0, 1, &s_raw_srv);
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

    if (!nv2a_d3d_init())
        return;
    if (!bind_targets(st, zclear || sclear, &s, &d, &gw, &gh))
        return;
    if (s) {
        if (flags & 0x10) mask |= D3D11_COLOR_WRITE_ENABLE_RED;
        if (flags & 0x20) mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
        if (flags & 0x40) mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
        if ((flags & 0x80) && surface_alpha_mode(s->color_fmt) < 0)
            mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    }
    if (!d) { zclear = sclear = 0; }
    if (!mask && !zclear && !sclear)
        return;
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
    if (x0 > x1 || y0 > y1)
        return;
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
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_clear_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb_clear);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_clear_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb_clear);
    ID3D11DeviceContext_Draw(s_ctx, 4, 0);
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
    ID3D11Texture2D *stage, *down;
    ID3D11RenderTargetView *down_rtv;
} WbSlot;
static WbSlot s_wb[MAX_SURFACES];

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
    if (need_down && !k->down) {
        k->down = make_texture(w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM,
                               D3D11_BIND_RENDER_TARGET, 0);
        if (!k->down) return 0;
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)k->down, NULL,
                                            &k->down_rtv);
    }
    return 1;
}

static void writeback_surfaces(void)
{
    static int enabled = -1;
    int i, any = 0;
    if (enabled < 0)
        enabled = !getenv("RECOMP_D3D_NO_WRITEBACK");
    if (!enabled)
        return;
    RECOMP_PROFILE_BEGIN("D3D11 write-back");
    for (i = 0; i < MAX_SURFACES; i++) {
        Surface *s = &s_surf[i];
        WbSlot *k = &s_wb[i];
        int scaled;
        if (!s->tex || !s->gpu_dirty || s->swizzled || (s->bpp != 2 && s->bpp != 4) ||
            surface_find(s->phys) != s ||
            !s->shadow || !guest_ok(s->va, (size_t)s->pitch * s->h))
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
        any = 1;
    }
    for (i = 0; any && i < MAX_SURFACES; i++) {
        Surface *s = &s_surf[i];
        WbSlot *k = &s_wb[i];
        D3D11_MAPPED_SUBRESOURCE m;
        uint8_t *dst;
        uint32_t x, y, rowb;
        int amode, r565;
        if (!s->tex || !s->gpu_dirty || s->swizzled || (s->bpp != 2 && s->bpp != 4) ||
            surface_find(s->phys) != s ||
            !s->shadow || !k->stage || k->w != s->w || k->h != s->h)
            continue;
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)k->stage, 0,
                                           D3D11_MAP_READ, 0, &m)))
            continue;
        dst = (uint8_t *)guest(s->va);
        rowb = s->w * s->bpp;
        amode = surface_alpha_mode(s->color_fmt);
        r565 = (s->color_fmt & 0xF) == 0x3;
        for (y = 0; y < s->h; y++) {
            const uint32_t *src = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
            uint8_t *row = dst + (size_t)y * s->pitch;
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
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)k->stage, 0);
        s->gpu_dirty = 0;
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
    if (!s_query_active) return;
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)s_query);
    while (ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)s_query, &n,
                                       sizeof n, 0) == S_FALSE)
        SwitchToThread();
    s_zpass += n;
    s_query_active = 0;
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
static void capture_backbuffer(ID3D11Texture2D *bb)
{
    static const char *prefix = (const char *)-1;
    static unsigned remaining, frame;
    char path[1024];
    FILE *f;
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
    {
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D *st = NULL;
        D3D11_MAPPED_SUBRESOURCE m;
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
            snprintf(path, sizeof path, "%s-%03u.bmp", prefix, frame++);
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
    remaining--;
}

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
    pc->dst_rect[0] = x0 / w * 2 - 1;
    pc->dst_rect[1] = 1 - y0 / h * 2;
    pc->dst_rect[2] = (x0 + target_w) / w * 2 - 1;
    pc->dst_rect[3] = 1 - (y0 + target_h) / h * 2;
    pc->src_uv[0] = pc->src_uv[1] = 0;
    pc->src_uv[2] = 1; pc->src_uv[3] = 1;
    pc->src_size[0] = (float)s->w; pc->src_size[1] = (float)s->h;
    pc->src_size[2] = (float)s->iw; pc->src_size[3] = (float)s->ih;
    pc->force_alpha[0] = 1.0f;
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
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_present_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb_present);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_present_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb_present);
    surface_resolve(s);
    {
        ID3D11ShaderResourceView *views[2] = { s->srv, pc->ovl_flags[0] ? s_ovl_srv : NULL };
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 2, views);
    }
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_linear_clamp);
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

static void present_surface(Surface *s)
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
            ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)bb, NULL, &rtv);
            composite(rtv, s_swap_w, s_swap_h, s, &pc, 1);
            if (!getenv("RECOMP_CAPTURE_SURFACE"))
                capture_backbuffer(bb);
            if (vsync < 0) vsync = getenv("RECOMP_VSYNC") ? atoi(getenv("RECOMP_VSYNC")) : 0;
            IDXGISwapChain1_Present(s_swap, (UINT)vsync, 0);
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
    int64_t now, period;
    if (limit < 0) {
        const char *e = getenv("RECOMP_FPS_LIMIT");
        limit = e ? atoi(e) : 60;
        QueryPerformanceFrequency(&freq);
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
    }
    if (limit <= 0)
        return;
    period = freq.QuadPart / limit;
    now = qpc();
    /* A frame that ran late is shown at once and starts a new cadence;
     * waiting for the next boundary would turn every slow frame into two. */
    next += period;
    if (!next || next <= now) {
        next = now;
        return;
    }
    {
        int64_t t0 = now;
        int64_t remain = next - now;
        int64_t spin = freq.QuadPart / 1000;            /* last millisecond */
        RECOMP_PROFILE_BEGIN("D3D11 pace");
        if (remain > spin && timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((remain - spin) * 10000000 / freq.QuadPart);
            if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
                WaitForSingleObject(timer, INFINITE);
        }
        while (qpc() < next)
            YieldProcessor();
        RECOMP_PROFILE_END();
        if (s_prof_on > 0) s_prof[PROF_PACE] += qpc() - t0;
    }
}

/* Internal resolution and aspect follow the window: RECOMP_RENDER_SCALE=N
 * and RECOMP_ASPECT pin them, else the window's client area -- the guest's
 * 480 lines become as many as the window has, at its aspect (F11 fullscreen:
 * the monitor's native resolution and shape). A resize applies once it has
 * settled for RESIZE_SETTLE_MS, by dropping every host surface; they are
 * rebuilt at the new size from guest memory, which the flip just updated.
 * The race camera's projection is built once per race, so an aspect change
 * waits until no race camera is active rather than stretch the race view. */
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
               InterlockedCompareExchange(&s_race_hud, 0, 0);
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
        for (i = 0; i < MAX_DEPTHS; i++) {
            if (s_depth[i].dsv) ID3D11DepthStencilView_Release(s_depth[i].dsv);
            if (s_depth[i].tex) ID3D11Texture2D_Release(s_depth[i].tex);
            memset(&s_depth[i], 0, sizeof s_depth[i]);
        }
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
    if (!nv2a_d3d_init())
        return;
    s_frame_bg_tex = 0;
    s_frame_tag_window = 1;
    s = surface_find(phys(surface_addr));
    if (!s && pitch)
        s = scan_surface(surface_addr, pitch);
    s_shown = s;
    s_last_flip = GetTickCount();
    {
        int64_t t0 = s_prof_on > 0 ? qpc() : 0, t1;
        writeback_surfaces();
        t1 = s_prof_on > 0 ? qpc() : 0;
        present_surface(s);
        if (s_prof_on > 0) {
            s_prof[PROF_WRITEBACK] += t1 - t0;
            s_prof[PROF_PRESENT] += qpc() - t1;
        }
    }
    pace_vblank();
    RECOMP_PROFILE_FRAME();
    prof_report();
    display_config(0);
    rdoc_poll();
    s_frame++;
}

void nv2a_d3d_tick(void)
{
    DWORD now;
    uint32_t va, pitch;
    if (s_init_state <= 0)
        return;
    now = GetTickCount();
    if (now - s_last_present < 16)
        return;
    if (xbox_FramebufferScanSource(&va, &pitch)) {
        Surface *s = scan_surface(va, pitch);
        present_surface(s);
        s_frame++;
        return;
    }
    /* Movies keep running while the title stops flipping. */
    if (s_shown && now - s_last_flip > 50) {
        XboxOverlay ovl;
        if (xbox_FramebufferOverlay(&ovl))
            present_surface(s_shown);
        else
            s_last_present = now;
    }
}

#else
#include "nv2a_d3d11.h"
int  nv2a_d3d_init(void) { return 0; }
int  nv2a_d3d_active(void) { return 0; }
void nv2a_d3d_clear(const NvD3DState *st, uint32_t f, uint32_t c, uint32_t z,
                    uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{ (void)st; (void)f; (void)c; (void)z; (void)x0; (void)y0; (void)x1; (void)y1; }
void nv2a_d3d_flip(uint32_t a, uint32_t p) { (void)a; (void)p; }
void nv2a_d3d_tick(void) {}
void nv2a_d3d_kick(void) {}
void nv2a_d3d_draw_ub(const NvD3DState *st, int t, const NvD3DAttrib a[16], const float d[16][4],
                      int pr, const uint32_t *i, uint32_t n, uint32_t lo, uint32_t hi,
                      const float (*pos)[4])
{ (void)st; (void)t; (void)a; (void)d; (void)pr; (void)i; (void)n; (void)lo; (void)hi; (void)pos; }
int nv2a_d3d_widescreen(void) { return 0; }
void nv2a_d3d_zpass_enable(int on) { (void)on; }
void nv2a_d3d_zpass_clear(void) {}
uint32_t nv2a_d3d_zpass_read(void) { return 0; }
float nv2a_d3d_display_aspect(void) { return 4.0f / 3.0f; }
void nv2a_d3d_note_race_camera(void) {}
void nv2a_d3d_set_hud_start(float x0, float y0, float x1, float y1) { (void)x0; (void)y0; (void)x1; (void)y1; }
void nv2a_d3d_note_frontend_camera(void) {}
#endif
