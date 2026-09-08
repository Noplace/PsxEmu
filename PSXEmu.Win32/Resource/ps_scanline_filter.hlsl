 // 2: CRT

// No output channel swizzle: this project's framebuffer texture is
// B8G8R8A8, matching Gpu::ResolveFramebuffer's own byte order already (see
// shaders/legacy_shaders.h for the same note - a swizzle here would just
// swap red and blue).
Texture2D g_Tex : register(t0);
SamplerState g_Linear : register(s1);
cbuffer Params : register(b0)
{
    float outW, outH, inW, inH;
};
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    float4 c = g_Tex.Sample(g_Linear, uv);
    float scanline = sin(uv.y * outH * 3.14159265 * 0.5) * 0.15 + 0.85;
    float2 dc = abs(uv - 0.5) * 2.0;
    float vignette = 1.0 - pow(max(dc.x, dc.y), 4.0) * 0.3;
    c.rgb *= scanline * vignette;
    return c;
}