// Present fragment: the §12.5 composite — scene + BLOOM_STRENGTH x
// bloom0, saturation clamp, optional CRT branch (13-art-direction.md
// §10 formulas; 06 §12.5 is the implementation owner), exact piecewise
// sRGB encode. The CRT branch is a UNIFORM branch (u_crt), never a
// shader permutation (§12.5 binding); u_crt == 0 skips it entirely.

Texture2D scene : register(t0, space2);
Texture2D bloom : register(t1, space2);
SamplerState samp : register(s0, space2);

cbuffer CompositeUniforms : register(b1, space3)
{
    float bloom_strength; // BLOOM_STRENGTH, default 0.6 (§12.5)
    float u_crt;          // 1.0 / 0.0 — render.crt
    float2 u_scene_px;    // (scene_w, scene_h) logical pixels
};

struct FSIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float3 srgb_encode(float3 c)
{
    float3 lo = 12.92f * c;
    float3 hi = 1.055f * pow(abs(c), 1.0f / 2.4f) - 0.055f;
    return lerp(hi, lo, step(c, 0.0031308f)); // per-channel select
}

float4 main(FSIn i) : SV_Target
{
    float3 c = scene.Sample(samp, i.uv).rgb
             + bloom_strength * bloom.Sample(samp, i.uv).rgb;
    // Saturation clamp: overflow desaturates toward white, no hue shift.
    float m = max(c.r, max(c.g, c.b));
    if (m > 1.0f)
        c = lerp(c / m, float3(1.0f, 1.0f, 1.0f), saturate((m - 1.0f) * 0.35f));
    c = clamp(c, 0.0f, 1.0f);
    // ---- optional CRT branch; skipped entirely when u_crt == 0 ----
    if (u_crt != 0.0f)
    {
        float2 spx = i.uv * u_scene_px;                    // scene px
        float scan = (fmod(floor(spx.y), 3.0f) < 1.0f) ? 1.0f : 0.0f;
        c *= 1.0f - 0.12f * scan;                           // 0.88x dark row
        float2 q = i.uv * 2.0f - 1.0f;                      // -1..1
        float r = length(q) * 0.70710678f;                  // 0 center, 1 corner
        c *= 1.0f - 0.15f * smoothstep(0.6f, 1.0f, r);      // vignette
    }
    return float4(srgb_encode(c), 1.0);
}
