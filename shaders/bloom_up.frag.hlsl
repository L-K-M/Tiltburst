// Bloom upsample (06-rendering.md §12.4): bilinear + additive into the
// destination via premultiplied blend; the shader itself just samples.

Texture2D src : register(t0, space2);
SamplerState samp : register(s0, space2);

struct FSIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(FSIn i) : SV_Target
{
    return src.Sample(samp, i.uv);
}
