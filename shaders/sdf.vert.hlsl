// SDF primitive pipeline vertex stage (06-rendering.md §8).
//
// Slot 0: unit quad strip, float2 corner in {-1,+1}^2.
// Slot 1: per-instance SdfInstance (128 bytes, eight float4 rows).

cbuffer ViewUniforms : register(b0, space1)
{
    float4x4 u_to_clip;   // space -> clip for this pass
    float2 u_target_px;   // current render target size
    float u_ppm;          // pixels per meter (0 in pixel-space passes)
    float u_time;         // seconds = snapshot.tick * 0.001
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 lp : TEXCOORD0;   // local meters within the instance quad
    nointerpolation float4 n0 : TEXCOORD1;
    nointerpolation float4 n1 : TEXCOORD2;
    nointerpolation float4 n2 : TEXCOORD3;
    nointerpolation float4 n3 : TEXCOORD4;
    nointerpolation float4 n4 : TEXCOORD5;
    nointerpolation float4 n5 : TEXCOORD6;
    nointerpolation float4 n6 : TEXCOORD7;
};

struct Inst
{
    float4 pos_half : TEXCOORD1; // cx, cy, hx, hy (quad half-extents incl pads)
    float4 rot_kind_p01 : TEXCOORD2;
    float4 p234_glowr : TEXCOORD3;
    float4 fill0 : TEXCOORD4;
    float4 fill1 : TEXCOORD5;
    float4 grad : TEXCOORD6;
    float4 stroke : TEXCOORD7;
    float4 glow : TEXCOORD8;
};

VSOut main(float2 corner : TEXCOORD0, Inst inst)
{
    VSOut o;

    const float c = cos(inst.rot_kind_p01.x);
    const float s = sin(inst.rot_kind_p01.x);
    const float2 lp = corner * inst.pos_half.zw;
    const float2 wp = inst.pos_half.xy + float2(lp.x * c - lp.y * s,
                                                lp.x * s + lp.y * c);

    o.pos = mul(u_to_clip, float4(wp, 0.0, 1.0));
    o.lp = lp;
    o.n0 = inst.rot_kind_p01;
    o.n1 = inst.p234_glowr;
    o.n2 = inst.fill0;
    o.n3 = inst.fill1;
    o.n4 = inst.grad;
    o.n5 = inst.stroke;
    o.n6 = inst.glow;

    return o;
}
