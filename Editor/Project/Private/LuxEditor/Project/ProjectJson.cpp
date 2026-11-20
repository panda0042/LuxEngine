#include <LuxEditor/Project/PathEncoding.hpp>
#include <LuxEditor/Project/ProjectJson.hpp>
#include <LuxEngine/Runtime/ProjectManifest.hpp>
#include <nlohmann/json.hpp>

namespace LuxEditor {
namespace {
using Json = nlohmann::ordered_json;

} // namespace

std::string SerializeProjectJson(const EditorProject& project, const std::filesystem::path& scene) {
    try {
        const auto& settings = project.GetSettings();
        Json dependencies = Json::array();
        for (const auto& dependency : settings.dependencies)
            dependencies.push_back(
                {{"package", dependency.package}, {"version", dependency.version}});
        Json root = {{"format", "LuxEngine.Project"},
                     {"version", 1},
                     {"name", project.GetName()},
                     {"engine_version", project.GetEngineVersion()},
                     {"project_version", settings.version},
                     {"template", ProjectTemplateName(project.GetTemplate())},
                     {"start_scene", GenericPathUtf8(scene)},
                     {"settings",
                      {{"icon", GenericPathUtf8(settings.icon)},
                       {"autosave_seconds", settings.autosaveSeconds},
                       {"backup_limit", settings.backupLimit},
                       {"snap_to_grid", settings.snapToGrid}}},
                     {"dependencies", std::move(dependencies)}};
        auto result = root.dump(2) + '\n';
        return result.size() <= 65536 ? result : std::string{};
    } catch (const std::exception&) {
        return {};
    }
}

bool DeserializeProjectJson(std::string_view data, ProjectSettings& settings,
                            std::string& engineVersion, std::string& templateName,
                            std::string& scene) {
    LuxEngine::ProjectManifestData manifest;
    if (!LuxEngine::ParseProjectManifestJson(data, manifest))
        return false;
    settings.name = std::move(manifest.name);
    engineVersion = std::move(manifest.engineVersion);
    settings.version = std::move(manifest.projectVersion);
    templateName = std::move(manifest.templateName);
    scene = std::move(manifest.startScene);
    settings.icon = PathFromUtf8(manifest.icon);
    settings.autosaveSeconds = manifest.autosaveSeconds;
    settings.backupLimit = manifest.backupLimit;
    settings.snapToGrid = manifest.snapToGrid;
    settings.dependencies.clear();
    for (auto& [package, version] : manifest.dependencies)
        settings.dependencies.push_back({std::move(package), std::move(version)});
    return true;
}
} // namespace LuxEditor
