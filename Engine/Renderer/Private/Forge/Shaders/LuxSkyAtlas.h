// Six cube faces with a one-texel skirt on each side. The skirt is evaluated
// beyond the face edge, so bilinear interpolation continues onto the next face.
float3 SkyFaceDirection(int face, float2 uv) {
    if (face < 2)
        return normalize(float3(face == 0 ? 1.0f : -1.0f, uv.y, uv.x));
    if (face < 4)
        return normalize(float3(uv.x, face == 2 ? 1.0f : -1.0f, uv.y));
    return normalize(float3(uv.x, uv.y, face == 4 ? 1.0f : -1.0f));
}

float2 SkyAtlasUV(float3 dir, float level, float rows) {
    const float3 a = abs(dir);
    float face;
    float2 uv;
    if (a.x >= max(a.y, a.z)) {
        face = dir.x > 0.0f ? 0.0f : 1.0f;
        uv = dir.zy / a.x;
    } else if (a.y >= a.z) {
        face = dir.y > 0.0f ? 2.0f : 3.0f;
        uv = dir.xz / a.y;
    } else {
        face = dir.z > 0.0f ? 4.0f : 5.0f;
        uv = dir.xy / a.z;
    }
    return (float2(face, level) * 34.0f + 17.0f + uv * 16.0f) / float2(204.0f, rows * 34.0f);
}

float SkyHarmonic(int k, float3 n) {
    if (k == 0)
        return .282095f;
    if (k == 1)
        return .488603f * n.y;
    if (k == 2)
        return .488603f * n.z;
    if (k == 3)
        return .488603f * n.x;
    if (k == 4)
        return 1.092548f * n.x * n.y;
    if (k == 5)
        return 1.092548f * n.y * n.z;
    if (k == 6)
        return .315392f * (3.0f * n.z * n.z - 1.0f);
    if (k == 7)
        return 1.092548f * n.x * n.z;
    return .546274f * (n.x * n.x - n.y * n.y);
}
