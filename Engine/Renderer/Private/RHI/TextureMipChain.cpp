#include "TextureMipChain.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>

namespace LuxEngine {
namespace {
float Decode(float value) {
    return value <= .04045f ? value / 12.92f : std::pow((value + .055f) / 1.055f, 2.4f);
}

const auto SrgbToLinear = [] {
    std::array<float, 256> table{};
    for (size_t index = 0; index < table.size(); ++index)
        table[index] = Decode(static_cast<float>(index) / 255.f);
    return table;
}();

float Encode(float value) {
    return value <= .0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.f / 2.4f) - .055f;
}

uint8 Byte(float value) {
    return static_cast<uint8>(std::lround(std::clamp(value, 0.f, 1.f) * 255.f));
}
} // namespace

bool Rhi::BuildTextureMipTail(uint32 width, uint32 height, std::span<const uint8> pixels,
                              TextureColorSpace colorSpace, bool normalMap,
                              std::vector<TextureMipLevel>& output, float alphaMaskCutoff,
                              TextureMipSemantic semantic) {
    const bool variance = semantic == TextureMipSemantic::NormalVariance;
    const bool roughness = semantic == TextureMipSemantic::PerceptualRoughness;
    const auto layout = GetTextureLayout(width, height, true);
    if (!layout.mipCount || (!pixels.empty() && pixels.size() != size_t(width) * height * 4) ||
        (colorSpace != TextureColorSpace::Linear && colorSpace != TextureColorSpace::Srgb) ||
        (semantic != TextureMipSemantic::Default && !variance && !roughness) ||
        (variance && !normalMap) || (roughness && normalMap) ||
        ((variance || roughness) &&
         (colorSpace != TextureColorSpace::Linear || alphaMaskCutoff != 0)) ||
        (normalMap && (colorSpace == TextureColorSpace::Srgb || alphaMaskCutoff != 0)) ||
        !std::isfinite(alphaMaskCutoff) || alphaMaskCutoff < 0 || alphaMaskCutoff > 1)
        return false;
    try {
        std::vector<TextureMipLevel> levels;
        levels.reserve(layout.mipCount - 1);
        constexpr std::array<uint8, 4> blank{};
        const bool alphaWeighted = colorSpace == TextureColorSpace::Srgb || alphaMaskCutoff > 0;
        while (width > 1 || height > 1) {
            const bool sourceIsBase = levels.empty();
            const bool fromMask = levels.empty() && alphaMaskCutoff > 0;
            TextureMipLevel target{std::max(1u, width / 2), std::max(1u, height / 2), {}};
            target.pixels.resize(static_cast<size_t>(target.width) * target.height * 4);
            for (uint32 y = 0; y < target.height; ++y) {
                const float top = static_cast<float>(y) * height / target.height;
                const float bottom = static_cast<float>(y + 1) * height / target.height;
                for (uint32 x = 0; x < target.width; ++x) {
                    const float left = static_cast<float>(x) * width / target.width;
                    const float right = static_cast<float>(x + 1) * width / target.width;
                    float sum[4] = {}, weight = 0, alphaWeight = 0;
                    for (uint32 sy = static_cast<uint32>(top);
                         sy < static_cast<uint32>(std::ceil(bottom)); ++sy) {
                        for (uint32 sx = static_cast<uint32>(left);
                             sx < static_cast<uint32>(std::ceil(right)); ++sx) {
                            const float area = (std::min(right, static_cast<float>(sx + 1)) -
                                                std::max(left, static_cast<float>(sx))) *
                                               (std::min(bottom, static_cast<float>(sy + 1)) -
                                                std::max(top, static_cast<float>(sy)));
                            const auto* pixel = pixels.empty()
                                                    ? blank.data()
                                                    : &pixels[(size_t(sy) * width + sx) * 4];
                            const float sourceAlpha = pixel[3] / 255.f;
                            const float alpha =
                                fromMask ? float(sourceAlpha >= alphaMaskCutoff) : sourceAlpha;
                            for (unsigned channel = 0; channel < 3; ++channel) {
                                float value = pixel[channel] / 255.f;
                                if (normalMap) {
                                    value = value * 2.f - 1.f;
                                    if (variance && !sourceIsBase)
                                        value *= sourceAlpha; // Restore length before the next area
                                                              // average.
                                } else if (roughness) {
                                    value *= value;
                                    value *= value;
                                } else if (colorSpace == TextureColorSpace::Srgb)
                                    value = SrgbToLinear[pixel[channel]] * alpha;
                                else if (alphaWeighted)
                                    value *= alpha;
                                sum[channel] += value * area;
                            }
                            sum[3] += alpha * area;
                            alphaWeight += alpha * area;
                            weight += area;
                        }
                    }
                    auto* pixel = &target.pixels[(static_cast<size_t>(y) * target.width + x) * 4];
                    const float length =
                        normalMap ? std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2])
                                  : 0.f;
                    for (unsigned channel = 0; channel < 3; ++channel) {
                        float value = sum[channel] / weight;
                        if (normalMap)
                            value = length > 1e-8f ? sum[channel] / length * .5f + .5f
                                                   : (channel == 2 ? 1.f : .5f);
                        else if (roughness)
                            value = std::sqrt(std::sqrt(std::max(0.f, value)));
                        else if (alphaWeighted) {
                            value = alphaWeight > 1e-8f ? sum[channel] / alphaWeight : 0.f;
                            if (colorSpace == TextureColorSpace::Srgb)
                                value = Encode(value);
                        }
                        pixel[channel] = Byte(value);
                    }
                    pixel[3] = Byte(variance ? length / weight : sum[3] / weight);
                }
            }
            levels.push_back(std::move(target));
            width = levels.back().width;
            height = levels.back().height;
            pixels = levels.back().pixels;
        }
        output = std::move(levels);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool BuildTextureMipChain(uint32 width, uint32 height, std::span<const uint8> pixels,
                          TextureColorSpace colorSpace, bool normalMap,
                          std::vector<TextureMipLevel>& output, float alphaMaskCutoff,
                          TextureMipSemantic semantic) {
    std::vector<TextureMipLevel> levels;
    if (pixels.empty() || !Rhi::BuildTextureMipTail(width, height, pixels, colorSpace, normalMap,
                                                    levels, alphaMaskCutoff, semantic))
        return false;
    try {
        levels.insert(levels.begin(), {width, height, {pixels.begin(), pixels.end()}});
        if (semantic == TextureMipSemantic::NormalVariance)
            for (size_t i = 3; i < levels.front().pixels.size(); i += 4)
                levels.front().pixels[i] = 255;
        output = std::move(levels);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
} // namespace LuxEngine
