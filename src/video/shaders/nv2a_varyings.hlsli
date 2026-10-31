/* FLATQ: nointerpolation for the flat-shaded pixel shader variant. Under
 * MSAA the rest interpolate at the centroid: an edge pixel's centre can lie
 * outside its triangle, and texcoords extrapolated there read past the tile
 * edge -- bright seams between ground tiles. */
#ifndef FLATQ
#define FLATQ centroid
#endif
struct PSIn { float4 pos:SV_Position; FLATQ float4 d0:COLOR0; FLATQ float4 d1:COLOR1;
  centroid float4 fog:FOG; centroid float4 t0:TEXCOORD0; centroid float4 t1:TEXCOORD1;
  centroid float4 t2:TEXCOORD2; centroid float4 t3:TEXCOORD3;
#if defined(__spirv__) && defined(STAGE_VS)
  /* Vulkan leaves a point's size undefined unless the vertex shader writes
   * it; D3D draws every point 1 pixel. */
  [[vk::builtin("PointSize")]] float psize : PSIZE;
#endif
};
