// Bloom downsample (06-rendering.md §12.2): 13-tap box+4-tent.

Texture2D src : register(t0, space2);
SamplerState samp : register(s0, space2);

cbuffer DownUniforms : register(b1, space3)
{
    float2 u_texel; // 1/src_size
};

struct FSIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(FSIn i) : SV_Target
{
    // 13-tap (Jimenez): center + 4 corners + 4 edge pairs.
    float3 a = src.Sample(samp, i.uv).rgb;
    float3 b = src.Sample(samp, i.uv + float2(-1.0f, -1.0f) * u_texel).rgb;
    float3 c = src.Sample(samp, i.uv + float2(1.0f, -1.0f) * u_texel).rgb;
    float3 d = src.Sample(samp, i.uv + float2(-1.0f, 1.0f) * u_texel).rgb;
    float3 e = src.Sample(samp, i.uv + float2(1.0f, 1.0f) * u_texel).rgb;
    float3 f = src.Sample(samp, i.uv + float2(-2.0f, 0.0f) * u_texel).rgb;
    float3 g = src.Sample(samp, i.uv + float2(2.0f, 0.0f) * u_texel).rgb;
    float3 h = src.Sample(samp, i.uv + float2(0.0f, -2.0f) * u_texel).rgb;
    float3 k = src.Sample(samp, i.uv + float2(0.0f, 2.0f) * u_texel).rgb;
    float3 outc = a * 0.125f;
    outc += (b + c + d + e) * 0.0625f;
    outc += (f + g + h + k) * 0.03125f;
    outc *= 2.0f; // weights sum to 0.5; scale to 1.0
    return float4(outc, 1.0f);
}
