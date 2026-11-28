#ifndef LUX_SCREEN_LIGHTING_H
#define LUX_SCREEN_LIGHTING_H

float ScreenEyeDepth(float z) {
    return gScene.up.w / min(z + gScene.right.w, -1e-7f);
}

// Integrate the visible normal-weighted arc between the two horizons of each view slice.
float GroundTruthVisibility(float3 world, float3 normal, float2 pixel, out(float3) bent) {
    bent = normal;
    const float radius = gScreenLighting.options.x;
    if (radius <= 0.0f)
        return 1.0f;
    const float3 offset = world - gLights.camera.xyz;
    const float depth = dot(offset, gScene.forward.xyz);
    if (depth <= 1e-5f)
        return 1.0f;
    const float3 view = -normalize(offset);
    const float3 xStep = gScene.right.xyz * (2.0f * gScene.frameSize.z);
    const float3 yStep = -gScene.up.xyz * (2.0f * gScene.frameSize.w);
    const float radiusPixels = min(96.0f, radius / max(depth * length(xStep), 1e-6f));
    if (radiusPixels < 1.0f)
        return 1.0f;

    float visibility = 0.0f;
    float3 occludedAxis = float3(0, 0, 0);
    const float pi = 3.14159265f;
    // Fixed screen-space rotations distribute the slices without temporal shimmer.
    const float rotation =
        frac(dot(fmod(floor(pixel), 4.0f), float2(0.25f, 0.5625f)) + gScene.shadowMetrics.z);
    for (int slice = 0; slice < 4; ++slice) {
        const float phi = (float(slice) + rotation) * (pi / 4.0f);
        const float2 axis = float2(cos(phi), sin(phi));
        const float3 sliceAxis = xStep * axis.x + yStep * axis.y;
        const float3 tangent = normalize(sliceAxis - view * dot(sliceAxis, view));
        const float2 projected = float2(dot(normal, view), dot(normal, tangent));
        const float lengthN = length(projected);
        if (lengthN < 1e-5f)
            continue;
        const float inclination = atan2(projected.y, projected.x);
        const float cosN = projected.x / lengthN, sinN = projected.y / lengthN;
        for (int side = -1; side <= 1; side += 2) {
            const float openAngle = clamp(inclination + float(side) * pi * 0.5f, -pi, pi);
            const float openCos = cos(openAngle);
            float horizonCos = openCos;
            for (int step = 1; step <= 5; ++step) {
                const float t = float(step) / 5.0f;
                const int2 tap =
                    int2(floor(pixel + axis * float(side) * max(1.0f, t * t * radiusPixels)));
                if (tap.x < 0 || tap.y < 0 || tap.x >= int(gScene.frameSize.x) ||
                    tap.y >= int(gScene.frameSize.y))
                    continue;
                const float z = LoadTex2D(gScreenDepth, NO_SAMPLER, tap, 0).r;
                if (z >= 1.0f)
                    continue;
                const float sampleDepth = ScreenEyeDepth(z);
                const float2 deltaPixels = float2(tap) + 0.5f - pixel;
                const float3 delta = offset * (sampleDepth / depth - 1.0f) +
                                     sampleDepth * (xStep * deltaPixels.x + yStep * deltaPixels.y);
                const float distance = length(delta);
                if (distance < 0.005f || distance >= radius)
                    continue;
                const float sampleCos = dot(delta, view) / distance;
                horizonCos =
                    max(horizonCos, lerp(openCos, sampleCos,
                                         1.0f - smoothstep(radius * 0.4f, radius, distance)));
            }
            occludedAxis += tangent * float(side) * max(0.0f, horizonCos - openCos) * lengthN;
            const float angle = float(side) * acos(clamp(horizonCos, -1.0f, 1.0f));
            visibility +=
                lengthN * 0.25f * (cosN - cos(2.0f * angle - inclination) + 2.0f * angle * sinN);
        }
    }
    bent = normalize(normal - occludedAxis * 0.25f);
    return saturate(visibility * 0.25f);
}

float ScreenContactShadow(float3 world, float3 normal) {
    const float range = gScreenLighting.options.y;
    const float3 light = -gLights.direction.xyz;
    if (range <= 0.0f || dot(normal, light) <= 0.0f)
        return 1.0f;
    const float depth = dot(world - gLights.camera.xyz, gScene.forward.xyz);
    const float footprint = depth * 2.0f *
                            max(length(gScene.right.xyz) * gScene.frameSize.z,
                                length(gScene.up.xyz) * gScene.frameSize.w);
    const float resolvable = smoothstep(.75f, 3.0f, range / max(footprint, .0001f));
    if (resolvable <= 0.0f)
        return 1.0f;
    const float3 origin = world + normal * max(0.007f, range * 0.012f);
    for (int step = 1; step <= 12; ++step) {
        const float t = float(step) / 12.0f;
        const float3 probe = origin + light * range * t;
        const float4 clip = mul(gDraw.viewProjection, float4(probe, 1.0f));
        if (clip.w <= 0.0f)
            break;
        const float2 uv = clip.xy / clip.w * float2(0.5f, -0.5f) + 0.5f;
        if (uv.x < 0.0f || uv.y < 0.0f || uv.x >= 1.0f || uv.y >= 1.0f)
            break;
        const int2 pixel = int2(uv * gScene.frameSize.xy);
        const float z = LoadTex2D(gScreenDepth, NO_SAMPLER, pixel, 0).r;
        const float sampleDepth = ScreenEyeDepth(z);
        const float delta = clip.w - sampleDepth;
        if (z < 1.0f && delta > max(0.006f, clip.w * 0.00012f) &&
            delta < range * 0.16f + clip.w * 0.001f) {
            // A neighboring texel on the receiver itself is often closer to the eye,
            // especially along a grazing slope. Reject it against the receiver plane.
            const float2 sampleOffset = (float2(pixel) + 0.5f) * gScene.frameSize.zw - uv;
            const float3 samplePoint =
                gLights.camera.xyz + (probe - gLights.camera.xyz) * (sampleDepth / clip.w) +
                2.0f * sampleDepth *
                    (gScene.right.xyz * sampleOffset.x - gScene.up.xyz * sampleOffset.y);
            const float planeDistance = dot(samplePoint - world, normal);
            const float bias = max(0.006f, min(range * 0.06f, footprint * 0.35f));
            if (planeDistance > bias) {
                const float thickness = range * 0.16f + clip.w * 0.001f;
                const float trust = smoothstep(bias, bias * 2.0f, planeDistance) *
                                    (1.0f - smoothstep(0.7f * thickness, thickness, delta));
                return 1.0f - resolvable * trust * (1.0f - smoothstep(0.65f, 1.0f, t));
            }
        }
    }
    return 1.0f;
}

// Approximate the reflected cone footprint with four depth-aware radiance samples.
// Missing coverage at silhouettes yields a lower weight for the environment blend.
float4 FilterReflectionHit(int2 pixel, float surfaceDepth, float distance, float roughness) {
    const float pixelsPerMetre =
        0.5f * gScene.frameSize.y / max(0.001f, surfaceDepth * length(gScene.up.xyz));
    const float radius = clamp(roughness * roughness * distance * pixelsPerMetre, 0.0f, 5.0f);
    const float3 center = LoadTex2D(gScreenColor, NO_SAMPLER, pixel, 0).rgb;
    if (radius < 0.5f)
        return float4(center, 1.0f);
    float3 color = 2.0f * center;
    float accepted = 2.0f;
    const float rotation = 1.570796f * gScene.shadowMetrics.z;
    const float2 axis = float2(cos(rotation), sin(rotation));
    const int2 maximum = int2(gScene.frameSize.xy) - 1;
    for (int i = 0; i < 4; ++i) {
        const int signTap = (i & 1) == 0 ? 1 : -1;
        const float2 tapAxis = i < 2 ? axis : float2(-axis.y, axis.x);
        const int2 delta = int2(round(tapAxis * (radius * float(signTap))));
        const int2 tap = clamp(pixel + delta, int2(0, 0), maximum);
        const float z = LoadTex2D(gScreenDepth, NO_SAMPLER, tap, 0).r;
        const float relativeDepth =
            abs(ScreenEyeDepth(z) - surfaceDepth) / max(0.12f, surfaceDepth * 0.025f);
        const float weight = z < 1.0f ? 1.0f - smoothstep(0.5f, 1.0f, relativeDepth) : 0.0f;
        color += weight * LoadTex2D(gScreenColor, NO_SAMPLER, tap, 0).rgb;
        accepted += weight;
    }
    return float4(color / accepted, accepted / 6.0f);
}

// Traverse 16x16, 4x4 and single-pixel cells. The minimum depth of each cell lets a
// projected ray skip empty space. Interpolate reciprocal eye depth along the screen line.
float4 HierarchicalReflection(float3 world, float3 normal, float3 ray, float roughness) {
    const float4 miss = float4(0, 0, 0, 0);
    if (gScreenLighting.options.z < 0.5f || roughness >= 0.65f)
        return miss;
    const float depth = dot(world - gLights.camera.xyz, gScene.forward.xyz);
    const float3 origin = world + normal * max(0.018f, depth * 0.0003f);
    const float dz = dot(ray, gScene.forward.xyz);
    const float range = dz < 0.0f ? min(160.0f, (depth - 0.05f) / -dz) : 160.0f;
    if (range <= 0.03f)
        return miss;
    const float4 a = mul(gDraw.viewProjection, float4(origin, 1.0f));
    const float4 b = mul(gDraw.viewProjection, float4(origin + ray * range, 1.0f));
    if (min(a.w, b.w) <= 0.0f)
        return miss;
    const float2 scale = gScene.frameSize.xy * float2(0.5f, -0.5f);
    const float2 start = a.xy / a.w * scale + 0.5f * gScene.frameSize.xy;
    const float2 delta = (b.xy / b.w - a.xy / a.w) * scale;
    const float lengthPixels = max(abs(delta.x), abs(delta.y));
    if (lengthPixels < 2.0f)
        return miss;
    const float2 signRay = float2(delta.x >= 0.0f ? 1.0f : 0.0f, delta.y >= 0.0f ? 1.0f : 0.0f);
    const float2 invDelta = float2(1.0f / (abs(delta.x) < 1e-5f ? 1e-5f : delta.x),
                                   1.0f / (abs(delta.y) < 1e-5f ? 1e-5f : delta.y));
    float t = 2.0f / lengthPixels;
    int level = 2;
    for (int iteration = 0; iteration < 72 && t < 1.0f; ++iteration) {
        const float2 position = start + delta * t;
        if (position.x < 0.0f || position.y < 0.0f || position.x >= gScene.frameSize.x ||
            position.y >= gScene.frameSize.y)
            break;
        const int cellSize = level == 2 ? 16 : (level == 1 ? 4 : 1);
        const int2 cell = int2(position) / cellSize;
        const float2 boundary = (float2(cell) + signRay) * float(cellSize);
        const float2 reach = (boundary - start) * invDelta;
        const float next = min(1.0f, min(abs(delta.x) < 1e-5f ? 1.0f : reach.x,
                                         abs(delta.y) < 1e-5f ? 1.0f : reach.y));
        const float2 bounds = level == 2 ? LoadTex2D(gDepthSixteenth, NO_SAMPLER, cell, 0).rg
                              : level == 1
                                  ? LoadTex2D(gDepthQuarter, NO_SAMPLER, cell, 0).rg
                                  : float2(LoadTex2D(gScreenDepth, NO_SAMPLER, cell, 0).r, 0.0f);
        const float surface = ScreenEyeDepth(bounds.x);
        const float farthest = level > 0 ? ScreenEyeDepth(bounds.y) : surface;
        const float rayStart = rcp(lerp(rcp(a.w), rcp(b.w), t));
        const float rayEnd = rcp(lerp(rcp(a.w), rcp(b.w), next));
        if (bounds.x < 1.0f && max(rayStart, rayEnd) >= surface &&
            min(rayStart, rayEnd) <= farthest + max(0.08f, farthest * 0.006f)) {
            if (level > 0) {
                --level;
                continue;
            }
            const float thickness = max(0.08f, surface * 0.006f);
            const float gap = max(0.0f, min(rayStart, rayEnd) - surface);
            if (gap < thickness) {
                // Interpolate reciprocal eye depth inside this pixel's traversal interval.
                // This solves the crossing analytically instead of treating the cell edge as a hit.
                const float denominator = rcp(b.w) - rcp(a.w);
                const float hitT = abs(denominator) > 1e-8f
                                       ? clamp((rcp(surface) - rcp(a.w)) / denominator, t, next)
                                       : t;
                const float2 hitPixel = start + delta * hitT;
                const float2 uv = hitPixel * gScene.frameSize.zw;
                const float2 edge = min(uv, 1.0f - uv);
                // Screen-line t is not a distance. Undo perspective before fading the
                // last part of the world-space trace towards the environment.
                const float travelFraction = hitT * rcp(lerp(rcp(a.w), rcp(b.w), hitT)) / b.w;
                const float travel = saturate(travelFraction) * range;
                const float hitTrust = (1.0f - smoothstep(0.4f, 1.0f, gap / thickness)) *
                                       (1.0f - smoothstep(0.5f, 1.0f, travelFraction)) *
                                       (1.0f - smoothstep(45.0f, 71.0f, float(iteration)));
                const float4 radiance = FilterReflectionHit(cell, surface, travel, roughness);
                const float confidence = smoothstep(0.0f, 0.06f, min(edge.x, edge.y)) *
                                         (1.0f - smoothstep(0.2f, 0.65f, roughness));
                return float4(radiance.rgb, radiance.a * hitTrust * confidence);
            }
        }
        t = max(next, t) + 0.005f / lengthPixels;
        level = min(level + 1, 2);
    }
    return miss;
}

#endif
