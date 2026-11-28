#ifndef LUX_RESTIR_RESAMPLING_H
#define LUX_RESTIR_RESAMPLING_H

#include "LuxBrdf.h"
#include "LuxRestirCommon.h"
#include "LuxRayVisibility.h"

struct RestirSurface {
    float3 position;
    float3 normal;
    float3 base;
    float metallic;
    float roughness;
    float distance;
    uint instance;
};

struct RestirSample {
    // Finite samples use an area-measure PDF, sky samples use solid angle.
    float3 positionOrDirection;
    float3 normal;
    float3 radiance;
    float kind;
};

struct RestirDIReservoir {
    uint light;
    float weightSum;
    float target;
    float count;
};

struct RestirGIReservoir {
    RestirSample proposal;
    float weightSum;
    float target;
    float count;
};

RestirDIReservoir RestirEmptyDI() {
    RestirDIReservoir reservoir;
    reservoir.light = 0u;
    reservoir.weightSum = 0.0f;
    reservoir.target = 0.0f;
    reservoir.count = 0.0f;
    return reservoir;
}

RestirGIReservoir RestirEmptyGI() {
    RestirGIReservoir reservoir;
    reservoir.proposal.positionOrDirection = float3(0.0f, 0.0f, 0.0f);
    reservoir.proposal.normal = float3(0.0f, 0.0f, 0.0f);
    reservoir.proposal.radiance = float3(0.0f, 0.0f, 0.0f);
    reservoir.proposal.kind = 0.0f;
    reservoir.weightSum = 0.0f;
    reservoir.target = 0.0f;
    reservoir.count = 0.0f;
    return reservoir;
}

RestirSurface RestirTrace(float3 origin, float3 direction, float limit) {
    RestirSurface surface;
    surface.position = origin;
    surface.normal = -direction;
    surface.base = float3(0.0f, 0.0f, 0.0f);
    surface.metallic = 0.0f;
    surface.roughness = 1.0f;
    surface.distance = -1.0f;
    surface.instance = 0u;

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.002f;
    ray.TMax = limit;
    RayQueryClosestHit(gRayScene, RAY_TRAVERSAL_FLAG_DEFAULT, RAY_FLAG_FORCE_OPAQUE, ray, 0x1u,
                       hit);
    RayQueryBeginForEachCandidate(hit) {
        RayQueryProceed(hit);
    }
    RayQueryEndForEachCandidate(hit) if (RayQueryIsHitTriangle(hit)) {
        surface.instance = RayQueryInstanceID(hit);
        const uint materialIndex = 5u * surface.instance;
        const float4 material = gRayInstances[materialIndex];
        const float4 optics = gRayInstances[materialIndex + 1u];
        const uint primitive = asuint(optics.z) + RayQueryPrimitiveIndex(hit);
        const float3 local = gRayTriangles[primitive].xyz;
        const float3 geometric = float3(dot(gRayInstances[materialIndex + 2u].xyz, local),
                                        dot(gRayInstances[materialIndex + 3u].xyz, local),
                                        dot(gRayInstances[materialIndex + 4u].xyz, local));
        const float3 normal = SafeDirection(geometric, -direction);
        surface.normal = dot(normal, direction) <= 0.0f ? normal : -normal;
        surface.distance = RayQueryRayT(hit);
        surface.position = origin + direction * surface.distance;
        surface.base = material.rgb;
        surface.metallic = material.w;
        surface.roughness = clamp(optics.x, 0.045f, 1.0f);
    }
    return surface;
}

float RestirDITarget(uint light, RestirSurface surface) {
    const float3 view = SafeDirection(gScene.camera.xyz - surface.position, surface.normal);
    return RestirLuminance(RestirPointContribution(light, surface.position, surface.base,
                                                   surface.metallic, surface.roughness,
                                                   surface.normal, view));
}

float3 RestirGIContribution(RestirSample proposal, RestirSurface surface) {
    const float3 delta = proposal.positionOrDirection - surface.position;
    const float distanceSquared = dot(delta, delta);
    if (proposal.kind == 1.0f && distanceSquared <= 0.0001f)
        return float3(0.0f, 0.0f, 0.0f);
    const float3 direction =
        proposal.kind == 1.0f ? delta * rsqrt(distanceSquared) : proposal.positionOrDirection;
    const float jacobian =
        proposal.kind == 1.0f ? saturate(dot(proposal.normal, -direction)) / distanceSquared : 1.0f;
    // Hemi-integrated incoming radiance normalized by pi. The raster pass
    // supplies the primary receiver's albedo and Fresnel term.
    return proposal.radiance *
           (saturate(dot(surface.normal, direction)) * jacobian / 3.141592653589793f);
}

RestirDIReservoir RestirDIUpdate(RestirDIReservoir reservoir, uint light, float target,
                                 float weight, float count, float decision) {
    reservoir.weightSum += weight;
    reservoir.count += count;
    if (weight > 0.0f && decision * reservoir.weightSum < weight) {
        reservoir.light = light;
        reservoir.target = target;
    }
    return reservoir;
}

RestirGIReservoir RestirGIUpdate(RestirGIReservoir reservoir, RestirSample proposal, float target,
                                 float weight, float count, float decision) {
    reservoir.weightSum += weight;
    reservoir.count += count;
    if (weight > 0.0f && decision * reservoir.weightSum < weight) {
        reservoir.proposal = proposal;
        reservoir.target = target;
    }
    return reservoir;
}

#endif
