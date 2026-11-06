/* Test-owned, unshifted depth view for fixtures exercising the caller hook.
 * This is deliberately supplied by tests, never as a runtime fallback. */
#ifdef Y16_MS
Texture2DMS<float> TestDepth : register(t0);
#else
PS_TEX(Texture2D<float>, TestDepth, 0);
#endif
#ifdef SDLGPU
PS_SAMP(TestDepthS, 0);
#endif
float4 y16d_ps(float4 p : SV_Position) : SV_Target {
  uint2 q = uint2(p.xy) / uint2(2, 1);
#ifdef Y16_MS
  float d = TestDepth.Load(int2(q), 0);
#else
  float d = TestDepth.Load(int3(q, 0));
#endif
  uint word = (uint)round(saturate(d) * 16777215.0) >> 8;
  return float4(1, (word >> 8) / 255.0, (word & 255u) / 255.0, 1);
}
