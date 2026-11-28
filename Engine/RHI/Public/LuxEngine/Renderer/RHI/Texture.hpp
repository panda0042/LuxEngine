#pragma once

#include <LuxEngine/Core/Core.hpp>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace LuxEngine {
class GpuDevice;

namespace Rhi {
class RenderBackend;
}

// Linear is the default for smooth artwork; Nearest preserves pixel-art texel edges.
enum class TextureFilter { Linear, Nearest };
enum class TextureColorSpace { Linear, Srgb };

enum class TextureAddressMode { Clamp, Repeat };

// PerceptualRoughness averages the GGX slope-variance parameter r⁴. NormalVariance
// stores the length of the mean normal in alpha. Use Default for packed normal/foam maps.
enum class TextureMipSemantic { Default, NormalVariance, PerceptualRoughness };

struct TextureSampling {
    bool mipmaps = false;
    TextureAddressMode addressMode = TextureAddressMode::Clamp;
    bool anisotropic = false;
    bool normalMap = false;
    TextureMipSemantic mipSemantic = TextureMipSemantic::Default;
};

struct TextureMipLevel {
    uint32 width = 0;
    uint32 height = 0;
    std::vector<uint8> pixels;
};

// A positive mask cutoff stores the covered area in alpha from mip level 1 onward.
LUX_API bool BuildTextureMipChain(uint32 width, uint32 height, std::span<const uint8> pixels,
                                  TextureColorSpace colorSpace, bool normalMap,
                                  std::vector<TextureMipLevel>& output, float alphaMaskCutoff = 0,
                                  TextureMipSemantic semantic = TextureMipSemantic::Default);

class LUX_API GpuTexture {
public:
    GpuTexture();
    ~GpuTexture();

    GpuTexture(const GpuTexture&) = delete;
    GpuTexture& operator=(const GpuTexture&) = delete;
    GpuTexture(GpuTexture&& other) noexcept;
    GpuTexture& operator=(GpuTexture&& other) noexcept;

    bool Initialize(GpuDevice* device, uint32 width, uint32 height, const void* pixelData = nullptr,
                    TextureFilter filter = TextureFilter::Linear,
                    TextureColorSpace colorSpace = TextureColorSpace::Linear,
                    TextureSampling sampling = {}, float alphaMaskCutoff = 0);

    bool Initialize(std::nullptr_t, uint32, uint32, const void* = nullptr,
                    TextureFilter = TextureFilter::Linear,
                    TextureColorSpace = TextureColorSpace::Linear, TextureSampling = {},
                    float alphaMaskCutoff = 0);

    void Release();

    uint32 GetWidth() const;

    uint32 GetHeight() const;

    bool IsValid() const;
    bool BelongsTo(const GpuDevice& device) const;

    TextureColorSpace GetColorSpace() const noexcept;
    uint32 GetMipCount() const noexcept;
    size_t GetStorageBytes() const noexcept;

private:
    friend class GpuDevice;
    friend class Renderer2D;
    friend class Renderer3D;
    std::weak_ptr<Rhi::RenderBackend> owner_;
    uint64 handle_ = 0;
    uint32 width_ = 0;
    uint32 height_ = 0;
    uint32 mipCount_ = 0;
    size_t storageBytes_ = 0;
    bool normalVariance_ = false;
    float alphaMaskCutoff_ = 0;
    TextureColorSpace colorSpace_ = TextureColorSpace::Linear;
};

} // namespace LuxEngine
