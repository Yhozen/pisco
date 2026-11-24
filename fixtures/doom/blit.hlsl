// The frame Doom drew, as a texture, onto the display back buffer: one fullscreen triangle, point sampled at
// 1:1, so every back-buffer pixel is the frame's pixel. The root signature travels with the shaders.
#define RS "DescriptorTable(SRV(t0), visibility = SHADER_VISIBILITY_PIXEL), " \
           "StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_POINT, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP, visibility = SHADER_VISIBILITY_PIXEL)"
Texture2D<float4> frame_tex : register(t0);
SamplerState frame_smp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
[RootSignature(RS)]
VSOut vs_main(uint id : SV_VertexID) {
  VSOut o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
[RootSignature(RS)]
float4 ps_main(VSOut i) : SV_Target { return float4(frame_tex.Sample(frame_smp, i.uv).rgb, 1); }
