#ifndef LUX_MATERIAL_RELIEF_H
#define LUX_MATERIAL_RELIEF_H

// Read linear height with the original UV footprint throughout the ray march.
// The gradient sampler remains defined even when neighboring pixels hit different layers.
#define ReliefSample(TEX, UV, DX, DY)                                                              \
    (fmod(gDraw.materialOptions.z, 2.0f) > 0.5f                                                    \
         ? SampleGradTex2D(TEX, gMaterialRepeat, UV, DX, DY).r                                     \
         : SampleGradTex2D(TEX, gMaterialClamp, UV, DX, DY).r)

float2 MaterialHeights(float2 uv, float2 dx, float2 dy) {
    return float2(gDraw.relief.z > 0.5f ? ReliefSample(gHeightTexture, uv, dx, dy) : 0.5f,
                  gDraw.relief.w > 0.5f ? ReliefSample(gTerrainHeight, uv, dx, dy) : 0.5f);
}

float HeightBlendWeight(float authored, float2 heights) {
    // Endpoints stay pure. Only the overlap can expose a high point of the other material.
    return saturate(authored +
                    4.0f * authored * (1.0f - authored) * (heights.y - heights.x) * gDraw.relief.y);
}

float2 ParallaxUV(float2 uv, float3 world, float authored, float2 dx, float2 dy) {
    const float3 px = ddx(world), py = ddy(world);
    const float3 areaNormal = cross(px, py);
    const float area2 = dot(areaNormal, areaNormal);
    if (gDraw.relief.x <= 0.0f || area2 < 1e-20f)
        return uv;
    const float3 view = normalize(gLights.camera.xyz - world);
    const float viewCos = abs(dot(view, areaNormal * rsqrt(area2)));
    // The UV gradients per metre also handle mirrored or nonuniformly scaled meshes.
    const float2 screenStep =
        float2(dot(cross(py, areaNormal), view), dot(cross(areaNormal, px), view)) / area2;
    const float2 ray = -(dx * screenStep.x + dy * screenStep.y) *
                       (gDraw.relief.x / max(viewCos, 0.15f)) * smoothstep(0.03f, 0.18f, viewCos);
    const float footprint = max(length(dx), length(dy));
    // Subpixel travel cannot reveal features; fade its cost and aliasing with distance.
    const float travel = length(ray) / max(footprint, 1e-7f);
    if (travel < 0.25f)
        return uv;
    const float2 offset = ray * smoothstep(0.25f, 1.0f, travel);
    const int steps = int(lerp(24.0f, 8.0f, viewCos));
    const float stepDepth = 1.0f / float(steps);
    float previousGap = 0.0f;
    for (int i = 0; i <= steps; ++i) {
        const float depth = float(i) * stepDepth;
        const float2 heights = MaterialHeights(uv + offset * depth, dx, dy);
        const float gap =
            1.0f - lerp(heights.x, heights.y, HeightBlendWeight(authored, heights)) - depth;
        if (gap <= 0.0f) {
            const float hit = i == 0 ? 0.0f
                                     : depth - stepDepth +
                                           stepDepth * previousGap / max(previousGap - gap, 1e-5f);
            return uv + offset * hit;
        }
        previousGap = gap;
    }
    return uv + offset;
}
#endif
