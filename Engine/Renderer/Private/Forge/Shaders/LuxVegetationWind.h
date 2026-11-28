#ifndef LUX_VEGETATION_WIND_H
#define LUX_VEGETATION_WIND_H

float3 ContactBend(float2 phase, float weight, float amplitude, float4 area) {
    if (area.w <= 0.0f || area.z <= .001f || weight <= 0.0f || amplitude <= 0.0f)
        return float3(0, 0, 0);
    const float2 delta = phase - area.xy;
    const float radius = length(delta);
    const float pushed =
        (1.0f - smoothstep(.15f * area.z, area.z, radius)) * area.w * weight * weight * amplitude;
    return float3(delta.x / max(radius, .02f), -.75f, delta.y / max(radius, .02f)) * pushed;
}

float3 EvaluateWind(float2 phase, float weight, float time, float2 wind, float amplitude) {
    const float speed = length(wind);
    if (amplitude <= 0.0f || weight <= 0.0f || speed < .001f)
        return float3(0, 0, 0);
    // Periodic world phase stays anchored when camera-relative coordinates rebase.
    const float2 pos = phase / 256.0f;
    const float t = time / 4096.0f;
    const float gust = sin(6.2831853f * (dot(pos, float2(5, 3)) - t * 730.0f));
    const float flutter = sin(6.2831853f * (dot(pos, float2(21, -15)) + t * 2360.0f));
    const float branch = sin(6.2831853f * (dot(pos, float2(-11, 8)) - t * 419.0f) + .65f * gust);
    const float tip = weight * weight * weight;
    const float bend = amplitude * saturate(speed / 8.0f) *
                       (.39f * weight * weight + .30f * weight * gust +
                        .20f * weight * weight * branch + .11f * tip * flutter);
    const float axial = amplitude * .10f * tip * flutter * saturate(speed / 8.0f);
    return float3(wind.x * bend - wind.y * axial, -speed * abs(bend) * .055f,
                  wind.y * bend + wind.x * axial) /
           speed;
}

float3 WindOffset(float3 relativePosition, float weight, float time) {
    const float2 phase = relativePosition.xz + gScene.weatherOrigin.xy;
    return EvaluateWind(phase, weight, time, gScene.wind.xy, gDraw.foliage.x) +
           ContactBend(phase, weight, gDraw.foliage.x, gScene.foliageContacts[0]) +
           ContactBend(phase, weight, gDraw.foliage.x, gScene.foliageContacts[1]);
}
#endif
