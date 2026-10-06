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
#include "nv2a_vsh_hlsl.h"
#include "fb_present.h"
#include "platform/recomp_profile.h"
#include "../d3d/d3d8_swizzle.h"

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
#define GUEST_W 640u
#define GUEST_H 480u

/* Race HUD edge placement is latched: the title builds its race camera's
 * projection once as a race starts (src/mm3_widescreen.c reports it), and
 * car select's camera ends it. Frames that draw a full-screen background
 * image are menus or loading screens and never get it, which keeps menus
 * reached from a race (and the race's own loading screen) in 4:3 layout. */
static volatile LONG s_race_hud;
static uint32_t s_frame_bg_tex;     /* this frame's background image; reset at flip */

void nv2a_d3d_note_race_camera(void) { InterlockedExchange(&s_race_hud, 1); }
void nv2a_d3d_note_frontend_camera(void) { InterlockedExchange(&s_race_hud, 0); }

static int nv2a_d3d_hud_active(void)
{
    return !s_frame_bg_tex && InterlockedCompareExchange(&s_race_hud, 0, 0) != 0;
}

/* The aspect the image is presented at: RECOMP_ASPECT (e.g. 16:9, 21:9, 1.6,
 * 4:3), else the primary monitor's, never narrower than the title's 4:3.
 * The title's projection is widened to match (src/mm3_widescreen.c). */
float nv2a_d3d_display_aspect(void)
{
    static float aspect;
    if (!aspect) {
        const char *e = getenv("RECOMP_ASPECT");
        float a = 0, w, h;
        if (e && sscanf(e, "%f:%f", &w, &h) == 2 && h > 0) a = w / h;
        else if (e && sscanf(e, "%f", &w) == 1) a = w;
        if (a <= 0) {
            DEVMODEA dm;
            memset(&dm, 0, sizeof dm);
            dm.dmSize = sizeof dm;
            if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsHeight)
                a = (float)dm.dmPelsWidth / (float)dm.dmPelsHeight;
        }
        if (a < 4.0f / 3.0f) a = 4.0f / 3.0f;
        if (a > 4.0f) a = 4.0f;
        aspect = a;
    }
    return aspect;
}

/* How much wider than 4:3 a display-sized surface is rendered. */
static float widen(void)
{
    return nv2a_d3d_display_aspect() * 3.0f / 4.0f;
}

static ID3D11VertexShader *s_vs, *s_present_vs, *s_clear_vs;
static ID3D11PixelShader  *s_ps, *s_present_ps, *s_clear_ps;
static ID3D11InputLayout  *s_layout;
static ID3D11Buffer *s_vb, *s_ib, *s_cb_vs, *s_cb_ps, *s_cb_present, *s_cb_clear;
static UINT s_vb_pos, s_ib_pos;
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
"struct VSIn { float4 pos:POSITION; float4 d0:COLOR0; float4 d1:COLOR1; float4 fog:FOG;\n"
"  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3; };\n"
"struct PSIn { float4 pos:SV_Position; float4 d0:COLOR0; float4 d1:COLOR1; float4 fog:FOG;\n"
"  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3; };\n"
"cbuffer VSC : register(b0) { float4 vs_map; uint4 vs_fogi; float4 vs_fogf; float4 vs_place; };\n"
"PSIn vs_main(VSIn i) {\n"
"  PSIn o;\n"
"  float w = i.pos.w;\n"
/* NV2A clamps raster w away from zero and infinity (xemu clampAwayZeroInf),
 * keeping its sign so geometry behind the eye still clips. */
"  float aw = clamp(abs(w), 5.421011e-20, 1.8446744e19);\n"
"  w = w < 0 ? -aw : aw;\n"
/* oPos is screen space after the D3D epilogue's divide; multiplying back by
 * w gives the clip-space position, so the host clipper and perspective-
 * correct interpolation see what the NV2A saw. */
"  o.pos = float4(((i.pos.x * vs_map.x - 1.0) * vs_map.w + vs_place.x) * w,\n"
"                 (1.0 - i.pos.y * vs_map.y) * vs_place.y * w,\n"
"                 i.pos.z * vs_map.z * w, w);\n"
"  o.d0 = saturate(i.d0); o.d1 = saturate(i.d1); o.fog = i.fog;\n"
"  o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3;\n"
"  return o;\n"
"}\n"
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
"};\n"
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
"float3 dotmap(float3 c, uint m) {\n"
"  if (m == 0) return c;\n"
"  if (m == 1) return float3(signed_c(c.r), signed_c(c.g), signed_c(c.b));\n"
"  return c * 2.0 - 1.0;\n"
"}\n"
"float4 samp2(uint n, float2 uv) {\n"
"  if (n == 0) return T2D0.Sample(S0, uv); if (n == 1) return T2D1.Sample(S1, uv);\n"
"  if (n == 2) return T2D2.Sample(S2, uv); return T2D3.Sample(S3, uv);\n"
"}\n"
"float4 sampc(uint n, float3 d) {\n"
"  if (n == 0) return TC0.Sample(S0, d); if (n == 1) return TC1.Sample(S1, d);\n"
"  if (n == 2) return TC2.Sample(S2, d); return TC3.Sample(S3, d);\n"
"}\n"
"float4 ps_main(PSIn i) : SV_Target {\n"
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
"    [loop] for (uint s = 0; s < nst; s++) {\n"
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
"  if (ps_flags & 2) {\n"
"    int v = (int)(o.a * 255.0 + 0.5), r = (int)(alpha_ref & 0xFF); bool ok = true;\n"
"    switch (alpha_func) {\n"
"    case 0x200: ok = false; break; case 0x201: ok = v < r; break;\n"
"    case 0x202: ok = v == r; break; case 0x203: ok = v <= r; break;\n"
"    case 0x204: ok = v > r; break; case 0x205: ok = v != r; break;\n"
"    case 0x206: ok = v >= r; break; }\n"
"    if (!ok) discard;\n"
"  }\n"
"  return o;\n"
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
"float4 clear_ps() : SV_Target { return ccol; }\n";

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
} PSConsts;

typedef struct {
    float dst_rect[4], src_uv[4], src_size[4];
    float ovl_out[4], ovl_walk[4], ovl_in[4];
    uint32_t ovl_flags[4];
    float force_alpha[4];
} PresentConsts;

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "nv2a_d3d11", NULL, NULL,
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

    if (!(b = compile("vs_main", "vs_5_0"))) return 0;
    {
        static const D3D11_INPUT_ELEMENT_DESC el[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,   D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "FOG",      0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 80,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 96,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 112, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                        ID3D10Blob_GetBufferSize(b), NULL, &s_vs);
        ID3D11Device_CreateInputLayout(s_dev, el, 8, ID3D10Blob_GetBufferPointer(b),
                                       ID3D10Blob_GetBufferSize(b), &s_layout);
        ID3D10Blob_Release(b);
    }
#define MAKE(entry, target, kind, out) \
    if (!(b = compile(entry, target))) return 0; \
    ID3D11Device_Create##kind(s_dev, ID3D10Blob_GetBufferPointer(b), \
                              ID3D10Blob_GetBufferSize(b), NULL, out); \
    ID3D10Blob_Release(b);
    MAKE("ps_main", "ps_5_0", PixelShader, &s_ps)
    MAKE("present_vs", "vs_5_0", VertexShader, &s_present_vs)
    MAKE("present_ps", "ps_5_0", PixelShader, &s_present_ps)
    MAKE("clear_vs", "vs_5_0", VertexShader, &s_clear_vs)
    MAKE("clear_ps", "ps_5_0", PixelShader, &s_clear_ps)
#undef MAKE

    s_vb = make_buffer(VB_BYTES, D3D11_BIND_VERTEX_BUFFER);
    s_ib = make_buffer(IB_BYTES, D3D11_BIND_INDEX_BUFFER);
    s_cb_vs = make_buffer(64, D3D11_BIND_CONSTANT_BUFFER);
    s_cb_ps = make_buffer(sizeof(PSConsts), D3D11_BIND_CONSTANT_BUFFER);
    s_cb_present = make_buffer(sizeof(PresentConsts), D3D11_BIND_CONSTANT_BUFFER);
    s_cb_clear = make_buffer(32, D3D11_BIND_CONSTANT_BUFFER);
    if (!s_vb || !s_ib || !s_cb_vs || !s_cb_ps || !s_cb_present || !s_cb_clear) {
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
    /* RECOMP_RENDER_SCALE=N, else the monitor's native height: the guest's
     * 480 lines become as many lines as the display has. */
    if ((e = getenv("RECOMP_RENDER_SCALE")) != NULL && atof(e) >= 0.5 && atof(e) <= 8.0) {
        s_scale = (float)atof(e);
    } else {
        DEVMODEA dm;
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        /* The largest image of the display aspect the monitor can show. */
        if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsHeight >= GUEST_H) {
            float fit_h = (float)dm.dmPelsHeight;
            if ((float)dm.dmPelsWidth / nv2a_d3d_display_aspect() < fit_h)
                fit_h = (float)dm.dmPelsWidth / nv2a_d3d_display_aspect();
            s_scale = fit_h / (float)GUEST_H;
            if (s_scale < 1.0f) s_scale = 1.0f;
        }
        if (s_scale > 8.0f) s_scale = 8.0f;
    }
    s_init_state = 1;
    fprintf(stderr, "[D3D11] renderer ready: %ux%u internal, display aspect %.3f\n",
            (uint32_t)(GUEST_W * s_scale * widen() + 0.5f), (uint32_t)(GUEST_H * s_scale + 0.5f),
            nv2a_d3d_display_aspect());
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
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    uint8_t *shadow;                /* guest bytes as of the last sync */
    int gpu_dirty;                  /* drawn since guest memory was updated */
    uint32_t synced_frame, used_frame;
} Surface;

typedef struct {
    uint32_t phys, iw, ih;
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

static ID3D11Texture2D *make_texture(UINT w, UINT h, UINT levels, UINT array,
                                     DXGI_FORMAT f, UINT bind, UINT misc)
{
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D *t = NULL;
    memset(&d, 0, sizeof d);
    d.Width = w ? w : 1;
    d.Height = h ? h : 1;
    d.MipLevels = levels;
    d.ArraySize = array;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    d.MiscFlags = misc;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &t)))
        return NULL;
    return t;
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
    }
}

static void surface_sync(Surface *s, int force)
{
    int64_t t0 = s_prof_on > 0 ? qpc() : 0;
    surface_sync_impl(s, force);
    if (s_prof_on > 0) s_prof[PROF_SYNC] += qpc() - t0;
}

static void surface_release(Surface *s)
{
    if (s_cur_rt == s) unbind_targets();
    if (s->srv) ID3D11ShaderResourceView_Release(s->srv);
    if (s->rtv) ID3D11RenderTargetView_Release(s->rtv);
    if (s->tex) ID3D11Texture2D_Release(s->tex);
    free(s->shadow);
    memset(s, 0, sizeof *s);
}

static Surface *surface_find(uint32_t p)
{
    int i;
    for (i = 0; i < MAX_SURFACES; i++)
        if (s_surf[i].tex && s_surf[i].phys == p)
            return &s_surf[i];
    return NULL;
}

static Surface *surface_get(uint32_t va, uint32_t pitch, uint32_t fmt,
                            uint32_t w, uint32_t h, int swizzled)
{
    uint32_t p = phys(va), bpp = surface_color_bpp(fmt);
    Surface *s = surface_find(p), *victim = NULL;
    int i;
    if (s && s->w >= w && s->h >= h && s->pitch == pitch && s->bpp == bpp &&
        s->swizzled == swizzled) {
        s->color_fmt = fmt;
        s->used_frame = s_frame;
        return s;
    }
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
    ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s->tex, NULL, &s->rtv);
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s->tex, NULL, &s->srv);
    s->used_frame = s_frame;
    {
        float zero[4] = { 0, 0, 0, 0 };
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s->rtv, zero);
    }
    surface_sync(s, 1);
    if (getenv("RECOMP_D3D_TRACE"))
        fprintf(stderr, "[D3D11] surface %08X %ux%u pitch %u fmt %X%s\n", p, w, h,
                pitch, fmt & 0xF, swizzled ? " swizzled" : "");
    return s;
}

static Depth *depth_get(uint32_t va, uint32_t iw, uint32_t ih)
{
    uint32_t p = phys(va);
    Depth *d, *victim = NULL;
    int i;
    for (i = 0; i < MAX_DEPTHS; i++) {
        d = &s_depth[i];
        if (d->tex && d->phys == p && d->iw == iw && d->ih == ih) {
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
    d->tex = make_texture(iw, ih, 1, 1, DXGI_FORMAT_D24_UNORM_S8_UINT,
                          D3D11_BIND_DEPTH_STENCIL, 0);
    if (!d->tex) return NULL;
    ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)d->tex, NULL, &d->dsv);
    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, d->dsv,
                                              D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                              1.0f, 0);
    d->phys = p; d->iw = iw; d->ih = ih; d->used_frame = s_frame;
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
        h = 1u << ((fmt >> 20) & 0xF);
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
        d = depth_get(st->zeta_addr, iw, ih);
    }
    if (!s && !d)
        return 0;
    if (s && d && (d->iw != s->iw || d->ih != s->ih))
        d = NULL;
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
        e = (TexEntry *)calloc(1, sizeof *e);
        if (!e) return NULL;
        e->tex = make_texture(t->width, t->height, (UINT)L.levels, (UINT)L.faces,
                              DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE,
                              L.faces == 6 ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0);
        if (!e->tex) { free(e); return NULL; }
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (L.faces == 6) {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            sd.TextureCube.MipLevels = (UINT)L.levels;
        } else {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = (UINT)L.levels;
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
        int f, lv;
        uint32_t *px = scratch_pixels((size_t)t->width * t->height);
        if (!px) return NULL;
        for (f = 0; f < L.faces; f++)
            for (lv = 0; lv < L.levels; lv++) {
                decode_level(t, &L, guest(t->addr) + f * L.face_stride + L.level_off[lv],
                             lv, pal, px);
                ID3D11DeviceContext_UpdateSubresource(
                    s_ctx, (ID3D11Resource *)e->tex,
                    (UINT)lv + (UINT)f * (UINT)L.levels, NULL,
                    px, L.level_w[lv] * 4, 0);
            }
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
    ID3D11ShaderResourceView *v = texture_get_impl(t);
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

static ID3D11SamplerState *sampler(const NvD3DTexture *t, int levels)
{
    uint32_t minf = (t->filter >> 16) & 0xFF, magf = (t->filter >> 24) & 0xF;
    int bias13 = (int)(t->filter & 0x1FFF);
    uint64_t key;
    void **slot;
    if (bias13 & 0x1000) bias13 -= 0x2000;
    key = (uint64_t)minf | ((uint64_t)magf << 8) | ((uint64_t)(t->address & 0xFFF) << 12) |
          ((uint64_t)(bias13 & 0x1FFF) << 24) | ((uint64_t)(levels & 15) << 37) |
          ((uint64_t)((t->control0 >> 6) & 0xFFFFFF) << 41) ^ ((uint64_t)t->border * 0x9E3779B1ull);
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
        d.MipLODBias = (float)bias13 / 256.0f;
        d.MaxAnisotropy = 1;
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
        int kind = K_SWZ, levels = 1;
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
            view = rt == s_cur_rt ? feedback_copy(rt) : rt->srv;
            if (kind == K_LIN) {
                pc.tex_scale[n][0] = 1.0f / (float)rt->w;
                pc.tex_scale[n][1] = 1.0f / (float)rt->h;
            } else {
                pc.tex_scale[n][0] = (float)t->width / (float)rt->w;
                pc.tex_scale[n][1] = (float)t->height / (float)rt->h;
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
        smp[n] = sampler(t, levels);
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
    pc.ps_flags = (st->rc_seen ? 1u : 0u) | (st->alpha_test ? 2u : 0u);
    pc.clip_plane = st->clip_plane_mode;
    pc.other_input = st->other_stage_input;
    pc.dot_map = st->dot_rgb_mapping;
    /* SET_FOG_COLOR keeps red in the low byte. */
    pc.fog_color[0] = (st->fog_color & 0xFF) / 255.0f;
    pc.fog_color[1] = ((st->fog_color >> 8) & 0xFF) / 255.0f;
    pc.fog_color[2] = ((st->fog_color >> 16) & 0xFF) / 255.0f;
    pc.fog_color[3] = (st->fog_color >> 24) / 255.0f;
    upload_cb(s_cb_ps, &pc, sizeof pc);

    if (st->poly_offset_fill) {
        bias = (int)st->poly_offset_units;
        slope = st->poly_offset_factor;
    }
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx,
        topology == NV_D3D_LINES ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST :
        topology == NV_D3D_POINTS ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST :
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_ps, NULL, 0);
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

/* squeeze scales x about the centre, shift moves it (NDC), yscale scales y
 * about the centre; only the pass-through (pre-transformed) shader reads
 * shift and yscale. */
static void vs_constants(const NvD3DState *st, uint32_t gw, uint32_t gh, uint32_t zmax,
                         float squeeze, float shift, float yscale)
{
    struct { float map[4]; uint32_t fogi[4]; float fogf[4]; float place[4]; } c;
    memset(&c, 0, sizeof c);
    c.map[0] = 2.0f / (float)gw;
    c.map[1] = 2.0f / (float)gh;
    c.map[2] = 1.0f / (float)zmax;
    c.map[3] = squeeze;
    c.fogi[0] = st->fog_enable;
    c.fogi[1] = st->fog_mode;
    c.fogf[0] = st->fog_param[0];
    c.fogf[1] = st->fog_param[1];
    c.place[0] = shift;
    c.place[1] = yscale;
    upload_cb(s_cb_vs, &c, sizeof c);
}

void nv2a_d3d_draw(const NvD3DState *st, int topology,
                   const NvD3DVertex *v, uint32_t nv,
                   const uint32_t *idx, uint32_t ni)
{
    Surface *s;
    uint32_t gw, gh, zmax;
    UINT vb_bytes = nv * sizeof(NvD3DVertex), ib_bytes = ni * 4;
    D3D11_MAPPED_SUBRESOURCE m;
    uint8_t *p;
    int64_t t_draw;

    if (!nv2a_d3d_init() || !nv || !ni || vb_bytes > VB_BYTES || ib_bytes > IB_BYTES)
        return;
    RECOMP_PROFILE_BEGIN("D3D11 draw");
    t_draw = s_prof_on > 0 ? qpc() : 0;
    s_prof_draws++;
    if (!setup_pipeline(st, topology, &s, &gw, &gh, &zmax)) {
        RECOMP_PROFILE_END();
        return;
    }
    {
        /* Widescreen placement of overlays drawn straight in screen space
         * (w = 1). Nothing is stretched:
         *  - draws with no image to distort (fades, fills) and copies of a
         *    widened render target span the wide surface 1:1;
         *  - full-screen image backgrounds are zoomed uniformly to cover it,
         *    cropping a little top and bottom;
         *  - the race HUD keeps its 4:3 shape and moves to the screen edge
         *    its half of the 4:3 layout belongs to;
         *  - everything else keeps its 4:3 proportions, centred. */
        float squeeze = 1.0f, shift = 0.0f, yscale = 1.0f;
        if (s && s->sx > s->sy * 1.001f) {
            float lo = 1e30f, hi = -1e30f, ylo = 1e30f, yhi = -1e30f;
            uint32_t k;
            int flat = 1;
            for (k = 0; k < nv && flat; k++) {
                if (v[k].pos[3] != 1.0f) flat = 0;
                if (v[k].pos[0] < lo) lo = v[k].pos[0];
                if (v[k].pos[0] > hi) hi = v[k].pos[0];
                if (v[k].pos[1] < ylo) ylo = v[k].pos[1];
                if (v[k].pos[1] > yhi) yhi = v[k].pos[1];
            }
            if (flat) {
                float narrow = s->sy / s->sx;
                int wide_x = lo <= 0.5f && hi >= (float)gw - 0.5f;
                int tall = ylo <= 0.5f && yhi >= (float)gh - 0.5f;
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
                    if (nv2a_d3d_hud_active()) {
                        if (hi <= (float)gw * 0.5f) shift = narrow - 1.0f;
                        else if (lo >= (float)gw * 0.5f) shift = 1.0f - narrow;
                    }
                }
            }
        }
        vs_constants(st, gw, gh, zmax, squeeze, shift, yscale);
    }
    if (!(p = ring_map(s_vb, &s_vb_pos, VB_BYTES, vb_bytes, &m))) {
        RECOMP_PROFILE_END();
        return;
    }
    memcpy(p, v, vb_bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
    if (!(p = ring_map(s_ib, &s_ib_pos, IB_BYTES, ib_bytes, &m))) {
        RECOMP_PROFILE_END();
        return;
    }
    memcpy(p, idx, ib_bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ib, 0);
    {
        UINT stride = sizeof(NvD3DVertex), offset = s_vb_pos;
        ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &offset);
        ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ib, DXGI_FORMAT_R32_UINT, s_ib_pos);
        ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout);
        ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb_vs);
        ID3D11DeviceContext_DrawIndexed(s_ctx, ni, 0, 0);
    }
    s_vb_pos += (vb_bytes + 255) & ~255u;
    s_ib_pos += (ib_bytes + 255) & ~255u;
    if (s_prof_on > 0) s_prof[PROF_DRAW] += qpc() - t_draw;
    RECOMP_PROFILE_END();
}

/* ------------------------------------------------ vertex programs on the GPU */

typedef struct {
    uint64_t key;
    volatile LONG state;         /* VSH_FREE .. VSH_FAILED */
    ID3D11VertexShader *vs;
    ID3DBlob *blob;
    char *src;                   /* HLSL, owned by the compile while pending */
} VshEntry;

enum { VSH_FREE, VSH_COMPILING, VSH_READY, VSH_FAILED };
#define VSH_SLOTS 1024
static VshEntry s_vsh[VSH_SLOTS];

typedef struct { uint64_t key; ID3D11InputLayout *il; } IlEntry;
#define IL_SLOTS 2048
static IlEntry s_il[IL_SLOTS];

static ID3D11Buffer *s_cb_vconst, *s_cb_vdef;
static uint32_t s_vconst_version;
static float s_vdef_last[16][4];
static int s_vdef_valid;

static uint64_t hash64(const void *p, size_t n, uint64_t h)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t i;
    for (i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

extern const uint32_t (*nv2a_vsh_program(uint32_t *start, uint32_t *version))[4];
extern const float (*nv2a_vsh_constants(uint32_t *version))[4];
#define NV_VSH_SLOTS 136

/* The input format, element size and shader-side decode for one NV2A array
 * format; 0 if this path does not handle it. */
static int attr_format(uint32_t type, uint32_t size, DXGI_FORMAT *f, uint32_t *bytes,
                       uint8_t *kind)
{
    static const DXGI_FORMAT flt[5] = { 0, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT,
                                        DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT };
    if (!size || size > 4) return 0;
    *kind = NV_VSH_IN_FLOAT4;
    switch (type) {
    case 2:                                             /* float */
        *f = flt[size]; *bytes = 4 * size; return 1;
    case 0:                                             /* D3DCOLOR */
        if (size < 3) return 0;
        *f = DXGI_FORMAT_B8G8R8A8_UNORM; *bytes = 4;
        if (size == 3) *kind = NV_VSH_IN_FLOAT3W1;
        return 1;
    case 4:                                             /* ubyte, normalised */
        *f = size == 1 ? DXGI_FORMAT_R8_UNORM : size == 2 ? DXGI_FORMAT_R8G8_UNORM
                       : DXGI_FORMAT_R8G8B8A8_UNORM;
        *bytes = size == 3 ? 4 : size;
        if (size == 3) *kind = NV_VSH_IN_FLOAT3W1;
        return 1;
    case 1:                                             /* short, normalised */
        *f = size == 1 ? DXGI_FORMAT_R16_SNORM : size == 2 ? DXGI_FORMAT_R16G16_SNORM
                       : DXGI_FORMAT_R16G16B16A16_SNORM;
        *bytes = size == 3 ? 8 : 2 * size;
        if (size == 3) *kind = NV_VSH_IN_FLOAT3W1;
        return 1;
    case 5:                                             /* short */
        *f = size == 1 ? DXGI_FORMAT_R16_SINT : size == 2 ? DXGI_FORMAT_R16G16_SINT
                       : DXGI_FORMAT_R16G16B16A16_SINT;
        *bytes = size == 3 ? 8 : 2 * size;
        *kind = size == 3 ? NV_VSH_IN_INT3W1 : NV_VSH_IN_INT4;
        return 1;
    case 6:                                             /* packed 11:11:10 */
        if (size != 1) return 0;
        *f = DXGI_FORMAT_R32_UINT; *bytes = 4; *kind = NV_VSH_IN_CMP;
        return 1;
    default:
        return 0;
    }
}

/* Compiling a program takes tens of milliseconds, and driving into a new
 * part of the city meets a dozen at once: done inline, every one was a
 * visible hitch. They compile on the thread pool instead, and until one is
 * ready its draws take the CPU vertex path, which gives the same picture. */
static void CALLBACK vsh_compile(PTP_CALLBACK_INSTANCE inst, void *arg)
{
    VshEntry *e = (VshEntry *)arg;
    ID3DBlob *code = NULL, *err = NULL;
    LONG state = VSH_FAILED;
    (void)inst;
    if (SUCCEEDED(D3DCompile(e->src, strlen(e->src), "nv2a_vsh", NULL, NULL, "main", "vs_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)) &&
        SUCCEEDED(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                                  ID3D10Blob_GetBufferSize(code), NULL, &e->vs))) {
        e->blob = code;
        state = VSH_READY;
    } else {
        static volatile LONG said;
        if (InterlockedIncrement(&said) <= 4)
            fprintf(stderr, "[D3D11] vertex program translation failed: %s\n",
                    err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (code) ID3D10Blob_Release(code);
    }
    if (err) ID3D10Blob_Release(err);
    free(e->src);
    e->src = NULL;
    InterlockedExchange(&e->state, state);
}

/* The program's shader if it is ready; NULL (draw on the CPU) otherwise. */
static VshEntry *vsh_get(uint64_t key, const uint32_t (*prog)[4], uint32_t start,
                         const NvVshInput in[16])
{
    uint32_t h = (uint32_t)(key ^ (key >> 32)) & (VSH_SLOTS - 1), n;
    VshEntry *e = NULL;
    static char src[96 * 1024];
    for (n = 0; n < VSH_SLOTS; n++) {
        e = &s_vsh[(h + n) & (VSH_SLOTS - 1)];
        if (e->state == VSH_FREE || e->key == key) break;
    }
    if (e->state != VSH_FREE)
        return e->key == key && e->state == VSH_READY ? e : NULL;  /* or table full */
    e->key = key;
    if (nv2a_vsh_to_hlsl(prog, start, NV_VSH_SLOTS, in, src, sizeof src) <= 0 ||
        !(e->src = _strdup(src))) {
        e->state = VSH_FAILED;
        return NULL;
    }
    e->state = VSH_COMPILING;
    if (!TrySubmitThreadpoolCallback(vsh_compile, e, NULL))
        vsh_compile(NULL, e);
    return e->state == VSH_READY ? e : NULL;
}

int nv2a_d3d_draw_vsh(const NvD3DState *st, int topology,
                      const NvD3DAttrib attr[16], const float def[16][4],
                      const uint32_t *idx, uint32_t ni, uint32_t lo, uint32_t hi)
{
    D3D11_INPUT_ELEMENT_DESC el[16];
    ID3D11Buffer *bufs[16];
    UINT strides[16], offsets[16];
    NvVshInput in[16];
    struct { const uint8_t *base; uint32_t stride, span; } grp[16];
    uint32_t ngrp = 0, nel = 0, i, start, pver, cver, count = hi - lo + 1;
    const uint32_t (*prog)[4];
    const float (*consts)[4];
    uint64_t key;
    VshEntry *e;
    ID3D11InputLayout *il = NULL;
    Surface *s;
    uint32_t gw, gh, zmax;
    D3D11_MAPPED_SUBRESOURCE m;
    uint8_t *p;
    UINT total = 0, ib_bytes = ni * 4;
    int64_t t_draw;
    static uint32_t s_prog_version;
    static uint64_t s_prog_hash;

    if (!nv2a_d3d_init() || !ni || hi < lo || ib_bytes > IB_BYTES)
        return 0;
    prog = nv2a_vsh_program(&start, &pver);
    if (start >= NV_VSH_SLOTS) return 0;
    if (pver != s_prog_version) {
        uint32_t k;
        if (nv2a_vsh_needs_cpu(prog, start, NV_VSH_SLOTS)) {
            s_prog_version = 0;
            return 0;
        }
        s_prog_hash = 0xCBF29CE484222325ull ^ start;
        for (k = start; k < NV_VSH_SLOTS; k++) {
            s_prog_hash = hash64(prog[k], 16, s_prog_hash);
            if (prog[k][3] & 1) break;
        }
        s_prog_version = pver;
    }

    /* Inputs: interleaved arrays share one upload and one buffer slot. */
    memset(in, 0, sizeof in);
    key = s_prog_hash;
    for (i = 0; i < 16; i++) {
        DXGI_FORMAT f;
        uint32_t bytes, g;
        uint8_t kind;
        if (!attr[i].enabled) {
            key = key * 31 + 0xF00 + i;
            continue;
        }
        if (!attr_format(attr[i].type, attr[i].size, &f, &bytes, &kind) ||
            (attr[i].stride & 3) ||
            /* elements align to min(4, component size) */
            ((uintptr_t)attr[i].data & (attr[i].type == 4 ? 0 : attr[i].type == 1 ||
                                        attr[i].type == 5 ? 1 : 3)))
            return 0;
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
        {
            uint32_t end = (uint32_t)(attr[i].data - grp[g].base) + bytes;
            if (end > grp[g].span) grp[g].span = end;
        }
        memset(&el[nel], 0, sizeof el[nel]);
        el[nel].SemanticName = "ATTR";
        el[nel].SemanticIndex = i;
        el[nel].Format = f;
        el[nel].InputSlot = g;
        el[nel].AlignedByteOffset = (UINT)(attr[i].data - grp[g].base);
        el[nel].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        nel++;
        in[i].enabled = 1;
        in[i].kind = kind;
        key = key * 31 + (attr[i].type << 4 | attr[i].size);
    }
    if (!(e = vsh_get(key, prog, start, in)))
        return 0;
    {
        /* Input layouts: by shader and element layout. */
        uint64_t lk = key;
        uint32_t h, n;
        for (i = 0; i < nel; i++) {
            lk = hash64(&el[i].Format, sizeof el[i].Format, lk ^ el[i].SemanticIndex);
            lk = hash64(&el[i].AlignedByteOffset, 4, lk ^ el[i].InputSlot);
        }
        h = (uint32_t)(lk ^ (lk >> 32)) & (IL_SLOTS - 1);
        for (n = 0; n < IL_SLOTS; n++) {
            IlEntry *x = &s_il[(h + n) & (IL_SLOTS - 1)];
            if (x->il && x->key == lk) { il = x->il; break; }
            if (!x->il) {
                if (FAILED(ID3D11Device_CreateInputLayout(s_dev, el, nel,
                        ID3D10Blob_GetBufferPointer(e->blob), ID3D10Blob_GetBufferSize(e->blob),
                        &x->il)))
                    return 0;
                x->key = lk;
                il = x->il;
                break;
            }
        }
        if (!il) return 0;
    }

    for (i = 0; i < ngrp; i++)
        total += (((grp[i].stride ? (count - 1) * grp[i].stride : 0) + grp[i].span) + 15) & ~15u;
    if (total > VB_BYTES) return 0;

    RECOMP_PROFILE_BEGIN("D3D11 draw");
    t_draw = s_prof_on > 0 ? qpc() : 0;
    s_prof_draws++;
    if (!setup_pipeline(st, topology, &s, &gw, &gh, &zmax)) {
        RECOMP_PROFILE_END();
        return 1;
    }
    vs_constants(st, gw, gh, zmax, 1.0f, 0.0f, 1.0f);
    if (!s_cb_vconst) {
        s_cb_vconst = make_buffer(192 * 16, D3D11_BIND_CONSTANT_BUFFER);
        s_cb_vdef = make_buffer(16 * 16, D3D11_BIND_CONSTANT_BUFFER);
    }
    consts = nv2a_vsh_constants(&cver);
    if (cver != s_vconst_version) {
        upload_cb(s_cb_vconst, consts, 192 * 16);
        s_vconst_version = cver;
    }
    if (!s_vdef_valid || memcmp(s_vdef_last, def, sizeof s_vdef_last)) {
        memcpy(s_vdef_last, def, sizeof s_vdef_last);
        upload_cb(s_cb_vdef, def, sizeof s_vdef_last);
        s_vdef_valid = 1;
    }

    /* Vertex data for [lo, hi], one span per group. */
    if (!(p = ring_map(s_vb, &s_vb_pos, VB_BYTES, total, &m))) {
        RECOMP_PROFILE_END();
        return 1;
    }
    {
        UINT at = 0;
        for (i = 0; i < ngrp; i++) {
            UINT bytes = (grp[i].stride ? (count - 1) * grp[i].stride : 0) + grp[i].span;
            memcpy(p + at, grp[i].base + (size_t)lo * grp[i].stride, bytes);
            bufs[i] = s_vb;
            strides[i] = grp[i].stride;
            offsets[i] = s_vb_pos + at;
            at += (bytes + 15) & ~15u;
        }
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
    if (!(p = ring_map(s_ib, &s_ib_pos, IB_BYTES, ib_bytes, &m))) {
        RECOMP_PROFILE_END();
        return 1;
    }
    {
        uint32_t *o = (uint32_t *)p;
        for (i = 0; i < ni; i++)
            o[i] = idx[i] - lo;
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ib, 0);
    {
        ID3D11Buffer *vcb[4] = { s_cb_vs, NULL, s_cb_vconst, s_cb_vdef };
        ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, ngrp, bufs, strides, offsets);
        ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ib, DXGI_FORMAT_R32_UINT, s_ib_pos);
        ID3D11DeviceContext_IASetInputLayout(s_ctx, il);
        ID3D11DeviceContext_VSSetShader(s_ctx, e->vs, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 4, vcb);
        ID3D11DeviceContext_DrawIndexed(s_ctx, ni, 0, 0);
    }
    s_vb_pos += (total + 255) & ~255u;
    s_ib_pos += (ib_bytes + 255) & ~255u;
    s_prof_verts += count;
    if (s_prof_on > 0) s_prof[PROF_DRAW] += qpc() - t_draw;
    RECOMP_PROFILE_END();
    return 1;
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
            !s->shadow || !guest_ok(s->va, (size_t)s->pitch * s->h))
            continue;
        scaled = s->iw != s->w || s->ih != s->h;
        if (!wb_slot_ready(k, s->w, s->h, scaled))
            continue;
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
    /* In guest pixels: the host target has scale^2 as many. */
    px = (double)s_zpass / ((double)s_cur_sx * s_cur_sy);
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
        if (remain > spin && timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((remain - spin) * 10000000 / freq.QuadPart);
            if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
                WaitForSingleObject(timer, INFINITE);
        }
        while (qpc() < next)
            YieldProcessor();
        if (s_prof_on > 0) s_prof[PROF_PACE] += qpc() - t0;
    }
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
    prof_report();
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
void nv2a_d3d_draw(const NvD3DState *st, int t, const NvD3DVertex *v, uint32_t nv,
                   const uint32_t *i, uint32_t ni) { (void)st; (void)t; (void)v; (void)nv; (void)i; (void)ni; }
void nv2a_d3d_clear(const NvD3DState *st, uint32_t f, uint32_t c, uint32_t z,
                    uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{ (void)st; (void)f; (void)c; (void)z; (void)x0; (void)y0; (void)x1; (void)y1; }
void nv2a_d3d_flip(uint32_t a, uint32_t p) { (void)a; (void)p; }
void nv2a_d3d_tick(void) {}
void nv2a_d3d_kick(void) {}
int nv2a_d3d_draw_vsh(const NvD3DState *st, int t, const NvD3DAttrib a[16], const float d[16][4],
                      const uint32_t *i, uint32_t n, uint32_t lo, uint32_t hi)
{ (void)st; (void)t; (void)a; (void)d; (void)i; (void)n; (void)lo; (void)hi; return 0; }
void nv2a_d3d_zpass_enable(int on) { (void)on; }
void nv2a_d3d_zpass_clear(void) {}
uint32_t nv2a_d3d_zpass_read(void) { return 0; }
float nv2a_d3d_display_aspect(void) { return 4.0f / 3.0f; }
void nv2a_d3d_note_race_camera(void) {}
void nv2a_d3d_note_frontend_camera(void) {}
#endif
