#ifndef LUX_FOG_SAMPLING_H
#define LUX_FOG_SAMPLING_H

// The first boundary is the camera origin. Near slices are denser while distant
// ones grow geometrically to keep a fixed volume size as view distance increases.
float FogDepthScale(float maxDistance) {
    return min(1.0f, maxDistance / 8.0f);
}

uint FogIndex(uint2 cell, uint slice) {
    return (slice * LUX_FOG_ROWS + cell.y) * LUX_FOG_COLUMNS + cell.x;
}

#endif
