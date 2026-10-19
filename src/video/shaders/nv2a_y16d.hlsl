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
#ifdef Y16_MS
Texture2DMS<float> Y16Z : register(t0);
#else
PS_TEX(Texture2D<float>, Y16Z, 0);
#endif
#ifdef SDLGPU
PS_SAMP(Y16ZS, 0);
#endif
float4 y16d_ps(float4 p : SV_Position) : SV_Target {
  uint2 q = uint2(p.xy);
  int zx = max(0, (int)floor((q.x + 0.5) * 0.5 - ccol.z));
#ifdef Y16_MS
  float d = Y16Z.Load(int2(zx, q.y), 0);
#else
  float d = Y16Z.Load(int3(zx, q.y, 0));
#endif
  float s = ccol.x, k = ccol.y;
  d = saturate(d);
  if (s > 1.0 && d < 1.0) { float x = d * s; d = x <= k ? x : k + (1.0 - k) * (1.0 - exp((k - x) / (1.0 - k))); }
  uint word = (uint)round(saturate(d) * 16777215.0) >> 8;
  return float4(1, (word >> 8) / 255.0, (word & 255u) / 255.0, 1);
}
