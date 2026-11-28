#ifndef LUX_RESTIR_COMMON_H
#define LUX_RESTIR_COMMON_H

#include "LuxRestirGrid.h"

uint RestirHash(uint value) {
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

float RestirRandom(uint seed) {
    return float(RestirHash(seed) >> 8u) / 16777216.0f;
}

float3 RestirPointContribution(uint index, float3 position, float3 base, float metallic,
                               float roughness, float3 normal, float3 view) {
    const float4 source = gLights.pointPositionRadius[index];
    const float4 color = gLights.pointColorIntensity[index];
    const float3 delta = source.xyz - position;
    const float distance = length(delta);
    const float falloff = saturate(1.0f - distance / max(source.w, 1e-5f));
    return Brdf(base, metallic, roughness, normal, view, SafeDirection(delta, normal)) * color.rgb *
           max(color.w, 0.0f) * falloff * falloff;
}

#endif
