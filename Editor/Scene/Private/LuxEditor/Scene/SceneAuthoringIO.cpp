#include <LuxEditor/Project/PathEncoding.hpp>
#include <LuxEditor/Scene/SceneAuthoringIO.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxUI/Widgets.hpp>
#include <optional>
#include <stdexcept>

namespace LuxEditor {
namespace {
namespace fs = std::filesystem;

LuxUI::String FileName(const fs::path& path) {
    const auto utf8 = PathUtf8(path);
    return LuxUI::String::fromUtf8(utf8.data(), static_cast<std::ptrdiff_t>(utf8.size()));
}

std::string RevisionHash(const LuxUI::CryptographicHash& hash) {
    return hash.result().toHex().toStdString();
}

fs::path CanonicalPath(const fs::path& path) {
    if (path.empty() || path.native().find(fs::path::value_type{}) != fs::path::string_type::npos)
        throw std::runtime_error("씬 파일 경로가 비어 있습니다.");
    std::error_code error;
    const auto absolute = fs::absolute(path, error);
    if (error)
        throw std::runtime_error("씬 파일의 절대 경로를 확인할 수 없습니다.");
    const auto canonical = fs::weakly_canonical(absolute, error);
    if (error)
        throw std::runtime_error("씬 파일 경로를 확인할 수 없습니다.");
    return canonical;
}

struct PathEdits {
    std::optional<LuxEngine::SceneSettings> settings;
    std::vector<std::pair<std::string*, std::string>> fields;

    void Apply(LuxEngine::Scene& scene) {
        if (settings && !scene.SetSettings(*settings))
            throw std::runtime_error("Invalid rebased environment path");
        for (auto& [field, value] : fields)
            field->swap(value);
    }
};

void RebaseSceneReferences(LuxEngine::Scene& scene, const fs::path& owner,
                           const fs::path* destination, std::uintmax_t& remaining, size_t depth = 0,
                           PathEdits* edits = nullptr, const AuthoredPathMapper& mapPath = {}) {
    if (depth > 16)
        throw std::runtime_error("씬 참조 스냅샷의 중첩 한도를 넘었습니다.");
    const auto resolve = [&](const std::string& resource) {
        auto path = PathFromUtf8(resource);
        if (path.is_relative())
            path = owner.parent_path() / path;
        const auto source = CanonicalPath(path);
        const auto absolute = mapPath ? mapPath(source) : source;
        const auto relative =
            destination ? absolute.lexically_relative(destination->parent_path()) : fs::path{};
        return GenericPathUtf8(destination && !relative.empty() ? relative : absolute);
    };
    const auto replace = [&](std::string& field, std::string value) {
        if (field == value)
            return;
        if (edits)
            edits->fields.emplace_back(&field, std::move(value));
        else
            field = std::move(value);
    };
    const auto rebase = [&](std::string& resource) {
        if (!resource.empty())
            replace(resource, resolve(resource));
    };
    const auto& currentSettings = scene.GetSettings();
    if (!currentSettings.render3D.environmentPath.empty()) {
        auto environment = resolve(currentSettings.render3D.environmentPath);
        if (environment != currentSettings.render3D.environmentPath) {
            auto settings = currentSettings;
            settings.render3D.environmentPath = std::move(environment);
            if (edits)
                edits->settings = std::move(settings);
            else if (!scene.SetSettings(settings))
                throw std::runtime_error("Invalid rebased environment path");
        }
    }
    for (auto entity : scene.GetEntities()) {
        if (auto* value = entity.TryGetComponent<LuxEngine::SpriteRendererComponent>())
            rebase(value->texturePath);
        if (auto* value = entity.TryGetComponent<LuxEngine::AnimatorComponent>())
            rebase(value->controllerPath);
        if (auto* value = entity.TryGetComponent<LuxEngine::ModelAnimationComponent>())
            rebase(value->modelPath);
        if (auto* value = entity.TryGetComponent<LuxEngine::AnimationComponent>()) {
            rebase(value->assetPath);
            rebase(value->clip.modelPath);
            rebase(value->clip.spriteSheet);
        }
        if (auto* mesh = entity.TryGetComponent<LuxEngine::MeshRendererComponent>())
            for (auto* resource :
                 {&mesh->meshPath, &mesh->material.baseColorTexture, &mesh->material.normalTexture,
                  &mesh->material.metallicTexture, &mesh->material.roughnessTexture,
                  &mesh->material.occlusionTexture})
                rebase(*resource);

        auto* reference = entity.TryGetComponent<LuxEngine::SceneReferenceComponent>();
        if (!reference)
            continue;
        replace(reference->path, resolve(reference->path));
        for (std::string* data : {&reference->baseline, &reference->suspended}) {
            if (data->empty())
                continue;
            if (data->size() > remaining)
                throw std::runtime_error("씬 참조 스냅샷이 128 MiB 한도를 넘었습니다.");
            remaining -= data->size();
            LuxEngine::Scene snapshot;
            if (!snapshot.DeserializeFromString(*data))
                throw std::runtime_error("저장된 씬 참조 상태가 손상되었습니다.");
            RebaseSceneReferences(snapshot, owner, destination, remaining, depth + 1, nullptr,
                                  mapPath);
            std::string rebased;
            if (!snapshot.SerializeToString(rebased))
                throw std::runtime_error("씬 참조 경로를 저장할 수 없습니다.");
            replace(*data, std::move(rebased));
        }
    }
}

std::shared_ptr<LuxEngine::Scene> CloneAuthoredScene(LuxEngine::Scene& scene) {
    std::string data;
    auto copy = std::make_shared<LuxEngine::Scene>();
    if (!scene.SerializeToString(data) || !copy->DeserializeFromString(data))
        throw std::runtime_error("씬 상태를 복사할 수 없습니다.");
    return copy;
}
} // namespace

std::optional<std::string> ReadSceneFileRevision(const fs::path& path, std::string& error) {
    std::error_code statusError;
    const auto status = fs::status(path, statusError);
    if (status.type() == fs::file_type::not_found &&
        (!statusError || statusError == std::errc::no_such_file_or_directory))
        return std::string{};
    LuxUI::File file(FileName(path));
    constexpr auto limit = 64ll * 1024 * 1024;
    if (statusError || !fs::is_regular_file(status) || !file.open(LuxUI::File::ReadOnly) ||
        file.size() > limit) {
        error = "씬 파일의 현재 상태를 확인할 수 없습니다: " + PathUtf8(path);
        return std::nullopt;
    }
    LuxUI::CryptographicHash hash(LuxUI::CryptographicHash::Sha256);
    long long total = 0;
    while (!file.atEnd()) {
        const auto bytes = file.read(64 * 1024);
        total += bytes.size();
        if (bytes.isEmpty() || total > limit) {
            error = "씬 파일의 현재 상태를 확인할 수 없습니다: " + PathUtf8(path);
            return std::nullopt;
        }
        hash.addData(bytes);
    }
    return RevisionHash(hash);
}

std::shared_ptr<LuxEngine::Scene> ReadAuthoredScene(const fs::path& path, std::string& error) {
    error.clear();
    try {
        const auto origin = CanonicalPath(path);
        auto authored = std::make_shared<LuxEngine::Scene>();
        if (!authored->Deserialize(GenericPathUtf8(origin))) {
            error = "씬 파일 형식이 올바르지 않거나 파일을 읽을 수 없습니다.";
            return {};
        }
        try {
            auto normalized = CloneAuthoredScene(*authored);
            std::uintmax_t remaining = 128 * 1024 * 1024;
            RebaseSceneReferences(*normalized, origin, nullptr, remaining);
            authored = std::move(normalized);
        } catch (const std::exception&) {
            // Keep damaged embedded snapshots intact so authored data remains available to repair.
        }
        authored->SetSourcePath(GenericPathUtf8(origin));
        return authored;
    } catch (const std::exception& failure) {
        error = failure.what();
        return {};
    }
}

bool SerializeAuthoredScene(LuxEngine::Scene& scene, const fs::path& owner, std::string& data,
                            std::string& error, const AuthoredPathMapper& mapPath) {
    try {
        auto copy = CloneAuthoredScene(scene);
        const auto target = CanonicalPath(owner);
        std::uintmax_t remaining = 128 * 1024 * 1024;
        RebaseSceneReferences(*copy, target, &target, remaining, 0, nullptr, mapPath);
        if (copy->SerializeToString(data))
            return true;
        error = "씬 경로를 직렬화하지 못했습니다.";
    } catch (const std::exception& failure) {
        error = failure.what();
    }
    return false;
}

bool SaveAuthoredScene(LuxEngine::Scene& scene, const fs::path& destination,
                       const fs::path& previous, std::string& error, bool rebaseLive,
                       std::string* diskRevision) {
    error.clear();
    try {
        auto copy = CloneAuthoredScene(scene);
        const auto target = CanonicalPath(destination);
        const auto origin = previous.empty() ? PathFromUtf8(scene.GetSourcePath()) : previous;
        const auto owner = origin.empty() ? fs::path{} : CanonicalPath(origin);
        std::uintmax_t remaining = 128 * 1024 * 1024;
        RebaseSceneReferences(*copy, owner, &target, remaining);
        PathEdits live;
        if (rebaseLive) {
            remaining = 128 * 1024 * 1024;
            RebaseSceneReferences(scene, owner, nullptr, remaining, 0, &live);
        }
        std::optional<LuxUI::LockFile> lock;
        if (diskRevision) {
            lock.emplace(FileName(target) + LUX_UI_TEXT(".lux-save-lock"));
            lock->setStaleLockTime(0);
            if (!lock->tryLock()) {
                error = "씬 파일을 저장할 잠금을 얻지 못했습니다: " + PathUtf8(target);
                return false;
            }
            const auto current = ReadSceneFileRevision(target, error);
            if (!current)
                return false;
            if (*current != *diskRevision) {
                error =
                    "씬 파일이 외부에서 변경됐습니다. 변경 내용을 확인한 뒤 다시 열어 주세요: " +
                    PathUtf8(target);
                return false;
            }
        }
        std::string written;
        if (copy->Serialize(GenericPathUtf8(target), diskRevision ? &written : nullptr)) {
            if (diskRevision) {
                LuxUI::CryptographicHash hash(LuxUI::CryptographicHash::Sha256);
                hash.addData(LuxUI::ByteArrayView(written.data(),
                                                  static_cast<std::ptrdiff_t>(written.size())));
                *diskRevision = RevisionHash(hash);
            }
            if (rebaseLive)
                live.Apply(scene);
            return true;
        }
        error = LuxEngine::Scene::GetLastSaveError();
        if (error.empty())
            error = "씬 파일을 저장할 수 없습니다.";
    } catch (const std::exception& failure) {
        error = failure.what();
    }
    return false;
}

bool NormalizeAuthoredScenePaths(LuxEngine::Scene& scene, const fs::path& owner,
                                 std::string& error) {
    try {
        std::uintmax_t remaining = 128 * 1024 * 1024;
        RebaseSceneReferences(scene, owner.empty() ? owner : CanonicalPath(owner), nullptr,
                              remaining);
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

} // namespace LuxEditor
