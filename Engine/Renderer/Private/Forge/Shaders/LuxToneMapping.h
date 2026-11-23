#ifndef LUX_TONE_MAPPING_H
#define LUX_TONE_MAPPING_H

// ACES fitted SDR transform by Stephen Hill, from Baking Lab (MIT).
// See docs/licenses/LICENSE-BakingLab.txt for the license.
float3 AcesFitted(float3 linearColor) {
    // Explicit row products give the same result on each FSL matrix convention.
    float3 aces = float3(dot(linearColor, float3(0.59719f, 0.35458f, 0.04823f)),
                         dot(linearColor, float3(0.07600f, 0.90834f, 0.01566f)),
                         dot(linearColor, float3(0.02840f, 0.13383f, 0.83777f)));
    const float3 numerator = aces * (aces + 0.0245786f) - 0.000090537f;
    const float3 denominator = aces * (0.983729f * aces + 0.4329510f) + 0.238081f;
    aces = numerator / denominator;
    return saturate(float3(dot(aces, float3(1.60475f, -0.53108f, -0.07367f)),
                           dot(aces, float3(-0.10208f, 1.10813f, -0.00605f)),
                           dot(aces, float3(-0.00327f, -0.07276f, 1.07602f))));
}

float3 ToneMapSdr(float3 color, float exposure, float aces) {
    color = clamp(color * exposure, 0.0f, 65504.0f);
    color = aces > 0.5f ? AcesFitted(color) : saturate(color);
    return lerp(12.92f * color,
                1.055f * pow(color, float3(1.0f / 2.4f, 1.0f / 2.4f, 1.0f / 2.4f)) - 0.055f,
                step(0.0031308f, color));
}

#endif
