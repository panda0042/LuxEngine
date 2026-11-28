#ifndef LUX_LOCAL_EXPOSURE_H
#define LUX_LOCAL_EXPOSURE_H

// A joint bilateral base layer in log radiance. Meter large, same-depth regions while
// suppressing cross-silhouette samples. Bounded offsets preserve contrast and highlights.
float LocalExposureEV(float2 uv, int2 pixel, int2 upper, float3 center, float globalEV) {
    const float strength = gPost.cameraEffects.y;
    if (strength <= 0.0f)
        return 0.0f;
    const float centerLog = log2(max(PostLuma(center), 1e-5f));
    const float z = max(EyeDepth(pixel), 1e-3f);
    float logSum = centerLog;
    float weights = 1.0f;
    for (uint i = 0; i < 16u; ++i) {
        const float angle = (float(i) + 0.5f) * 2.39996323f;
        const float distance = sqrt((float(i) + 0.5f) / 16.0f);
        const float2 d =
            float2(cos(angle), sin(angle)) * distance * (1.0f / (gPost.dimensions.y * 18.0f));
        const int2 p = clamp(int2(floor(uv / gPost.dimensions.xy + d)), int2(0, 0), upper);
        const float4 tap = LoadTex2D(gPostSource, NO_SAMPLER, p, 0);
        const float l = log2(max(PostLuma(tap.rgb / max(tap.a, 1e-5f)), 1e-5f));
        const float dz = abs(log2(max(EyeDepth(p), 1e-3f) / z));
        const float dl = l - centerLog;
        const float w = tap.a * exp2(-2.0f * distance * distance - 7.0f * dz - 0.28f * dl * dl);
        logSum += l * w;
        weights += w;
    }
    // Adapt the base layer only. 0.6 retains some large-area scene contrast at full strength.
    const float ev = 0.6f * (log2(0.18f) - globalEV - logSum / weights);
    return strength * clamp(ev, -1.5f, 1.5f);
}
#endif
