#ifndef LUX_SURFACE_WEATHER_H
#define LUX_SURFACE_WEATHER_H

// Integer spatial and temporal periods keep the phase continuous through large-world
// camera rebases and the wrapped weather clock. Frequencies approximate gravity-capillary
// dispersion on a deep freshwater surface. Each wavelength is represented by a wave pair.
void AddRippleBand(float2 position, float2 dx, float2 dy, float2 windDir, float time, float2 cycles,
                   float frequency, float phase, float slope, inout(float2) gradient,
                   inout(float) lostVariance) {
    const float tau = 6.2831853f;
    const float2 direction = normalize(cycles);
    const float alignment = dot(direction, windDir);
    const float amplitude = slope * (.38f + .62f * alignment * alignment);
    const float pixelPhase = tau * max(abs(dot(dx, cycles)), abs(dot(dy, cycles)));
    const float resolved = 1.0f - smoothstep(1.1f, 3.14159f, pixelPhase);
    // Unresolved capillary waves still roughen the specular lobe.
    lostVariance += .5f * amplitude * amplitude * (1.0f - resolved * resolved);
    if (resolved > .001f) {
        const float place = tau * frac(dot(position, cycles) + phase);
        const float travel = tau * frac(time * frequency);
        const float pair = cos(place) * cos(travel) + alignment * sin(place) * sin(travel);
        gradient += direction * (resolved * amplitude * pair);
    }
}

float3 RippleNormal(float3 world, float3 n, out(float) variance) {
    variance = 0.0f;
    const float strength = gDraw.surfaceWeather.y;
    if (strength <= 0.0f)
        return n;
    const float2 dx = ddx(world.xz) / 256.0f, dy = ddy(world.xz) / 256.0f;
    const float2 pos = frac((world.xz + gScene.weatherOrigin.xy) / 256.0f);
    const float time = gScene.wind.z / 4096.0f;
    const float speed = length(gScene.wind.xy);
    const float2 windDir = speed > .001f ? gScene.wind.xy / speed : float2(1, 0);
    float2 gradient = float2(0, 0);
    AddRippleBand(pos, dx, dy, windDir, time, float2(81, 46), 3087, .21f, .090f, gradient,
                  variance);
    AddRippleBand(pos, dx, dy, windDir, time, float2(57, 114), 3612, .70f, .065f, gradient,
                  variance);
    AddRippleBand(pos, dx, dy, windDir, time, float2(349, -71), 6038, .37f, .080f, gradient,
                  variance);
    AddRippleBand(pos, dx, dy, windDir, time, float2(174, 443), 6982, .63f, .075f, gradient,
                  variance);
    AddRippleBand(pos, dx, dy, windDir, time, float2(1703, 986), 14320, .83f, .068f, gradient,
                  variance);
    AddRippleBand(pos, dx, dy, windDir, time, float2(1129, -2153), 15966, .09f, .052f, gradient,
                  variance);
    const float gain = strength * (.18f + .82f * saturate(speed / 6.0f));
    variance *= gain * gain;
    const float3 worldSlope = float3(gradient.x, 0, gradient.y);
    return SafeDirection(n - gain * (worldSlope - n * dot(n, worldSlope)), n);
}

void ApplySurfaceWetness(float3 geometric, float metal, inout(float3) albedo, inout(float3) normal,
                         inout(float) roughness) {
    if (gDraw.surfaceWeather.x <= 0.0f)
        return;
    // Horizontal faces retain rain; vertical faces drain. Use the geometric normal so
    // small normal-map variations don't create a false rain mask on walls.
    const float film = gDraw.surfaceWeather.x * (.18f + .82f * saturate(geometric.y));
    const float absorption = film * (1.0f - metal) * (.25f + .75f * roughness);
    albedo *= lerp(float3(1, 1, 1), pow(clamp(albedo, .035f, 1.0f), float3(.32f, .32f, .32f)),
                   absorption);
    roughness = lerp(roughness, min(roughness, .065f + .14f * roughness), film);
    normal = SafeDirection(lerp(normal, geometric, .6f * film * film), normal);
}
#endif
