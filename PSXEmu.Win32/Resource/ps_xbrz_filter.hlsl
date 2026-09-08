// No output channel swizzle: this project's framebuffer texture is
// B8G8R8A8, matching Gpu::ResolveFramebuffer's own byte order already (see
// shaders/legacy_shaders.h for the same note - a swizzle here would just
// swap red and blue).
Texture2D g_Tex : register(t0);
SamplerState g_Point : register(s0);

// Helper function: Calculates perceptual brightness (Luma) difference to detect pixel-art edges
float ColorDiff(float4 c1, float4 c2)
{
    // Standard YUV luma weights for accurate edge detection
    float3 weights = float3(0.299, 0.587, 0.114);
    return dot(abs(c1.rgb - c2.rgb), weights);
}

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    // 1. Automatically grab texture dimensions (No need for external constant buffers)
    uint width, height;
    g_Tex.GetDimensions(width, height);
    float2 texSize = float2((float) width, (float) height);
    float2 texel = 1.0 / texSize;

    // 2. Calculate integer pixel coordinates and fractional offsets
    float2 pixel_coord = uv * texSize;
    float2 center_pixel = floor(pixel_coord) + 0.5;
    float2 f = frac(pixel_coord); // Range [0.0 to 1.0] inside the current pixel

    float2 uv_center = center_pixel * texel;

    // 3. Sample the 3x3 neighborhood (Point sampling is optimal here)
    float4 c11 = g_Tex.Sample(g_Point, uv_center); // Center Pixel

    float4 c10 = g_Tex.Sample(g_Point, uv_center + float2(0.0, -texel.y)); // Top
    float4 c12 = g_Tex.Sample(g_Point, uv_center + float2(0.0, texel.y)); // Bottom
    float4 c01 = g_Tex.Sample(g_Point, uv_center + float2(-texel.x, 0.0)); // Left
    float4 c21 = g_Tex.Sample(g_Point, uv_center + float2(texel.x, 0.0)); // Right

    float4 c00 = g_Tex.Sample(g_Point, uv_center + float2(-texel.x, -texel.y)); // Top-Left
    float4 c20 = g_Tex.Sample(g_Point, uv_center + float2(texel.x, -texel.y)); // Top-Right
    float4 c02 = g_Tex.Sample(g_Point, uv_center + float2(-texel.x, texel.y)); // Bottom-Left
    float4 c22 = g_Tex.Sample(g_Point, uv_center + float2(texel.x, texel.y)); // Bottom-Right

    // 4. Determine which quadrant of the current pixel we are rendering in
    float2 quad = sign(f - 0.5); // Will be -1.0 or 1.0
    
    // Fetch the two orthogonal neighbors and the diagonal corner for the current quadrant
    float4 n1 = (quad.y < 0.0) ? c10 : c12; // Vertical neighbor
    float4 n2 = (quad.x < 0.0) ? c01 : c21; // Horizontal neighbor
    float4 corner = (quad.x < 0.0 && quad.y < 0.0) ? c00 :
                    (quad.x > 0.0 && quad.y < 0.0) ? c20 :
                    (quad.x < 0.0 && quad.y > 0.0) ? c02 : c22;

    float4 result = c11;

    // 5. Edge detection logic (xBR style)
    // If the adjacent neighbors are similar to each other, but different from the center,
    // it indicates a staircase edge that needs to be rounded off.
    float edge_diff = ColorDiff(n1, n2);
    
    // Tolerance values (0.15 and 0.1) can be tweaked to make the filter more or less aggressive
    if (edge_diff < 0.15 && ColorDiff(c11, n1) > 0.1 && ColorDiff(c11, n2) > 0.1 && ColorDiff(c11, corner) > 0.1)
    {
        // 6. Smooth the corner
        float2 dist_to_center = abs(f - 0.5);
        float diag_dist = dist_to_center.x + dist_to_center.y;
        
        // If we cross the diagonal threshold of the pixel corner, we blend it
        if (diag_dist > 0.5)
        {
            // Smoothstep creates that buttery-smooth curve associated with xBRZ
            float blend = smoothstep(0.5, 1.0, diag_dist);
            
            // Note: blending with n1 (or n2, since they are similar) creates the curve
            result = lerp(c11, n1, blend);
        }
    }

    return result;
}