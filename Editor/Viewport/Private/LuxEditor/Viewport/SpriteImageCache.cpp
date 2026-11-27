#include <LuxEditor/Viewport/SpriteImageCache.hpp>
#include <LuxEngine/Assets/AssetManager.hpp>
#include <LuxEngine/ECS/ECS.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace LuxEditor {
namespace {
std::pair<int, int> SpriteCell(int size, LuxEngine::uint32 cell, LuxEngine::uint32 count) {
    const auto start = static_cast<int>(std::ceil(double(cell) * size / count - .5));
    const auto end = static_cast<int>(std::ceil(double(cell + 1) * size / count - .5));
    const int first = std::clamp(start, 0, size - 1);
    return {first, std::clamp(end, first + 1, size)};
}
} // namespace

LuxUI::Rect SpriteFrameRect(int width, int height,
                            const LuxEngine::SpriteRendererComponent& sprite) {
    if (width <= 0 || height <= 0 || !sprite.columns || !sprite.rows || sprite.columns > 4096 ||
        sprite.rows > 4096 || sprite.frame >= sprite.columns * sprite.rows)
        return {};
    const auto [left, right] = SpriteCell(width, sprite.frame % sprite.columns, sprite.columns);
    const auto [top, bottom] = SpriteCell(height, sprite.frame / sprite.columns, sprite.rows);
    return {left, top, right - left, bottom - top};
}

void SpriteImageCache::Clear() {
    entries_.clear();
    bytes_ = 0;
}

LuxUI::Image SpriteImageCache::Read(const std::string& path) {
    if (path.empty())
        return {};
    const std::filesystem::path file(
        std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
    std::error_code error;
    const auto modified = std::filesystem::last_write_time(file, error);
    const auto size = error ? 0 : std::filesystem::file_size(file, error);
    const auto revision = error ? 0 : LuxEngine::AssetManager::GetTextureRevision(path);
    const auto now = std::chrono::steady_clock::now();
    if (const auto found = entries_.find(path); found != entries_.end()) {
        const auto& entry = found->second;
        if (!error && entry.modified == modified && entry.size == size &&
            entry.revision == revision && (!entry.image.isNull() || now < entry.retryAfter))
            return entry.image;
        bytes_ -= static_cast<std::size_t>(entry.image.sizeInBytes());
        entries_.erase(found);
    }
    if (error)
        return {};
    int width = 0, height = 0;
    std::vector<LuxEngine::uint8> pixels;
    LuxUI::Image image;
    if (LuxEngine::AssetManager::DecodeTextureCpu(path, width, height, pixels))
        image = LuxUI::Image(pixels.data(), width, height, LuxUI::Image::Format_RGBA8888).copy();
    constexpr std::size_t budget = 64 * 1024 * 1024;
    const auto bytes = static_cast<std::size_t>(image.sizeInBytes());
    if (bytes <= budget) {
        if (entries_.size() >= 256 || bytes > budget - bytes_)
            Clear();
        entries_.emplace(path,
                         Entry{modified, size, revision, image, now + std::chrono::seconds(1)});
        bytes_ += bytes;
    }
    return image;
}

bool SpriteImageCache::Draw(LuxUI::Painter& painter, const std::string& path,
                            const LuxEngine::SpriteRendererComponent& sprite,
                            const LuxEngine::Vec4& tint, const LuxUI::PolygonF& destination) {
    if (destination.size() != 4 ||
        !std::all_of(destination.begin(), destination.end(), [](const auto& point) {
            return std::isfinite(point.x()) && std::isfinite(point.y());
        }))
        return false;
    const std::array<float, 4> channels{tint.x, tint.y, tint.z, tint.w};
    if (!std::all_of(channels.begin(), channels.end(),
                     [](float value) { return std::isfinite(value); }))
        return false;
    auto image = Read(path);
    auto source = SpriteFrameRect(image.width(), image.height(), sprite);
    if (source.isEmpty())
        return false;
    LuxUI::PolygonF unit;
    unit << LuxUI::PointF(0, 0) << LuxUI::PointF(1, 0) << LuxUI::PointF(1, 1)
         << LuxUI::PointF(0, 1);
    LuxUI::Transform projection;
    if (!LuxUI::Transform::quadToQuad(unit, destination, projection))
        return false;
    if (!std::all_of(channels.begin(), channels.end(), [](float value) { return value == 1; })) {
        image = image.copy(source);
        if (image.isNull())
            return false;
        source = image.rect();
        for (int y = 0; y < image.height(); ++y) {
            auto* row = image.scanLine(y);
            for (int x = 0; x < image.width(); ++x)
                for (std::size_t c = 0; c < channels.size(); ++c) {
                    auto& value = row[static_cast<std::size_t>(x) * 4 + c];
                    value = static_cast<unsigned char>(
                        std::clamp(float(value) * channels[c], 0.f, 255.f) + .5f);
                }
        }
    }
    painter.save();
    painter.setRenderHint(LuxUI::Painter::SmoothPixmapTransform);
    painter.setWorldTransform(projection, true);
    painter.drawImage(LuxUI::RectF(0, 0, 1, 1), image, LuxUI::RectF(source));
    painter.restore();
    return true;
}
} // namespace LuxEditor
