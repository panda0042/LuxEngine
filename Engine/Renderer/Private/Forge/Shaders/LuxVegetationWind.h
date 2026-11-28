#ifndef LUX_VEGETATION_WIND_H
#define LUX_VEGETATION_WIND_H

float3 WindOffset(float3 relativePosition, float weight, float time) {
    const float speed = length(gScene.wind.xy);
    if (gDraw.foliage.x <= 0.0f || weight <= 0.0f || speed < .001f)
        return float3(0, 0, 0);
    // Periodic world phase stays anchored when camera-relative coordinates rebase.
    const float2 pos = (relativePosition.xz + gScene.weatherOrigin.xy) / 256.0f;
    const float t = time / 4096.0f;
    const float gust = sin(6.2831853f * (dot(pos, float2(5, 3)) - t * 730.0f));
    const float flutter = sin(6.2831853f * (dot(pos, float2(21, -15)) + t * 2360.0f));
    const float bend = gDraw.foliage.x * saturate(speed / 8.0f) * weight * weight *
                       (.40f + .42f * gust + .18f * flutter);
    return float3(gScene.wind.x, 0, gScene.wind.y) * (bend / speed);
}
#endif
