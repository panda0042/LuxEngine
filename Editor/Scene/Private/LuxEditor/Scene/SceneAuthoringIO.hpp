#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace LuxEngine {
class Scene;
}

namespace LuxEditor {

// Reads cached authored data without requiring referenced files to exist or compose successfully.
std::shared_ptr<LuxEngine::Scene> ReadAuthoredScene(const std::filesystem::path& path,
                                                    std::string& error);
std::optional<std::string> ReadSceneFileRevision(const std::filesystem::path& path,
                                                 std::string& error);
using AuthoredPathMapper = std::function<std::filesystem::path(const std::filesystem::path&)>;
bool SerializeAuthoredScene(LuxEngine::Scene& scene, const std::filesystem::path& owner,
                            std::string& data, std::string& error,
                            const AuthoredPathMapper& mapPath = {});
// Rebases references in a temporary copy and atomically saves it, preserving the live scene.
bool SaveAuthoredScene(LuxEngine::Scene& scene, const std::filesystem::path& destination,
                       const std::filesystem::path& previous, std::string& error,
                       bool rebaseLive = false, std::string* diskRevision = nullptr);
bool NormalizeAuthoredScenePaths(LuxEngine::Scene& scene, const std::filesystem::path& owner,
                                 std::string& error);

} // namespace LuxEditor
