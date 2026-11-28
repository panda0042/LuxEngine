#ifndef LUX_RESTIR_GRID_H
#define LUX_RESTIR_GRID_H
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

float RestirLuminance(float3 rgb) {
    return dot(max(rgb, float3(0.0f, 0.0f, 0.0f)), float3(0.2126f, 0.7152f, 0.0722f));
}

#define LUX_RESTIR_CANDIDATE_FLOATS4 9u
#define LUX_RESTIR_HISTORY_FLOATS4 11u
#endif
