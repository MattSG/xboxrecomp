/* Synthetic caller converter: visualise depth, not packed Y16 emulation. */
#ifdef Y16_MS
Texture2DMS<float> TestDepth : register(t0);
#else
PS_TEX(Texture2D<float>, TestDepth, 0);
#endif
#ifdef SDLGPU
PS_SAMP(TestDepthS, 0);
#endif
float4 y16d_ps(float4 p : SV_Position) : SV_Target {
#ifdef Y16_MS
  float d = TestDepth.Load(int2(uint2(p.xy) / uint2(2,1)), 0);
#else
  float d = TestDepth.Load(int3(uint2(p.xy) / uint2(2,1), 0));
#endif
  return float4(1,d,0,1);
}
