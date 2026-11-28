#include <LuxEngine/Renderer/RHI/Texture.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace LuxEngine;

namespace {
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void Dimensions() {
    for (const auto dimensions : {std::array<uint32, 2>{1, 1}, {1, 9}, {13, 1}, {7, 5}, {16, 8}}) {
        const auto [width, height] = dimensions;
        std::vector<uint8> pixels(static_cast<size_t>(width) * height * 4, 127);
        std::vector<TextureMipLevel> levels;
        Check(BuildTextureMipChain(width, height, pixels, TextureColorSpace::Linear, false, levels),
              "Valid texture was rejected");
        uint32 w = width, h = height;
        for (const auto& level : levels) {
            Check(level.width == w && level.height == h &&
                      level.pixels.size() == static_cast<size_t>(w) * h * 4,
                  "Mip dimensions or byte budget are wrong");
            for (const auto value : level.pixels)
                Check(value == 127, "Constant image changed during downsampling");
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
        }
        Check(levels.back().width == 1 && levels.back().height == 1,
              "Mip chain omitted the final level");
    }
}

void Colour() {
    std::vector<uint8> pixels{0, 0, 0, 255, 255, 255, 255, 255};
    std::vector<TextureMipLevel> levels;
    Check(BuildTextureMipChain(2, 1, pixels, TextureColorSpace::Srgb, false, levels),
          "sRGB texture was rejected");
    Check(levels[1].pixels[0] >= 187 && levels[1].pixels[0] <= 189,
          "sRGB mip was averaged in encoded space");
    Check(BuildTextureMipChain(2, 1, pixels, TextureColorSpace::Linear, false, levels),
          "Linear texture was rejected");
    Check(levels[1].pixels[0] == 128, "Data map did not use linear averaging");
    pixels = {255, 0, 0, 255, 0, 0, 255, 0};
    Check(BuildTextureMipChain(2, 1, pixels, TextureColorSpace::Srgb, false, levels),
          "Alpha texture was rejected");
    Check(levels[1].pixels[0] == 255 && levels[1].pixels[2] == 0 && levels[1].pixels[3] == 128,
          "Transparent texels contaminated the colour mip");
    pixels = {0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 255};
    Check(BuildTextureMipChain(3, 1, pixels, TextureColorSpace::Linear, false, levels),
          "Odd texture was rejected");
    Check(levels[1].pixels[0] == 85, "Odd-size downsampling lost the final texel");
}

void Normals() {
    const std::array<uint8, 8> pixels{255, 128, 128, 255, 128, 255, 128, 255};
    std::vector<TextureMipLevel> levels;
    Check(BuildTextureMipChain(2, 1, pixels, TextureColorSpace::Linear, true, levels),
          "Normal map was rejected");
    const auto& normal = levels.back().pixels;
    const float x = normal[0] / 127.5f - 1.f, y = normal[1] / 127.5f - 1.f,
                z = normal[2] / 127.5f - 1.f;
    Check(std::abs(std::sqrt(x * x + y * y + z * z) - 1.f) < .015f,
          "Normal mip was not normalized");
    Check(normal[0] > 210 && normal[1] > 210, "Normal mip orientation is wrong");
    Check(BuildTextureMipChain(2, 1, pixels, TextureColorSpace::Linear, true, levels, 0,
                               TextureMipSemantic::NormalVariance) &&
              std::abs(int(levels.back().pixels[3]) - 181) <= 1,
          "mean normal length must retain subpixel variance");
    const std::array<uint8, 8> roughness{0, 0, 0, 255, 255, 255, 255, 255};
    Check(BuildTextureMipChain(2, 1, roughness, TextureColorSpace::Linear, false, levels, 0,
                               TextureMipSemantic::PerceptualRoughness) &&
              std::abs(int(levels.back().pixels[0]) - 214) <= 1,
          "roughness mips must average GGX slope variance in linear space");
}

void AlphaMaskCoverage() {
    const std::array<uint8, 16> pixels{255, 0, 0, 192, 0, 0, 255, 64, 0, 0, 255, 64, 0, 0, 255, 64};
    std::vector<TextureMipLevel> levels;
    Check(BuildTextureMipChain(4, 1, pixels, TextureColorSpace::Srgb, false, levels, .5f),
          "Alpha mask mip chain was rejected");
    Check(std::equal(pixels.begin(), pixels.end(), levels.front().pixels.begin()),
          "Mask mip generation changed the original texture");
    Check(levels.back().pixels == std::vector<uint8>({255, 0, 0, 64}),
          "Minified mask did not preserve coverage and covered texel color");
    Check(BuildTextureMipChain(4, 1, pixels, TextureColorSpace::Srgb, false, levels, .2f) &&
              levels.back().pixels[3] == 255,
          "Mask coverage ignored the material cutoff");

    // Irregular 13x7 footprints must preserve the integral even at 3x1 and 1x1.
    std::vector<uint8> thin(13 * 7 * 4, 0);
    unsigned covered = 0;
    for (unsigned y = 0; y < 7; ++y)
        for (unsigned x = 0; x < 13; ++x) {
            const bool leaf = (x + y * 2) % 5 == 0;
            thin[4 * (y * 13 + x) + 3] = leaf ? 191 : 48;
            covered += leaf;
        }
    Check(BuildTextureMipChain(13, 7, thin, TextureColorSpace::Srgb, false, levels, .5f),
          "Irregular alpha-coverage mips build");
    const double expected = double(covered) / 91.;
    for (size_t i = 1; i < levels.size(); ++i) {
        double total = 0;
        for (size_t a = 3; a < levels[i].pixels.size(); a += 4)
            total += levels[i].pixels[a] / 255.;
        Check(std::abs(total / (levels[i].width * levels[i].height) - expected) <= 1.5 / 255.,
              "A masked NPOT mip lost source coverage");
    }
}

void AtomicFailure() {
    std::vector<TextureMipLevel> levels{{1, 1, {1, 2, 3, 4}}};
    const std::array<uint8, 4> pixel{};
    Check(!BuildTextureMipChain(0, 1, pixel, TextureColorSpace::Linear, false, levels),
          "Zero dimension was accepted");
    Check(!BuildTextureMipChain(8193, 1, pixel, TextureColorSpace::Linear, false, levels),
          "Oversize dimension was accepted");
    Check(!BuildTextureMipChain(2, 1, pixel, TextureColorSpace::Linear, false, levels),
          "Truncated pixels were accepted");
    Check(!BuildTextureMipChain(1, 1, pixel, TextureColorSpace::Srgb, true, levels),
          "sRGB normal map was accepted");
    Check(levels.size() == 1 && levels[0].pixels == std::vector<uint8>({1, 2, 3, 4}),
          "Failed mip generation replaced the existing chain");
}
} // namespace

int main() {
    try {
        Dimensions();
        Colour();
        Normals();
        AlphaMaskCoverage();
        AtomicFailure();
        std::cout << "Texture mip dimensions, colour space, normals and failure contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
