/* SPEC: a variant specialised to one combiner configuration. The control
 * words below become constants (SPEC_DECL) so the compiler folds the
 * interpreter away; the cbuffer keeps its layout under other names. */
#ifdef SPEC
#define c_icw cb_c_icw
#define a_icw cb_a_icw
#define c_ocw cb_c_ocw
#define a_ocw cb_a_ocw
#define fin0 cb_fin0
#define fin1 cb_fin1
#define rc_control cb_rc_control
#define stage_prog cb_stage_prog
#define alpha_func cb_alpha_func
#define ps_flags cb_ps_flags
#define clip_plane cb_clip_plane
#define other_input cb_other_input
#define dot_map cb_dot_map
#define tex_info cb_tex_info
#define RC_LOOP [unroll]
#else
#define RC_LOOP [loop]
#endif
cbuffer PSC : CBUF(1) {
  uint4 c_icw[2]; uint4 a_icw[2]; uint4 c_ocw[2]; uint4 a_ocw[2];
  float4 cf0[8]; float4 cf1[8]; float4 fin_c0; float4 fin_c1;
  uint fin0; uint fin1; uint rc_control; uint stage_prog;
  uint alpha_func; uint alpha_ref; uint ps_flags; uint clip_plane;
  uint other_input; uint dot_map; uint pad0; uint pad1;
  uint4 tex_info[4];      /* x: bit0 alphakill, y: signed mask (filter>>28) | bit8 volume */
  float4 tex_scale[4];    /* xy multiply the coordinate (1/size for linear) */
  float4 fog_color;
  float4 bump_mat[4]; float4 bump_lum[4];
  float4 zp0; float4 zp1;
  float4 wclip[8]; uint wclip_mode; uint3 wclip_pad;  /* 1 keep inside, 2 keep outside */
};
#ifdef SDLGPU
/* Border addressing, which SDL3 GPU samplers lack (CLAMP_TO_BORDER): those
 * axes clamp to the edge and a coordinate outside [0, 1] takes the border
 * colour here. tex_border_uv: by stage, bits 0/1/2 select u/v/w. */
cbuffer PSB : CBUF(2) { float4 tex_border[4]; uint4 tex_border_uv; };
#endif
#ifdef SPEC
#undef c_icw
#undef a_icw
#undef c_ocw
#undef a_ocw
#undef fin0
#undef fin1
#undef rc_control
#undef stage_prog
#undef alpha_func
#undef ps_flags
#undef clip_plane
#undef other_input
#undef dot_map
#undef tex_info
SPEC_DECL
#endif
PS_TEX(Texture2D, T2D0, 0); PS_TEX(Texture2D, T2D1, 1); PS_TEX(Texture2D, T2D2, 2); PS_TEX(Texture2D, T2D3, 3);
PS_TEX(TextureCube, TC0, 4); PS_TEX(TextureCube, TC1, 5); PS_TEX(TextureCube, TC2, 6); PS_TEX(TextureCube, TC3, 7);
/* Slot 8 is reserved; volume bindings remain at 9..12. */
PS_TEX(Texture3D, TV0, 9); PS_TEX(Texture3D, TV1, 10); PS_TEX(Texture3D, TV2, 11); PS_TEX(Texture3D, TV3, 12);
PS_SAMP(S0, 0); PS_SAMP(S1, 1); PS_SAMP(S2, 2); PS_SAMP(S3, 3);
/* A cube map samples with its stage's sampler: one slot of its own on SDL3
 * GPU, the 2D one on D3D11. */
#ifdef SDLGPU
PS_SAMP(SC0, 4); PS_SAMP(SC1, 5); PS_SAMP(SC2, 6); PS_SAMP(SC3, 7);
PS_SAMP(SV0, 9); PS_SAMP(SV1, 10); PS_SAMP(SV2, 11); PS_SAMP(SV3, 12);
#else
#define SC0 S0
#define SC1 S1
#define SC2 S2
#define SC3 S3
#define SV0 S0
#define SV1 S1
#define SV2 S2
#define SV3 S3
#endif
uint U8(uint4 a[2], uint s) { return a[s >> 2][s & 3]; }
float map_in(float x, uint b) {
  float p = max(x, 0.0);
  switch ((b >> 5) & 7) {
  case 0: return p; case 1: return 1.0 - saturate(x); case 2: return 2.0 * p - 1.0;
  case 3: return -2.0 * p + 1.0; case 4: return p - 0.5; case 5: return -p + 0.5;
  case 6: return x; default: return -x; }
}
float3 map_in3(float3 v, uint b) { return float3(map_in(v.x, b), map_in(v.y, b), map_in(v.z, b)); }
float3 map_out3(float3 x, uint m) {
  if (m == 0x08) return x - 0.5; if (m == 0x10) return x * 2.0; if (m == 0x18) return (x - 0.5) * 2.0;
  if (m == 0x20) return x * 4.0; if (m == 0x30) return x * 0.5; return x;
}
float signed_c(float c) { float s = c * 255.0; return clamp((s >= 127.5 ? s - 256.0 : s) / 127.0, -1.0, 1.0); }
float4 apply_signed(float4 t, uint m) {
  if (m & 1) t.a = signed_c(t.a); if (m & 2) t.r = signed_c(t.r);
  if (m & 4) t.g = signed_c(t.g); if (m & 8) t.b = signed_c(t.b); return t;
}
/* SET_DOT_RGBMAPPING, as xemu's sign1/2/3: 1 MINUS_1_TO_1_D3D is the
 * biased (x - 128) / 127 that D3D's _bx2 on a texture read produces (255 is
 * +1), 2 the GL mapping, 3 two's complement. */
float3 dotmap(float3 c, uint m) {
  float3 x = round(saturate(c) * 255.0);
  if (m == 1) return (x - 128.0) / 127.0;
  if (m == 2) return x >= 128.0 ? (x - 255.5) / 127.5 : (x + 0.5) / 127.5;
  if (m == 3) return (x >= 128.0 ? x - 256.0 : x) / 127.0;
  return c;
}
float4 bspline0(float2 uv, float lod) {
  float2 sz; float nl; T2D0.GetDimensions(0, sz.x, sz.y, nl); lod = min(lod, nl - 1.0); sz = max(floor(sz / exp2(lod)), 1.0);
  float2 p = uv * sz - 0.5, f = frac(p), f2 = f * f, f3 = f2 * f;
  float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0, w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
  float2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0, w3 = f3 / 6.0;
  float2 s0 = w0 + w1, s1 = w2 + w3; p -= f;
  float2 c0 = (p - 0.5 + w1 / s0) / sz, c1 = (p + 1.5 + w3 / s1) / sz;
  return s0.y * (s0.x * T2D0.SampleLevel(S0, c0, lod) + s1.x * T2D0.SampleLevel(S0, float2(c1.x, c0.y), lod)) +
         s1.y * (s0.x * T2D0.SampleLevel(S0, float2(c0.x, c1.y), lod) + s1.x * T2D0.SampleLevel(S0, c1, lod));
}
float4 samp2(uint n, float2 uv) {
#ifdef SDLGPU
  uint bu = tex_border_uv[n];
  if (((bu & 1u) && (uv.x < 0.0 || uv.x > 1.0)) || ((bu & 2u) && (uv.y < 0.0 || uv.y > 1.0)))
    return tex_border[n];
#endif
  if (n == 0 && (ps_flags & 4)) return bspline0(uv, 0.0);
  if (n == 0) return T2D0.Sample(S0, uv); if (n == 1) return T2D1.Sample(S1, uv);
  if (n == 2) return T2D2.Sample(S2, uv); return T2D3.Sample(S3, uv);
}
/* A point-sampled tap at a fixed spot of an upscaled render target (light
 * flares test their visibility with three such taps of the frame's
 * alpha). On the NV2A it reads one guest pixel; here that pixel is a block
 * of host pixels, and a single host texel inside it changes with every
 * sub-pixel move of the scene -- glows and tail lights flickered. The block
 * the guest pixel became is averaged instead. g: guest texel coordinates,
 * inv: 1 / guest size. */
float4 ldn(uint n, int2 p) {
  if (n == 0) return T2D0.Load(int3(p, 0)); if (n == 1) return T2D1.Load(int3(p, 0));
  if (n == 2) return T2D2.Load(int3(p, 0)); return T2D3.Load(int3(p, 0));
}
float4 tapbox(uint n, float2 g, float2 inv) {
  uint w, h;
  if (n == 0) T2D0.GetDimensions(w, h); else if (n == 1) T2D1.GetDimensions(w, h);
  else if (n == 2) T2D2.GetDimensions(w, h); else T2D3.GetDimensions(w, h);
  float2 dim = float2(w, h), sc = dim * inv, gi = floor(g);
  int2 a = clamp(int2(floor(gi * sc + 0.5)), 0, int2(dim) - 1);
  int2 b = clamp(int2(floor((gi + 1.0) * sc + 0.5)), a + 1, int2(dim));
  float4 acc = 0; float cnt = 0;
  [loop] for (int y = a.y; y < b.y && y < a.y + 8; y++)
    [loop] for (int x = a.x; x < b.x && x < a.x + 8; x++) { acc += ldn(n, int2(x, y)); cnt += 1; }
  return acc / max(cnt, 1.0);
}
float4 sampc(uint n, float3 d) {
  if (n == 0) return TC0.Sample(SC0, d); if (n == 1) return TC1.Sample(SC1, d);
  if (n == 2) return TC2.Sample(SC2, d); return TC3.Sample(SC3, d);
}
float4 sampv(uint n, float3 uv) {
#ifdef SDLGPU
  uint bu = tex_border_uv[n];
  if (((bu & 1u) && (uv.x < 0.0 || uv.x > 1.0)) || ((bu & 2u) && (uv.y < 0.0 || uv.y > 1.0)) ||
      ((bu & 4u) && (uv.z < 0.0 || uv.z > 1.0))) return tex_border[n];
#endif
  if (n == 0) return TV0.Sample(SV0, uv); if (n == 1) return TV1.Sample(SV1, uv);
  if (n == 2) return TV2.Sample(SV2, uv); return TV3.Sample(SV3, uv);
}
float4 shade(PSIn i, out uint cov) {
  if (wclip_mode != 0u) {   /* window clip regions the scissor cannot express */
    bool in_any = false;
    [unroll] for (uint w = 0; w < 8; w++)
      in_any = in_any || (all(i.pos.xy >= wclip[w].xy) && all(i.pos.xy < wclip[w].zw));
    if (in_any == (wclip_mode == 2u)) discard;
  }
  float4 tc[4] = { i.t0, i.t1, i.t2, i.t3 };
  float4 T[4] = { float4(0,0,0,1), float4(0,0,0,1), float4(0,0,0,1), float4(0,0,0,1) }; float dots[4] = { 0, 0, 0, 0 };
  [unroll] for (uint n = 0; n < 4; n++) {
    uint mode = (stage_prog >> (n * 5)) & 31;
    uint inp = n == 2 ? (other_input >> 16) & 1 : (n == 3 ? (other_input >> 20) & 3 : 0);
    uint dm = n ? (dot_map >> ((n - 1) * 4)) & 7 : 0;
    float4 c = tc[n]; float4 t = float4(0, 0, 0, 1);
    float q = c.w != 0.0 ? c.w : 1.0;
    if ((mode == 1 || mode == 2) && (tex_info[n].y & 256u)) t = sampv(n, c.xyz / q);
    else if (mode == 1 || mode == 2) {
      float2 g = c.xy / q, d = abs(ddx(g)) + abs(ddy(g));
      if (tex_info[n].z != 0u && max(d.x, d.y) < 1e-4) t = tapbox(n, g, tex_scale[n].xy);
      else t = samp2(n, g * tex_scale[n].xy);
    }
    else if (mode == 3) t = sampc(n, c.xyz);
    else if (mode == 4) t = saturate(c);
    else if (mode == 5) {
      [unroll] for (uint j = 0; j < 4; j++) {
        bool ge = (clip_plane >> (n * 4 + j)) & 1;
        if (ge ? c[j] >= 0.0 : c[j] < 0.0) discard; }
      t = float4(0, 0, 0, 0);
    } else if (mode == 6 || mode == 7) {
      float4 s = T[inp]; float4 m = bump_mat[n];
      float2 uv = c.xy / q * tex_scale[n].xy;
      uv += float2(m.x * s.r + m.z * s.g, m.y * s.r + m.w * s.g) * tex_scale[n].zw;
      t = samp2(n, uv);
      if (mode == 7) t.rgb *= saturate(s.b * bump_lum[n].x + bump_lum[n].y);
    } else if (mode == 17 || mode == 10) {
      dots[n] = dot(c.xyz, dotmap(T[inp].rgb, dm)); t = float4(0, 0, 0, 0);
    } else if (mode == 9) {
      dots[n] = dot(c.xyz, dotmap(T[inp].rgb, dm));
      /* Stage 0 has no previous dot product (dxil rejects reading dots[-1]). */
      t = samp2(n, float2(dots[n ? n - 1 : 0], dots[n]) * tex_scale[n].xy);
    } else if (mode == 13 || mode == 14 || mode == 12 || mode == 18 || mode == 11) {
      dots[n] = dot(c.xyz, dotmap(T[inp].rgb, dm));
      float3 N = float3(dots[1], dots[2], dots[3]);
      if (mode == 12 || mode == 18) {
        float3 E = mode == 12 ? float3(tc[1].w, tc[2].w, tc[3].w) : float3(0, 0, 1);
        N = 2.0 * N * dot(N, E) / max(dot(N, N), 1e-20) - E;
      }
      if (mode == 13) t = (tex_info[n].y & 256u) ? sampv(n, N) : samp2(n, N.xy * tex_scale[n].xy); else t = sampc(n, N);
    } else if (mode == 15) t = samp2(n, T[inp].ar);
    else if (mode == 16) t = samp2(n, T[inp].gb);
    if (mode != 0 && mode != 4 && mode != 5 && mode != 10 && mode != 17) {
      t = apply_signed(t, tex_info[n].y);
      if ((tex_info[n].x & 1) && t.a == 0.0) discard;
    }
    T[n] = t;
  }
  float4 R[16];
  [unroll] for (uint k = 0; k < 16; k++) R[k] = float4(0, 0, 0, 0);
  R[3] = float4(fog_color.rgb, i.fog.x); R[4] = i.d0; R[5] = i.d1;
  R[8] = T[0]; R[9] = T[1]; R[10] = T[2]; R[11] = T[3];
  float4 o;
  if (!(ps_flags & 1)) {
/* No combiner program yet: texture 0 modulated by diffuse. */
    o = (stage_prog & 31) ? T[0] * i.d0 : i.d0;
  } else {
    R[12].a = (stage_prog & 31) ? T[0].a : 1.0;
    uint nst = min(rc_control & 0xFF, 8); uint fl8 = rc_control >> 8;
    RC_LOOP for (uint s = 0; s < nst; s++) {
      uint icw = U8(c_icw, s), ocw = U8(c_ocw, s), aicw = U8(a_icw, s), aocw = U8(a_ocw, s);
      if (((ocw | aocw) & 0xFFF) == 0) continue;
      R[1] = cf0[(fl8 & 0x010) ? s : 0]; R[2] = cf1[(fl8 & 0x100) ? s : 0];
      bool mux = (fl8 & 1) ? R[12].a >= 0.5 : (((uint)(R[12].a * 255.0)) & 1) != 0;
      uint b;
      b = icw >> 24; float3 A = map_in3((b & 0x10) ? R[b & 15].aaa : R[b & 15].rgb, b);
      b = icw >> 16; float3 B = map_in3((b & 0x10) ? R[b & 15].aaa : R[b & 15].rgb, b);
      b = icw >> 8;  float3 C = map_in3((b & 0x10) ? R[b & 15].aaa : R[b & 15].rgb, b);
      b = icw;       float3 D = map_in3((b & 0x10) ? R[b & 15].aaa : R[b & 15].rgb, b);
      b = aicw >> 24; float aA = map_in((b & 0x10) ? R[b & 15].a : R[b & 15].b, b);
      b = aicw >> 16; float aB = map_in((b & 0x10) ? R[b & 15].a : R[b & 15].b, b);
      b = aicw >> 8;  float aC = map_in((b & 0x10) ? R[b & 15].a : R[b & 15].b, b);
      b = aicw;       float aD = map_in((b & 0x10) ? R[b & 15].a : R[b & 15].b, b);
      uint fl = ocw >> 12, afl = aocw >> 12;
      float3 pab = (fl & 2) ? dot(A, B).xxx : A * B;
      float3 pcd = (fl & 1) ? dot(C, D).xxx : C * D;
      float3 sum = (fl & 4) ? (mux ? pcd : pab) : pab + pcd;
      float3 ab = clamp(map_out3(pab, fl & 0x38), -1.0, 1.0);
      float3 cd = clamp(map_out3(pcd, fl & 0x38), -1.0, 1.0);
      float3 ms = clamp(map_out3(sum, fl & 0x38), -1.0, 1.0);
      float apab = aA * aB, apcd = aC * aD;
      float asum = (afl & 4) ? (mux ? apcd : apab) : apab + apcd;
      float aab = clamp(map_out3(apab.xxx, afl & 0x38).x, -1.0, 1.0);
      float acd = clamp(map_out3(apcd.xxx, afl & 0x38).x, -1.0, 1.0);
      float ams = clamp(map_out3(asum.xxx, afl & 0x38).x, -1.0, 1.0);
      uint d;
      d = (ocw >> 4) & 15; if (d) { R[d].rgb = ab; if (fl & 0x80) R[d].a = ab.b; }
      d = ocw & 15;        if (d) { R[d].rgb = cd; if (fl & 0x40) R[d].a = cd.b; }
      d = (ocw >> 8) & 15; if (d) R[d].rgb = ms;
      d = (aocw >> 4) & 15; if (d) R[d].a = aab;
      d = aocw & 15;        if (d) R[d].a = acd;
      d = (aocw >> 8) & 15; if (d) R[d].a = ams;
      R[0] = float4(0, 0, 0, 0);
    }
    if (fin0 | fin1) {
      uint ff = fin1 & 0xFF; uint b;
      R[1] = fin_c0; R[2] = fin_c1;
      float3 va = (ff & 0x40) ? 1.0 - R[5].rgb : R[5].rgb;
      float3 vb = (ff & 0x20) ? 1.0 - R[12].rgb : R[12].rgb;
      R[14] = float4((ff & 0x80) ? saturate(va + vb) : va + vb, 0);
      b = fin1 >> 24; float3 E = map_in3((b & 0x10) ? R[b & 15].aaa : R[b & 15].rgb, b);
      b = fin1 >> 16; float3 F = map_in3((b & 0x10) ? R[b & 15].aaa : R[b & 15].rgb, b);
      R[15] = float4(E * F, 0);
      b = fin0 >> 24; float3 A = map_in3((b & 0x10) ? R[b & 15].aaa : R[b & 15].rgb, b);
      b = fin0 >> 16; float3 B = map_in3((b & 0x10) ? R[b & 15].aaa : R[b & 15].rgb, b);
      b = fin0 >> 8;  float3 C = map_in3((b & 0x10) ? R[b & 15].aaa : R[b & 15].rgb, b);
      b = fin0;       float3 D = map_in3((b & 0x10) ? R[b & 15].aaa : R[b & 15].rgb, b);
      b = fin1 >> 8;  float G = map_in((b & 0x10) ? R[b & 15].a : R[b & 15].b, b);
      o = float4(D + C * (1.0 - A) + B * A, G);
    } else o = R[12];
  }
  o = saturate(o);
  cov = 0xFFFFFFFFu;
/* Alpha test. On a multisampled target an inequality test becomes sample
 * coverage across the one-pixel band around its threshold (e = signed
 * distance past it, in the 8-bit units the test compares), so cut-out
 * edges -- foliage, fences -- are antialiased where they were before; the
 * colour and alpha written stay as they were. Single-sample: the plain test. */
  float a8 = o.a * 255.0, fw = max(fwidth(a8), 1e-3);
  if (ps_flags & 2) {
    int v = (int)(a8 + 0.5), r = (int)(alpha_ref & 0xFF); bool ok = true; float e = 0;
    switch (alpha_func) {
    case 0x200: ok = false; break; case 0x201: ok = v < r; e = r - 0.5 - a8; break;
    case 0x202: ok = v == r; break; case 0x203: ok = v <= r; e = r + 0.5 - a8; break;
    case 0x204: ok = v > r; e = a8 - (r + 0.5); break; case 0x205: ok = v != r; break;
    case 0x206: ok = v >= r; e = a8 - (r - 0.5); break; }
    uint ns = RT_SAMPLES;
    bool ineq = alpha_func == 0x201 || alpha_func == 0x203 || alpha_func == 0x204 || alpha_func == 0x206;
    if (ns > 1 && ineq) {
      uint k = (uint)round(saturate(e / fw + 0.5) * ns);
      if (k == 0) discard;
      cov = k >= 32 ? 0xFFFFFFFFu : (1u << k) - 1u;
    } else if (!ok) discard;
  }
  return o;
}
float4 ps_main(PSIn i, out uint cov : SV_Coverage) : SV_Target { return shade(i, cov); }
/* W-buffering (SET_CONTROL0 Z_PERSPECTIVE): the depth
 * buffer holds w, interpolated perspective-correctly (fog.y carries it), in
 * 24-bit fixed point -- linear precision, unlike z/w, so distant decals and
 * foliage do not fight. Polygon offset is applied here as xemu does: the
 * rasteriser's depth bias never reaches a shader-written depth. zp0 = offset
 * factor, units, host pixels per guest pixel x/y; zp1 = 1/(zmax+1), zmax,
 * reserved, reserved. */
void ps_wdepth(PSIn i, out float4 c : SV_Target, out float dep : SV_Depth, out uint cov : SV_Coverage) {
  c = shade(i, cov);
  float z = i.fog.y;
  z += zp0.y + zp0.x * max(abs(ddx(z)) * zp0.z, abs(ddy(z)) * zp0.w);
  z = clamp(z, 0.0, zp1.y);
  dep = asfloat(asuint(floor(z) * zp1.x) + 1u);
}
