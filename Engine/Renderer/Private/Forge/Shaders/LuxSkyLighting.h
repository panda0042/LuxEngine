float3 SkySpecularLighting(float3 d, float rough) {
    const float level = saturate(rough) * 4.0f;
    const float lo = floor(level);
    const float3 a =
        SampleLvlTex2D(LUX_SKY_GLOSS, gSamplerTrilinearClamp, SkyAtlasUV(d, lo, 5), 0).rgb;
    const float3 b = SampleLvlTex2D(LUX_SKY_GLOSS, gSamplerTrilinearClamp,
                                    SkyAtlasUV(d, min(lo + 1.0f, 4.0f), 5), 0)
                         .rgb;
    return lerp(a, b, level - lo);
}

float3 SkyDiffuseLighting(float3 dir) {
    float3 color = float3(0, 0, 0);
    UNROLL for (int i = 0; i < 9; ++i) color +=
        LoadTex2D(LUX_SKY_SH, NO_SAMPLER, int2(i, 0), 0).rgb * SkyHarmonic(i, dir);
    return max(color, float3(0, 0, 0));
}
