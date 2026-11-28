#ifndef LUX_FOG_PARAMS_H
#define LUX_FOG_PARAMS_H

// Mirrored by FogConstants in LuxFog.inl.
STRUCT(LuxFogData) {
    DATA(float4, right, None); // xyz: scaled camera basis; w: projection A
    DATA(float4, up, None);    // xyz: scaled camera basis; w: projection B
    DATA(float4, forward, None);
    DATA(float4, colorDensity, None); // rgb: single-scattering albedo; w: base extinction
    DATA(float4, options, None); // distance, height falloff, observer height above base, anisotropy
    DATA(float4, lightDirection, None); // xyz: unit vector toward the light
    DATA(float4, lightRadiance, None);
    DATA(float4, ambientRadiance, None);
    DATA(float4x4, shadowMatrix, None);
    DATA(float4, shadowOptions, None); // enable, texel size, depth bias, normal bias
    DATA(float4, composite, None);     // 1/width, 1/height, exposure, ACES enable
    DATA(uint4, virtualShadowPages, None);
    DATA(float4, media, None);
    DATA(float4, local[4], None);
    DATA(float4, localDensity, None);
    DATA(float4, noiseOrigin, None); // wrapped world origin and number of point sources
    DATA(float4, cloud, None);
    DATA(float4x4, previousViewProjection, None);
    DATA(float4, displacement, None);
    DATA(float4, pointPosition[16], None);
    DATA(float4, pointRadiance[16], None);
};

#define LUX_FOG_COLUMNS 64u
#define LUX_FOG_ROWS 36u
#define LUX_FOG_SLICES 32u

#endif
