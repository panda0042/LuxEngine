#ifndef LUX_RADIANCE_CASCADES_PARAMS_H
#define LUX_RADIANCE_CASCADES_PARAMS_H

// Each angular cell divides into four equal-solid-angle children in the next interval.
#define LUX_CASCADE_NEAR_PROBES 8u
#define LUX_CASCADE_MIDDLE_PROBES 6u
#define LUX_CASCADE_FAR_PROBES 4u
#define LUX_CASCADE_NEAR_ANGLES 4u
#define LUX_CASCADE_MIDDLE_ANGLES 8u
#define LUX_CASCADE_FAR_ANGLES 16u
#define LUX_CASCADE_SH_STRIDE 3u
#define LUX_CASCADE_SH_HEADER 2u

STRUCT(LuxRadianceCascadesData) {
    DATA(uint4, shape, None);     // probes, angles, next probes, next angles
    DATA(float4, origin, None);   // relative origin, first-cascade spacing
    DATA(float4, interval, None); // probe spacing, ray start and end, 1 for SH projection
    DATA(float4, environment, None);
    DATA(float4, optics, None); // environment rotation, IBL enabled, strength, shadow ray length
};

#endif
