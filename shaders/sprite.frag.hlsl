// M1 overlay quad pipeline fragment stage: untextured flat tint.
// Colors arrive premultiplied; the swapchain/UI path writes authored
// sRGB values without conversion (06-rendering.md §5).

struct FSIn
{
    float4 pos : SV_Position;
    float4 tint : TEXCOORD0;
};

float4 main(FSIn frag) : SV_Target
{
    return frag.tint;
}
