// Present pass: samples the portrait scene target and draws it as the
// rotated letterbox quad (06-rendering.md §6.4 / §12.5 pass F, bloomless
// variant until M13). Corner positions AND uvs are precomputed CPU-side.

cbuffer ViewUniforms : register(b0, space1)
{
    float4x4 u_to_clip; // identity for pre-transformed NDC corners
    float2 u_target_px;
    float u_ppm;
    float u_time;
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

struct VIn
{
    float2 pos : TEXCOORD0; // swapchain NDC (letterbox rect corners)
    float2 uv : TEXCOORD1;  // scene texel uv (top-left origin)
};

VSOut main(VIn v)
{
    VSOut o;
    o.pos = mul(u_to_clip, float4(v.pos, 0.0, 1.0));
    o.uv = v.uv;
    return o;
}
