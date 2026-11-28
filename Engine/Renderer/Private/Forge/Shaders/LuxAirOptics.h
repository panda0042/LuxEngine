#ifndef LUX_AIR_OPTICS_H
#define LUX_AIR_OPTICS_H

// Spherical-shell optical depth, distances in kilometres. Concentrate LUT coordinates
// near sea level and the ground horizon where air mass changes steeply.
float AirHorizon(float h) {
    return -sqrt(saturate(h * (12720.0f + h) / ((6360.0f + h) * (6360.0f + h))));
}

float2 AirUV(float h, float mu) {
    const float horizon = AirHorizon(h);
    const float u = sqrt(saturate((mu - horizon) / (1.0f - horizon)));
    return (float2(u, sqrt(saturate(h * .01f))) * float2(127, 63) + .5f) / float2(128, 64);
}

float3 AirExtinction(float h, float aerosol) {
    return float3(.005802f, .013558f, .0331f) * exp(-h / 8.0f) +
           .00444f * aerosol * exp(-h / 1.2f) +
           float3(.000650f, .001881f, .000085f) * saturate(1.0f - abs(h - 25.0f) / 15.0f);
}

float AirDistance(float h, float mu) {
    const float r = 6360.0f + h;
    const float b = r * mu;
    if (mu < AirHorizon(h))
        return h * (12720.0f + h) / max(.0001f, -b + sqrt(max(0.0f, b * b - h * (12720.0f + h))));
    return -b + sqrt(max(0.0f, b * b + (100.0f - h) * (12820.0f + h)));
}

float AirHeight(float h, float mu, float t) {
    // Rationalized to retain metre-scale altitude differences in the fp32 path.
    const float r = 6360.0f + h;
    const float q = t * (2.0f * r * mu + t);
    return max(0.0f, h + q / (r + sqrt(max(0.0f, r * r + q))));
}

float3 AirTransmittance(float h, float mu, float aerosol) {
    const float distance = AirDistance(h, mu);
    float3 tau = float3(0, 0, 0);
    float previous = 0;
    for (int i = 1; i <= 40; ++i) {
        const float u = float(i) / 40.0f;
        const float end = distance * u * u;
        tau += AirExtinction(AirHeight(h, mu, (previous + end) * .5f), aerosol) * (end - previous);
        previous = end;
    }
    return exp(-tau);
}

float2 SkyViewUV(float3 d) {
    const float u = atan2(d.z, d.x) * .15915494f + .5f;
    const float v = .5f + .5f * sign(d.y) * sqrt(abs(d.y));
    return float2((u * 512.0f + 1.0f) / 514.0f, (v * 255.0f + .5f) / 256.0f);
}

float3 SkyViewDirection(float2 pixel) {
    const float phi = ((pixel.x - 1.0f) / 512.0f - .5f) * 6.2831853f;
    const float h = clamp(2.0f * (pixel.y - .5f) / 255.0f - 1.0f, -1.0f, 1.0f);
    const float y = h * abs(h), r = sqrt(saturate(1.0f - y * y));
    return float3(r * cos(phi), y, r * sin(phi));
}

#endif
