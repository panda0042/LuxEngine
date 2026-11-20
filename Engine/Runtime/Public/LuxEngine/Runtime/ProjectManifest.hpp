#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace LuxEngine {
struct ProjectManifestData {
    std::string name, engineVersion, projectVersion, templateName, startScene, icon;
    unsigned autosaveSeconds = 0, backupLimit = 0;
    bool snapToGrid = false;
    std::vector<std::pair<std::string, std::string>> dependencies;
};

bool ParseProjectManifestJson(std::string_view data, ProjectManifestData& manifest);
std::string_view ProjectManifestContent(std::string_view data);
bool IsValidProjectDependency(std::string_view package, std::string_view version);
bool IsValidProjectDependencyVersion(std::string_view version);

// Resolves the declared start scene inside a packaged project's content directory.
// Reads JSON project schema 1 and legacy LUXPROJECT versions 1–2.
bool ReadProjectStartScene(const std::filesystem::path& contentRoot,
                           std::filesystem::path& sceneFile, std::string& error);
} // namespace LuxEngine
