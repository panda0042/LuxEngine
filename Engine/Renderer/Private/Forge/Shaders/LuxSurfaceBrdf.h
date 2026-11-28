#ifndef LUX_SURFACE_BRDF_H
#define LUX_SURFACE_BRDF_H

// Anisotropic GGX with height-correlated Smith visibility in the UV tangent frame.
float3 EllipticalSpecular(float3 n, float3 t, float3 v, float3 l, float rough, float anisotropy,
                          float3 f0) {
    const float nl = saturate(dot(n, l)), nv = saturate(dot(n, v));
    const float3 h = SafeDirection(l + v, n);
    t = SafeDirection(t - n * dot(t, n), abs(n.y) < .9f ? normalize(cross(float3(0, 1, 0), n))
                                                        : normalize(cross(float3(1, 0, 0), n)));
    const float3 b = cross(n, t);
    const float aspect = sqrt(1.0f - abs(anisotropy) * .9f);
    const float a = max(.0025f, rough * rough);
    const float ax = anisotropy > 0 ? a / aspect : a * aspect, ay = a * a / ax;
    const float3 projected = float3(dot(h, t) / ax, dot(h, b) / ay, dot(h, n));
    const float k = dot(projected, projected);
    const float d = 1.0f / max(3.14159265f * ax * ay * k * k, .0000001f);
    const float smith = .5f / max(nl * length(float3(ax * dot(v, t), ay * dot(v, b), nv)) +
                                      nv * length(float3(ax * dot(l, t), ay * dot(l, b), nl)),
                                  .0001f);
    return Fresnel(saturate(dot(v, h)), f0) * (nl * d * smith);
}

float3 PrincipledLighting(float3 base, float metal, float rough, float3 n, float3 v, float3 l,
                          float3 tangent, float3 coatNormal, float thickness, float3 geometric,
                          float skinFraction) {
    const int model = int(gDraw.lobes.x);
    if (model == 0)
        return Brdf(base, metal, rough, n, v, l);
    const float nl = saturate(dot(n, l)), nv = max(.0001f, saturate(dot(n, v)));
    const float3 h = SafeDirection(v + l, n);
    const float lh = saturate(dot(l, h));
    const float fd90 = .5f + 2.0f * rough * lh * lh;
    const float fd = (1.0f + (fd90 - 1.0f) * pow(1.0f - nl, 5.0f)) *
                     (1.0f + (fd90 - 1.0f) * pow(1.0f - nv, 5.0f));
    const float3 f0 = lerp(MaterialDielectricF0(), base, metal);
    const float3 fr = Fresnel(lh, f0);
    const float tr = gDraw.optics.z;
    float3 diffuse = (1.0f - fr) * ((1.0f - metal) / 3.141593f) * base * (fd * nl);
    if (model == 2) {
        // Leaf normals describe veins and needles; orient transmission about the thin geometric
        // sheet.
        const float back = saturate(-dot(geometric, l));
        const float path = thickness / max(.18f, back);
        const float3 transmission =
            exp(-path / max(gDraw.foliage.yzw, float3(.000001f, .000001f, .000001f)));
        const float forward = .6f + .4f * pow(saturate(dot(-l, v)), 3.0f);
        diffuse =
            diffuse * (1.0f - .35f * tr) + base * transmission * (tr * back * forward / 3.141593f);
    } else if (tr > 0) {
        const float back = saturate(-dot(geometric, l));
        const float3 absorption = -log(max(base, float3(.01f, .01f, .01f)));
        diffuse = lerp(
            diffuse,
            (1.0f - fr) * exp(-absorption * thickness / max(.08f, back)) * back / 3.141593f, tr);
    }
    float3 specular = EllipticalSpecular(n, tangent, v, l, rough, gDraw.lobes.w, f0);
    if (model == 3 || model == 7) {
        // Skin's broad oil/stratum-corneum response and tighter surface lobe share the incident
        // energy.
        specular =
            lerp(specular, EllipticalSpecular(n, tangent, v, l, max(.07f, .52f * rough), 0.0f, f0),
                 .28f * skinFraction);
    } else if (model == 4) {
        // Offset longitudinal lobes around the strand tangent model the cuticle and cortex.
        const float3 strand = SafeDirection(tangent, float3(0, 1, 0));
        const float sinThetaH = dot(strand, h);
        const float width = max(.08f, rough * .48f);
        const float shift = .10f;
        const float longitudinal = exp(-.5f * pow((sinThetaH - shift) / width, 2.0f)) / width;
        const float secondary =
            exp(-.5f * pow((sinThetaH + 1.5f * shift) / (1.6f * width), 2.0f)) / (1.6f * width);
        specular = (.7f * longitudinal * fr + .3f * secondary * fr * base) *
                   sqrt(saturate(1.0f - dot(strand, l) * dot(strand, l))) * .25f;
    }
    const float sheen = model == 5 ? max(.35f, gDraw.optics.x)
                                   : gDraw.optics.x * (model == 7 ? 1.0f - skinFraction : 1.0f);
    if (sheen > 0) {
        // Charlie distribution and Neubelt cloth visibility.
        const float ir = 1.0f / max(.12f, rough), nh = saturate(dot(n, h));
        const float distribution =
            (2.0f + ir) * pow(max(.0001f, 1.0f - nh * nh), .5f * ir) / 6.2831853f;
        const float visibility = 1.0f / max(.04f, 4.0f * (nl + nv - nl * nv));
        diffuse *= 1.0f - .25f * sheen;
        specular += sqrt(max(base, float3(0, 0, 0))) * sheen * (distribution * visibility * nl);
    }
    float3 response = diffuse + specular;
    const float coat = gDraw.lobes.y;
    if (coat > 0) {
        const float fcV = .04f + .96f * pow(1.0f - saturate(dot(coatNormal, v)), 5.0f);
        const float fcL = .04f + .96f * pow(1.0f - saturate(dot(coatNormal, l)), 5.0f);
        response *= (1.0f - coat * fcV) * (1.0f - coat * fcL);
        response += coat * EllipticalSpecular(coatNormal, tangent, v, l, gDraw.lobes.z, 0,
                                              float3(.04f, .04f, .04f));
    }
    return response;
}
#endif
