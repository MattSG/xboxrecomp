cbuffer UBC : CBUF(0) { uint4 at[16]; float4 DEF[16]; uint4 ub_info; float4 ub_map;
  uint4 ub_fogi; float4 ub_fogf; float4 ub_place; };
cbuffer UBP : CBUF(2) { uint4 PROG[136]; };
cbuffer UBK : CBUF(3) { float4 C[192]; };
ByteAddressBuffer VB : VS_BUFFER(0);
uint ldw(uint a) {
  uint b = a & ~3u, s = (a & 3u) * 8u, l = VB.Load(b);
  return s ? (l >> s) | (VB.Load(b + 4u) << (32u - s)) : l;
}
float s16(uint a) { return (float)((int)(ldw(a) << 16) >> 16); }
float4 fetch(uint i, uint vid) {
  uint4 d = at[i];
  if (d.w == 0u) return DEF[i];
  uint a = d.x + vid * d.y;
  float4 r = float4(0, 0, 0, 1);
  if (d.z == 0u) { uint c = ldw(a); r = float4((c >> 16) & 255u, (c >> 8) & 255u, c & 255u, c >> 24) / 255.0; }
  else if (d.z == 6u) { int u = (int)ldw(a);
    r = float4((float)((u << 21) >> 21) / 1023.0, (float)((u << 10) >> 21) / 1023.0, (float)(u >> 22) / 511.0, 1.0); }
  else if (d.z == 2u) { [unroll] for (uint k = 0; k < 4; k++) if (k < d.w) r[k] = asfloat(ldw(a + 4u * k)); }
  else if (d.z == 4u) { uint c = ldw(a); [unroll] for (uint k = 0; k < 4; k++) if (k < d.w) r[k] = (float)((c >> (8u * k)) & 255u) / 255.0; }
  else if (d.z == 1u) { [unroll] for (uint k = 0; k < 4; k++) if (k < d.w) r[k] = s16(a + 2u * k) / 32767.0; }
  else if (d.z == 5u) { [unroll] for (uint k = 0; k < 4; k++) if (k < d.w) r[k] = s16(a + 2u * k); }
  return r;
}
static float4 R[13];
static float4 V[16];
static float4 O[13];
/* A component by dynamic index, by selection: indexing a vector compiles to
 * a dot product with a one-hot vector, and inf * 0 there turns every
 * component of a register holding an infinity (rcp of 0) into NaN. */
float cmp4(float4 s, uint i) { return i == 0u ? s.x : (i == 1u ? s.y : (i == 2u ? s.z : s.w)); }
float4 rd(uint swz, uint neg, uint mux, uint reg, uint vi, uint ci) {
  float4 s = mux == 1u ? R[reg < 13u ? reg : 0u] : (mux == 2u ? V[vi] : C[ci]);
  float4 r = float4(cmp4(s, (swz >> 6) & 3u), cmp4(s, (swz >> 4) & 3u), cmp4(s, (swz >> 2) & 3u), cmp4(s, swz & 3u));
  return neg ? -r : r;
}
float4 msk(float4 d, float4 v, uint m) {
  return float4((m & 8u) ? v.x : d.x, (m & 4u) ? v.y : d.y, (m & 2u) ? v.z : d.z, (m & 1u) ? v.w : d.w);
}
float ub_fog(float d) {
  if (!ub_fogi.x) return 1.0;
  float f, px = ub_fogf.x, py = ub_fogf.y; uint mode = ub_fogi.y;
  if (mode == 0x800u || mode == 0x802u) f = px + exp2(d * py * 16.0) - 1.5;
  else if (mode == 0x801u || mode == 0x803u) f = px + exp2(-d * d * py * py * 32.0) - 1.5;
  else f = px + d * py - 1.0;
  if (mode == 0x802u || mode == 0x803u || mode == 0x804u) f = abs(f);
  if (isnan(f)) f = 1.0;
  return saturate(f);
}
PSIn ub_vs(uint vid : SV_VertexID) {
  PSIn o;
#if defined(__spirv__) && defined(STAGE_VS)
  o.psize = 1.0;
#endif
  float4 P, d0, d1, t0, t1, t2, t3;
  float fog = 1.0;
  if (ub_info.y) {
    [unroll] for (uint i = 0; i < 16; i++) V[i] = fetch(i, vid);
    [unroll] for (uint j = 0; j < 13; j++) { R[j] = float4(0, 0, 0, 0); O[j] = float4(0, 0, 0, 0); }
    O[3].w = 1; O[4].w = 1; O[9].w = 1; O[10].w = 1; O[11].w = 1; O[12].w = 1;
    int a0 = 0;
    [loop] for (uint s = ub_info.x; s < 136u; s++) {
      uint4 ins = PROG[s];
      uint mac = (ins.y >> 21) & 15u, ilu = (ins.y >> 25) & 7u, vi = (ins.y >> 9) & 15u;
      int ci = (int)((ins.y >> 13) & 255u) + (((ins.w >> 1) & 1u) ? a0 : 0);
      uint cu = (ci >= 0 && ci < 192) ? (uint)ci : 0u;
      float4 A = rd(ins.y & 255u, (ins.y >> 8) & 1u, (ins.z >> 26) & 3u, (ins.z >> 28) & 15u, vi, cu);
      float4 B = rd((ins.z >> 17) & 255u, (ins.z >> 25) & 1u, (ins.z >> 11) & 3u, (ins.z >> 13) & 15u, vi, cu);
      float4 Cc = rd((ins.z >> 2) & 255u, (ins.z >> 10) & 1u, (ins.w >> 28) & 3u, ((ins.z & 3u) << 2) | (ins.w >> 30), vi, cu);
      float4 mr = mac ? A : float4(0, 0, 0, 0), ir = ilu ? Cc : float4(0, 0, 0, 0);
      if (mac == 2u) mr = A * B;
      else if (mac == 3u) mr = A + Cc;
      else if (mac == 4u) mr = A * B + Cc;
      else if (mac == 5u) mr = dot(A.xyz, B.xyz).xxxx;
      else if (mac == 6u) mr = (dot(A.xyz, B.xyz) + B.w).xxxx;
      else if (mac == 7u) mr = dot(A, B).xxxx;
      else if (mac == 8u) mr = float4(1.0, A.y * B.y, A.z, B.w);
      else if (mac == 9u) mr = A < B ? A : B;
      else if (mac == 10u) mr = A >= B ? A : B;
      else if (mac == 11u) mr = A < B ? 1.0 : 0.0;
      else if (mac == 12u) mr = A >= B ? 1.0 : 0.0;
      else if (mac == 13u) a0 = (int)floor(A.x + 0.001);
      float x = Cc.x;
      if (ilu == 2u) ir = (x == 0.0 ? asfloat(0x7F800000u) : 1.0 / x).xxxx;
      else if (ilu == 3u) { float f = 1.0 / x; float af = clamp(abs(f), 5.42101e-20, 1.884467e19);
        ir = (f < 0 ? -af : af).xxxx; }
      else if (ilu == 4u) ir = (1.0 / sqrt(abs(x))).xxxx;
      else if (ilu == 5u) { float fl = floor(x); ir = float4(exp2(fl), x - fl, exp2(x), 1.0); }
      else if (ilu == 6u) { float ax = abs(x);
        if (ax == 0.0) ir = float4(-asfloat(0x7F800000u), 1.0, -asfloat(0x7F800000u), 1.0);
        else { float e = floor(log2(ax)); ir = float4(e, ax / exp2(e), log2(ax), 1.0); } }
      else if (ilu == 7u) { float nl = max(x, 0.0), nh = max(Cc.y, 0.0), p = clamp(Cc.w, -127.9961, 127.9961);
        ir = float4(1.0, nl, x > 0.0 ? (p == 0.0 ? 1.0 : pow(nh, p)) : 0.0, 1.0); }
      uint mm = (ins.w >> 24) & 15u, im = (ins.w >> 16) & 15u, td = (ins.w >> 20) & 15u;
      uint om = (ins.w >> 12) & 15u, oa = (ins.w >> 3) & 255u;
      /* As xemu: paired with an ILU op the MAC cannot write R1 (the ILU
       * owns it), and an output comes only from a unit that ran. */
      if (mac != 0u && mac != 13u && mm != 0u && td < 13u && !(ilu != 0u && td == 1u))
        R[td] = msk(R[td], mr, mm);
      if (ilu != 0u && im != 0u) { uint it = mac ? 1u : td; if (it < 13u) R[it] = msk(R[it], ir, im); }
      /* ponytail: writes to the constant file (out-to-c[]) are dropped;
       * a local copy of C[] would support them. */
      if (om != 0u && ((ins.w >> 11) & 1u) && (((ins.w >> 2) & 1u) ? ilu : mac) != 0u) {
        float4 ov = ((ins.w >> 2) & 1u) ? ir : mr;
        if (oa == 0u) R[12] = msk(R[12], ov, om);
        else if (oa < 13u) O[oa] = msk(O[oa], ov, om);
      }
      if (ins.w & 1u) break;
    }
    P = R[12]; d0 = O[3]; d1 = O[4]; fog = ub_fog(O[5].x);
    t0 = O[9]; t1 = O[10]; t2 = O[11]; t3 = O[12];
  } else {
    /* Pre-transformed: position (x, y, z, rhw), diffuse 3, specular 4,
     * texture coordinates 9-12. */
    float4 p0 = fetch(0, vid);
    P = float4(p0.xyz, at[0].w == 4u && p0.w > 0.0 ? 1.0 / p0.w : 1.0);
    d0 = fetch(3, vid); d1 = fetch(4, vid);
    t0 = fetch(9, vid); t1 = fetch(10, vid); t2 = fetch(11, vid); t3 = fetch(12, vid);
  }
  float px = P.x;
  if (ub_info.z != 0xFFFFFFFFu) px += asfloat(VB.Load(ub_info.z + vid * 4u));
/* The NV2A rasterises with 4-bit sub-pixel precision, truncating (xemu
 * roundScreenCoords), so the Xbox D3D screen offset 0.53125 puts a
 * full-target quad's left/top edge exactly on the centre of pixel 0, and the
 * top-left rule covers the whole pixel. Upscaled, that edge splits pixel 0's
 * block of host pixels and leaves its first half unwritten -- a wrapped
 * water ripple map then showed the unwritten strip as seams across the
 * water. A coordinate that truncates to that first centre is placed on the
 * target edge, as the hardware covers it; everything else is unchanged. */
  float py = P.y;
  if (ub_place.z != 0.0 && px >= 0.5 && px < 0.5625) px = 0.0;
  if (ub_place.z != 0.0 && py >= 0.5 && py < 0.5625) py = 0.0;
  if (ub_place.w != 0.0) {
    bool flat = abs(P.w - 1.0) < 1e-6;
    if (!flat || (frac(px) >= 0.5 && frac(px) < 0.5625)) px -= ub_place.w;
    if (!flat || (frac(py) >= 0.5 && frac(py) < 0.5625)) py -= ub_place.w;
  }
  /* NV2A clamps raster w away from zero and infinity (xemu clampAwayZeroInf),
   * keeping its sign so geometry behind the eye still clips. oPos is screen
   * space after the D3D epilogue's divide; multiplying back by w gives the
   * clip-space position, so the host clipper and perspective-correct
   * interpolation see what the NV2A saw. */
  float w = P.w;
  float aw = clamp(abs(w), 5.421011e-20, 1.8446744e19);
  w = w < 0 ? -aw : aw;
  o.pos = float4(((px * ub_map.x - 1.0) * ub_map.w + ub_place.x) * w,
                 (1.0 - py * ub_map.y) * ub_place.y * w, P.z * ub_map.z * w, w);
  o.d0 = saturate(d0); o.d1 = saturate(d1); o.fog = float4(fog, w, 0, 0);
  o.t0 = t0; o.t1 = t1; o.t2 = t2; o.t3 = t3;
  return o;
}
