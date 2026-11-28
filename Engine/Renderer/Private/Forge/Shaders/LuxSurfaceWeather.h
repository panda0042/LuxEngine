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

float RainHash(float2 p) {
    p -= floor(p / 1024.0f) * 1024.0f;
    float3 h = frac(p.xyx * float3(.1031f, .11369f, .13787f));
    h += dot(h, h.yzx + 19.19f);
    return frac((h.x + h.y) * h.z);
}

// Independent raindrop centres and arrival times. Neighbour cells retain expanding
// rings crossing cell boundaries. High-frequency slopes fade into unresolved variance.
float2 RainSlopes(float2 p, float clock, float rain, float footprint) {
    if (rain <= .001f || footprint > .18f)
        return float2(0, 0);
    const float2 cell = floor(p);
    const float2 f = frac(p);
    float2 slope = float2(0, 0);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            const float2 id = cell + float2(x, y);
            const float t = clock + RainHash(id + 43.0f);
            const float cycle = floor(t);
            const float age = frac(t);
            const float2 seed = id + float2(17, 31) * fmod(cycle, 1024.0f);
            const float2 delta = f - float2(x, y) - float2(RainHash(seed), RainHash(seed + 8.70f));
            const float r = length(delta);
            const float d = r - age * .8f;
            const float envelope = exp(-100.0f * d * d - 3.0f * age) * smoothstep(0.0f, .09f, age);
            slope += delta / max(r, .005f) * cos(d * 51.0f) * envelope;
        }
    return rain * .13f * slope * (1.0f - smoothstep(.025f, .18f, footprint));
}

float3 RippleNormal(float3 world, float3 n, out(float) variance) {
    variance = 0.0f;
    const float strength = gDraw.surfaceWeather.y;
    if (strength <= 0.0f)
        return n;
    const float2 dx = ddx(world.xz) / 256.0f, dy = ddy(world.xz) / 256.0f;
    const float2 pos0 = (world.xz + gScene.weatherOrigin.xy) / 256.0f;
    const float2 cycle = 6.2831853f * pos0;
    const float2 pos = pos0 + .007f * float2(sin(cycle.y * 3.0f + sin(cycle.x)),
                                             sin(cycle.x * 2.0f + cos(cycle.y * 3.0f)));
    // Different wave packets occupy different patches instead of drawing parallel
    // sinusoidal stripes across the full lake. Warp is periodic through origin rebases.
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
    gradient *= .66f + .34f * sin(cycle.x * 5.0f + cos(cycle.y * 3.0f));
    gradient += RainSlopes(pos0 * 1024.0f, gScene.wind.z, gDraw.surfaceWeather.z,
                           max(length(dx), length(dy)) * 1024.0f);
    const float3 worldSlope = float3(gradient.x, 0, gradient.y);
    return SafeDirection(n - gain * (worldSlope - n * dot(n, worldSlope)), n);
}

void ApplySurfaceWetness(float3 pos, float3 geometric, float metal, inout(float3) albedo,
                         inout(float3) normal, inout(float) roughness) {
    if (gDraw.surfaceWeather.x <= 0.0f)
        return;
    // Horizontal faces retain rain; vertical faces drain. Use the geometric normal so
    // small normal-map variations don't create a false rain mask on walls.
    const float film = gDraw.surfaceWeather.x * (.18f + .82f * saturate(geometric.y));
    const float absorption = film * (1.0f - metal) * (.25f + .75f * roughness);
    albedo *= lerp(float3(1, 1, 1), pow(clamp(albedo, .035f, 1.0f), float3(.32f, .32f, .32f)),
                   absorption);
    roughness = lerp(roughness, min(roughness, .065f + .14f * roughness), film);
    const float2 xz = pos.xz + gScene.weatherOrigin.xy;
    const float2 phase = xz * (6.2831853f / 256.0f);
    const float depression = .5f + .24f * sin(phase.x * 29.0f + sin(phase.y * 13.0f)) +
                             .22f * cos(phase.y * 19.0f - phase.x * 9.0f);
    const float puddle = smoothstep(.48f, .92f, film) * smoothstep(.25f, .72f, depression) *
                         smoothstep(.93f, .995f, geometric.y);
    roughness = lerp(roughness, .045f, puddle);
    const float2 drops = RainSlopes(xz * 4.0f, gScene.wind.z, gDraw.surfaceWeather.z * puddle,
                                    4.0f * max(length(ddx(pos.xz)), length(ddy(pos.xz))));
    normal = SafeDirection(lerp(normal, geometric, max(.6f * film * film, puddle)) -
                               float3(drops.x, 0, drops.y),
                           normal);
}
#endif
