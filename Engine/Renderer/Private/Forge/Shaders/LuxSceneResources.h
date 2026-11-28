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

#define MaterialSampleGrad(tex, sampler, uv)                                                       \
    SampleGradTex2D(tex, sampler, uv, ddx(uv) * gScene.skyLighting.z,                              \
                    ddy(uv) * gScene.skyLighting.z)
#define SampleMaterial(texture, uv)                                                                 \
    (gDraw.options.x > 0.5f           ? (fmod(gDraw.materialOptions.z, 2.0f) > 0.5f                 \
                                             ? MaterialSampleGrad(texture, gSamplerPointWrap, uv)   \
                                             : MaterialSampleGrad(texture, gSamplerPointClamp, uv)) \
     : gDraw.materialOptions.z < 0.5f ? MaterialSampleGrad(texture, gMaterialClamp, uv)             \
     : gDraw.materialOptions.z < 1.5f ? MaterialSampleGrad(texture, gMaterialRepeat, uv)            \
     : gDraw.materialOptions.z < 2.5f                                                               \
         ? MaterialSampleGrad(texture, gMaterialAnisotropicClamp, uv)                               \
         : MaterialSampleGrad(texture, gMaterialAnisotropicRepeat, uv))

#if defined(STAGE_FRAG)
// Hash the leaf's texture-space footprint so overlapping foliage cards have independent coverage.
uint CoverageHash(int2 cell, uint salt) {
    uint value = uint(cell.x) * 0x9e3779b9u + uint(cell.y) * 0x85ebca6bu + salt * 0xc2b2ae35u;
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

float MaskCoverageNoise(int2 cell, uint level) {
    const uint value = CoverageHash(cell, uint(gDraw.previousWind.w) ^ (level * 0x27d4eb2du));
    return (float(value >> 8u) + 0.5f) * (1.0f / 16777216.0f);
}

float FilteredMaskThreshold(float2 texel, float lod) {
    const float2 cell = texel * exp2(-floor(lod));
    const float t = frac(lod);
    const float sampleValue =
        lerp(MaskCoverageNoise(int2(floor(cell)), uint(floor(lod))),
             MaskCoverageNoise(int2(floor(cell * 0.5f)), uint(floor(lod)) + 1u), t);
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

// Toroidal blue-noise rank tile, optimized across all 64 coverage levels.
// Per-placement and per-block rotations break up repeated patterns on adjacent cards.
float SpatialMaskThreshold(int2 pixel, uint seed) {
    const uint salt = CoverageHash(pixel >> 3, seed);
    const uint2 cell = (uint2(pixel) + uint2(salt, salt >> 8u)) & 7u;
    const uint4 packed = cell.y < 2u   ? uint4(0xa3c2a9u, 0x1709b6u, 0x35ecdau, 0xf52018u)
                         : cell.y < 4u ? uint4(0xb55060u, 0x32c8b9u, 0x1bfaf4u, 0x5fa3ddu)
                         : cell.y < 6u ? uint4(0x523408u, 0x903bb5u, 0x2786fbu, 0xbdc3a5u)
                                       : uint4(0xca70a1u, 0x2eaf93u, 0x116c51u, 0xdd91dfu);
    const uint word = packed[(cell.y & 1u) * 2u + (cell.x >> 2u)];
    return (float((word >> (6u * (cell.x & 3u))) & 63u) + .5f) / 64.0f;
}

bool BelowLodCoverage(float2 position) {
    const float transition = gDraw.lod.x;
    if (transition == 0.0f)
        return false;
    const float sampleValue = SpatialMaskThreshold(int2(position), 0u);
    return transition > 0.0f ? sampleValue < transition : sampleValue >= -transition;
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
            footprint = max(major / max(1.0f, gScene.skyLighting.w), area / max(major, 1e-6f));
        }
        const float lod = log2(max(footprint * gScene.skyLighting.z, 1.0f));
        const float coverageWeight = saturate(lod);
        const float threshold =
            gDraw.textureOptions.z > 1.5f && lod > 0.0f
                ? FilteredMaskThreshold(uv * gDraw.textureOptions.xy, lod)
                : SpatialMaskThreshold(int2(position), uint(gDraw.previousWind.w));
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

#include "LuxSkyAtlas.h"

#define LUX_SKY_GLOSS gSkyGloss
#define LUX_SKY_SH gSkySH
#include "LuxSkyLighting.h"
#undef LUX_SKY_GLOSS
#undef LUX_SKY_SH

#define LUX_CLOUD_TEXTURE gCloudShadow
#include "LuxCloudVisibility.h"
#undef LUX_CLOUD_TEXTURE

float CloudVisibility(float3 relativePosition) {
    return CloudShadowTransmittance(relativePosition, -gLights.direction.xyz, gScene.skyLighting.y,
                                    gScene.atmosphere.z, gScene.atmosphere.w);
}

float3 MaterialDielectricF0() {
    if (gDraw.lobes.x < .5f)
        return float3(.04f, .04f, .04f);
    const float term = (gDraw.optics.y - 1.0f) / (gDraw.optics.y + 1.0f);
    return term * term * gDraw.specular.rgb;
}

float3 EnvironmentLighting(float3 base, float metal, float rough, float3 n, float3 v, float4 screen,
                           float3 bent, float3 diffuseVisibility, float specularVisibility) {
    const float nv = saturate(dot(n, v));
    const float3 f0 = lerp(MaterialDielectricF0(), base, metal);
    const bool dynamicSky = gScene.skyLighting.x > 0.5f;
    if (!dynamicSky && gScene.options.y < 0.5f)
        return diffuseVisibility * base * (1.0f - metal) * gScene.environment.rgb *
                   gScene.environment.w +
               screen.rgb * screen.a * f0;
    float3 irradiance = dynamicSky ? SkyDiffuseLighting(bent)
                                   : SampleLvlTexCube(gDiffuseEnv, gSamplerTrilinearClamp,
                                                      RotateEnvironment(bent), 0.0f)
                                         .rgb;
    if (int(gDraw.lobes.x) == 2) {
        const float3 backside = dynamicSky ? SkyDiffuseLighting(-bent)
                                           : SampleLvlTexCube(gDiffuseEnv, gSamplerTrilinearClamp,
                                                              RotateEnvironment(-bent), 0)
                                                 .rgb;
        const float3 transmission = exp(
            -gDraw.textureOptions.w / max(gDraw.foliage.yzw, float3(.000001f, .000001f, .000001f)));
        irradiance = lerp(irradiance, backside, .5f * gDraw.optics.z * transmission);
    }
    const float3 reflected = dynamicSky
                                 ? SkySpecularLighting(reflect(-v, n), rough)
                                 : SampleLvlTexCube(gSpecularEnv, gSamplerTrilinearClamp,
                                                    RotateEnvironment(reflect(-v, n)), rough * 7.0f)
                                       .rgb;
    const float2 brdf =
        SampleLvlTex2D(gBrdfLut, gSamplerTrilinearClamp, float2(nv, rough), 0.0f).rg;
    const float3 f = f0 + (max(float3(1.0f - rough, 1.0f - rough, 1.0f - rough), f0) - f0) *
                              pow(1.0f - nv, 5.0f);
    // Recover microfacet multiple scattering from the directional albedo of the split-sum LUT.
    const float3 energy = gDraw.lobes.x > 0 ? 1.0f + f0 * (1.0f / max(.20f, brdf.x + brdf.y) - 1.0f)
                                            : float3(1, 1, 1);
    return diffuseVisibility * (1.0f - f) * (1.0f - metal) * base * irradiance *
               gScene.environment.w +
           lerp(reflected * (gScene.environment.w * specularVisibility), screen.rgb, screen.a) *
               (f0 * brdf.x + brdf.y) * energy;
}

#if defined(STAGE_FRAG)
float ShadowDepthAt(int2 pixel, int resolution, int cascade) {
    const int2 local = clamp(pixel, int2(0, 0), int2(resolution - 1, resolution - 1));
    int2 atlas =
        cascade < 0 || cascade == 3 ? ShadowAtlasPixel(local, gScene.virtualShadowPages) : local;
    if (atlas.x < 0)
        return 1.0f;
    if (cascade >= 0)
        atlas += int2(cascade & 1, cascade >> 1) * resolution;
    return LoadTex2D(gShadowMap, NO_SAMPLER, atlas, 0).r;
}

float2 ShadowDiskTap(int i, int count) {
    const float r = sqrt((float(i) + 0.5f) / float(count));
    const float angle = float(i) * 2.39996323f;
    return float2(cos(angle), sin(angle)) * r;
}

// Compare against the local receiver plane at the actual shadow texel center.
float ShadowSeparation(float2 texel, float2 offset, float depth, float2 gradient, int resolution,
                       int cascade) {
    const int2 tap = int2(floor(texel + offset));
    return depth + dot(gradient, float2(tap) + 0.5f - texel) -
           ShadowDepthAt(tap, resolution, cascade);
}

float FilterShadow(float3 world, float3 normal, float3 outer, float2 outerGradient, int cascade) {
    const float4 crop = cascade < 0 ? float4(1, 0, 0, 0) : gScene.shadowCascades[max(cascade, 0)];
    const int resolution = int(round(1.0f / gScene.shadowOptions.y));
    const float texelWorld = gScene.shadowMetrics.x / crop.x;
    const float texelDepth = texelWorld * gScene.shadowMetrics.y;
    const float cosine = saturate(dot(normal, -gLights.direction.xyz));
    const float sine = sqrt(saturate(1.0f - cosine * cosine));
    const float normalBias = min(gScene.shadowOptions.w, texelWorld * 0.8f) * sine;
    const float3 bias = mul(gScene.shadowMatrix, float4(normal * normalBias, 0.0f)).xyz;
    const float3 p = outer + bias;
    const float2 uv = (p.xy * crop.x + crop.yz) * float2(0.5f, -0.5f) + 0.5f;
    if (uv.x < 0.0f || uv.y < 0.0f || uv.x >= 1.0f || uv.y >= 1.0f || p.z < 0.0f || p.z > 1.0f)
        return 1.0f;
    const float2 texel = uv * float(resolution);
    const float2 gradient = clamp(outerGradient / crop.x, -4.0f * texelDepth, 4.0f * texelDepth);
    const float receiver = p.z - min(gScene.shadowOptions.z, texelDepth * (0.18f + 0.7f * sine));
    const float penumbraScale = gScreenLighting.options.w * crop.x;
    if (penumbraScale > 0.0f) {
        const float search = clamp(receiver * penumbraScale, 1.0f, 40.0f);
        float count = 0.0f, sum = 0.0f;
        for (int i = 0; i < 17; ++i) {
            const float2 offset = i == 0 ? float2(0, 0) : ShadowDiskTap(i - 1, 16) * search;
            const float gap =
                ShadowSeparation(texel, offset, receiver, gradient, resolution, cascade);
            if (gap > 0.0f) {
                sum += gap;
                count += 1.0f;
            }
        }
        if (count == 0.0f)
            return 1.0f;
        const float radius = clamp(sum / count * penumbraScale, 0.85f, 40.0f);
        float visibility = 0.0f;
        for (int j = 0; j < 24; ++j) {
            const float gap = ShadowSeparation(texel, ShadowDiskTap(j, 24) * radius, receiver,
                                               gradient, resolution, cascade);
            visibility += gap <= 0.0f ? 1.0f : 0.0f;
        }
        return visibility / 24.0f;
    }
    float visibility = 0.0f, total = 0.0f;
    UNROLL for (int y = -1; y <= 1; ++y) {
        UNROLL for (int x = -1; x <= 1; ++x) {
            const float2 offset = float2(x, y);
            const float2 w = max(1.5f - abs(floor(texel) + offset + 0.5f - texel), 0.0f);
            const float tap = w.x * w.y;
            total += tap;
            visibility +=
                ShadowSeparation(texel, offset, receiver, gradient, resolution, cascade) <= 0.0f
                    ? tap
                    : 0.0f;
        }
    }
    return visibility / total;
}

float DirectionalShadow(float3 world, float3 normal) {
    if (gScene.shadowOptions.x < 0.5f)
        return 1.0f;
    const float4 clip = mul(gScene.shadowMatrix, float4(world, 1.0f));
    const float3 outer = clip.xyz / clip.w;
    const float3 dx = ddx(outer), dy = ddy(outer);
    const float det = dx.x * dy.y - dx.y * dy.x;
    const float2 dz = abs(det) > 1e-16f
                          ? float2(dx.z * dy.y - dy.z * dx.y, dy.z * dx.x - dx.z * dy.x) / det
                          : float2(0, 0);
    const float2 gradient = dz * float2(2.0f, -2.0f) * gScene.shadowOptions.y;
    if (gScene.shadowCascades[3].w <= 0.0f)
        return FilterShadow(world, normal, outer, gradient, -1);
    const float depth = dot(world - gLights.camera.xyz, gScene.forward.xyz);
    const int level = depth < gScene.shadowCascades[0].w   ? 0
                      : depth < gScene.shadowCascades[1].w ? 1
                      : depth < gScene.shadowCascades[2].w ? 2
                                                           : 3;
    const float farDepth = gScene.shadowCascades[level].w;
    if (depth >= farDepth)
        return 1.0f;
    const float nearDepth = level == 0 ? 0.0f : gScene.shadowCascades[max(level - 1, 0)].w;
    const float transition = smoothstep(lerp(nearDepth, farDepth, 0.85f), farDepth, depth);
    const float current = FilterShadow(world, normal, outer, gradient, level);
    if (transition <= 0.0f)
        return current;
    const float next = level < 3 ? FilterShadow(world, normal, outer, gradient, level + 1) : 1.0f;
    return lerp(current, next, transition);
}
#endif

#endif
