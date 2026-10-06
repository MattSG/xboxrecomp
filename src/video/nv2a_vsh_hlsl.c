/**
 * NV2A vertex program -> HLSL vertex shader.
 *
 * The same instruction set src/kernel/nv2a_vsh_interp.c executes on the CPU,
 * emitted as straight-line HLSL so the host GPU runs it per vertex instead.
 * Semantics follow the interpreter exactly (it is the tested reference): both
 * units read their sources before either writes, an ILU paired with a MAC
 * writes R1, temp 12 is oPos, ARL floors with a small epsilon, ILU ops take
 * the C operand's x.
 *
 * The generated shader ends in the same epilogue as the renderer's
 * pass-through VS (clip-space reconstruction of oPos, clamped colours, the
 * fixed-function fog factor), so its output matches that VS's PSIn exactly.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include "nv2a_vsh_hlsl.h"

static uint32_t field(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

typedef struct { char *p; size_t cap, len; int overflow; } Buf;

static void put(Buf *b, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (b->overflow) return;
    va_start(ap, fmt);
    n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len) { b->overflow = 1; return; }
    b->len += (size_t)n;
}

static const char *mask_str(uint32_t m, char out[5])
{
    int n = 0;
    if (m & 8) out[n++] = 'x';
    if (m & 4) out[n++] = 'y';
    if (m & 2) out[n++] = 'z';
    if (m & 1) out[n++] = 'w';
    out[n] = 0;
    return out;
}

static void src_expr(Buf *b, const uint32_t *ins, int which)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg;
    if (which == 0) {
        neg = field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        mux = field(ins, 2, 26, 2); reg = field(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        mux = field(ins, 2, 11, 2); reg = field(ins, 2, 13, 4);
    } else {
        neg = field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        mux = field(ins, 3, 28, 2);
        reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
    }
    put(b, "%s", neg ? "-" : "");
    if (mux == 1)
        put(b, "R[%u]", reg <= 12 ? reg : 0);
    else if (mux == 2)
        put(b, "v[%u]", field(ins, 1, 9, 4));
    else if (field(ins, 3, 1, 1))
        put(b, "C[cidx(a0 + %u)]", field(ins, 1, 13, 8));
    else
        put(b, "C[%u]", field(ins, 1, 13, 8) < 192 ? field(ins, 1, 13, 8) : 0);
    put(b, ".%c%c%c%c", sw[(swz >> 6) & 3], sw[(swz >> 4) & 3],
        sw[(swz >> 2) & 3], sw[swz & 3]);
}

int nv2a_vsh_needs_cpu(const uint32_t (*prog)[4], uint32_t start, uint32_t slots)
{
    uint32_t s;
    for (s = start; s < slots; s++) {
        const uint32_t *ins = prog[s];
        if (field(ins, 3, 12, 4) && !field(ins, 3, 11, 1))
            return 1;                 /* writes the constant file */
        if (field(ins, 3, 0, 1))
            return 0;
    }
    return 1;                         /* no FINAL: nothing sensible to run */
}

static void emit_input(Buf *b, int i, const NvVshInput *in)
{
    if (!in->enabled) {
        put(b, "  v[%d] = DEF[%d];\n", i, i);
        return;
    }
    switch (in->kind) {
    case NV_VSH_IN_FLOAT4:
        put(b, "  v[%d] = vin.a%d;\n", i, i);
        break;
    case NV_VSH_IN_FLOAT3W1:
        put(b, "  v[%d] = float4(vin.a%d.xyz, 1.0);\n", i, i);
        break;
    case NV_VSH_IN_INT4:
        put(b, "  v[%d] = (float4)vin.a%d;\n", i, i);
        break;
    case NV_VSH_IN_INT3W1:
        put(b, "  v[%d] = float4((float3)vin.a%d.xyz, 1.0);\n", i, i);
        break;
    case NV_VSH_IN_CMP:
        put(b, "  { int u = (int)vin.a%d;\n"
               "    v[%d] = float4((float)((u << 21) >> 21) / 1023.0,\n"
               "                   (float)((u << 10) >> 21) / 1023.0,\n"
               "                   (float)(u >> 22) / 511.0, 1.0); }\n", i, i);
        break;
    }
}

int nv2a_vsh_to_hlsl(const uint32_t (*prog)[4], uint32_t start, uint32_t slots,
                     const NvVshInput in[16], char *out, size_t cap)
{
    Buf b = { out, cap, 0, 0 };
    uint32_t s;
    int i;

    put(&b,
        "cbuffer VSC : register(b0) { float4 vs_map; uint4 fogi; float4 fogf; };\n"
        "cbuffer VK : register(b2) { float4 C[192]; };\n"
        "cbuffer VD : register(b3) { float4 DEF[16]; };\n"
        "struct VIn {\n");
    for (i = 0; i < 16; i++) {
        if (!in[i].enabled) continue;
        switch (in[i].kind) {
        case NV_VSH_IN_FLOAT4: case NV_VSH_IN_FLOAT3W1:
            put(&b, "  float4 a%d : ATTR%d;\n", i, i); break;
        case NV_VSH_IN_INT4: case NV_VSH_IN_INT3W1:
            put(&b, "  int4 a%d : ATTR%d;\n", i, i); break;
        case NV_VSH_IN_CMP:
            put(&b, "  uint a%d : ATTR%d;\n", i, i); break;
        }
    }
    put(&b, "  uint vid : SV_VertexID;\n};\n"
        "struct PSIn { float4 pos:SV_Position; float4 d0:COLOR0; float4 d1:COLOR1; float4 fog:FOG;\n"
        "  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3; };\n"
        "uint cidx(int i) { return (uint)i < 192u ? (uint)i : 0u; }\n"
        "float fog_factor(float d) {\n"
        "  if (!fogi.x) return 1.0;\n"
        "  float f, px = fogf.x, py = fogf.y; uint mode = fogi.y;\n"
        "  if (mode == 0x800 || mode == 0x802) f = px + exp2(d * py * 16.0) - 1.5;\n"
        "  else if (mode == 0x801 || mode == 0x803) f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n"
        "  else f = px + d * py - 1.0;\n"
        "  if (mode == 0x802 || mode == 0x803 || mode == 0x804) f = abs(f);\n"
        "  if (isnan(f)) f = 1.0;\n"
        "  return saturate(f);\n"
        "}\n"
        "PSIn main(VIn vin) {\n"
        "  float4 v[16];\n");
    for (i = 0; i < 16; i++)
        emit_input(&b, i, &in[i]);
    put(&b,
        "  float4 R[13];\n"
        "  [unroll] for (int k = 0; k < 13; k++) R[k] = float4(0, 0, 0, 0);\n"
        "  float4 o[13];\n"
        "  [unroll] for (int j = 0; j < 13; j++) o[j] = float4(0, 0, 0, 0);\n"
        "  o[3].w = 1; o[4].w = 1; o[9].w = 1; o[10].w = 1; o[11].w = 1; o[12].w = 1;\n"
        "  int a0 = 0;\n");

    for (s = start; s < slots; s++) {
        const uint32_t *ins = prog[s];
        uint32_t mac = field(ins, 1, 21, 4), ilu = field(ins, 1, 25, 3);
        uint32_t mac_mask = field(ins, 3, 24, 4), ilu_mask = field(ins, 3, 16, 4);
        uint32_t tdst = field(ins, 3, 20, 4);
        uint32_t omask = field(ins, 3, 12, 4), oaddr = field(ins, 3, 3, 8);
        char m[5];

        put(&b, "  { // %u\n    float4 A = ", s); src_expr(&b, ins, 0);
        put(&b, ";\n    float4 B = "); src_expr(&b, ins, 1);
        put(&b, ";\n    float4 Cc = "); src_expr(&b, ins, 2);
        put(&b, ";\n    float4 mr = A, ir = Cc;\n");
        switch (mac) {
        case 2: put(&b, "    mr = A * B;\n"); break;
        case 3: put(&b, "    mr = A + Cc;\n"); break;
        case 4: put(&b, "    mr = A * B + Cc;\n"); break;
        case 5: put(&b, "    mr = dot(A.xyz, B.xyz).xxxx;\n"); break;
        case 6: put(&b, "    mr = (dot(A.xyz, B.xyz) + B.w).xxxx;\n"); break;
        case 7: put(&b, "    mr = dot(A, B).xxxx;\n"); break;
        case 8: put(&b, "    mr = float4(1.0, A.y * B.y, A.z, B.w);\n"); break;
        case 9: put(&b, "    mr = A < B ? A : B;\n"); break;
        case 10: put(&b, "    mr = A >= B ? A : B;\n"); break;
        case 11: put(&b, "    mr = A < B ? 1.0 : 0.0;\n"); break;
        case 12: put(&b, "    mr = A >= B ? 1.0 : 0.0;\n"); break;
        case 13: put(&b, "    a0 = (int)floor(A.x + 0.001);\n"); break;
        default: break;
        }
        switch (ilu) {
        case 2: put(&b, "    ir = (Cc.x == 0.0 ? 1.#INF : 1.0 / Cc.x).xxxx;\n"); break;
        case 3: put(&b, "    { float f = 1.0 / Cc.x; float af = clamp(abs(f), 5.42101e-20, 1.884467e19);\n"
                         "      ir = (f < 0 ? -af : af).xxxx; }\n"); break;
        case 4: put(&b, "    ir = (1.0 / sqrt(abs(Cc.x))).xxxx;\n"); break;
        case 5: put(&b, "    { float fl = floor(Cc.x); ir = float4(exp2(fl), Cc.x - fl, exp2(Cc.x), 1.0); }\n"); break;
        case 6: put(&b, "    { float ax = abs(Cc.x);\n"
                         "      if (ax == 0.0) ir = float4(-1.#INF, 1.0, -1.#INF, 1.0);\n"
                         "      else { float e = floor(log2(ax)); ir = float4(e, ax / exp2(e), log2(ax), 1.0); } }\n"); break;
        case 7: put(&b, "    { float nl = max(Cc.x, 0.0), nh = max(Cc.y, 0.0), p = clamp(Cc.w, -127.9961, 127.9961);\n"
                         "      ir = float4(1.0, nl, Cc.x > 0.0 ? pow(nh, p) : 0.0, 1.0); }\n"); break;
        default: break;
        }
        if (mac && mac != 13 && mac_mask && tdst <= 12)
            put(&b, "    R[%u].%s = mr.%s;\n", tdst, mask_str(mac_mask, m), m);
        if (ilu && ilu_mask) {
            uint32_t it = mac ? 1u : tdst;
            if (it <= 12)
                put(&b, "    R[%u].%s = ir.%s;\n", it, mask_str(ilu_mask, m), m);
        }
        if (omask && field(ins, 3, 11, 1)) {
            const char *src = field(ins, 3, 2, 1) ? "ir" : "mr";
            if (oaddr == 0)
                put(&b, "    R[12].%s = %s.%s;\n", mask_str(omask, m), src, m);
            else if (oaddr < 13)
                put(&b, "    o[%u].%s = %s.%s;\n", oaddr, mask_str(omask, m), src, m);
        }
        put(&b, "  }\n");
        if (field(ins, 3, 0, 1))
            break;
    }
    if (s >= slots)
        return 0;

    /* The pass-through VS's epilogue: oPos is screen space after the D3D
     * divide, with clip-space w kept; rebuild clip space for the host. */
    put(&b,
        "  PSIn p;\n"
        "  float w = R[12].w;\n"
        "  float aw = clamp(abs(w), 5.421011e-20, 1.8446744e19);\n"
        "  w = w < 0 ? -aw : aw;\n"
        "  p.pos = float4((R[12].x * vs_map.x - 1.0) * vs_map.w * w, (1.0 - R[12].y * vs_map.y) * w,\n"
        "                 R[12].z * vs_map.z * w, w);\n"
        "  p.d0 = saturate(o[3]); p.d1 = saturate(o[4]);\n"
        "  p.fog = float4(fog_factor(o[5].x), 0, 0, 0);\n"
        "  p.t0 = o[9]; p.t1 = o[10]; p.t2 = o[11]; p.t3 = o[12];\n"
        "  return p;\n"
        "}\n");
    return b.overflow ? -1 : (int)b.len;
}
