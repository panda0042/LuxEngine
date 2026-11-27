#pragma once

#include <LuxEngine/Core/Math.hpp>
#include <LuxUI/Widgets.hpp>
#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace LuxEngine {
struct SpriteRendererComponent;
}

namespace LuxEditor {
LuxUI::Rect SpriteFrameRect(int width, int height,
                            const LuxEngine::SpriteRendererComponent& sprite);

class SpriteImageCache {
public:
    bool Draw(LuxUI::Painter& painter, const std::string& path,
              const LuxEngine::SpriteRendererComponent& sprite, const LuxEngine::Vec4& tint,
              const LuxUI::PolygonF& destination);
    void Clear();

private:
    struct Entry {
        std::filesystem::file_time_type modified;
        std::uintmax_t size;
        LuxEngine::uint64 revision;
        LuxUI::Image image;
        std::chrono::steady_clock::time_point retryAfter;
    };

    LuxUI::Image Read(const std::string& path);
    std::unordered_map<std::string, Entry> entries_;
    std::size_t bytes_ = 0;
};
} // namespace LuxEditor
