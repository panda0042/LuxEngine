#ifndef LUX_SUBSURFACE_H
#define LUX_SUBSURFACE_H

// Locally cylindrical diffusion approximation. Integrate incident diffuse light
// along the curved surface with a two-sided exponential profile per wavelength.
float3 SubsurfaceDiffuse(float3 base, float metal, float3 n, float3 l, float curvature,
                         float thickness) {
    const float3 radius = gDraw.foliage.yzw;
    const float cosine = clamp(dot(n, l), -1.0f, 1.0f);
    const float sine = sqrt(saturate(1.0f - cosine * cosine));
    float3 diffuse = float3(0, 0, 0);
    for (int i = 0; i < 4; ++i) {
        // Midpoint quantiles of the scattering distance distribution.
        const float distance = -log(1.0f - (float(i) + .5f) * .25f);
        const float3 arc = min(radius * curvature * distance, 1.5708f);
        const float3 center = cosine * cos(arc);
        const float3 offset = sine * sin(arc);
        diffuse += .125f * (max(center + offset, 0.0f) + max(center - offset, 0.0f));
    }

    if (thickness > 0.0f)
        diffuse += max(-cosine, 0.0f) * exp(-thickness / max(radius * abs(cosine), .000001f));
    // Replace only the Lambert term already evaluated by the PBR material.
    return (diffuse - max(cosine, 0.0f)) * base * ((1.0f - metal) * .96f / 3.141593f);
}
#endif
