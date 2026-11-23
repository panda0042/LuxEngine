#pragma once

#include <LuxEngine/Renderer/RHI/Texture.hpp>
#include <algorithm>
#include <bit>

namespace LuxEngine::Rhi {

inline constexpr uint32 MaxMipTextureDimension = 8192;
inline constexpr auto MaxTextureMipLevels = std::bit_width(MaxMipTextureDimension);

struct TextureLayout {
    uint32 mipCount = 0;
    size_t bytes = 0;
};

constexpr TextureLayout GetTextureLayout(uint32 width, uint32 height, bool mipmaps) {
    const uint32 limit = mipmaps ? MaxMipTextureDimension : 16384;
    if (!width || !height || width > limit || height > limit)
        return {};
    if (!mipmaps || (width == 1 && height == 1))
        return {1, static_cast<size_t>(std::max(2u, width)) * std::max(2u, height) * 4};

    TextureLayout layout;
    for (;;) {
        ++layout.mipCount;
        layout.bytes += static_cast<size_t>(width) * height * 4;
        if (width == 1 && height == 1)
            return layout;
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }
}

bool BuildTextureMipTail(uint32 width, uint32 height, std::span<const uint8> pixels,
                         TextureColorSpace colorSpace, bool normalMap,
                         std::vector<TextureMipLevel>& output, float alphaMaskCutoff);

} // namespace LuxEngine::Rhi
