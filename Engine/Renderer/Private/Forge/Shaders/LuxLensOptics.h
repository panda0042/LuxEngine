#ifndef LUX_LENS_OPTICS_H
#define LUX_LENS_OPTICS_H

// Express radii in physical pixels, normalized to the corner, independently of aspect ratio.
float LensRadiusSquared(float2 uv) {
    const float2 viewport = 1.0f / gPost.dimensions.xy;
    const float2 fromCenter = (uv * 2.0f - 1.0f) * viewport;
    return dot(fromCenter, fromCenter) / dot(viewport, viewport);
}

float2 DistortedUv(float2 uv, float r2) {
    const float k = gPost.optics.w;
    // A reciprocal warp stays monotone for both signs in the supported range. Fit
    // the projected frame to the sensor so bilinear taps do not repeat border strips.
    const float fit = min(1.0f, 1.0f - k);
    return .5f + (uv - .5f) * (fit / (1.0f - k * r2));
}

float3 LensChromaticShift(float3 scene, float4 center, float2 uv, float r2) {
    const float2 radial = (uv - .5f) / gPost.dimensions.xy;
    const float2 direction = radial * rsqrt(max(dot(radial, radial), 1e-6f));
    const float2 offset =
        direction * r2 * gPost.optics.z * gPost.dimensions.xy / (1080.0f * gPost.dimensions.y);
    const float4 red = SampleLvlTex2D(gPostSource, gSamplerTrilinearClamp, uv + offset, 0.0f);
    const float4 blue = SampleLvlTex2D(gPostSource, gSamplerTrilinearClamp, uv - offset, 0.0f);
    const float3 original = center.rgb / max(center.a, 1e-5f);
    const float r = red.a > 1e-5f ? red.r / red.a : original.r;
    const float b = blue.a > 1e-5f ? blue.b / blue.a : original.b;
    return max(scene + float3(r - original.r, 0, b - original.b), 0.0f);
}

float3 FlareSource(float2 uv) {
    const float2 edge = min(uv, 1.0f - uv);
    const float edgeWeight = smoothstep(0.0f, .065f, min(edge.x, edge.y));
    const float3 radiant = SampleLvlTex2D(gBloomMid, gSamplerTrilinearClamp, uv, 0.0f).rgb;
    const float luma = PostLuma(radiant);
    return radiant * (edgeWeight * smoothstep(.35f, 1.6f, luma));
}

float3 LensGhosts(float2 uv) {
    if (gPost.flare.x <= 0.0f)
        return float3(0, 0, 0);
    const float2 axis = uv - .5f;
    // Sensor reflections form inverted, scaled copies across the optical centre.
    float3 ghosts = FlareSource(.5f - 1.5f * axis) * float3(.18f, .11f, .07f);
    ghosts += FlareSource(.5f - .72f * axis) * float3(.058f, .09f, .14f);
    ghosts += FlareSource(.5f + 2.2f * axis) * float3(.045f, .07f, .045f);
    const float2 sensor = axis / gPost.dimensions.xy * gPost.dimensions.y;
    const float radius = length(sensor);
    if (radius > .001f) {
        const float2 haloUv = .5f - axis * (1.0f - .30f / radius);
        ghosts += FlareSource(haloUv) * (.055f * smoothstep(.08f, .20f, radius));
    }
    return ghosts * gPost.flare.x;
}

uint FilmHash(uint n) {
    n ^= n >> 16u;
    n *= 0x7feb352du;
    n ^= n >> 15u;
    n *= 0x846ca68bu;
    return n ^ (n >> 16u);
}

float3 FilmResponse(float3 display, uint2 pixel) {
    if (gPost.optics.y <= 0.0f)
        return display;
    const uint key = pixel.x + 65537u * pixel.y + 2654435761u * uint(gPost.flare.y);
    const float noise =
        (float(FilmHash(key) & 65535u) - float(FilmHash(key + 1u) & 65535u)) / 65535.0f;
    const float luma = saturate(PostLuma(display));
    // Zero-mean triangular grain; peak amplitude in the midtones.
    return saturate(display + noise * .16f * gPost.optics.y * sqrt(luma * (1.0f - luma)));
}
#endif
