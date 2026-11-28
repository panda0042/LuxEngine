// Each 4x2 atlas cell includes a wrapped border of 1/32 of the source tile per side.
float2 PaletteUV(int i, float2 uv) {
    return (float2(i % 4, i / 4) + (frac(uv) * 32.0f + 1.0f) / 34.0f) / float2(4, 2);
}

#define PaletteFetch(TEX, I, UV, DX, DY)                                                           \
    SampleGradTex2D(TEX, gMaterialAnisotropicClamp, PaletteUV(I, UV),                              \
                    (DX) * float2(8.0f / 34.0f, 16.0f / 34.0f),                                    \
                    (DY) * float2(8.0f / 34.0f, 16.0f / 34.0f))

float2 PaletteOffset(float2 cell) {
    const float3 h = frac(float3(cell.x, cell.y, cell.x) * float3(.1031f, .11369f, .13787f));
    const float j = dot(h, h.yzx + 19.19f);
    return frac((h.xy + j) * (h.yz + h.zx + 2.0f * j));
}

// Project a short ray into the dominant height layer. All intersected microdetail uses the
// displaced coordinates. The perturbation fades gracefully before the tangent-plane horizon.
float2 PaletteParallax(int layer, float2 uv, float2 du, float2 dv, float3 view, float3 n, int axis,
                       float scale) {
    const float facing = abs(dot(n, view));
    const float depth = gDraw.relief.x;
    const float2 ray = (axis == 0   ? view.zy
                        : axis == 1 ? view.xz
                                    : view.xy) *
                       (-depth * scale / max(facing, .2f)) * smoothstep(.07f, .24f, facing);
    if (dot(ray, ray) < max(dot(du, du), dot(dv, dv)) * .15f)
        return uv;
    float previous = 0.0f;
    for (int step = 0; step <= 12; ++step) {
        const float f = float(step) / 12.0f;
        const float gap = 1.0f - PaletteFetch(gTerrainRoughness, layer, uv + ray * f, du, dv).a - f;
        if (gap <= 0.0f) {
            const float hit =
                step == 0 ? 0.0f
                          : (float(step - 1) + previous / max(.00001f, previous - gap)) / 12.0f;
            return uv + ray * hit;
        }
        previous = gap;
    }
    return uv + ray;
}

void TerrainPalette(float4 vertexWeights, float3 pos, float2 baseUV, float3 geom,
                    out(float3) albedo, out(float3) shading, out(float) rough) {
    float4 family =
        saturate(float4(vertexWeights.rgb, 1.0f - dot(vertexWeights.rgb, float3(1, 1, 1))));
    family /= max(dot(family, float4(1, 1, 1, 1)), .001f);
    float3 axis = pow(abs(geom), float3(6, 6, 6));
    axis /= max(dot(axis, float3(1, 1, 1)), .001f);
    const float wet = saturate(vertexWeights.a);
    float3 macro = float3(baseUV.x, (pos.y + gScene.atmosphere.w) * .5f, baseUV.y);
    const float3 dx = ddx(macro), dy = ddy(macro);
    const float2 q = baseUV * .067f;
    // Continuous low-frequency warp decorrelates tiling without discontinuous UV derivatives.
    const float2 warp =
        .28f * float2(sin(q.y + .6f * sin(q.x * 1.7f)), cos(q.x - .5f * sin(q.y * 1.3f)));
    albedo = float3(0, 0, 0);
    float3 slopes = float3(0, 0, 0);
    rough = 0.0f;
    float total = 0.0f;
    const float3 view = normalize(gLights.camera.xyz - pos);
    for (int side = 0; side < 3; ++side) {
        if (axis[side] < .005f)
            continue;
        const float2 uv = (side == 0 ? macro.zy : side == 1 ? macro.xz : macro.xy) + warp;
        const float2 du = side == 0 ? dx.zy : side == 1 ? dx.xz : dx.xy;
        const float2 dv = side == 0 ? dy.zy : side == 1 ? dy.xz : dy.xy;
        for (int i = 0; i < 8; ++i) {
            const float w = family[i / 2] * (i % 2 == 0 ? 1.0f - wet : wet) * axis[side];
            if (w < .001f)
                continue;
            // Cliff fissures span many metres, with a separate rock micro-normal.
            const float scale = i < 2 ? .14f : (i == 4 ? .6f : 1.0f);
            const float2 local = uv * scale + float(i) * float2(.137f, .293f);
            const float2 tilePosition = local * .6f, cell = floor(tilePosition),
                         f = frac(tilePosition);
            const bool lower = f.x + f.y < 1.0f;
            const float2 anchor = lower ? cell : cell + 1.0f;
            const float3 bary = lower ? float3(1.0f - f.x - f.y, f.x, f.y)
                                      : float3(f.x + f.y - 1.0f, 1.0f - f.y, 1.0f - f.x);
            const float2 dx = du * scale, dy = dv * scale;
            float4 material = float4(0, 0, 0, 0);
            float3 layerColor = float3(0, 0, 0);
            for (int k = 0; k < 3; ++k) {
                const float2 site = k == 0   ? anchor
                                    : k == 1 ? cell + float2(1, 0)
                                             : cell + float2(0, 1);
                float2 sampleUV = local + PaletteOffset(site);
                if (w > .20f && length(pos) < 32.0f)
                    sampleUV = PaletteParallax(i, sampleUV, dx, dy, view, geom, side, scale * .32f);
                material += bary[k] * PaletteFetch(gTerrainRoughness, i, sampleUV, dx, dy);
                layerColor += bary[k] * PaletteFetch(gTerrainColor, i, sampleUV, dx, dy).rgb;
            }
            const float hWeight = w * exp2(4.0f * gDraw.relief.y * (material.a - .5f));
            albedo += hWeight * layerColor;
            rough += hWeight * material.b;
            const float2 xy = material.rg * 2.0f - 1.0f;
            const float2 slope = xy / sqrt(max(.18f, 1.0f - dot(xy, xy)));
            slopes += hWeight * (side == 0   ? float3(0, slope.y, slope.x)
                                 : side == 1 ? float3(slope.x, 0, slope.y)
                                             : float3(slope.x, slope.y, 0));
            total += hWeight;
        }
    }
    const float norm = 1.0f / max(total, .001f);
    albedo *= norm;
    rough *= norm;
    shading =
        normalize(geom + (slopes - geom * dot(geom, slopes)) * norm * gDraw.materialOptions.x);
}
