#ifndef LUX_STRUCTS_H
#define LUX_STRUCTS_H

// Per-draw constants shared by every LuxForge shader (filled by LuxForge.cpp).
STRUCT(LuxDraw) {
    DATA(float4x4, viewProjection, None);
    DATA(float4x4, model, None);
    DATA(float4, albedo, None);
    // x: sRGB texture, y: metallic, z: roughness, w: PBR (same as the Vulkan backend)
    DATA(float4, colorOptions, None);
    // x: 1 = nearest filtering, 0 = bilinear
    DATA(float4, options, None);
    DATA(float4, materialOptions, None);
    DATA(float4, water, None); // RGB absorption per world unit and index of refraction
    DATA(float4, textureOptions, None);
    DATA(float4, coastalBreaker, None);
    DATA(float4, surfaceWeather, None); // wet film saturation, water ripple slope strength
    DATA(float4, foliage, None);        // wind displacement (m), RGB subsurface radius (m)
    DATA(float4x4, previousMvp, None);
    DATA(float4x4, previousWindProjection, None);
    DATA(float4, emissive, None); // linear HDR radiance and textured flag
    // POM depth in metres, layer height contrast, base and layer height flags.
    DATA(float4, relief, None);
    DATA(float4, previousWind, None); // time, surface ID, FFT history flag, stable coverage seed
    DATA(float4, previousPhaseX, None);
    DATA(float4, previousPhaseZ, None);
    DATA(float4, previousDeformation, None); // old wind.xy, amplitude, breaker height
    DATA(float4, lobes, None);
    DATA(float4, optics, None);
    DATA(float4, specular, None);
    DATA(float4, lod, None);
    DATA(float4, previousCoast, None); // old unwrapped-minus-wrapped origin, breaker time
};

STRUCT(LuxScreenLightingData) {
    DATA(float4, options, None); // GTAO radius, contact distance, enable Hi-Z SSR, PCSS texel scale
};

STRUCT(LuxRayHistoryParams) {
    DATA(float4x4, previousViewProjection, None);
    DATA(float4, displacement, None); // current origin minus previous, w permits reprojection
    DATA(float4, controls, None);     // frame seed, temporal resampling, denoise
};

// Lighting set with SetLighting (16 point lights, like the Vulkan backend).
STRUCT(LuxLights) {
    DATA(float4, direction, None);
    DATA(float4, colorIntensity, None);
    DATA(uint4, count, None);
    DATA(float4, camera, None);
    DATA(float4, pointPositionRadius[16], None);
    DATA(float4, pointColorIntensity[16], None);
};

// Constants of one HDR scene pass (Shaders/Scene3D.hlsli SceneData; filled by LuxForge.cpp).
STRUCT(LuxSceneData) {
    DATA(float4, camera, None);      // xyz: position, w: exposure
    DATA(float4, environment, None); // rgb: ambient colour, w: intensity
    // x: rotation (radians), y: IBL, z: flags (ACES 1, edge AA 2), w: sky
    DATA(float4, options, None);
    DATA(float4, right, None); // w: perspective depth coefficient A
    DATA(float4, up, None);    // w: perspective depth coefficient B
    DATA(float4, forward, None);
    DATA(float4x4, shadowMatrix, None);
    DATA(float4, shadowOptions, None); // x: on, y: 1/resolution, z: depth bias, w: normal bias
    DATA(float4, frameSize, None);     // width, height, 1/width, 1/height
    DATA(float4, rayOptions, None);    // effects mask, distance, bounces, diffuse strength
    DATA(float4, waterReflection, None);
    DATA(uint4, virtualShadowPages, None);
    DATA(float4, atmosphere, None);
    DATA(float4, wind, None);
    DATA(float4, weatherOrigin, None);
    DATA(float4, foliageContacts[2], None);
    DATA(float4, previousContacts[2], None);
    DATA(float4, skyLighting, None);
    DATA(float4, solarSource, None);
    DATA(float4, shadowCascades[4], None);
    DATA(float4, shadowMetrics, None);
};

#endif
