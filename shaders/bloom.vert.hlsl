// Bloom/present shared vertex: fullscreen triangle-strip quad.

struct VSIn
{
    uint vid : SV_VertexID;
};

struct FSIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

static const float2 kQuad[4] = {
    float2(-1.0f, -1.0f), float2(1.0f, -1.0f),
    float2(-1.0f, 1.0f), float2(1.0f, 1.0f)
};

FSIn main(VSIn i)
{
    FSIn o;
    o.pos = float4(kQuad[i.vid & 3u], 0.0f, 1.0f);
    o.uv = (kQuad[i.vid & 3u] + 1.0f) * 0.5f;
    o.uv.y = 1.0f - o.uv.y; // texture v-down vs clip y-up
    return o;
}
