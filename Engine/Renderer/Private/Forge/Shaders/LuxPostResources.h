#ifndef LUX_POST_RESOURCES_H
#define LUX_POST_RESOURCES_H

STRUCT(LuxPostData) {
    DATA(float4, dimensions, None); // output inverse dimensions, input inverse dimensions
    DATA(float4, lens, None);       // bloom intensity, threshold, focus blur radius, focus metres
    DATA(float4, camera, None);  // perspective depth A/B, manual exposure, tone and edge AA flags
    DATA(float4, grading, None); // LUT edge size, LUT strength, auto exposure, first reduction
    // physical bloom, local exposure, blade count, physical CoC scale (px)
    DATA(float4, cameraEffects, None);
    DATA(float4, optics, None); // vignette, grain, chromatic aberration in 1080p pixels, distortion
    DATA(float4, flare, None);  // flare strength, temporal seed
    // elapsed seconds, history valid, log2 of photometric exposure calibration
    DATA(float4, adaptation, None);
    DATA(float4, meterRange, None);
    DATA(float4, meterResponse, None);
    DATA(float4, bloomFocus, None); // radius, scatter, focal length, spare
    DATA(float4, gradeControls, None);
    DATA(float4, gradeLift, None);
    DATA(float4, gradeGamma, None);
    DATA(float4, gradeGain, None);
    DATA(float4, gradeShadows, None);
    DATA(float4, gradeMids, None);
    DATA(float4, gradeHighs, None);
    DATA(float4, diagnostics, None);
};

#include "LuxPost.srt.h"

STRUCT(FullscreenOutput) {
    DATA(float4, position, SV_Position);
    DATA(float2, uv, TEXCOORD0);
};

float PostLuma(float3 c) {
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}
#endif
