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
        const float coverageWeight = saturate(log2(max(footprint, 1.0f)));
        const uint2 pixel = uint2(position) & 3u;
        const uint x = pixel.x ^ pixel.y;
        const uint bayer =
            ((x & 1u) << 3u) | ((pixel.y & 1u) << 2u) | (x & 2u) | ((pixel.y & 2u) >> 1u);
        const float threshold = (float(bayer) + 0.5f) / 16.0f;
        cutoff = lerp(cutoff, tintAlpha * threshold, coverageWeight);
    }
    return tintAlpha * alpha < cutoff;
}
#endif

// Port of Shaders/Scene3D.hlsli.
float3 RotateEnvironment(float3 v) {
    const float c = cos(gScene.options.x);
    const float s = sin(gScene.options.x);
    return float3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

float3 EnvironmentLighting(float3 base, float metal, float rough, float3 n, float3 v) {
    const float nv = saturate(dot(n, v));
    const float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), base, metal);
    if (gScene.options.y < 0.5f)
        return base * (1.0f - metal) * gScene.environment.rgb * gScene.environment.w;
    const float3 irradiance =
        SampleLvlTexCube(gDiffuseEnv, gSamplerTrilinearClamp, RotateEnvironment(n), 0.0f).rgb;
    const float3 reflected = SampleLvlTexCube(gSpecularEnv, gSamplerTrilinearClamp,
                                              RotateEnvironment(reflect(-v, n)), rough * 7.0f)
                                 .rgb;
    const float2 brdf =
        SampleLvlTex2D(gBrdfLut, gSamplerTrilinearClamp, float2(nv, rough), 0.0f).rg;
    const float3 f = f0 + (max(float3(1.0f - rough, 1.0f - rough, 1.0f - rough), f0) - f0) *
                              pow(1.0f - nv, 5.0f);
    return ((1.0f - f) * (1.0f - metal) * base * irradiance + reflected * (f0 * brdf.x + brdf.y)) *
           gScene.environment.w;
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
