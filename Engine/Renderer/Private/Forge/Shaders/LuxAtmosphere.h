#ifndef LUX_ATMOSPHERE_H
#define LUX_ATMOSPHERE_H

// Spherical Earth, distances in kilometres. Same extinction coefficients and
// density scale heights as the offline Atmosphere panorama baker.
float3 SkyScattering(float3 view, float3 sun, float altitude, float aerosol,
                     float3 solarIrradiance) {
    const float radius = 6360.0f;
    const float top = radius + 100.0f;
    const float r = radius + clamp(altitude * .001f, .002f, 99.0f);
    const float b = r * view.y;
    const float groundDisc = b * b - (r - radius) * (r + radius);
    const bool ground = b < 0.0f && groundDisc >= 0.0f;
    const float distance =
        ground ? (r - radius) * (r + radius) / max(-b + sqrt(max(groundDisc, 0.0f)), .001f)
               : -b + sqrt(b * b + (top - r) * (top + r));
    const float3 betaR = float3(.005802f, .013558f, .0331f);
    const float betaM = .00444f * aerosol;
    const float mu = clamp(dot(view, sun), -1.0f, 1.0f);
    const float phaseR = .05968f * (1.0f + mu * mu);
    const float phaseM = .02865f / pow(max(.04f, 1.64f - 1.6f * mu), 1.5f);
    float3 scatter = float3(0.0f, 0.0f, 0.0f), depth = float3(0.0f, 0.0f, 0.0f);
    float previous = 0.0f;
    for (int i = 1; i <= 12; ++i) {
        const float u = float(i) / 12.0f;
        const float end = distance * u * u;
        const float ds = end - previous;
        const float3 pos = float3(0, r, 0) + view * ((end + previous) * .5f);
        const float h = max(length(pos) - radius, 0.0f);
        const float2 density = exp(-h / float2(8.0f, 1.2f));
        const float3 extinction = betaR * density.x + betaM * density.y;
        const float sunY = dot(normalize(pos), sun);
        const float horizon = -sqrt(saturate(2.0f * h / radius));
        if (sunY > horizon) {
            // Optical air mass from a curved exponential atmosphere, finite at the horizon.
            const float elevation = max(sunY - horizon, 0.0f);
            const float2 solarDepth =
                density * float2(8, 1.2f) / sqrt(elevation * elevation + float2(.00078f, .00012f));
            const float3 tr =
                exp(-depth - extinction * (.5f * ds) - betaR * solarDepth.x - betaM * solarDepth.y);
            scatter += tr * (betaR * density.x * phaseR + .90f * betaM * density.y * phaseM) * ds;
        }
        depth += extinction * ds;
        previous = end;
    }
    if (!ground && sun.y > 0.0f)
        scatter += exp(-depth) * smoothstep(.999980f, .999992f, mu) * 16.0f;
    return .002f * float3(.34f, .46f, 1.0f) + scatter * 18.0f * solarIrradiance;
}

float CloudHash(float3 p) {
    // 256-cell periodic lattice, so rebasing the camera does not move cloud features.
    p -= floor(p / 256.0f) * 256.0f;
    p = frac(p * float3(.1031f, .11369f, .13787f));
    p += dot(p, p.yzx + 19.19f);
    return frac((p.x + p.y) * p.z);
}

float CloudNoise(float3 p) {
    const float3 cell = floor(p);
    float3 t = frac(p);
    t = t * t * (3.0f - 2.0f * t);
    return lerp(
        lerp(lerp(CloudHash(cell), CloudHash(cell + float3(1, 0, 0)), t.x),
             lerp(CloudHash(cell + float3(0, 1, 0)), CloudHash(cell + float3(1, 1, 0)), t.x), t.y),
        lerp(lerp(CloudHash(cell + float3(0, 0, 1)), CloudHash(cell + float3(1, 0, 1)), t.x),
             lerp(CloudHash(cell + float3(0, 1, 1)), CloudHash(cell + float3(1, 1, 1)), t.x), t.y),
        t.z);
}

float CloudDensity(float3 p, float coverage, float footprint) {
    const float height = saturate((p.y - 1250.0f) / 1150.0f);
    const float envelope = smoothstep(0.0f, .18f, height) * (1.0f - smoothstep(.5f, 1.0f, height));
    const float3 uv = p / 1024.0f;
    // Fade sub-step detail to its mean at grazing view angles to prevent stacked bands.
    const float medium =
        lerp(CloudNoise(uv * 2.0f + 27.0f), .5f, smoothstep(150.0f, 500.0f, footprint));
    const float fine = lerp(CloudNoise(uv * 8.0f), .5f, smoothstep(40.0f, 130.0f, footprint));
    const float shape = CloudNoise(uv) * .65f + medium * .25f + fine * .10f;
    return saturate((shape - lerp(.82f, .18f, coverage)) * 2.8f) * envelope;
}

float3 CloudLayer(float3 sky, float3 view, float3 sun, float coverage, float3 eye,
                  float3 solarIrradiance) {
    if (coverage <= 0.0f || abs(view.y) < .00001f)
        return sky;
    const float2 bounds = (float2(1250.0f, 2400.0f) - eye.y) / view.y;
    const float entry = max(min(bounds.x, bounds.y), 0.0f);
    const float end = min(max(bounds.x, bounds.y), 35000.0f);
    if (end <= entry || (eye.y < 0.0f && view.y < 0.0f))
        return sky;
    const int steps = int(clamp(ceil((end - entry) / 80.0f), 16.0f, 48.0f));
    const float ds = (end - entry) / float(steps);
    const float daylight = smoothstep(-.10f, .28f, sun.y);
    const float3 direct = lerp(float3(1.0f, .38f, .16f), float3(1.0f, .97f, .92f), daylight);
    float transmittance = 1.0f;
    float3 scatter = float3(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < steps; ++i) {
        const float distance = entry + (float(i) + .5f) * ds;
        const float3 pos = eye + distance * view;
        const float density = CloudDensity(pos, coverage, ds);
        if (density > .001f) {
            const float sunDepth = 140.0f * CloudDensity(pos + sun * 70.0f, coverage, 140.0f) +
                                   380.0f * CloudDensity(pos + sun * 330.0f, coverage, 380.0f);
            const float phase = .45f + .85f * pow(saturate(dot(sun, view)), 8.0f);
            const float3 source =
                float3(.015f, .022f, .04f) +
                daylight * solarIrradiance *
                    (float3(.16f, .22f, .32f) + direct * (phase * exp(-sunDepth * .005f)));
            const float opacity = 1.0f - exp(-density * ds * .005f);
            scatter += transmittance * opacity * source;
            transmittance *= 1.0f - opacity;
            if (transmittance < .015f)
                break;
        }
    }
    return lerp(sky, scatter + transmittance * sky, 1.0f - smoothstep(18000.0f, 35000.0f, entry));
}
#endif
