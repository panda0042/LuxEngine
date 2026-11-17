#pragma once
#include <cstdlib>
#include <string_view>

namespace LuxEngine::Testing {
inline bool HasGpuDisplay() {
    const auto value = [](const char* name) {
        const char* text = std::getenv(name);
        return std::string_view(text ? text : "");
    };
    const auto driver = value("SDL_VIDEODRIVER");
    if (driver == "dummy" || driver == "offscreen")
        return false;
#if defined(__linux__)
    if (driver == "x11")
        return !value("DISPLAY").empty();
    if (driver == "wayland")
        return !value("WAYLAND_DISPLAY").empty();
    return !value("DISPLAY").empty() || !value("WAYLAND_DISPLAY").empty();
#else
    return true;
#endif
}
} // namespace LuxEngine::Testing
