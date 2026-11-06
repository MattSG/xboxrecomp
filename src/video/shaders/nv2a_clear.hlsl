cbuffer CC : CBUF(0) { float4 ccol; float4 cz; };
float4 clear_vs(uint id : SV_VertexID) : SV_Position {
  float2 c = float2(id & 1, id >> 1); return float4(c.x * 2 - 1, 1 - c.y * 2, cz.x, 1);
}
float4 clear_ps() : SV_Target { return ccol; }
