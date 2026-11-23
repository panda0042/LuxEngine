#ifndef LUX_RADIANCE_CASCADES_LIGHTING_H
#define LUX_RADIANCE_CASCADES_LIGHTING_H

#include "LuxRadianceCascadesParams.h"
#include "LuxRadianceCascadesSampling.h"

float3 SampleRadianceCascades(float3 position, float3 normal, float3 fallback) {
    const float4 lattice = gCascadeIrradiance[0];
    const uint probes = LUX_CASCADE_NEAR_PROBES;
    // Sample on the visible side of a surface. Fade to the ordinary IBL result at
    // the local probe volume's edge and outside its coverage.
    const float3 samplePosition = position + normal * (0.35f * lattice.w);
    const float3 coordinate = CascadeGridPosition(samplePosition, lattice.xyz, probes, lattice.w);
    const float3 border = min(coordinate, float(probes - 1u) - coordinate);
    const float coverage = saturate(min(border.x, min(border.y, border.z)));
    if (coverage <= 0.0f)
        return fallback;

    const uint3 lower = uint3(coordinate);
    const float3 fraction = coordinate - float3(lower);
    const float4 basis = float4(1.0f, normal);
    float3 result = float3(0.0f, 0.0f, 0.0f);
    for (uint corner = 0u; corner < 8u; ++corner) {
        const uint3 offset = CascadeCorner(corner);
        const uint index = LUX_CASCADE_SH_HEADER +
                           CascadeProbeIndex(lower + offset, probes) * LUX_CASCADE_SH_STRIDE;
        const float3 irradiance = float3(dot(gCascadeIrradiance[index], basis),
                                         dot(gCascadeIrradiance[index + 1u], basis),
                                         dot(gCascadeIrradiance[index + 2u], basis));
        result += irradiance * CascadeCornerWeight(offset, fraction);
    }
    return lerp(fallback, max(result, 0.0f), coverage * gCascadeIrradiance[1].x);
}

#endif
