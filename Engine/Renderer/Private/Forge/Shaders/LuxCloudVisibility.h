#ifndef LUX_CLOUD_VISIBILITY_H
#define LUX_CLOUD_VISIBILITY_H

float CloudShadowTransmittance(float3 relativePosition, float3 sun, float strength, float coverage,
                               float altitude) {
    if (strength <= 0.0f || coverage <= 0.0f)
        return 1.0f;
    if (sun.y <= .015f)
        return 1.0f;
    const float2 plane = relativePosition.xz - sun.xz / sun.y * (relativePosition.y + altitude);
    const float edge = max(abs(plane.x), abs(plane.y));
    const float2 nearUV = clamp(plane / 2048.0f + .5f, .00390625f, .99609375f);
    const float2 farUV = clamp(plane / 16384.0f + .5f, .00390625f, .99609375f);
    const float nearT = SampleLvlTex2D(LUX_CLOUD_TEXTURE, gSamplerTrilinearClamp,
                                       float2(nearUV.x * .5f, nearUV.y), 0)
                            .r;
    const float farT = SampleLvlTex2D(LUX_CLOUD_TEXTURE, gSamplerTrilinearClamp,
                                      float2(.5f + farUV.x * .5f, farUV.y), 0)
                           .r;
    const float trans = lerp(nearT, farT, smoothstep(768.0f, 1000.0f, edge));
    return lerp(1.0f, trans, strength * (1.0f - smoothstep(7300.0f, 8160.0f, edge)));
}

#endif
