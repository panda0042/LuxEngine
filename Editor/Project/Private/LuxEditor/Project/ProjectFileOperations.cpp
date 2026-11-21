#include <LuxEditor/Project/PathEncoding.hpp>
#include <LuxEditor/Project/ProjectFileOperations.hpp>
#include <algorithm>

namespace LuxEditor {
bool IsExcludedProjectPath(const std::filesystem::path& relative) {
    for (auto part = relative.begin(); part != relative.end(); ++part) {
        auto name = PathUtf8(*part);
        std::transform(name.begin(), name.end(), name.begin(), [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
        });
        if (name == ".lux" || name == ".git" ||
            (part == relative.begin() &&
             (name == "build" || name == "out" || name.starts_with("cmake-build"))))
            return true;
    }
    return false;
}

void RenameProjectPath(const std::filesystem::path& source,
                       const std::filesystem::path& destination, std::error_code& error) {
#ifdef _WIN32
    for (const int delay : {10, 20, 40, 80, 160, 200}) {
        std::filesystem::rename(source, destination, error);
        if (!error || (error.value() != 5 && error.value() != 32 && error.value() != 33))
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
#endif
    std::filesystem::rename(source, destination, error);
}
} // namespace LuxEditor
