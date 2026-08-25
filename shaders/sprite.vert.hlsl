// M1 overlay quad pipeline: instanced, untextured, flat-tint quads
// (06-rendering.md §9 shape without the atlas path).
//
// Vertex buffer slot 0: unit quad strip, float2 corner in {-1,+1}^2.
// Vertex buffer slot 1: per-instance data (SDL_GPU_VERTEXINPUTRATE_INSTANCE).

cbuffer ViewUniforms : register(b0, space1)
{
    float4x4 u_to_clip;   // target px -> clip for this pass
    float2 u_target_px;   // current render target size in pixels
    float u_ppm;          // pixels per meter (0 in pixel-space passes)
    float u_time;         // seconds = snapshot.tick * 0.001 (never wall clock)
};

struct VSOut
{
    float4 pos : SV_Position;
    float4 tint : TEXCOORD0;
};

struct Inst
{
    float4 rect : TEXCOORD1;  // cx, cy, half_w, half_h — target px
    float4 color : TEXCOORD2; // premultiplied rgba
};

VSOut main(float2 corner : TEXCOORD0, Inst inst)
{
    VSOut o;

    float2 p = inst.rect.xy + corner * inst.rect.zw;
    o.pos = mul(u_to_clip, float4(p, 0.0, 1.0));
    o.tint = inst.color;

    return o;
}
