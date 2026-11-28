#ifndef LUX_RESTIR_COMMON_H
#define LUX_RESTIR_COMMON_H

// Match the CPU dispatch in LuxRestir.inl. Two-pixel spacing also limits primary
// ray cost at small resolutions; at large resolutions keep both axes at most 640.
#define LUX_RESTIR_MAX_AXIS 640u
#define LUX_RESTIR_DI 16u
#define LUX_RESTIR_GI 32u
#define LUX_RESTIR_OUTPUT_FLOATS4 5u
#define LUX_RAY_DENOISE 256u

uint RestirSpacing(uint2 size) {
    return max(2u, (max(size.x, size.y) + LUX_RESTIR_MAX_AXIS - 1u) / LUX_RESTIR_MAX_AXIS);
}

uint2 RestirGrid(uint2 size, uint spacing) {
    return (size + spacing - 1u) / spacing;
}

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

float RestirLuminance(float3 rgb) {
    return dot(max(rgb, float3(0.0f, 0.0f, 0.0f)), float3(0.2126f, 0.7152f, 0.0722f));
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
