// Settings
#define XBR_EDGE_STR 0.6
#define XBR_WEIGHT 1.0
#define XBR_ANTI_RINGING 1.0
#define MODE 0.0
#define XBR_EDGE_SHP 0.4
#define XBR_TEXTURE_SHP 1.0

static const float3 Y_VEC = float3(0.2126, 0.7152, 0.0722);

float RGBtoYUV(float3 color)
{
    return dot(color, Y_VEC);
}

float df(float A, float B)
{
    return abs(A - B);
}

float d_wd(float wp1, float wp2, float wp3, float wp4, float wp5, float wp6,
           float b0, float b1, float c0, float c1, float c2, float d0,
           float d1, float d2, float d3, float e1, float e2, float e3, float f2, float f3)
{
    return (wp1 * (df(c1, c2) + df(c1, c0) + df(e2, e1) + df(e2, e3)) +
            wp2 * (df(d2, d3) + df(d0, d1)) +
            wp3 * (df(d1, d3) + df(d0, d2)) +
            wp4 * df(d1, d2) +
            wp5 * (df(c0, c2) + df(e1, e3)) +
            wp6 * (df(b0, b1) + df(f2, f3)));
}

float hv_wd(float wp1, float wp2, float wp3, float wp4, float wp5, float wp6,
            float i1, float i2, float i3, float i4, float e1, float e2, float e3, float e4)
{
    return (wp4 * (df(i1, i2) + df(i3, i4)) +
            wp1 * (df(i1, e1) + df(i2, e2) + df(i3, e3) + df(i4, e4)) +
            wp3 * (df(i1, e2) + df(i3, e4) + df(e1, i2) + df(e3, i4)));
}

float3 min4(float3 a, float3 b, float3 c, float3 d)
{
    return min(a, min(b, min(c, d)));
}

float3 max4(float3 a, float3 b, float3 c, float3 d)
{
    return max(a, max(b, max(c, d)));
}

float max4float(float a, float b, float c, float d)
{
    return max(a, max(b, max(c, d)));
}