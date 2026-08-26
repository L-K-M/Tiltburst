// SDF primitive pipeline fragment stage (06-rendering.md §8.3–§8.4).
// Kinds: 0 CIRCLE, 1 RING, 2 RBOX, 3 CAPSULE, 4 ARC, 5 BALL.

struct FSIn
{
    float4 pos : SV_Position;
    float2 lp : TEXCOORD0;
    float4 n0 : TEXCOORD1; // rot, kind, p0, p1
    float4 n1 : TEXCOORD2; // p2, p3, p4, glow_radius
    float4 n2 : TEXCOORD3; // fill0 rgba (premultiplied linear)
    float4 n3 : TEXCOORD4; // fill1
    float4 n4 : TEXCOORD5; // grad: xy dir, z length|radius, w mode
    float4 n5 : TEXCOORD6; // stroke rgb, w = width (m); 0 = none
    float4 n6 : TEXCOORD7; // glow rgb, a = intensity [0,2]
};

float sd_circle(float2 p, float r)
{
    return length(p) - r;
}

float sd_ring(float2 p, float r, float ht)
{
    return abs(length(p) - r) - ht;
}

float sd_rbox(float2 p, float2 b, float cr)
{
    float2 q = abs(p) - b + cr;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - cr;
}

float sd_capsule(float2 p, float2 a, float2 b, float r)
{
    float2 pa = p - a, ba = b - a;
    float h = saturate(dot(pa, ba) / dot(ba, ba));
    return length(pa - ba * h) - r;
}

// Arc [a0,a1] CCW from +x.
float sd_arc(float2 p, float a0, float a1, float r, float ht)
{
    const float mid = 0.5 * (a0 + a1);
    const float ap = 0.5 * (a1 - a0);
    const float cs = cos(-mid), sn = sin(-mid);
    float2 q = float2(p.x * cs - p.y * sn, p.x * sn + p.y * cs);
    q.x = abs(q.x);
    const float2 sc = float2(sin(ap), cos(ap));
    float d = (sc.y * q.x > sc.x * q.y) ? length(q - sc * r)
                                        : abs(length(q) - r);
    return d - ht;
}

float sd_dispatch(float kind, float2 lp, float4 P01, float4 P234)
{
    if (kind == 0.0) return sd_circle(lp, P01.z);                       // CIRCLE
    if (kind == 1.0) return sd_ring(lp, P01.z, P01.w);                  // RING
    if (kind == 2.0) return sd_rbox(lp, P01.zw, P234.x);                // RBOX
    if (kind == 3.0)                                                    // CAPSULE
        return sd_capsule(lp, P01.zw, P234.xy, P234.z);
    if (kind == 4.0)                                                    // ARC
        return sd_arc(lp, P01.z, P01.w, P234.x, P234.y);
    return sd_circle(lp, P01.z);                                        // BALL
}

float4 eval_fill(float4 f0, float4 f1, float4 grad, float2 lp)
{
    if (grad.w == 0.0) return f0;
    float t = (grad.w == 1.0)
                  ? saturate(dot(lp, grad.xy) / grad.z + 0.5)
                  : saturate(length(lp) / grad.z);
    return lerp(f0, f1, t);
}

float4 main(FSIn i) : SV_Target
{
    const float kind = i.n0.y;
    const float4 P01 = i.n0;
    const float4 P234 = i.n1;

    const float d = sd_dispatch(kind, i.lp, P01, P234);
    const float aa = max(fwidth(d), 1e-6);
    const float cov = saturate(0.5 - d / aa);

    const float4 fill = eval_fill(i.n2, i.n3, i.n4, i.lp);

    const float stroke_w = i.n5.w;
    const float sc = (stroke_w > 0.0)
                         ? saturate(0.5 - (abs(d) - stroke_w * 0.5) / aa)
                         : 0.0;

    // Premultiplied output; glow rides RGB only so blending stays additive
    // where the body does not cover (06 §8.4).
    float4 col = fill * cov * (1.0 - sc) + float4(i.n5.rgb, 1.0) * sc;

    if (i.n1.w > 0.0) {
        const float g = exp(-max(d, 0.0) * 4.0 / i.n1.w);
        col.rgb += i.n6.rgb * i.n6.a * g;
    }

    col.a = max(col.a, cov * fill.a);
    return col;
}
