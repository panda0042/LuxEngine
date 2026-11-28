#include <LuxEditor/App/EditorWindow.hpp>
#include <LuxEditor/Project/PathEncoding.hpp>
#include <LuxEditor/Scene/ModelImport.hpp>
#include <LuxEditor/Scene/SceneDependencies.hpp>
#include <LuxEditor/Ui/IconFactory.hpp>
#include <LuxEditor/Viewport/SceneViewport3D.hpp>
#include <LuxEngine/Assets/AssetManager.hpp>
#include <LuxEngine/Assets/BasicModelLibrary.hpp>
#include <LuxEngine/Assets/ModelAsset.hpp>
#include <LuxUI/Widgets.hpp>
#include <algorithm>
#include <cctype>

namespace LuxEditor {
namespace {
LuxUI::String MeshFailure(const std::string& path) {
    const auto error = LuxEngine::AssetManager::GetAssetError(path);
    return error.empty() ? LUX_UI_TEXT("모델 파일을 읽을 수 없습니다.")
                         : LuxUI::String::fromUtf8(error);
}

bool IsInsideProject(const std::filesystem::path& file, const std::filesystem::path& root) {
    std::error_code error;
    const auto canonicalRoot = std::filesystem::canonical(root, error);
    if (error)
        return false;
    const auto canonicalFile = std::filesystem::canonical(file, error);
    if (error || !std::filesystem::is_regular_file(canonicalFile, error) || error)
        return false;
    const auto relative = canonicalFile.lexically_relative(canonicalRoot);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

bool IsFbx(const std::filesystem::path& path) {
    auto extension = PathUtf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".fbx";
}

bool ValidateGltfAsset(const EditorProject& project, const std::string& path, std::string& error,
                       LuxEngine::int32 submesh = -1) {
    LuxEngine::Scene modelScene;
    auto& mesh = modelScene.CreateEntity().AddComponent<LuxEngine::MeshRendererComponent>();
    mesh.meshPath = path;
    mesh.modelSubmesh = submesh;
    return ValidateProjectSceneReferences(project, modelScene, error);
}
} // namespace

void EditorWindow::AddBasicModelMenu(LuxUI::Menu* parent) {
    if (basicModelActions_.empty()) {
        for (const auto& model : LuxEngine::BasicModelLibrary::GetModels()) {
            const std::string id = model.id;
            auto* action = new LuxUI::Action(MakeEditorIcon(EditorIcon::Mesh),
                                             LuxUI::String::fromUtf8(model.name), this);
            action->setObjectName(LUX_UI_TEXT("addBasicModel.") + LuxUI::String::fromUtf8(id));
            action->setToolTip(LUX_UI_TEXT("CC0 기본 모델을 프로젝트에 추가합니다."));
            connect(action, &LuxUI::Action::triggered, this, [this, id] { AddBasicModel(id); });
            basicModelActions_.push_back(action);
        }
    }
    auto* menu = parent->addMenu(MakeEditorIcon(EditorIcon::Mesh), LUX_UI_TEXT("기본 모델"));
    menu->addActions(
        LuxUI::List<LuxUI::Action*>(basicModelActions_.begin(), basicModelActions_.end()));
    auto* reimport = parent->addAction(LUX_UI_TEXT("선택 모델을 원본에서 다시 가져오기"));
    reimport->setObjectName(LUX_UI_TEXT("reimportSelectedModel"));
    connect(reimport, &LuxUI::Action::triggered, this, [this] {
        if (preview_.IsActive() || !project_ || !document_.GetSelected())
            return;
        const auto selected = document_.GetSelected();
        if (!FindImportedModelRoot(document_.GetScene(), selected.GetId())) {
            if (!selected.HasComponent<LuxEngine::MeshRendererComponent>())
                return;
            const auto& mesh = selected.GetComponent<LuxEngine::MeshRendererComponent>();
            std::string error;
            const auto path = document_.GetScene().ResolveAssetPath(mesh.meshPath);
            const bool loaded = LuxEngine::ModelAsset::IsGltfPath(path)
                                    ? ValidateGltfAsset(*project_, path, error, mesh.modelSubmesh)
                                    : LuxEngine::AssetManager::ReloadMesh(
                                          path, error, !mesh.material.baseColorTexture.empty());
            if (!loaded) {
                ShowError(LUX_UI_TEXT("모델 다시 가져오기"), LuxUI::String::fromUtf8(error));
                return;
            }
            RefreshAll();
            return;
        }
        ModelImportResult result;
        bool imported = false;
        RecordEdit(LUX_UI_TEXT("모델 다시 가져오기"), [this, &result, &imported] {
            std::string error;
            if (!ReimportModelAsset(document_.GetScene(), document_.GetSelected().GetId(),
                                    project_->GetFile().parent_path(), result, error))
                throw std::runtime_error(error);
            document_.Select(result.root);
            document_.MarkDirty();
            imported = true;
        });
        if (imported) {
            for (const auto& warning : result.warnings)
                Log(LuxUI::String::fromUtf8(warning));
            RefreshAll();
        }
    });
}

void EditorWindow::AddBasicModel(const std::string& id) {
    if (preview_.IsActive())
        return;
    if (!project_) {
        ShowError(LUX_UI_TEXT("기본 모델 추가"),
                  LUX_UI_TEXT("기본 모델을 추가하려면 프로젝트를 먼저 열어 주세요."));
        return;
    }
    LuxEngine::BasicModelInstallation installed;
    std::string error;
    if (!LuxEngine::BasicModelLibrary::InstallIntoProject(id, project_->GetFile().parent_path(),
                                                          installed, error)) {
        ShowError(LUX_UI_TEXT("기본 모델 추가"), LuxUI::String::fromUtf8(error));
        return;
    }
    AddModelAsset(installed.modelPath);
}

void EditorWindow::AddModelAsset(const std::filesystem::path& path) {
    if (preview_.IsActive())
        return;
    if (!project_ || !IsInsideProject(path, project_->GetFile().parent_path())) {
        ShowError(LUX_UI_TEXT("모델 배치"),
                  LUX_UI_TEXT("프로젝트 내부의 모델 파일을 선택해 주세요."));
        return;
    }
    const auto modelPath = PathUtf8(path);
    if (LuxEngine::ModelAsset::IsGltfPath(modelPath)) {
        std::string error;
        if (!ValidateGltfAsset(*project_, modelPath, error)) {
            ShowError(LUX_UI_TEXT("glTF 모델 배치"), LuxUI::String::fromUtf8(error));
            return;
        }
        RecordEdit(LUX_UI_TEXT("glTF 모델 배치"), [this, &path, &modelPath] {
            document_.CreateEntity(PathUtf8(path.stem()))
                .AddComponent<LuxEngine::MeshRendererComponent>()
                .meshPath = modelPath;
        });
        viewportTabs_->setCurrentIndex(0);
        RefreshAll();
        canvas3D_->FocusSelection();
        return;
    }
    LuxEngine::BasicModelInstallation installed;
    const bool basic = LuxEngine::BasicModelLibrary::IdentifyInstalledModel(path, installed);
    if (!basic && IsFbx(path)) {
        std::string error;
        const auto model = LuxEngine::ModelAsset::LoadFbx(modelPath, error);
        if (!model) {
            ShowError(LUX_UI_TEXT("FBX 모델 배치"), LuxUI::String::fromUtf8(error));
            return;
        }
        ModelImportResult result;
        bool imported = false;
        RecordEdit(LUX_UI_TEXT("FBX 모델 계층 배치"), [this, model, &result, &imported] {
            std::string error;
            if (!ImportModelAsset(document_.GetScene(), *model, project_->GetFile().parent_path(),
                                  result, error))
                throw std::runtime_error(error);
            document_.Select(result.root);
            document_.MarkDirty();
            imported = true;
        });
        if (!imported)
            return;
        for (const auto& warning : result.warnings)
            Log(LuxUI::String::fromUtf8(warning));
        viewportTabs_->setCurrentIndex(0);
        RefreshAll();
        canvas3D_->FrameAll();
        return;
    }
    LuxEngine::RawMeshData data;
    if (!LuxEngine::AssetManager::LoadMesh(modelPath, data)) {
        ShowError(LUX_UI_TEXT("모델 배치"), MeshFailure(modelPath));
        return;
    }
    if (basic && (!data.hasTexCoords || !LuxEngine::AssetManager::ValidateTextureFile(
                                            PathUtf8(installed.baseColorTexture)))) {
        ShowError(LUX_UI_TEXT("모델 배치"),
                  LUX_UI_TEXT("기본 모델의 UV 또는 색상 텍스처를 확인할 수 없습니다."));
        return;
    }
    const auto* descriptor =
        basic ? LuxEngine::BasicModelLibrary::FindModel(PathUtf8(path.stem())) : nullptr;
    const auto name = descriptor ? std::string(descriptor->name) : PathUtf8(path.stem());
    const auto texture = basic ? PathUtf8(installed.baseColorTexture) : std::string{};
    RecordEdit(LUX_UI_TEXT("모델 배치"), [this, modelPath, texture, name] {
        auto entity = document_.CreateEntity(name);
        auto& mesh = entity.AddComponent<LuxEngine::MeshRendererComponent>();
        mesh.meshPath = modelPath;
        mesh.material.baseColorTexture = texture;
    });
    viewportTabs_->setCurrentIndex(0);
    RefreshAll();
    canvas3D_->FocusSelection();
}

void EditorWindow::ChooseModelAsset() {
    if (preview_.IsActive() || !document_.GetSelected())
        return;
    const auto tab = document_.GetActiveTabId();
    const auto entity = document_.GetSelected().GetId();
    const auto file = LuxUI::FileDialog::getOpenFileName(
        this, LUX_UI_TEXT("모델 선택"), meshPath_->text(),
        LUX_UI_TEXT(
            "3D 모델 (*.obj *.OBJ *.fbx *.FBX *.gltf *.GLTF *.glb *.GLB);;"
            "Wavefront OBJ (*.obj *.OBJ);;FBX (*.fbx *.FBX);;glTF (*.gltf *.GLTF *.glb *.GLB)"));
    if (file.isEmpty())
        return;
    if (document_.GetActiveTabId() != tab || document_.GetSelectedId(tab) != entity) {
        Log(LUX_UI_TEXT("선택이 변경되어 모델 적용을 취소했습니다."), true);
        return;
    }
    const auto path = file.toUtf8().toStdString();
    const auto selected = document_.GetSelected();
    const auto importedRoot = FindImportedModelRoot(document_.GetScene(), selected.GetId());
    LuxEngine::BasicModelInstallation installed;
    if (IsFbx(PathFromUtf8(path)) &&
        (importedRoot ||
         !LuxEngine::BasicModelLibrary::IdentifyInstalledModel(PathFromUtf8(path), installed))) {
        if (!project_ || !IsInsideProject(PathFromUtf8(path), project_->GetFile().parent_path())) {
            ShowError(LUX_UI_TEXT("모델 불러오기"),
                      LUX_UI_TEXT("프로젝트 내부의 모델 파일을 선택해 주세요."));
            return;
        }
        std::string error;
        const auto model = LuxEngine::ModelAsset::LoadFbx(path, error);
        if (!model) {
            ShowError(LUX_UI_TEXT("FBX 모델 불러오기"), LuxUI::String::fromUtf8(error));
            return;
        }
        ModelImportResult result;
        bool imported = false;
        RecordEdit(LUX_UI_TEXT("FBX 모델 파일 변경"), [this, model, path, importedRoot, &result,
                                                       &imported] {
            auto& scene = document_.GetScene();
            std::string error;
            if (importedRoot) {
                if (!ReimportModelAsset(scene, importedRoot, project_->GetFile().parent_path(),
                                        result, error, PathFromUtf8(path)))
                    throw std::runtime_error(error);
            } else {
                const auto previous = document_.GetSelected().GetId();
                const auto parent = scene.GetParent(previous);
                const auto placement = scene.GetComponent<LuxEngine::TransformComponent>(previous);
                if (!ImportModelAsset(scene, *model, project_->GetFile().parent_path(), result,
                                      error))
                    throw std::runtime_error(error);
                if (!scene.SetParent(result.root, parent, false))
                    throw std::runtime_error("모델의 부모 오브젝트를 유지하지 못했습니다.");
                scene.GetComponent<LuxEngine::TransformComponent>(result.root) = placement;
                for (const auto child : scene.GetChildren(previous))
                    if (!scene.SetParent(child, result.root, true))
                        throw std::runtime_error("모델의 하위 오브젝트를 보존하지 못했습니다.");
                scene.DestroyEntity(LuxEngine::Entity{previous, &scene});
            }
            document_.Select(result.root);
            document_.MarkDirty();
            imported = true;
        });
        if (imported) {
            for (const auto& warning : result.warnings)
                Log(LuxUI::String::fromUtf8(warning));
            RefreshAll();
        }
        return;
    }
    if (importedRoot) {
        ShowError(LUX_UI_TEXT("모델 불러오기"),
                  LUX_UI_TEXT("FBX 계층의 원본은 FBX 모델로 변경해 주세요. OBJ 모델은 별도로 "
                              "배치할 수 있습니다."));
        return;
    }
    if (LuxEngine::ModelAsset::IsGltfPath(path)) {
        std::string error;
        if (!project_ || !IsInsideProject(PathFromUtf8(path), project_->GetFile().parent_path())) {
            ShowError(LUX_UI_TEXT("모델 불러오기"),
                      LUX_UI_TEXT("외부 glTF 모델은 먼저 에셋 가져오기로 추가해 주세요."));
            return;
        }
        if (!ValidateGltfAsset(*project_, path, error)) {
            ShowError(LUX_UI_TEXT("glTF 모델 불러오기"), LuxUI::String::fromUtf8(error));
            return;
        }
        RecordEdit(LUX_UI_TEXT("glTF 모델 파일 변경"), [this, &path] {
            auto entity = document_.GetSelected();
            if (!entity.HasComponent<LuxEngine::MeshRendererComponent>())
                entity.AddComponent<LuxEngine::MeshRendererComponent>();
            auto& mesh = entity.GetComponent<LuxEngine::MeshRendererComponent>();
            if (document_.GetScene().ResolveAssetPath(mesh.meshPath) != path)
                mesh.material = {};
            mesh.meshPath = path;
            mesh.modelSubmesh = -1;
            document_.MarkDirty();
        });
        RefreshAll();
        return;
    }
    LuxEngine::RawMeshData data;
    if (!LuxEngine::AssetManager::LoadMesh(path, data, true)) {
        ShowError(LUX_UI_TEXT("모델 불러오기"), MeshFailure(path));
        return;
    }
    const auto hasTextures = [&] {
        if (!selected.HasComponent<LuxEngine::MeshRendererComponent>())
            return false;
        const auto& material = selected.GetComponent<LuxEngine::MeshRendererComponent>().material;
        return !material.baseColorTexture.empty() || !material.normalTexture.empty() ||
               !material.metallicTexture.empty() || !material.roughnessTexture.empty() ||
               !material.occlusionTexture.empty() || !material.emissiveTexture.empty() ||
               !material.heightTexture.empty() || material.terrainLayer.has_value();
    };
    if (hasTextures() && !data.hasTexCoords) {
        ShowError(LUX_UI_TEXT("모델 불러오기"),
                  LUX_UI_TEXT("텍스처를 유지하려면 UV 좌표가 있는 모델이 필요합니다."));
        return;
    }
    RecordEdit(LUX_UI_TEXT("모델 파일 변경"), [this, path] {
        auto selected = document_.GetSelected();
        if (!selected.HasComponent<LuxEngine::MeshRendererComponent>())
            selected.AddComponent<LuxEngine::MeshRendererComponent>();
        auto& mesh = selected.GetComponent<LuxEngine::MeshRendererComponent>();
        if (LuxEngine::ModelAsset::IsGltfPath(mesh.meshPath))
            mesh.modelSubmesh = -1;
        mesh.meshPath = path;
        document_.MarkDirty();
    });
    RefreshAll();
}

} // namespace LuxEditor
