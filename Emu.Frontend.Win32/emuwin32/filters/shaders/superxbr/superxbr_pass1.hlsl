#include "superxbr_common.hlsli"

Texture2D SourceTexture : register(t0); // Output of Pass 0
Texture2D OriginalTexture : register(t1); // Original Unscaled Image
// s2 is the engine's clamp-to-edge point sampler (s0 wraps, which would pull the opposite
// edge of the picture into the border pixels).
SamplerState PointSampler : register(s2);

cbuffer PassCb : register(b0)
{
    float2 OutputSize;   // this pass's render target, in pixels
    float2 TextureSize; // Size of Pass 0 output
};

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float2 fp = frac(uv * TextureSize);
    float2 dir = fp - float2(0.5, 0.5);
    
    if ((dir.x * dir.y) > 0.0)
    {
        return (fp.x > 0.5) ? SourceTexture.Sample(PointSampler, uv) : OriginalTexture.Sample(PointSampler, uv);
    }
    
    float2 g1 = (fp.x > 0.5) ? float2(0.5 / TextureSize.x, 0.0) : float2(0.0, 0.5 / TextureSize.y);
    float2 g2 = (fp.x > 0.5) ? float2(0.0, 0.5 / TextureSize.y) : float2(0.5 / TextureSize.x, 0.0);

    float3 P0 = OriginalTexture.Sample(PointSampler, uv - 3.0 * g1).xyz;
    float3 P1 = SourceTexture.Sample(PointSampler, uv - 3.0 * g2).xyz;
    float3 P2 = SourceTexture.Sample(PointSampler, uv + 3.0 * g2).xyz;
    float3 P3 = OriginalTexture.Sample(PointSampler, uv + 3.0 * g1).xyz;

    float3 B = SourceTexture.Sample(PointSampler, uv - 2.0 * g1 - g2).xyz;
    float3 C = OriginalTexture.Sample(PointSampler, uv - g1 - 2.0 * g2).xyz;
    float3 D = SourceTexture.Sample(PointSampler, uv - 2.0 * g1 + g2).xyz;
    float3 E = OriginalTexture.Sample(PointSampler, uv - g1).xyz;
    float3 F = SourceTexture.Sample(PointSampler, uv - g2).xyz;
    float3 G = OriginalTexture.Sample(PointSampler, uv - g1 + 2.0 * g2).xyz;
    float3 H = SourceTexture.Sample(PointSampler, uv + g2).xyz;
    float3 I = OriginalTexture.Sample(PointSampler, uv + g1).xyz;

    float3 F4 = OriginalTexture.Sample(PointSampler, uv + g1 - 2.0 * g2).xyz;
    float3 I4 = SourceTexture.Sample(PointSampler, uv + 2.0 * g1 - g2).xyz;
    float3 H5 = OriginalTexture.Sample(PointSampler, uv + g1 + 2.0 * g2).xyz;
    float3 I5 = SourceTexture.Sample(PointSampler, uv + 2.0 * g1 + g2).xyz;

    float b = RGBtoYUV(B);
    float c = RGBtoYUV(C);
    float d = RGBtoYUV(D);
    float e = RGBtoYUV(E);
    float f = RGBtoYUV(F);
    float g = RGBtoYUV(G);
    float h = RGBtoYUV(H);
    float i = RGBtoYUV(I);

    float i4 = RGBtoYUV(I4);
    float p0 = RGBtoYUV(P0);
    float i5 = RGBtoYUV(I5);
    float p1 = RGBtoYUV(P1);
    float h5 = RGBtoYUV(H5);
    float p2 = RGBtoYUV(P2);
    float f4 = RGBtoYUV(F4);
    float p3 = RGBtoYUV(P3);

    float wp1 = 8.0, wp2 = 0.0, wp3 = 0.0, wp4 = 0.0, wp5 = 0.0, wp6 = 0.0;
    float weight1 = (XBR_WEIGHT * 1.75068 / 10.0);
    float weight2 = (XBR_WEIGHT * 1.29633 / 10.0 / 2.0);

    float d_edge = d_wd(wp1, wp2, wp3, wp4, wp5, wp6, d, b, g, e, c, p2, h, f, p1, h5, i, f4, i5, i4) -
                    d_wd(wp1, wp2, wp3, wp4, wp5, wp6, c, f4, b, f, i4, p0, e, i, p3, d, h, i5, g, h5);
    float hv_edge = hv_wd(wp1, wp2, wp3, wp4, wp5, wp6, f, i, e, h, c, i5, b, h5) -
                    hv_wd(wp1, wp2, wp3, wp4, wp5, wp6, e, f, h, i, d, f4, g, i4);

    float limits = XBR_EDGE_STR + 0.000001;
    float edge_strength = smoothstep(0.0, limits, abs(d_edge));
    
    float4 w1 = float4(-weight1, weight1 + 0.5, weight1 + 0.5, -weight1);
    float4 w2 = float4(-weight2, weight2 + 0.25, weight2 + 0.25, -weight2);
    
    float3 c3 = w2.x * (D + G) + w2.y * (E + H) + w2.z * (F + I) + w2.w * (F4 + I4);
    float3 c4 = w2.x * (C + B) + w2.y * (F + E) + w2.z * (I + H) + w2.w * (I5 + H5);

    float3 c1 = w1.x * P2 + w1.y * H + w1.z * F + w1.w * P1;
    float3 c2 = w1.x * P0 + w1.y * E + w1.z * I + w1.w * P3;

    float3 color = lerp(lerp(c1, c2, step(0.0, d_edge)), lerp(c3, c4, step(0.0, hv_edge)), 1.0 - edge_strength);

    float3 min_sample = min4(E, F, H, I) + (1.0 - XBR_ANTI_RINGING) * lerp((P2 - H) * (F - P1), (P0 - E) * (I - P3), step(0.0, d_edge));
    float3 max_sample = max4(E, F, H, I) - (1.0 - XBR_ANTI_RINGING) * lerp((P2 - H) * (F - P1), (P0 - E) * (I - P3), step(0.0, d_edge));
    color = clamp(color, min_sample, max_sample);

    return float4(color, 1.0);
}