#ifndef LUX_ATMOSPHERE_H
#define LUX_ATMOSPHERE_H
#include "LuxAirOptics.h"

// Spherical Earth, distances in kilometres. Same extinction coefficients and
// density scale heights as the offline Atmosphere panorama baker.
float3 AtmosphereRadiance(float3 view, float3 sun, float altitude, float aerosol,
                          float3 solarIrradiance, bool solarDisc) {
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
        const float3 ozone = float3(.000650f, .001881f, .000085f);
        const float ozoneDensity = saturate(1.0f - abs(h - 25.0f) / 15.0f);
        const float3 extinction = betaR * density.x + betaM * density.y + ozone * ozoneDensity;
        const float sunY = dot(normalize(pos), sun);
        const float horizon = -sqrt(saturate(2.0f * h / radius));
        if (sunY > horizon) {
            // Optical air mass from a curved exponential atmosphere, finite at the horizon.
            const float elevation = max(sunY - horizon, 0.0f);
            const float2 solarDepth =
                density * float2(8, 1.2f) / sqrt(elevation * elevation + float2(.00078f, .00012f));
#ifdef LUX_ATMOSPHERE_TABLE
            const float2 uv = AirUV(h, sunY);
            const float3 lightTr = SampleLvlTex2D(LUX_ATMOSPHERE_TABLE, gSamplerTrilinearClamp,
                                                  float2(uv.x * .5f, uv.y), 0)
                                       .rgb;
            const float3 multiple = SampleLvlTex2D(LUX_ATMOSPHERE_TABLE, gSamplerTrilinearClamp,
                                                   float2(.5f + uv.x * .5f, uv.y), 0)
                                        .rgb;
            const float3 viewTr = exp(-depth - extinction * (.5f * ds));
            scatter += viewTr *
                       ((betaR * density.x * phaseR + .90f * betaM * density.y * phaseM) * lightTr +
                        (betaR * density.x + .90f * betaM * density.y) * multiple) *
                       ds;
#else
            const float3 tr =
                exp(-depth - extinction * (.5f * ds) - betaR * solarDepth.x - betaM * solarDepth.y);
            scatter += tr * (betaR * density.x * phaseR + .90f * betaM * density.y * phaseM) * ds;
#endif
        }
        depth += extinction * ds;
        previous = end;
    }
    if (ground) {
        // Lower hemisphere supplies the diffuse reflection of sunlit terrain to the sky probe.
        scatter += exp(-depth) * float3(.12f, .14f, .12f) * saturate(sun.y) * .31830989f;
    } else if (solarDisc && sun.y > 0.0f) {
        const float radiusCos = .99998919f; // 0.00465 rad angular radius
        const float aa = clamp(fwidth(mu) * .5f, .00000025f, .000008f);
        scatter += exp(-depth) * smoothstep(radiusCos - aa, radiusCos + aa, mu) / .00006793f;
    }
    return .000025f * float3(.34f, .46f, 1.0f) + scatter * solarIrradiance;
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

// Distance to jittered lattice sites. Restrict the jitter to keep distant sites
// from affecting the compact erosion kernel outside the eight enclosing corners.
float CloudCellular(float3 p) {
    const float3 cell = floor(p);
    const float3 f = frac(p);
    float nearest = 1.0f;
    UNROLL for (int i = 0; i < 8; ++i) {
        const float3 corner = float3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        const float h = CloudHash(cell + corner);
        const float3 delta = corner - f + .36f * (frac(h * float3(13.13f, 41.71f, 83.37f)) - .5f);
        nearest = min(nearest, dot(delta, delta));
    }
    return saturate(sqrt(nearest) * 1.3f);
}

float CloudDensity(float3 p, float coverage, float footprint) {
    const float height = saturate((p.y - 1250.0f) / 1150.0f);
    const float3 uv = p / 1024.0f;
    const float weather = CloudNoise(float3(uv.x * .25f, 7.0f, uv.z * .25f));
    const float tower = smoothstep(.25f, .8f, weather + coverage * .16f);
    const float envelope =
        smoothstep(0.0f, .12f, height) *
        (1.0f - smoothstep(lerp(.2f, .57f, tower), lerp(.52f, 1.0f, tower), height));
    const float base = CloudNoise(uv) * .67f + CloudNoise(uv * 2.0f + 27.0f) * .33f;
    const float localCoverage = coverage + (weather - .5f) * .32f;
    float mass = (base - lerp(.79f, .23f, localCoverage)) * 3.8f * envelope;
    if (mass <= 0.0f)
        return 0.0f;
    const float erosion = lerp(CloudCellular(uv * 4.0f), .5f, smoothstep(50.0f, 180.0f, footprint));
    return saturate(mass - erosion * .32f);
}

float CloudPhase(float mu, float g) {
    return (1.0f - g * g) / (12.56637f * pow(max(1.0f + g * g - 2.0f * g * mu, .015f), 1.5f));
}

float3 CloudTransport(float3 sky, float3 view, float3 sun, float coverage, float3 eye,
                      float3 solarIrradiance, out(float) visibleSky) {
    visibleSky = 1.0f;
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
            const float mu = dot(sun, view);
            const float opticalDepth = sunDepth * .005f;
            const float powder = 1.0f - exp(-2.0f * density);
            const float phase = .85f * CloudPhase(mu, .72f) + .15f * CloudPhase(mu, -.25f);
            const float multiple = .35f * CloudPhase(mu, .3f) * exp(-opticalDepth * .35f);
            const float3 source =
                float3(.00002f, .00003f, .00005f) +
                daylight * solarIrradiance *
                    (float3(.020f, .032f, .055f) *
                         (.4f + .6f * saturate((pos.y - 1250.0f) / 1150.0f)) +
                     direct * (phase * exp(-opticalDepth) + multiple) * (.8f + .4f * powder));
            const float opacity = 1.0f - exp(-density * ds * .005f);
            scatter += transmittance * opacity * source;
            transmittance *= 1.0f - opacity;
            if (transmittance < .015f)
                break;
        }
    }
    const float horizonFade = 1.0f - smoothstep(18000.0f, 35000.0f, entry);
    visibleSky = lerp(1.0f, transmittance, horizonFade);
    return scatter * horizonFade + visibleSky * sky;
}
#endif
