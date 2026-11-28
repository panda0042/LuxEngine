#ifndef LUX_BOKEH_H
#define LUX_BOKEH_H

float EyeDepth(int2 pixel) {
    const float depth = LoadTex2D(gPostDepth, NO_SAMPLER, pixel, 0).r;
    return -gPost.camera.y / max(-gPost.camera.x - depth, 1e-7f);
}

// Uniform-area samples of a circular pupil or a fan of equal-area aperture blade triangles.
// The pattern is fixed in lens space to keep stationary highlights stable across frames.
float2 ApertureTap(uint index, uint count, float blades) {
    const float t = float(index) + 0.5f;
    const float radial = sqrt(t / float(count));
    const float turn = frac(t * 0.61803398875f);
    if (blades < 3.0f)
        return radial * float2(cos(6.2831853f * turn), sin(6.2831853f * turn));
    const float segment = turn * blades;
    const float arc = 6.2831853f / blades;
    const float a = floor(segment) * arc - 1.5707963f;
    return radial * lerp(float2(cos(a), sin(a)), float2(cos(a + arc), sin(a + arc)), frac(segment));
}

float BlurRadius(float depth, float focus) {
    // Thin-lens signed circle of confusion; negative denotes the near field.
    const float defocus = 1.0f - focus / max(depth, 1e-4f);
    const float radiusLimit = min(gPost.lens.z, 32.0f);
    if (gPost.cameraEffects.w > 0.0f)
        return clamp(gPost.cameraEffects.w * (gPost.lens.w - gPost.bloomFocus.z) /
                         max(focus - gPost.bloomFocus.z, .0001f) * defocus,
                     -radiusLimit, radiusLimit);
    return clamp(defocus, -1.0f, 1.0f) * radiusLimit;
}

float3 ApplyLens(int2 pixel, int2 upper, float4 center, float focus) {
    if (gPost.lens.z <= 0.0f)
        return center.rgb / center.a;
    const float z = EyeDepth(pixel);
    const float nearBias = max(0.025f * z, 0.02f);
    const float radius = abs(BlurRadius(z, focus));
    const float blades = gPost.cameraEffects.z;
    float3 sum = center.rgb;
    float weight = center.a;
    if (radius >= 0.55f) {
        for (uint i = 0; i < 36u; ++i) {
            const float2 p = clamp(float2(pixel) + radius * ApertureTap(i, 36u, blades),
                                   float2(0, 0), float2(upper));
            const float sampleZ = EyeDepth(int2(round(p)));
            const float4 tap = SampleLvlTex2D(gPostSource, gSamplerTrilinearClamp,
                                              (p + 0.5f) * gPost.dimensions.xy, 0.0f);
            // The HDR gather retains highlights above 1 until after optical integration.
            const float w = smoothstep(-nearBias, 0.0f, sampleZ - z);
            sum += tap.rgb * w;
            weight += tap.a * w;
        }
    }

    // An out-of-focus foreground light can spill over more distant geometry, even when
    // the background itself is in focus. Test the source's CoC at the aperture sample.
    float3 nearSum = float3(0, 0, 0);
    float nearCoverage = 0.0f;
    const float maxRadius = min(gPost.lens.z, 32.0f);
    if (maxRadius >= 0.55f) {
        for (uint i = 0; i < 12u; ++i) {
            const float2 unitTap = ApertureTap(i, 12u, blades);
            const float2 p =
                clamp(float2(pixel) + maxRadius * unitTap, float2(0, 0), float2(upper));
            const float sz = EyeDepth(int2(round(p)));
            if (sz >= z - nearBias)
                continue;
            const float nearRadius = -BlurRadius(sz, focus);
            // Radial sample position in normalized aperture space, also valid for a polygon.
            const float needed = sqrt((float(i) + 0.5f) / 12.0f) * maxRadius;
            const float w = smoothstep(needed - 0.5f, needed + 0.5f, nearRadius);
            const float4 tap = SampleLvlTex2D(gPostSource, gSamplerTrilinearClamp,
                                              (p + 0.5f) * gPost.dimensions.xy, 0.0f);
            nearSum += tap.rgb * w;
            nearCoverage += tap.a * w;
        }
    }
    const float3 background = sum / max(weight, 1e-5f);
    return lerp(background, nearSum / max(nearCoverage, 1e-5f), saturate(nearCoverage / 12.0f));
}
#endif
