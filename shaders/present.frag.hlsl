// Present fragment: sample the scene and apply the exact piecewise sRGB
// encode (06-rendering.md §5, §12.5 without bloom/CRT until M13).

Texture2D scene : register(t0, space2);
SamplerState samp : register(s0, space2);

struct FSIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float3 srgb_encode(float3 c)
{
    float3 lo = 12.92f * c;
    float3 hi = 1.055f * pow(c, 1.0f / 2.4f) - 0.055f;
    return lerp(hi, lo, step(c, 0.0031308f)); // per-channel select
}

float4 main(FSIn i) : SV_Target
{
    const float3 c = max(scene.Sample(samp, i.uv).rgb, 0.0f); // pow(neg) -> NaN
    return float4(srgb_encode(c), 1.0);
}
