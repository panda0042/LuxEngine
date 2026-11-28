#ifndef LUX_SCENE_COLOR_GRADE_H
#define LUX_SCENE_COLOR_GRADE_H

float3 SceneGrade(float3 hdr) {
    if (gPost.gradeControls.w < .5f)
        return hdr;
    // Adapt the authored illuminant to D65 in scene-linear RGB.
    hdr *= float3(gPost.gradeLift.w, gPost.gradeGamma.w, gPost.gradeGain.w);
    const float luminance = max(PostLuma(hdr), 0.0f);
    const float stop = log2(max(luminance, 1e-6f) / .18f);
    const float shadow = 1.0f - smoothstep(-3.0f, -.25f, stop);
    const float highlight = smoothstep(.25f, 3.0f, stop);
    hdr *=
        exp2(shadow * gPost.gradeShadows.rgb + (1.0f - shadow - highlight) * gPost.gradeMids.rgb +
             highlight * gPost.gradeHighs.rgb);
    hdr = pow(max(hdr + gPost.gradeLift.rgb, 0.0f), 1.0f / gPost.gradeGamma.rgb) *
          gPost.gradeGain.rgb;
    const float Y = max(PostLuma(hdr), 1e-6f);
    hdr *= pow(Y / .18f, gPost.gradeControls.z - 1.0f);
    const float peak = max(max(hdr.r, hdr.g), hdr.b);
    const float chroma = (peak - min(min(hdr.r, hdr.g), hdr.b)) / max(peak, 1e-6f);
    const float strength =
        gPost.gradeControls.x * (1.0f + gPost.gradeControls.y * (1.0f - saturate(chroma)));
    return max(lerp(float3(PostLuma(hdr), PostLuma(hdr), PostLuma(hdr)), hdr, strength), 0.0f);
}
#endif
