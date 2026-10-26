/* A 32-bit colour surface read as LU_IMAGE_Y16: twice as wide, texel 2x the
 * low 16-bit word of pixel x (G:B), 2x+1 the high one (A:R), as R=1, G=high byte, B=low byte, A=1
 * (NV2A Y16 byte lanes; xemu ac72edfc, hardware texture-format tests). MSAA surfaces are read at one sample: averaging the bytes
 * of packed values (such as depth packed into G:B for fog) would corrupt them. */
float4 y16_vs(uint id : SV_VertexID) : SV_Position {
  float2 c = float2((id << 1) & 2, id & 2); return float4(c * float2(2, -2) + float2(-1, 1), 0, 1);
}
#ifdef Y16_MS
Texture2DMS<float4> Y16Src : register(t0);
#else
PS_TEX(Texture2D<float4>, Y16Src, 0);
#endif
#ifdef SDLGPU
PS_SAMP(Y16SrcS, 0);
#endif
float4 y16_ps(float4 p : SV_Position) : SV_Target {
  uint2 q = uint2(p.xy);
#ifdef Y16_MS
  float4 c = Y16Src.Load(int2(q.x >> 1, q.y), 0);
#else
  float4 c = Y16Src.Load(int3(q.x >> 1, q.y, 0));
#endif
  uint4 b = (uint4)round(saturate(c) * 255.0);
  uint word = (q.x & 1u) ? (b.a << 8 | b.r) : (b.g << 8 | b.b);
  return float4(1, (word >> 8) / 255.0, (word & 255u) / 255.0, 1);
}
