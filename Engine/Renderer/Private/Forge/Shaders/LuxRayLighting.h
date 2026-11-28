#ifndef LUX_RAY_LIGHTING_H
#define LUX_RAY_LIGHTING_H

#include "LuxRayVisibility.h"
#include "LuxRestirCommon.h"
#include "LuxRadianceCascadesLighting.h"

bool RayEffect(uint effect) {
    return (uint(gScene.rayOptions.x) & effect) != 0u;
}

float3 BiasRayOrigin(float3 position, float3 normal, float3 direction) {
    const float scale = max(abs(position.x), max(abs(position.y), abs(position.z)));
    const float bias = 0.002f + scale * 2.5e-7f;
    return position + normal * (dot(normal, direction) < 0.0f ? -bias : bias);
}

uint RestirReceiverIndex(float2 pixel, float3 position, float3 normal) {
    const uint2 size = uint2(gScene.frameSize.xy);
    const uint spacing = RestirSpacing(size);
    const uint2 grid = RestirGrid(size, spacing);
    const uint2 cell = min(uint2(pixel) / spacing, grid - 1u);
    const uint index = (cell.y * grid.x + cell.x) * LUX_RESTIR_OUTPUT_FLOATS4;
    const float4 receiver = gRestirLighting[index];
    const float3 receiverNormal = gRestirLighting[index + 1u].xyz;
    const float3 delta = receiver.xyz - position;
    const float footprint = max(length(gScene.right.xyz) * gScene.frameSize.z,
                                length(gScene.up.xyz) * gScene.frameSize.w) *
                            float(spacing) * receiver.w;
    // Reject inputs across occluders, on cutout/deformed triangle silhouettes,
    // and when the raster shading normal disagrees with the ray-hit geometry.
    if (receiver.w <= 0.0f || dot(receiverNormal, normal) < 0.9f ||
        abs(dot(delta, receiverNormal)) > max(0.008f, receiver.w * 0.0015f) ||
        length(delta) > max(0.012f, footprint * 4.0f))
        return 0xffffffffu;
    return index;
}

float3 RestirDirectLighting(float4 reservoir, float3 position, float3 base, float metallic,
                            float roughness, float3 normal, float3 view, float3 tangent,
                            float3 coatNormal, float thickness, float3 geometric,
                            float skinFraction) {
    if (reservoir.y <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);
    const uint light = uint(reservoir.x);
    const float3 lightPosition = gLights.pointPositionRadius[light].xyz;
    const float3 origin = BiasRayOrigin(position, normal, lightPosition - position);
    const float3 delta = lightPosition - origin;
    const float distance = length(delta);
    if (distance > 0.008f && RayOccluded(origin, delta / distance, max(distance - 0.005f, 0.0011f)))
        return float3(0.0f, 0.0f, 0.0f);
    const float4 source = gLights.pointPositionRadius[light],
                 radiance = gLights.pointColorIntensity[light];
    const float3 sourceDelta = source.xyz - position;
    const float attenuation = saturate(1.0f - length(sourceDelta) / max(source.w, .00001f));
    return PrincipledLighting(base, metallic, roughness, normal, view,
                              SafeDirection(sourceDelta, normal), tangent, coatNormal, thickness,
                              geometric, skinFraction) *
           radiance.rgb * (max(radiance.w, 0.0f) * attenuation * attenuation * reservoir.y);
}

float RayDirectionalShadow(float3 position, float3 normal) {
    const float visibility = DirectionalShadow(position, normal);
    const float3 towardSun = -gLights.direction.xyz;
    if (!RayEffect(1u) || visibility <= 0.0f || dot(normal, towardSun) <= 0.0f)
        return visibility;
    const float3 origin = BiasRayOrigin(position, normal, towardSun);
    return RayOccluded(origin, towardSun, gScene.rayOptions.y) ? 0.0f : visibility;
}

struct LuxRayHit {
    float distance;
    float3 normal;
    float3 base;
    float metal;
    float roughness;
    float ior;
    float transmission;
};

LuxRayHit TraceClosestSurface(float3 origin, float3 direction) {
    LuxRayHit surface;
    surface.distance = -1.0f;
    surface.normal = float3(0.0f, 1.0f, 0.0f);
    surface.base = float3(0.0f, 0.0f, 0.0f);
    surface.metal = 0.0f;
    surface.roughness = 1.0f;
    surface.ior = 1.0f;
    surface.transmission = 0.0f;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.001f;
    ray.TMax = gScene.rayOptions.y;
    RayQueryClosestHit(gRayScene, RAY_TRAVERSAL_FLAG_DEFAULT, RAY_FLAG_FORCE_OPAQUE, ray, 0x3u,
                       hit);
    RayQueryBeginForEachCandidate(hit) {
        RayQueryProceed(hit);
    }
    RayQueryEndForEachCandidate(hit) if (RayQueryIsHitTriangle(hit)) {
        const uint instance = 5u * RayQueryInstanceID(hit);
        const float4 material = gRayInstances[instance];
        const float4 optics = gRayInstances[instance + 1u];
        const uint primitive = asuint(optics.z) + RayQueryPrimitiveIndex(hit);
        const float4 surfaceData = gRayTriangles[primitive];
        const float3 local = surfaceData.xyz;
        const float3 geometricNormal = float3(dot(gRayInstances[instance + 2u].xyz, local),
                                              dot(gRayInstances[instance + 3u].xyz, local),
                                              dot(gRayInstances[instance + 4u].xyz, local));
        surface.distance = RayQueryRayT(hit);
        surface.normal = SafeDirection(geometricNormal, -direction);
        const uint packedColor = asuint(surfaceData.w);
        const uint3 vertexColor =
            (uint3(packedColor, packedColor, packedColor) >> uint3(0u, 10u, 20u)) & 1023u;
        surface.base = material.rgb * (float3(vertexColor) / 1023.0f);
        surface.metal = material.w;
        surface.roughness = optics.x;
        surface.ior = optics.y;
        surface.transmission = optics.w;
    }
    return surface;
}

float3 RayEnvironment(float3 direction, float roughness) {
    if (gScene.skyLighting.x > .5f)
        return SkySpecularLighting(direction, roughness) * gScene.environment.w;
    if (gScene.options.y > 0.5f) {
        return SampleLvlTexCube(gSpecularEnv, gSamplerTrilinearClamp, RotateEnvironment(direction),
                                roughness * 7.0f)
                   .rgb *
               gScene.environment.w;
    }
    return gScene.environment.rgb * gScene.environment.w;
}

float3 RayDiffuseEnvironment(float3 normal) {
    if (gScene.skyLighting.x > .5f)
        return SkyDiffuseLighting(normal) * gScene.environment.w;
    if (gScene.options.y > 0.5f) {
        return SampleLvlTexCube(gDiffuseEnv, gSamplerTrilinearClamp, RotateEnvironment(normal),
                                0.0f)
                   .rgb *
               gScene.environment.w;
    }
    return gScene.environment.rgb * gScene.environment.w;
}

float3 ShadeRayHit(float3 position, float3 normal, float3 view, LuxRayHit surface) {
    const float3 sun = gLights.colorIntensity.rgb * max(gLights.colorIntensity.w, 0.0f);
    const float roughness = clamp(surface.roughness, 0.045f, 1.0f);
    float3 radiance =
        Brdf(surface.base, surface.metal, roughness, normal, view, -gLights.direction.xyz) * sun *
        RayDirectionalShadow(position, normal);
    for (uint i = 0u; i < min(gLights.count.x, 16u); ++i) {
        const float4 light = gLights.pointPositionRadius[i];
        const float4 color = gLights.pointColorIntensity[i];
        const float3 delta = light.xyz - position;
        const float falloff = saturate(1.0f - length(delta) / max(light.w, 1e-5f));
        radiance += Brdf(surface.base, surface.metal, roughness, normal, view,
                         SafeDirection(delta, normal)) *
                    color.rgb * max(0.0f, color.w) * falloff * falloff;
    }
    return radiance;
}

// RGB contains incident radiance. W contains the first geometric hit distance or -1
// for a miss, allowing primary water refraction to retain the opaque-screen fallback.
float4 TraceRayPath(float3 origin, float3 direction, float roughness, uint bounces,
                    LuxRayHit surface) {
    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    float3 throughput = float3(1.0f, 1.0f, 1.0f);
    const float firstDistance = surface.distance;
    for (uint bounce = 0u; bounce < bounces; ++bounce) {
        if (surface.distance < 0.0f)
            break;
        const float3 hitPosition = origin + direction * surface.distance;
        const bool entering = dot(surface.normal, direction) < 0.0f;
        const float3 normal = entering ? surface.normal : -surface.normal;
        const float dielectric = (surface.ior - 1.0f) / (surface.ior + 1.0f);
        const float f0 = surface.ior > 1.0f ? dielectric * dielectric : 0.04f;
        const float3 baseFresnel = lerp(float3(f0, f0, f0), surface.base, surface.metal);
        const float3 fresnel = Fresnel(dot(-direction, normal), baseFresnel);
        const float3 diffuse = RayDiffuseEnvironment(normal) * surface.base *
                               (1.0f - surface.metal) * (1.0f - fresnel);
        const float3 direct = ShadeRayHit(hitPosition, normal, -direction, surface);
        radiance += throughput * (direct + diffuse) * (1.0f - surface.transmission);

        const float eta = entering ? rcp(surface.ior) : surface.ior;
        const float3 transmitted = refract(direction, normal, eta);
        const bool transmits =
            surface.transmission > 0.0f && dot(transmitted, transmitted) > 1e-10f;
        if (transmits) {
            direction = transmitted;
            throughput *= (1.0f - fresnel) * surface.transmission;
        } else {
            direction = reflect(direction, normal);
            if (surface.transmission <= 0.0f)
                throughput *= fresnel;
        }
        origin = BiasRayOrigin(hitPosition, normal, direction);
        roughness = max(roughness, surface.roughness);
        if (bounce + 1u < bounces)
            surface = TraceClosestSurface(origin, direction);
    }
    radiance += throughput * RayEnvironment(direction, roughness);
    return float4(radiance, firstDistance);
}

float4 TraceRayRadiance(float3 origin, float3 direction, float roughness, uint bounces) {
    return TraceRayPath(origin, direction, roughness, bounces,
                        TraceClosestSurface(origin, direction));
}

uint PixelRayHash(uint value) {
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

float3 DiffuseRay(float3 normal, uint seed) {
    const float u = float(PixelRayHash(seed) & 0xffffu) / 65536.0f;
    const float v = float(PixelRayHash(seed + 1u) & 0xffffu) / 65536.0f;
    const float radius = sqrt(u);
    const float angle = 6.28318531f * v;
    const float3 axis =
        abs(normal.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    const float3 tangent = normalize(cross(axis, normal));
    const float3 bitangent = cross(normal, tangent);
    return tangent * (radius * cos(angle)) + bitangent * (radius * sin(angle)) +
           normal * sqrt(1.0f - u);
}

float3 RayEnvironmentLighting(float3 base, float metal, float roughness, float3 normal, float3 view,
                              float3 position, float2 pixel, float4 restirGI, float4 denoisedSpec,
                              float4 screenHit, float3 bent, float3 diffuseVisibility,
                              float specularVisibility) {
    const float nv = saturate(dot(normal, view));
    const float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), base, metal);
    const float3 fresnel =
        f0 + (max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), f0) - f0) *
                 pow(1.0f - nv, 5.0f);
    const bool useIbl = gScene.options.y > 0.5f || gScene.skyLighting.x > 0.5f;
    const float3 diffuseWeight =
        (useIbl ? 1.0f - fresnel : float3(1.0f, 1.0f, 1.0f)) * (1.0f - metal) * base;
    float3 irradiance = RayDiffuseEnvironment(bent) * diffuseVisibility;
    const bool useCascades = RayEffect(64u);
    const bool traceDiffuse = RayEffect(8u);
    if (useCascades)
        irradiance = SampleRadianceCascades(position, normal, irradiance);
    if (restirGI.w > 0.0f) {
        irradiance = RayEffect(LUX_RESTIR_GI) ? restirGI.rgb
                                              : lerp(irradiance, restirGI.rgb, gScene.rayOptions.w);
    } else if (!useCascades && traceDiffuse && metal < 1.0f && gScene.rayOptions.w > 0.0f) {
        const uint seed = PixelRayHash(uint(pixel.x) + uint(pixel.y) * uint(gScene.frameSize.x));
        const float3 first = DiffuseRay(normal, seed);
        const float3 second = DiffuseRay(normal, seed ^ 0xa511e9b3u);
        const float3 origin = BiasRayOrigin(position, normal, first);
        const float3 a = TraceRayRadiance(origin, first, 1.0f, 1u).rgb;
        const float3 b = TraceRayRadiance(origin, second, 1.0f, 1u).rgb;
        irradiance = lerp(irradiance, (a + b) * 0.5f, gScene.rayOptions.w);
    }

    float3 reflected = float3(0.0f, 0.0f, 0.0f);
    const float3 direction = reflect(-view, normal);
    if (useIbl)
        reflected = RayEnvironment(direction, roughness) * specularVisibility;
    reflected = lerp(reflected, screenHit.rgb, screenHit.a);
    if (RayEffect(2u)) {
        const float weight = 1.0f - smoothstep(0.25f, 0.85f, roughness);
        if (weight > 0.0f && denoisedSpec.w > 0.0f) {
            reflected = lerp(reflected, denoisedSpec.rgb, weight);
        } else if (weight > 0.0f) {
            const float3 origin = BiasRayOrigin(position, normal, direction);
            const float4 reflectedRay =
                TraceRayRadiance(origin, direction, roughness, uint(gScene.rayOptions.z));
            reflected = lerp(reflected, reflectedRay.rgb, weight);
        }
    }
    float3 specularWeight = fresnel;
    if (useIbl) {
        const float2 brdf =
            SampleLvlTex2D(gBrdfLut, gSamplerTrilinearClamp, float2(nv, roughness), 0.0f).rg;
        specularWeight = f0 * brdf.x + brdf.y;
    }
    return diffuseWeight * irradiance + reflected * specularWeight;
}

#endif
