#include "RHI/RenderBackend.hpp"
#include "TextureMipChain.hpp"
#include <LuxEngine/Renderer/RHI/GpuDevice.hpp>
#include <LuxEngine/Renderer/RHI/Texture.hpp>
#include <array>
#include <cmath>

namespace LuxEngine {

GpuTexture::~GpuTexture() {
    Release();
}

GpuTexture::GpuTexture(GpuTexture&& other) noexcept
    : owner_(std::move(other.owner_)), handle_(other.handle_), width_(other.width_),
      height_(other.height_), mipCount_(other.mipCount_), storageBytes_(other.storageBytes_),
      alphaMaskCutoff_(other.alphaMaskCutoff_), colorSpace_(other.colorSpace_) {
    other.owner_.reset();
    other.handle_ = 0;
    other.width_ = other.height_ = 0;
    other.colorSpace_ = TextureColorSpace::Linear;
}

GpuTexture& GpuTexture::operator=(GpuTexture&& other) noexcept {
    if (this != &other) {
        Release();
        owner_ = std::move(other.owner_);
        handle_ = other.handle_;
        width_ = other.width_;
        height_ = other.height_;
        colorSpace_ = other.colorSpace_;
        mipCount_ = other.mipCount_;
        storageBytes_ = other.storageBytes_;
        alphaMaskCutoff_ = other.alphaMaskCutoff_;
        other.owner_.reset();
        other.handle_ = 0;
        other.width_ = other.height_ = 0;
        other.colorSpace_ = TextureColorSpace::Linear;
    }
    return *this;
}

bool GpuTexture::Initialize(GpuDevice* device, uint32 width, uint32 height, const void* pixelData,
                            TextureFilter filter, TextureColorSpace colorSpace,
                            TextureSampling sampling, float alphaMaskCutoff) {
    if (!device || !device->IsReady())
        return false;
    const auto layout = Rhi::GetTextureLayout(width, height, sampling.mipmaps);
    if (!layout.mipCount || (filter != TextureFilter::Linear && filter != TextureFilter::Nearest) ||
        (colorSpace != TextureColorSpace::Linear && colorSpace != TextureColorSpace::Srgb) ||
        (sampling.normalMap && (colorSpace == TextureColorSpace::Srgb || alphaMaskCutoff != 0)) ||
        !std::isfinite(alphaMaskCutoff) || alphaMaskCutoff < 0 || alphaMaskCutoff > 1 ||
        (sampling.addressMode != TextureAddressMode::Clamp &&
         sampling.addressMode != TextureAddressMode::Repeat))
        return false;
    alphaMaskCutoff = std::ceil(alphaMaskCutoff * 255.f) / 255.f;
    std::array<const void*, Rhi::MaxTextureMipLevels> mipPixels{pixelData};
    std::vector<TextureMipLevel> tail;
    if (layout.mipCount > 1 && (pixelData || sampling.normalMap)) {
        const std::span<const uint8> basePixels(static_cast<const uint8*>(pixelData),
                                                pixelData ? size_t(width) * height * 4 : 0);
        if (!Rhi::BuildTextureMipTail(width, height, basePixels, colorSpace, sampling.normalMap,
                                      tail, alphaMaskCutoff))
            return false;
        for (size_t level = 0; level < tail.size(); ++level)
            mipPixels[level + 1] = tail[level].pixels.data();
    }
    uint64 handle = 0;
    if (!device->backend_->CreateTexture(width, height, {mipPixels.data(), layout.mipCount}, filter,
                                         colorSpace, handle))
        return false;
    Release();
    handle_ = handle;
    owner_ = device->backend_;
    width_ = width;
    height_ = height;
    colorSpace_ = colorSpace;
    mipCount_ = layout.mipCount;
    alphaMaskCutoff_ = layout.mipCount > 1 ? alphaMaskCutoff : 0;
    storageBytes_ = layout.bytes;
    return true;
}

void GpuTexture::Release() {
    if (auto owner = owner_.lock())
        owner->DestroyTexture(handle_);
    owner_.reset();
    handle_ = 0;
    width_ = height_ = 0;
    colorSpace_ = TextureColorSpace::Linear;
    mipCount_ = 0;
    storageBytes_ = 0;
    alphaMaskCutoff_ = 0;
}

bool GpuTexture::IsValid() const {
    auto owner = owner_.lock();
    return owner && owner->HasTexture(handle_);
}

bool GpuTexture::BelongsTo(const GpuDevice& device) const {
    auto owner = owner_.lock();
    return owner && owner == device.backend_ && owner->HasTexture(handle_);
}

} // namespace LuxEngine
