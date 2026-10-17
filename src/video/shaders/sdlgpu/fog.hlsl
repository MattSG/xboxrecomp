/* SDL3 GPU compile unit: the dedicated host fog pass, as nv2a_distance_fog.h
 * runs it on D3D11 (single-sample there; SDL3 GPU targets always are). */
#include "../nv2a_bindings.hlsli"
cbuffer Fog : CBUF(0) { float4 band; float4 colour; float4 sky; };
PS_TEX(Texture2D<float>, Depth, 0);
PS_SAMP(DepthS, 0);
float4 fog_vs(uint id : SV_VertexID) : SV_Position {
  float2 p = float2((id << 1) & 2, id & 2);
  return float4(p * float2(2,-2) + float2(-1,1), 0, 1);
}
float4 fog_ps(float4 p : SV_Position) : SV_Target {
  float z = Depth.Load(int3(int2(p.xy), 0));
  if (colour.a > 0) {
    float a = z >= 0.99999 ? 1 : 0;
    if (sky.w > 0) {
      float t = saturate(dot(float3(p.xy,1),sky.xyz) * sky.w);
      a *= t*t*(3-2*t);
    }
    return float4(0,0,0,a);
  }
  if (z >= 0.99999) return float4(colour.rgb,band.w);
  float x = saturate((z * band.z - band.x) * band.y);
  return float4(colour.rgb, 1.0 - exp2(-6.643856 * x * x));
}
