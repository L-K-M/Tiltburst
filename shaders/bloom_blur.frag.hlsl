// Bloom separable blur (06-rendering.md §12.3): 9-tap Gaussian,
// sigma = 2.0 source texels; direction via u_dir (texel step).

Texture2D src : register(t0, space2);
SamplerState samp : register(s0, space2);

cbuffer BlurUniforms : register(b1, space3)
{
    float2 u_dir; // texel step * axis
};

struct FSIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

static const float W[5] = { 0.227027f, 0.194595f, 0.121622f, 0.054054f, 0.016216f };

float4 main(FSIn i) : SV_Target
{
    float3 acc = src.Sample(samp, i.uv).rgb * W[0];
    for (int t = 1; t <= 4; ++t)
    {
        acc += src.Sample(samp, i.uv + u_dir * float(t)).rgb * W[t];
        acc += src.Sample(samp, i.uv - u_dir * float(t)).rgb * W[t];
    }
    return float4(acc, 1.0f);
}
