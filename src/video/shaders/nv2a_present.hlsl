/* Presentation and uploads: a textured rectangle, optionally with the PVIDEO
 * overlay composited the way the display hardware does. */
cbuffer PC : CBUF(0) { float4 dst_rect; float4 src_uv; float4 src_size;
  float4 ovl_out; float4 ovl_walk; float4 ovl_in; uint4 ovl_flags; float4 force_alpha; };
PS_TEX(Texture2D, SrcTex, 0); PS_TEX(Texture2D, OvlTex, 1); PS_SAMP(LinS, 0);
struct PO { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
PO present_vs(uint id : SV_VertexID) {
  float2 c = float2(id & 1, id >> 1); PO o;
  o.pos = float4(lerp(dst_rect.x, dst_rect.z, c.x), lerp(dst_rect.y, dst_rect.w, c.y), 0, 1);
  o.uv = lerp(src_uv.xy, src_uv.zw, c); return o;
}
PS_SAMP(PointS, 1);
/* The source read point-sampled and the overlay read filtered: slots of their
 * own on SDL3 GPU (see nv2a_bindings.hlsli). */
#ifdef SDLGPU
PS_TEX(Texture2D, SrcTexP, 2); PS_SAMP(PointSP, 2); PS_TEX(Texture2D, OvlTexL, 3); PS_SAMP(LinSL, 3);
#else
#define SrcTexP SrcTex
#define PointSP PointS
#define OvlTexL OvlTex
#define LinSL LinS
#endif
float4 present_ps(PO i) : SV_Target {
  if (ovl_flags.w) {
/* Masked upload: only pixels the CPU changed replace what the GPU drew. */
    if (OvlTex.Sample(PointS, i.uv).r < 0.5) discard;
    return SrcTexP.Sample(PointSP, i.uv);
  }
  float4 c = SrcTex.Sample(LinS, i.uv);
  if (ovl_flags.x) {
    float2 g = i.uv * src_size.xy;
/* On a widened image the movie stays 4:3, centred (ovl_in.z = widening). */
    float2 d = float2(ovl_in.w + (g.x - ovl_in.w) * ovl_in.z, g.y) - ovl_out.xy;
    if (all(d >= 0) && all(d < ovl_out.zw)) {
      bool show = true;
      if (ovl_flags.y) {
        uint k = ovl_flags.z;
        float3 key = float3((k >> 16) & 255, (k >> 8) & 255, k & 255) / 255.0;
        float3 fb = SrcTex.Load(int3(floor(g) * src_size.zw / src_size.xy, 0)).rgb;
        show = all(abs(fb - key) < 1.5 / 255.0);
      }
      if (show) c = OvlTexL.Sample(LinSL, (ovl_walk.xy + d * ovl_walk.zw) / ovl_in.xy);
    }
  }
/* Loading (force_alpha.y > 0, a phase): a turning arc in the bottom-right
 * corner while the title has stopped flipping; .zw is the target's size. */
  if (force_alpha.y > 0) {
    float s = force_alpha.w / 720.0;
    float2 d = i.pos.xy - (force_alpha.zw - 40.0 * s);
    float ring = saturate(2.0 * s - abs(length(d) - 13.0 * s));
    float a = frac(atan2(d.y, d.x) / 6.2831853 - force_alpha.y);
    c.rgb = lerp(c.rgb, float3(0.91, 0.64, 0.24), ring * smoothstep(0.15, 1.0, a));
  }
  return force_alpha.x >= 0 ? float4(c.rgb, force_alpha.x) : c;
}
