#ifndef LUX_SCENE_RESOURCES_H
#define LUX_SCENE_RESOURCES_H

#include "LuxStructs.h"
#include "LuxVirtualShadow.h"
#ifdef LUX_RAY_QUERY
#include "fsl_ext.h"
#include "LuxRayScene.srt.h"
#else
#include "LuxScene.srt.h"
#endif

#define SampleMaterial(texture, uv)                                                                \
    (gDraw.options.x > 0.5f           ? (fmod(gDraw.materialOptions.z, 2.0f) > 0.5f                \
                                             ? SampleTex2D(texture, gSamplerPointWrap, uv)         \
                                             : SampleTex2D(texture, gSamplerPointClamp, uv))       \
     : gDraw.materialOptions.z < 0.5f ? SampleTex2D(texture, gMaterialClamp, uv)                   \
     : gDraw.materialOptions.z < 1.5f ? SampleTex2D(texture, gMaterialRepeat, uv)                  \
     : gDraw.materialOptions.z < 2.5f ? SampleTex2D(texture, gMaterialAnisotropicClamp, uv)        \
                                      : SampleTex2D(texture, gMaterialAnisotropicRepeat, uv))

#if defined(STAGE_FRAG)
// Hash the leaf's texture-space footprint so overlapping foliage cards have independent coverage.
float MaskCoverageNoise(int2 cell) {
    uint value = uint(cell.x) * 0x9e3779b9u + uint(cell.y) * 0x85ebca6bu;
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    return (float(value >> 8u) + 0.5f) * (1.0f / 16777216.0f);
}

float FilteredMaskThreshold(float2 texel, float lod) {
    const float2 cell = texel * exp2(-floor(lod));
    const float t = frac(lod);
    const float sampleValue =
        lerp(MaskCoverageNoise(int2(floor(cell))), MaskCoverageNoise(int2(floor(cell * 0.5f))), t);
    // Restore a uniform threshold after cross-fading between the two footprint scales.
    const float a = min(t, 1.0f - t);
    if (a < 0.001f)
        return sampleValue;
    if (sampleValue < a)
        return sampleValue * sampleValue / (2.0f * a * (1.0f - a));
    if (sampleValue > 1.0f - a)
        return 1.0f - (1.0f - sampleValue) * (1.0f - sampleValue) / (2.0f * a * (1.0f - a));
    return (sampleValue - 0.5f * a) / (1.0f - a);
}

bool BelowAlphaCutoff(float alpha, float tintAlpha, float2 uv, float2 position) {
    float cutoff = gDraw.options.z;
    if (gDraw.textureOptions.z > 0.5f && gDraw.options.x < 0.5f) {
        const float2 dx = ddx(uv) * gDraw.textureOptions.xy;
        const float2 dy = ddy(uv) * gDraw.textureOptions.xy;
        const float major = max(length(dx), length(dy));
        float footprint = major;
        if (gDraw.materialOptions.z > 1.5f) {
            const float area = abs(dx.x * dy.y - dx.y * dy.x);
            footprint = max(major * 0.125f, area / max(major, 1e-6f));
        }
        const float lod = log2(max(footprint, 1.0f));
        const float coverageWeight = saturate(lod);
        const uint2 pixel = uint2(position) & 3u;
        const uint x = pixel.x ^ pixel.y;
        const uint bayer =
            ((x & 1u) << 3u) | ((pixel.y & 1u) << 2u) | (x & 2u) | ((pixel.y & 2u) >> 1u);
        const float threshold = gDraw.textureOptions.z > 1.5f && lod > 0.0f
                                    ? FilteredMaskThreshold(uv * gDraw.textureOptions.xy, lod)
                                    : (float(bayer) + 0.5f) / 16.0f;
        cutoff = lerp(cutoff, tintAlpha * threshold, coverageWeight);
    }
    // At a fully minified footprint the threshold is scaled by tintAlpha.
    // Even a fully covered source texel cannot pass if its vertex tint is below
    // the authored threshold (including exactly transparent vertices).
    return tintAlpha < gDraw.options.z || tintAlpha * alpha < cutoff;
}
#endif

// Port of Shaders/Scene3D.hlsli.
float3 RotateEnvironment(float3 v) {
    const float c = cos(gScene.options.x);
    const float s = sin(gScene.options.x);
    return float3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

float3 EnvironmentLighting(float3 base, float metal, float rough, float3 n, float3 v,
                           float4 screen) {
    const float nv = saturate(dot(n, v));
    const float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), base, metal);
    if (gScene.options.y < 0.5f)
        return base * (1.0f - metal) * gScene.environment.rgb * gScene.environment.w +
               screen.rgb * screen.a * f0;
    const float3 irradiance =
        SampleLvlTexCube(gDiffuseEnv, gSamplerTrilinearClamp, RotateEnvironment(n), 0.0f).rgb;
    const float3 reflected = SampleLvlTexCube(gSpecularEnv, gSamplerTrilinearClamp,
                                              RotateEnvironment(reflect(-v, n)), rough * 7.0f)
                                 .rgb;
    const float2 brdf =
        SampleLvlTex2D(gBrdfLut, gSamplerTrilinearClamp, float2(nv, rough), 0.0f).rg;
    const float3 f = f0 + (max(float3(1.0f - rough, 1.0f - rough, 1.0f - rough), f0) - f0) *
                              pow(1.0f - nv, 5.0f);
    return (1.0f - f) * (1.0f - metal) * base * irradiance * gScene.environment.w +
           lerp(reflected * gScene.environment.w, screen.rgb, screen.a) * (f0 * brdf.x + brdf.y);
}

float ShadowDepthAt(int2 pixel, int resolution) {
    const int2 virtualPixel = clamp(pixel, int2(0, 0), int2(resolution - 1, resolution - 1));
    const int2 atlas = ShadowAtlasPixel(virtualPixel, gScene.virtualShadowPages);
    if (atlas.x < 0)
        return 1.0f;
    return LoadTex2D(gShadowMap, NO_SAMPLER, atlas, 0).r;
}

// Deterministic equal-area disk taps. The separation of blocker and receiver in the
// orthographic light projection scales the physical penumbra of a directional disk.
float2 ShadowDiskTap(int i, int count) {
    const float r = sqrt((float(i) + 0.5f) / float(count));
    const float angle = float(i) * 2.39996323f;
    return float2(cos(angle), sin(angle)) * r;
}

float PercentageCloserSoftShadow(float2 texel, float receiverDepth, int resolution) {
    const float texelsPerDepth = gScreenLighting.options.w;
    const float search = clamp(receiverDepth * texelsPerDepth, 1.0f, 30.0f);
    const float centerDepth = ShadowDepthAt(int2(texel), resolution);
    float count = centerDepth < receiverDepth ? 1.0f : 0.0f;
    float sum = centerDepth * count;
    for (int i = 0; i < 16; ++i) {
        const float z = ShadowDepthAt(int2(texel + ShadowDiskTap(i, 16) * search), resolution);
        if (z < receiverDepth) {
            sum += z;
            count += 1.0f;
        }
    }
    if (count == 0.0f)
        return 1.0f;
    const float radius = clamp((receiverDepth - sum / count) * texelsPerDepth, 0.75f, 30.0f);
    float visibility = 0.0f;
    for (int j = 0; j < 24; ++j) {
        const float z = ShadowDepthAt(int2(texel + ShadowDiskTap(j, 24) * radius), resolution);
        visibility += z >= receiverDepth ? 1.0f : 0.0f;
    }
    return visibility / 24.0f;
}

float DirectionalShadow(float3 world, float3 normal) {
    if (gScene.shadowOptions.x < 0.5f)
        return 1.0f;
    const float4 clip =
        mul(gScene.shadowMatrix, float4(world + normal * gScene.shadowOptions.w, 1.0f));
    const float3 p = clip.xyz / clip.w;
    const float2 uv = float2(p.x * 0.5f + 0.5f, 0.5f - p.y * 0.5f);
    if (uv.x < 0.0f || uv.y < 0.0f || uv.x > 1.0f || uv.y > 1.0f || p.z < 0.0f || p.z > 1.0f)
        return 1.0f;
    const int resolution = int(round(1.0f / gScene.shadowOptions.y));
    const float2 texel = uv * float(resolution);
    if (gScreenLighting.options.w > 0.0f)
        return PercentageCloserSoftShadow(texel, p.z - gScene.shadowOptions.z, resolution);
    const int2 center = int2(texel);
    const float2 offset = texel - float2(center) - 0.5f;
    const bool soften = gScene.waterReflection.w >= 1.0f;
    float visibility = 0.0f;
    float kernelWeight = 0.0f;
    UNROLL for (int y = -1; y <= 1; ++y) {
        UNROLL for (int x = -1; x <= 1; ++x) {
            const int2 virtualPixel =
                clamp(center + int2(x, y), int2(0, 0), int2(resolution - 1, resolution - 1));
            const int2 atlasPixel = ShadowAtlasPixel(virtualPixel, gScene.virtualShadowPages);
            const float2 weight = 1.5f - abs(float2(x, y) - offset);
            const float tap = soften ? max(weight.x, 0.0f) * max(weight.y, 0.0f) : 1.0f;
            kernelWeight += tap;
            if (atlasPixel.x < 0)
                visibility += tap;
            else
                visibility += p.z - gScene.shadowOptions.z <=
                                      LoadTex2D(gShadowMap, NO_SAMPLER, atlasPixel, 0).r
                                  ? tap
                                  : 0.0f;
        }
    }
    return visibility / kernelWeight;
}

#endif
