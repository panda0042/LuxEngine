#ifndef LUX_RADIANCE_CASCADES_SAMPLING_H
#define LUX_RADIANCE_CASCADES_SAMPLING_H

uint CascadeProbeIndex(uint3 coordinate, uint width) {
    return (coordinate.z * width + coordinate.y) * width + coordinate.x;
}

uint3 CascadeProbeCoordinate(uint index, uint width) {
    return uint3(index % width, (index / width) % width, index / (width * width));
}

float3 CascadeGridPosition(float3 position, float3 origin, uint width, float spacing) {
    return (position - origin) / spacing + (float(width) - 1.0f) * 0.5f;
}

uint3 CascadeCorner(uint corner) {
    return uint3(corner & 1u, (corner >> 1u) & 1u, corner >> 2u);
}

float CascadeCornerWeight(uint3 corner, float3 fraction) {
    const float3 weight = lerp(1.0f - fraction, fraction, float3(corner));
    return weight.x * weight.y * weight.z;
}

#endif
