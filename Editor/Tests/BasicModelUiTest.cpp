#include <LuxEditor/App/EditorWindow.hpp>
#include <LuxEditor/Project/EditorProject.hpp>
#include <LuxEditor/Project/PathEncoding.hpp>
#include <LuxEditor/Viewport/EngineSceneRenderer.hpp>
#include <LuxEditor/Viewport/ModelViewportGeometry.hpp>
#include <LuxEngine/Assets/BasicModelLibrary.hpp>
#include <LuxEngine/Runtime/ScenePlayer.hpp>
#include <LuxUI/Widgets.hpp>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace {
namespace fs = std::filesystem;
using namespace LuxEditor;
using namespace LuxEngine;

void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

LuxUI::Action* Action(EditorWindow& window, const char* name) {
    auto* action = window.findChild<LuxUI::Action*>(LuxUI::String::fromLatin1(name));
    Check(action, "Basic model command is missing from the editor");
    return action;
}

void Close(EditorWindow& window) {
    for (const auto& tab : window.GetDocument().ListTabs())
        window.GetDocument().SetDirty(tab.id, false);
    window.close();
    LuxUI::Application::processEvents();
}

void RequiresProject() {
    LuxUI::Settings().clear();
    EditorWindow window;
    window.show();
    bool notified = false;
    LuxUI::Timer::singleShot(0, [&] {
        for (auto* widget : LuxUI::Application::topLevelWidgets())
            if (auto* message = qobject_cast<LuxUI::MessageBox*>(widget)) {
                notified = message->text().contains(LUX_UI_TEXT("프로젝트"));
                message->accept();
            }
    });
    Action(window, "addBasicModel.shape-cube")->trigger();
    Check(notified && window.GetDocument().GetScene().GetEntityCount() == 0,
          "Basic model placement without a project must explain the missing destination");
    Close(window);
}

void PlacementAndContentBrowser(const fs::path& root) {
    std::string error;
    auto project = EditorProject::Create(root / PathFromUtf8("모델 프로젝트"), "Models",
                                         ProjectTemplate::Empty, error);
    Check(project.has_value(), error.c_str());
    LuxUI::Settings().clear();
    LuxUI::Settings().setValue(LUX_UI_TEXT("lastProject"),
                               LuxUI::String::fromUtf8(PathUtf8(project->GetFile())));
    EditorWindow window;
    window.show();
    LuxUI::Application::processEvents();
    auto& document = window.GetDocument();
    auto* history = window.findChild<LuxUI::UndoStack*>();
    auto* inspector = window.findChild<LuxUI::LineEdit*>(LUX_UI_TEXT("meshAssetPath"));
    auto* filter = window.findChild<LuxUI::ComboBox*>(LUX_UI_TEXT("assetTypeFilter"));
    auto* browser = window.findChild<LuxUI::ListWidget*>(LUX_UI_TEXT("assetGrid"));
    Check(history && inspector && filter && browser, "Model editor controls are unavailable");
    Check(document.GetScene().GetEntityCount() == 0, "Empty project fixture contains objects");
    Action(window, "addBasicModel.shape-cube")->trigger();
    Check(document.GetScene().GetEntityCount() == 1 && history->count() == 1,
          "Basic model creation must produce one object and one undo command");
    const auto id = document.GetSelected().GetId();
    const auto& mesh = document.GetSelected().GetComponent<MeshRendererComponent>();
    const auto model = PathFromUtf8(mesh.meshPath);
    const auto texture = PathFromUtf8(mesh.material.baseColorTexture);
    const auto projectRoot = fs::canonical(project->GetFile().parent_path());
    Check(model.filename() == "shape-cube.fbx" && !model.lexically_relative(projectRoot).empty() &&
              *model.lexically_relative(projectRoot).begin() != ".." &&
              fs::is_regular_file(model) && fs::is_regular_file(texture) &&
              fs::is_regular_file(model.parent_path() / "License.txt") &&
              fs::is_regular_file(model.parent_path() / "Provenance.json") &&
              inspector->text() == LuxUI::String::fromUtf8(mesh.meshPath),
          "Basic model placement must use project-owned FBX, palette texture and license data");
    history->undo();
    Check(document.GetScene().GetEntityCount() == 0 && fs::is_regular_file(model),
          "Undo must remove the placed object while preserving reusable project assets");
    history->redo();
    Check(document.GetSelected().GetId() == id &&
              document.GetSelected().GetComponent<MeshRendererComponent>().meshPath ==
                  PathUtf8(model),
          "Redo must restore the same model reference and selection");
    Action(window, "addBasicModel.shape-cylinder")->trigger();
    Check(document.GetScene().GetEntityCount() == 2 &&
              PathFromUtf8(document.GetSelected().GetComponent<MeshRendererComponent>().meshPath)
                      .parent_path() == model.parent_path(),
          "Additional basic models must reuse the installed package");
    filter->setCurrentIndex(filter->findText(LUX_UI_TEXT("모델")));
    Check(browser->count() == 10, "Content Browser model filter must list the ten FBX models");
    const auto slopes =
        browser->findItems(LUX_UI_TEXT("shape-slope.fbx"), LuxUI::Flags::MatchContains);
    Check(slopes.size() == 1, "Packaged FBX was not discoverable in the Content Browser");
    browser->setCurrentItem(slopes.front());
    browser->itemDoubleClicked(slopes.front());
    Check(document.GetScene().GetEntityCount() == 3 &&
              PathFromUtf8(document.GetSelected().GetComponent<MeshRendererComponent>().meshPath)
                      .filename() == "shape-slope.fbx" &&
              !document.GetSelected()
                   .GetComponent<MeshRendererComponent>()
                   .material.baseColorTexture.empty(),
          "Double-clicking an installed FBX must place it with its verified palette texture");
    const auto obj = model.parent_path() / "custom.OBJ";
    std::ofstream(obj) << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    filter->setCurrentIndex(0);
    filter->setCurrentIndex(filter->findText(LUX_UI_TEXT("모델")));
    const auto objects = browser->findItems(LUX_UI_TEXT("custom.OBJ"), LuxUI::Flags::MatchContains);
    Check(objects.size() == 1 && browser->count() == 11,
          "Model filtering must include case-insensitive existing OBJ assets");
    browser->setCurrentItem(objects.front());
    browser->itemDoubleClicked(objects.front());
    Check(document.GetScene().GetEntityCount() == 4 &&
              document.GetSelected().GetComponent<MeshRendererComponent>().meshPath ==
                  PathUtf8(obj) &&
              document.GetSelected()
                  .GetComponent<MeshRendererComponent>()
                  .material.baseColorTexture.empty(),
          "Generic OBJ placement must not inherit a catalog material by accident");
    const auto sceneFile = projectRoot / "Scenes" / "Placed.lux";
    fs::create_directories(sceneFile.parent_path());
    Check(document.SaveAs(PathUtf8(sceneFile)), "Placed model scene did not save");
    Scene reopened;
    Check(reopened.Deserialize(PathUtf8(sceneFile)) && reopened.GetEntityCount() == 4,
          "Saved model scene did not reopen");
    const auto& reopenedMesh = reopened.GetComponent<MeshRendererComponent>(id);
    Check(fs::equivalent(PathFromUtf8(reopened.ResolveAssetPath(reopenedMesh.meshPath)), model) &&
              fs::equivalent(
                  PathFromUtf8(reopened.ResolveAssetPath(reopenedMesh.material.baseColorTexture)),
                  texture),
          "Saving and reopening must preserve project-relative FBX and palette references");
    Close(window);
}

void AppendWord(std::string& bytes, uint32 word) {
    for (int shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<char>((word >> shift) & 255));
}

void WriteBytes(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    Check(bool(output), "write glTF editor fixture");
}

void WriteGltf(const fs::path& path, const std::string& png, bool binary) {
    std::string vertices;
    for (float value :
         {-.6f, -.4f, 0.f, .6f, -.4f, 0.f, 0.f, .6f, 0.f, 0.f, 1.f, 1.f, 1.f, .5f, 0.f})
        AppendWord(vertices, std::bit_cast<uint32>(value));
    std::string document = R"({"asset":{"version":"2.0"},"scene":0,
        "scenes":[{"nodes":[0]}],"nodes":[{"translation":[0.6,0,0],"children":[1]},{"mesh":0}],
        "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",
                      "min":[-0.6,-0.4,0],"max":[0.6,0.6,0]},
                     {"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"}],
        "meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"material":0}]}],
        "materials":[{"doubleSided":true,"pbrMetallicRoughness":{"metallicFactor":0,
                       "baseColorTexture":{"index":0}}}],"textures":[{"source":0}],
        "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},
                       {"buffer":0,"byteOffset":36,"byteLength":24})";
    if (!binary) {
        WriteBytes(path.parent_path() / "Geometry/triangle.bin", vertices);
        WriteBytes(path,
                   document + R"(],"buffers":[{"byteLength":60,"uri":"Geometry/triangle.bin"}],
                    "images":[{"uri":"Images/fill.png"}]})");
        return;
    }
    document += R"(,{"buffer":0,"byteOffset":60,"byteLength":)" + std::to_string(png.size()) +
                R"(}],"buffers":[{"byteLength":)" + std::to_string(vertices.size() + png.size()) +
                R"(}],"images":[{"bufferView":2,"mimeType":"image/png"}]})";
    vertices += png;
    vertices.resize((vertices.size() + 3) & ~size_t{3});
    document.resize((document.size() + 3) & ~size_t{3}, ' ');
    std::string glb;
    AppendWord(glb, 0x46546c67);
    AppendWord(glb, 2);
    AppendWord(glb, static_cast<uint32>(28 + document.size() + vertices.size()));
    AppendWord(glb, static_cast<uint32>(document.size()));
    AppendWord(glb, 0x4e4f534a);
    glb += document;
    AppendWord(glb, static_cast<uint32>(vertices.size()));
    AppendWord(glb, 0x004e4942);
    WriteBytes(path, glb + vertices);
}

void CheckRedModels(const LuxUI::Image& image) {
    int left = 0, right = 0;
    for (int y = image.height() / 4; y < image.height() * 3 / 4; ++y)
        for (int x = image.width() / 12; x < image.width() * 11 / 12; ++x) {
            const auto pixel = image.pixelColor(x, y);
            if (pixel.red() > 60 && pixel.red() > 3 * pixel.green() &&
                pixel.red() > 3 * pixel.blue())
                (x < image.width() / 2 ? left : right)++;
        }
    Check(left > 25 && right > 25, "the imported GLB and glTF textures render on opposite sides");
}

void GltfEditorAndPlayer(const fs::path& root, bool gpu) {
    const auto source = root / "Source";
    fs::create_directories(source / "Geometry");
    fs::create_directories(source / "Images");
    LuxUI::Image red(1, 1, LuxUI::Image::Format_RGBA8888);
    red.fill(LuxUI::Color(255, 0, 0));
    const auto pngFile = source / "Images/fill.png";
    Check(red.save(LuxUI::String::fromUtf8(PathUtf8(pngFile)), "PNG"), "write model texture");
    std::ifstream pixels(pngFile, std::ios::binary);
    const std::string png{std::istreambuf_iterator<char>(pixels), {}};
    const auto external = source / PathFromUtf8("외부 모델.gltf");
    const auto embedded = source / "Embedded.GLB";
    WriteGltf(external, png, false);
    WriteGltf(embedded, png, true);

    std::string error;
    const auto project =
        EditorProject::Create(root / "Project", "glTF", ProjectTemplate::Empty, error);
    Check(project.has_value(), error.c_str());
    LuxUI::Settings().clear();
    LuxUI::Settings().setValue(LUX_UI_TEXT("lastProject"),
                               LuxUI::String::fromUtf8(PathUtf8(project->GetFile())));
    EditorWindow window;
    window.show();
    LuxUI::Application::processEvents();
    if (!window.ImportAssetFiles({external, embedded}, error))
        throw std::runtime_error(error);
    const auto externalFolder = project->GetFile().parent_path() / "Assets" / external.stem();
    Check(fs::is_regular_file(externalFolder / "Geometry/triangle.bin") &&
              fs::is_regular_file(externalFolder / "Images/fill.png"),
          "external glTF dependencies retain their relative layout");
    Check(!window.ImportAssetFiles({external}, error), "duplicate imports cannot overwrite assets");
    auto* filter = window.findChild<LuxUI::ComboBox*>(LUX_UI_TEXT("assetTypeFilter"));
    auto* browser = window.findChild<LuxUI::ListWidget*>(LUX_UI_TEXT("assetGrid"));
    Check(filter && browser, "find the model browser controls");
    filter->setCurrentIndex(filter->findText(LUX_UI_TEXT("모델")));
    Check(browser->count() == 2, "the model filter lists glTF and GLB without their sidecars");
    const auto place = [&](const fs::path& file) {
        const auto entries = browser->findItems(LuxUI::String::fromUtf8(PathUtf8(file.filename())),
                                                LuxUI::Flags::MatchContains);
        Check(entries.size() == 1, "find the imported model in the content browser");
        browser->setCurrentItem(entries.front());
        browser->itemDoubleClicked(entries.front());
        auto entity = window.GetDocument().GetSelected();
        Check(entity && entity.HasComponent<MeshRendererComponent>() &&
                  entity.GetComponent<MeshRendererComponent>().modelSubmesh == -1 &&
                  !entity.HasComponent<ModelAnimationComponent>(),
              "place a whole static model without an animation binding");
        return entity;
    };
    auto first = place(external);
    Vec3 minimum, maximum;
    Check(ModelViewportBounds(first, minimum, maximum) && std::abs(minimum.x) < .001 &&
              std::abs(maximum.x - 1.2) < .001 &&
              IntersectModelViewport({{.6, 0, 2}, {0, 0, -1}}, first).has_value() &&
              !IntersectModelViewport({{1.1, .5, 2}, {0, 0, -1}}, first),
          "viewport framing and picking include the glTF node transform and triangle shape");
    auto second = place(embedded);
    second.GetComponent<TransformComponent>().position.x = -1.7;
    auto& document = window.GetDocument();
    auto camera = document.CreateEntity("Camera");
    camera.GetComponent<TransformComponent>().position.z = 3;
    camera.AddComponent<CameraComponent>().isPerspective = true;
    const auto scenePath = project->GetSceneFile();
    Check(document.SaveAs(PathUtf8(scenePath)), "save the editor-authored glTF scene");
    Scene reopened;
    Check(reopened.Deserialize(PathUtf8(scenePath)) &&
              reopened.GetComponentEntityIds<MeshRendererComponent>().size() == 2 &&
              PathFromUtf8(reopened.GetComponent<MeshRendererComponent>(first.GetId()).meshPath)
                  .is_relative(),
          "reopening preserves the two models and their project-relative paths");
    if (gpu) {
        EngineSceneRenderer::SetForceAllowed(true);
        EngineSceneRenderer renderer;
        PerspectiveCamera view;
        view.SetPerspective(ToRadians(45), 4.0 / 3, .1, 100);
        view.SetPosition({0, 0, 3});
        view.SetTarget({0, 0, 0});
        LuxUI::Image frame;
        Check(renderer.Render3D(reopened, 320, 240, {}, 0, 0, 3, frame, nullptr, &view),
              "render the imported meshes in the editing viewport");
        CheckRedModels(frame);
        renderer.Shutdown();

        ScenePlayerOptions playerOptions;
        playerOptions.scenePath = PathUtf8(scenePath);
        playerOptions.width = 320;
        playerOptions.height = 240;
        ScenePlayer player(playerOptions);
        Check(player.Initialize() && player.LoadedScene() &&
                  player.GetGpuDevice().RequestFrameCapture() &&
                  player.RunFrame() == Application::FrameResult::Continue,
              "open and render the saved editor scene in the player runtime");
        GpuFrameCapture capture;
        Check(player.GetGpuDevice().ReadFrameCapture(capture), "capture the player frame");
        CheckRedModels(LuxUI::Image(capture.rgba.data(), static_cast<int>(capture.width),
                                    static_cast<int>(capture.height),
                                    LuxUI::Image::Format_RGBA8888));
        player.Shutdown();

        LuxUI::Process process;
        process.setProgram(LuxUI::String::fromUtf8(LUX_GLTF_TEST_PLAYER));
        process.setWorkingDirectory(LuxUI::CoreApplication::applicationDirPath());
        process.setArguments({LuxUI::String::fromUtf8(PathUtf8(scenePath)), LUX_UI_TEXT("--frames"),
                              LUX_UI_TEXT("2"), LUX_UI_TEXT("--size"), LUX_UI_TEXT("320"),
                              LUX_UI_TEXT("240")});
        process.start();
        Check(process.waitForStarted(10000) && process.waitForFinished(30000) &&
                  process.exitStatus() == LuxUI::Process::NormalExit && process.exitCode() == 0,
              "LuxPlayer executable runs the scene saved by the editor");
    }
    Close(window);
}
} // namespace

int main(int argc, char** argv) {
    LuxUI::Application app(argc, argv);
    LuxUI::CoreApplication::setOrganizationName(LUX_UI_TEXT("LuxModelTests"));
    LuxUI::CoreApplication::setApplicationName(LUX_UI_TEXT("BasicModelUi"));
    LuxUI::TemporaryDir settings;
    LuxUI::TemporaryDir files;
    LuxUI::Settings::setDefaultFormat(LuxUI::Settings::IniFormat);
    LuxUI::Settings::setPath(LuxUI::Settings::IniFormat, LuxUI::Settings::UserScope,
                             settings.path());
    try {
        Check(settings.isValid() && files.isValid(),
              "Temporary model test directories are invalid");
        const auto root = PathFromUtf8(files.path().toUtf8().toStdString());
        const std::string_view suite = argc > 1 ? argv[1] : "basic";
        if (suite == "gltf" || suite == "gltf-gpu") {
            GltfEditorAndPlayer(root, suite == "gltf-gpu");
            std::cout << "glTF/GLB editor import, placement and scene persistence passed\n";
            return 0;
        }
        RequiresProject();
        PlacementAndContentBrowser(root);
        std::cout
            << "Basic model menu, content placement, material, undo and scene persistence passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
