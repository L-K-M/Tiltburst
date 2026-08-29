// Bloom bright pass (06-rendering.md §12.1): soft-knee threshold.
// THRESH = 1.0, KNEE = 0.6: below the knee quadratic, above linear.

Texture2D src : register(t0, space2);
SamplerState samp : register(s0, space2);

struct FSIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(FSIn i) : SV_Target
{
    const float3 c = src.Sample(samp, i.uv).rgb;
    const float br = max(c.r, max(c.g, c.b));
    // Soft knee: contribution = max(br - thresh + knee, 0) / (4*knee)
    // scaled so at br == thresh+knee it equals br - thresh.
    const float thresh = 1.0f;
    const float knee = 0.6f * thresh;
    const float soft = clamp(br - thresh + knee, 0.0f, 2.0f * knee);
    const float contribution = max(soft * soft / (4.0f * knee + 1e-6f), br - thresh);
    const float scale = br > 1e-6f ? contribution / br : 0.0f;
    return float4(c * scale, 1.0f);
}
