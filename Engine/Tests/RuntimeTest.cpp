// Tests for the Runtime library: LuxPlayer argument parsing, view fitting and a scene file for
// the LuxPlayer smoke test.
#include <LuxEngine/Assets/ModelAsset.hpp>
#include <LuxEngine/ECS/ECS.hpp>
#include <LuxEngine/Physics/PhysicsEngine.hpp>
#include <LuxEngine/Runtime/ScenePlayer.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <SDL3/SDL_timer.h>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
int checks = 0;

struct RuntimeFiles {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() / std::filesystem::path(u8"lux-runtime-씬") /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

    RuntimeFiles() {
        std::filesystem::create_directories(root);
    }

    ~RuntimeFiles() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

void Check(bool ok, const char* message) {
    ++checks;
    if (!ok)
        throw std::runtime_error(message);
}

bool Near(float a, float b) {
    return std::abs(a - b) < 0.001f;
}

namespace Gltf {
using namespace LuxEngine;

std::string Utf8Path(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void AppendLittleEndian(std::vector<uint8>& bytes, uint32 value, unsigned count = 4) {
    for (unsigned i = 0; i < count; ++i)
        bytes.push_back(static_cast<uint8>(value >> (i * 8)));
}

void WriteModelBytes(const std::filesystem::path& file, std::span<const uint8> bytes) {
    std::ofstream stream(file, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    Check(bool(stream), "write glTF test data");
}

std::filesystem::path WriteGltfFixture(const std::filesystem::path& root, bool binary,
                                       bool secondInstance = false) {
    using Json = nlohmann::ordered_json;
    constexpr std::array<uint8, 70> redImage{
        137, 80,  78, 71, 13, 10,  26,  10, 0,   0,   0,   13,  73,  72,  68,  82, 0, 0,
        0,   1,   0,  0,  0,  1,   8,   6,  0,   0,   0,   31,  21,  196, 137, 0,  0, 0,
        13,  73,  68, 65, 84, 120, 156, 99, 248, 207, 192, 240, 31,  0,   5,   0,  1, 255,
        137, 153, 61, 29, 0,  0,   0,   0,  73,  69,  78,  68,  174, 66,  96,  130};
    std::vector<uint8> bytes;
    for (float value : {-.6f, -.4f, 0.f, .6f, -.4f, 0.f, 0.f, .6f, 0.f, 0.f, 0.f, 1.f,
                        0.f,  0.f,  1.f, 0.f, 0.f,  1.f, 0.f, 1.f, 1.f, 1.f, .5f, 0.f})
        AppendLittleEndian(bytes, std::bit_cast<uint32>(value));
    for (const auto index : {0u, 1u, 2u})
        AppendLittleEndian(bytes, index, 2);

    auto json = Json::parse(R"({
        "asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
        "nodes":[{"name":"Parent","translation":[0.6,0,0],"children":[1]},
                 {"name":"Triangle","scale":[1.1,0.8,1],"mesh":0}],
        "buffers":[{"byteLength":102,"uri":"mesh.bin"}],
        "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},
                       {"buffer":0,"byteOffset":36,"byteLength":36,"target":34962},
                       {"buffer":0,"byteOffset":72,"byteLength":24,"target":34962},
                       {"buffer":0,"byteOffset":96,"byteLength":6,"target":34963}],
        "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",
                      "min":[-0.6,-0.4,0],"max":[0.6,0.6,0]},
                     {"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},
                     {"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},
                     {"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}],
        "meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},
                                   "indices":3,"material":0}]}],
        "materials":[{"name":"Paint","pbrMetallicRoughness":{"baseColorTexture":{"index":0},
                       "metallicFactor":0,"roughnessFactor":0.7},"doubleSided":true}],
        "textures":[{"source":0}],"images":[{"uri":"paint.png"}]
    })");
    if (secondInstance) {
        json["nodes"][0]["children"].push_back(2);
        json["nodes"].push_back({{"name", "Second"}, {"translation", {-1.2, 0, 0}}, {"mesh", 0}});
    }
    if (!binary) {
        WriteModelBytes(root / "mesh.bin", bytes);
        WriteModelBytes(root / "paint.png", redImage);
        const auto file = root / "triangle.gltf";
        std::ofstream(file, std::ios::binary) << json.dump();
        return file;
    }

    bytes.resize(104);
    bytes.insert(bytes.end(), redImage.begin(), redImage.end());
    json["buffers"][0] = {{"byteLength", bytes.size()}};
    json["bufferViews"].push_back(
        {{"buffer", 0}, {"byteOffset", 104}, {"byteLength", redImage.size()}});
    json["images"][0] = {{"bufferView", 4}, {"mimeType", "image/png"}};
    auto document = json.dump();
    document.resize((document.size() + 3) & ~size_t{3}, ' ');
    bytes.resize((bytes.size() + 3) & ~size_t{3});
    std::vector<uint8> glb;
    AppendLittleEndian(glb, 0x46546c67);
    AppendLittleEndian(glb, 2);
    AppendLittleEndian(glb, static_cast<uint32>(28 + document.size() + bytes.size()));
    AppendLittleEndian(glb, static_cast<uint32>(document.size()));
    AppendLittleEndian(glb, 0x4e4f534a);
    glb.insert(glb.end(), document.begin(), document.end());
    AppendLittleEndian(glb, static_cast<uint32>(bytes.size()));
    AppendLittleEndian(glb, 0x004e4942);
    glb.insert(glb.end(), bytes.begin(), bytes.end());
    const auto file = root / "triangle.glb";
    WriteModelBytes(file, glb);
    return file;
}

void Assets() {
    RuntimeFiles files;
    std::string error;
    Check(ModelAsset::IsGltfPath("models.with.dots\\SHAPE.GLB") &&
              ModelAsset::IsGltfPath("nested/shape.gltf") &&
              !ModelAsset::IsGltfPath("saved.gltf/asset"),
          "glTF format selection is case-insensitive and uses the filename extension");
    for (bool binary : {false, true}) {
        const auto file = WriteGltfFixture(files.root, binary);
        const auto model = ModelAsset::Load(Utf8Path(file), error);
        if (!model)
            throw std::runtime_error(error);
        Check(model->nodes.size() == 2 && model->submeshes.size() == 1 &&
                  model->materials.size() == 1 && !model->textures.empty(),
              "glTF keeps its parent and child nodes, geometry and material");
        Check(model->sourceDependencies.size() == (binary ? 0 : 2),
              "only external buffers and textures are exposed as model dependencies");
        const auto& mesh = model->submeshes.front();
        const auto& node = model->nodes.at(mesh.node);
        Check(node.name == "Triangle" && node.parent >= 0 && mesh.vertices.size() == 3 &&
                  mesh.indices.size() == 3 && mesh.hasTexCoords &&
                  std::abs(node.worldTransform.TransformPoint({}).x - .6) < .001,
              "glTF local vertices and hierarchical world transform are available");
        const auto texture = model->materials.front().baseColorTexture;
        Check(texture >= 0 && model->materials.front().doubleSided &&
                  std::abs(model->materials.front().roughness - .7) < .001 &&
                  (binary ? !model->textures.at(static_cast<size_t>(texture)).embedded.empty()
                          : !model->textures.at(static_cast<size_t>(texture)).path.empty()),
              "glTF external texture and GLB embedded texture are represented");
        ModelPose pose;
        Check(model->Evaluate(0, -1, pose, error) && pose.submeshes.size() == 1,
              "a static model supplies its rest pose");

        Scene scene;
        auto entity = scene.CreateEntity("Model");
        entity.AddComponent<MeshRendererComponent>().meshPath = Utf8Path(file);
        const auto view = FitSceneView(scene, 1);
        Check(std::abs(view.centerX - .6f) < .05f,
              "whole glTF camera fit includes the parent node transform");
        entity.GetComponent<MeshRendererComponent>().modelSubmesh = 0;
        Check(std::abs(FitSceneView(scene, 1).centerX) < .05f,
              "explicit glTF submesh camera fit uses the authored entity transform");
        entity.GetComponent<MeshRendererComponent>().modelSubmesh = -1;
        if (!binary) {
            std::ofstream(files.root / "mesh.bin", std::ios::binary | std::ios::trunc) << 'x';
            Check(ModelAsset::Load(Utf8Path(file), error) == model && !error.empty(),
                  "corrupt external buffer keeps the last valid model with a diagnostic");
            Check(std::abs(FitSceneView(scene, 1).centerX - view.centerX) < .05f,
                  "camera fit keeps the last valid hierarchy bounds after a buffer error");
            Check(std::filesystem::remove(file), "the original glTF can be removed");
            Check(ModelAsset::Load(Utf8Path(file), error) == model && !error.empty(),
                  "missing source keeps the last valid model with a diagnostic");
            WriteGltfFixture(files.root, false);
            Check(ModelAsset::Load(Utf8Path(file), error) && error.empty(),
                  "fixed glTF resources load without the previous diagnostic");
        }
    }
    {
        const auto original = files.root / "triangle.gltf";
        std::ifstream input(original);
        auto document = nlohmann::json::parse(input);
        auto& material = document.at("materials").at(0);
        material["emissiveFactor"] = {.5, 1, .25};
        material["emissiveTexture"] = {{"index", 0}};
        material["extensions"]["KHR_materials_emissive_strength"] = {{"emissiveStrength", 6}};
        document["extensionsRequired"] = {"KHR_materials_emissive_strength"};
        document["extensionsUsed"] = document["extensionsRequired"];
        const auto emissive = files.root / "lamp.gltf";
        std::ofstream(emissive) << document.dump();
        const auto model = ModelAsset::Load(Utf8Path(emissive), error);
        Check(model && model->materials.front().emissive.y == 6 &&
                  model->materials.front().emissive.x == 3 &&
                  model->materials.front().emissiveTexture == 0,
              "required glTF emissive strength preserves HDR radiance and its color texture");
    }
    std::ifstream source(files.root / "triangle.gltf", std::ios::binary);
    auto escaped = nlohmann::ordered_json::parse(source);
    const auto nested = files.root / "nested";
    std::filesystem::create_directory(nested);
    std::filesystem::copy_file(files.root / "paint.png", nested / "paint.png");
    escaped["buffers"][0]["uri"] = "../mesh.bin";
    const auto outside = nested / "bad.gltf";
    std::ofstream(outside) << escaped.dump();
    Check(!ModelAsset::Load(Utf8Path(outside), error) && !error.empty(),
          "a glTF buffer cannot escape the model directory");
}

void PlayerGpu() {
    RuntimeFiles files;
    const auto model = WriteGltfFixture(files.root, true, true);
    const auto path = files.root / "gltf-scene.lux";
    Scene scene;
    auto settings = scene.GetSettings();
    settings.clearColor = {.01f, .02f, .04f, 1};
    Check(scene.SetSettings(settings), "set the glTF fixture clear color");
    auto camera = scene.CreateEntity("Camera");
    camera.GetComponent<TransformComponent>().position.z = 3;
    camera.AddComponent<CameraComponent>().isPerspective = true;
    scene.CreateEntity("glTF Triangle").AddComponent<MeshRendererComponent>().meshPath =
        Utf8Path(model.filename());
    Check(scene.Serialize(Utf8Path(path)), "save the GLB reference in the ordinary scene format");
    ScenePlayerOptions options;
    options.scenePath = Utf8Path(path);
    options.width = 320;
    options.height = 240;
    ScenePlayer player(options);
    Check(player.Initialize() && player.LoadedScene(), "open the authored GLB scene in LuxPlayer");
    const auto hasBothParts = [](const GpuFrameCapture& capture) {
        std::array<size_t, 2> redPixels{};
        for (uint32 y = capture.height / 4; y < capture.height * 3 / 4; ++y)
            for (uint32 x = capture.width / 5; x < capture.width * 4 / 5; ++x) {
                if (x >= capture.width * 3 / 10 && x < capture.width * 7 / 10)
                    continue;
                const auto offset = (static_cast<size_t>(y) * capture.width + x) * 4;
                const auto red = capture.rgba.at(offset);
                redPixels[x >= capture.width / 2] += red > 60 &&
                                                     red > 3 * capture.rgba.at(offset + 1) &&
                                                     red > 3 * capture.rgba.at(offset + 2);
            }
        return redPixels[0] > 20 && redPixels[1] > 20;
    };
    const auto checkFrame = [&] {
        Check(player.GetGpuDevice().RequestFrameCapture() &&
                  player.RunFrame() == Application::FrameResult::Continue,
              "draw the GLB scene");
        GpuFrameCapture capture;
        Check(player.GetGpuDevice().ReadFrameCapture(capture) && capture.width >= 32 &&
                  capture.height >= 24,
              "capture the authored GLB scene");
        return hasBothParts(capture);
    };
    Check(checkFrame(), "both GLB instances retain the hierarchy and embedded base texture");
    Check(std::filesystem::remove(model), "the active GLB source can be removed");
    Check(checkFrame(), "both GLB instances stay in place after deletion of the active source");
    {
        WriteGltfFixture(files.root, true, true);
        const auto modelPath = Utf8Path(model);
        std::string error;
        const auto snapshot = ModelAsset::Load(modelPath, error);
        Check(snapshot && error.empty() && snapshot->submeshes.size() == 2,
              "snapshot the original two-part GLB");
        auto& gpu = player.GetGpuDevice();
        Renderer3D::Context context(&gpu);
        Renderer3D::ScopedContext useContext(context);
        PerspectiveCamera view(ToRadians(45), 4.0 / 3.0, .1, 100);
        view.SetPosition({0, 0, 3});
        view.SetTarget({0, 0, 0});
        Check(gpu.RequestFrameCapture() && gpu.BeginFrame() == GpuDevice::FrameStart::Ready &&
                  gpu.Clear(.01f, .02f, .04f, 1),
              "begin drawing the snapshotted GLB");
        context.BeginScene(view, settings.render3D, settings.exposure);
        const auto drawPart = [&](int32 index) {
            const auto node = snapshot->submeshes[static_cast<size_t>(index)].node;
            return Renderer3D::DrawGltfSubmesh(modelPath, snapshot->nodes[node].worldTransform, {},
                                               index, snapshot, {});
        };
        Check(drawPart(0), "the first draw uses the two-part snapshot");
        WriteGltfFixture(files.root, true);
        const auto replacement = ModelAsset::Load(modelPath, error);
        Check(replacement && error.empty() && replacement != snapshot &&
                  replacement->submeshes.size() == 1,
              "the GLB changes to a valid one-part model between draws");
        Check(drawPart(1), "the second draw still uses the two-part snapshot");
        context.EndScene();
        Check(gpu.EndFrame(), "present the frame drawn from the model snapshot");
        GpuFrameCapture capture;
        Check(gpu.ReadFrameCapture(capture) && hasBothParts(capture),
              "file replacement between draws preserves both parts in the frame");
    }
    player.Shutdown();
}
} // namespace Gltf

using Args = std::vector<std::string>;
using LuxEngine::ParseScenePlayerArguments;

void Parse() {
    std::string error;
    auto options = ParseScenePlayerArguments({"scene.lux"}, error);
    Check(options && options->scenePath == "scene.lux" && !options->view && !options->fullscreen &&
              options->maxFrames == 0,
          "scene path alone");
    options =
        ParseScenePlayerArguments({"a.lux", "--title", "게임", "--size", "800", "600",
                                   "--fullscreen", "--view", "1.5", "-2", "3", "--frames", "10"},
                                  error);
    Check(options && options->title == "게임" && options->width == 800 && options->height == 600 &&
              options->fullscreen && options->maxFrames == 10,
          "all options");
    Check(options && options->view && Near(options->view->centerX, 1.5f) &&
              Near(options->view->centerY, -2.0f) && Near(options->view->halfHeight, 3.0f),
          "view option");
    options = ParseScenePlayerArguments({"a.lux", "--view", "1.5e1", "-.5", "3e-1"}, error);
    Check(options && options->view && Near(options->view->centerX, 15.0f) &&
              Near(options->view->centerY, -.5f) && Near(options->view->halfHeight, .3f),
          "view accepts decimal and scientific notation");
    const std::vector<Args> invalid = {
        {},
        {"--title", "x"},
        {"a.lux", "b.lux"},
        {"a.lux", "--size", "10", "600"},
        {"a.lux", "--size", "800"},
        {"a.lux", "--view", "0", "0", "0"},
        {"a.lux", "--view", "0", "0", "nan"},
        {"a.lux", "--view", "0", "0", "inf"},
        {"a.lux", "--view", "0", "0", "1e999"},
        {"a.lux", "--view", " 1", "0", "3"},
        {"a.lux", "--view", "1 ", "0", "3"},
        {"a.lux", "--view", "+1", "0", "3"},
        {"a.lux", "--view", "1x", "0", "3"},
        {"a.lux", "--view", "1,5", "0", "3"},
        {"a.lux", "--frames", "-1"},
        {"a.lux", "--scripts"},
        {"a.lux", "--scripts", "--fullscreen", "--frames", "1"},
        {"a.lux", "--asset-root"},
        {"a.lux", "--asset-root", "--fullscreen"},
        {"a.lux", "--unknown"},
    };
    for (const auto& args : invalid) {
        error.clear();
        Check(!ParseScenePlayerArguments(args, error) && !error.empty(),
              "invalid arguments rejected");
    }
    Check(ParseScenePlayerArguments({"scene.lux"}, error).has_value() && error.empty(),
          "successful parsing clears an earlier error");
    options = ParseScenePlayerArguments(
        {"scene.lux", "--scripts", "./-scripts.dll", "--fullscreen"}, error);
    Check(options && options->scriptAssemblyPath == "./-scripts.dll" && options->fullscreen,
          "a relative script path with a dash needs no special quoting inside the parser");
    options = ParseScenePlayerArguments({"a.lux", "--asset-root", "한글 Assets"}, error);
    Check(options && options->assetRoot == "한글 Assets", "sprite asset root parses as UTF-8");

    namespace fs = std::filesystem;
    RuntimeFiles files;
    const auto authored = files.root / "scenes";
    const auto assets = files.root / "assets";
    fs::create_directories(authored);
    fs::create_directories(assets / "images");
    std::ofstream(assets / "images" / "sprite.png").put('x');
    const auto file = authored / "sprite.lux";
    const auto utf8 = file.u8string();
    LuxEngine::Scene scene;
    auto entity = scene.CreateEntity("Sprite");
    entity.AddComponent<LuxEngine::SpriteRendererComponent>().texturePath = "images/sprite.png";
    Check(scene.Serialize({reinterpret_cast<const char*>(utf8.data()), utf8.size()}),
          "asset-root test scene saves");
    LuxEngine::SceneManager manager;
    Check(manager.Preload(file, error), "default asset root is preloaded");
    const auto overridden = manager.LoadWithSpriteAssetRoot(file, assets);
    Check(static_cast<bool>(overridden),
          "player can load sprite assets from an explicit directory");
    const auto& sprite =
        overridden.scene->GetComponent<LuxEngine::SpriteRendererComponent>(entity.GetId());
    const auto pathFromUtf8 = [](const std::string& value) {
        return fs::path(
            std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
    };
    Check(fs::equivalent(pathFromUtf8(sprite.texturePath), assets / "images" / "sprite.png"),
          "explicit sprite asset root bypasses the ordinary preload result");
    const auto ordinary = manager.Load(file);
    Check(ordinary &&
              pathFromUtf8(
                  ordinary.scene->GetComponent<LuxEngine::SpriteRendererComponent>(entity.GetId())
                      .texturePath)
                      .parent_path() == fs::weakly_canonical(authored / "images"),
          "custom asset root leaves ordinary scene resolution intact");
    Check(!manager.LoadWithSpriteAssetRoot(file, files.root / "missing"),
          "missing sprite asset directory is rejected");

    std::ofstream(files.root / "project.luxproj").put('\n');
    fs::create_directories(files.root / "images");
    std::ofstream(files.root / "images" / "sprite.png").put('x');
    const auto legacy = authored / "legacy.lux";
    std::ofstream(legacy) << "LuxEngineScene 3 1\n7\n1 \"Legacy\"\n1 0 0 0 0 0 0 1 1 1\n"
                             "1 1 1 1 1 \"images/sprite.png\"\n0\n0\n0\n0\n0\n0\n0\n0\n";
    const auto oldScene = manager.Load(legacy);
    Check(oldScene &&
              fs::equivalent(
                  pathFromUtf8(oldScene.scene->GetComponent<LuxEngine::SpriteRendererComponent>(7)
                                   .texturePath),
                  files.root / "images" / "sprite.png"),
          "legacy sprite paths resolve against the original project marker");
    const auto oldOverride = manager.LoadWithSpriteAssetRoot(legacy, assets);
    Check(oldOverride &&
              fs::equivalent(pathFromUtf8(oldOverride.scene
                                              ->GetComponent<LuxEngine::SpriteRendererComponent>(7)
                                              .texturePath),
                             assets / "images" / "sprite.png"),
          "explicit sprite asset directory also overrides a legacy project marker");
}

void Fit() {
    const LuxEngine::ScenePlayerCameraView aggregateCamera{{0, 3, 9}, {}, {0, 1, 0}, 60};
    Check(Near(aggregateCamera.nearClip, .1f) && Near(aggregateCamera.farClip, 1000.f),
          "public runtime camera aggregate preserves original clipping defaults");
    LuxEngine::Scene empty;
    auto view = LuxEngine::FitSceneView(empty);
    Check(Near(view.centerX, 0) && Near(view.centerY, 0) && view.halfHeight > 0,
          "empty scene view");
    LuxEngine::Scene scene;
    auto a = scene.CreateEntity("a");
    a.GetComponent<LuxEngine::TransformComponent>().position = {-10, 0, 0};
    auto b = scene.CreateEntity("b");
    b.GetComponent<LuxEngine::TransformComponent>().position = {10, 4, 0};
    view = LuxEngine::FitSceneView(scene);
    Check(Near(view.centerX, 0) && Near(view.centerY, 2), "view centred on entities");
    // 21 units wide at 16:9 needs a half height of at least 21/2*9/16.
    Check(view.halfHeight >= 21.0f * 0.5f * 9.0f / 16.0f, "view fits width");
    constexpr double portrait = 9.0 / 16.0;
    const auto portraitView = LuxEngine::FitSceneView(scene, portrait);
    const auto portraitCameras =
        LuxEngine::ResolveScenePlayerCameras(scene, portrait, portraitView);
    const auto portraitMatrix = portraitCameras.camera2D.GetViewProjectionMatrix();
    Check(std::abs(portraitMatrix.TransformPoint({-10.5, 0, 0}).x) < 1 &&
              std::abs(portraitMatrix.TransformPoint({10.5, 4, 0}).x) < 1,
          "automatic framing includes the scene width in a portrait window");
    auto map = scene.CreateEntity("map");
    map.GetComponent<LuxEngine::TransformComponent>().position = {0, -50, 0};
    map.AddComponent<LuxEngine::TilemapComponent>();
    view = LuxEngine::FitSceneView(scene);
    Check(view.centerY < 0 && view.halfHeight > 25, "tilemap extends view");
    LuxEngine::Scene rotated;
    auto sprite = rotated.CreateEntity("rotated");
    auto& transform = sprite.GetComponent<LuxEngine::TransformComponent>();
    transform.scale = {4, 2, 1};
    transform.rotation.z = LuxEngine::ToRadians(90);
    view = LuxEngine::FitSceneView(rotated);
    Check(view.halfHeight >= 2.19f, "rotated bounds fit the visible frame");
    LuxEngine::Scene parented;
    auto parent = parented.CreateEntity("parent");
    parent.GetComponent<LuxEngine::TransformComponent>().position = {20, 10, 0};
    auto child = parented.CreateEntity("child");
    child.GetComponent<LuxEngine::TransformComponent>().position = {4, 2, 0};
    Check(parented.SetParent(child.GetId(), parent.GetId(), false), "fit hierarchy initializes");
    view = LuxEngine::FitSceneView(parented);
    Check(Near(view.centerX, 22) && Near(view.centerY, 11),
          "view uses world transforms for parented scene objects");
    LuxEngine::Scene cameraScene;
    auto camera = cameraScene.CreateEntity("primary camera");
    auto& cameraComponent = camera.AddComponent<LuxEngine::CameraComponent>();
    cameraComponent.isPerspective = true;
    cameraComponent.fov = 70;
    cameraComponent.nearClip = .75f;
    cameraComponent.farClip = 75.f;
    auto cameraView = LuxEngine::ResolveScenePlayerCamera(cameraScene, {});
    Check(Near(cameraView.position.x, 0) && Near(cameraView.position.z, 0) &&
              Near(cameraView.target.z, -1) && Near(cameraView.up.y, 1) &&
              Near(cameraView.fovDegrees, 70) && Near(cameraView.nearClip, .75f) &&
              Near(cameraView.farClip, 75.f),
          "an origin camera looks forward rather than degenerating into eye == target");
    const auto projection =
        LuxEngine::Mat4::Perspective(LuxEngine::ToRadians(cameraView.fovDegrees), 4.f / 3.f,
                                     cameraView.nearClip, cameraView.farClip);
    const auto depth = [&](float z) {
        return (projection.elements[10] * z + projection.elements[14]) / -z;
    };
    Check(Near(depth(-.75f), 0) && Near(depth(-75.f), 1) && depth(-.5f) < 0 && depth(-100.f) > 1,
          "resolved runtime camera clip settings map the custom near/far planes and reject outside "
          "depth");
    cameraComponent.nearClip = 1e20f;
    cameraComponent.farClip = 1e21f;
    const auto largeCamera = LuxEngine::ResolveScenePlayerCamera(cameraScene, {});
    Check(largeCamera.nearClip == 1e20f && largeCamera.farClip == 1e21f,
          "runtime accepts large clipping ranges whose projection fits in float");
    const float extremeFar = std::numeric_limits<float>::max();
    const std::array<std::pair<float, float>, 7> invalidClips{
        {{0, 75},
         {-1, 75},
         {75, 75},
         {80, 75},
         {std::numeric_limits<float>::quiet_NaN(), 75},
         {1, std::numeric_limits<float>::infinity()},
         {std::nextafter(extremeFar, 0.0f), extremeFar}}};
    for (const auto [near, far] : invalidClips) {
        cameraComponent.nearClip = near;
        cameraComponent.farClip = far;
        const auto fallback = LuxEngine::ResolveScenePlayerCamera(cameraScene, {});
        Check(Near(fallback.nearClip, .1f) && Near(fallback.farClip, 1000.f),
              "invalid camera clipping ranges fall back to a finite runtime projection");
    }
    cameraComponent.nearClip = .75f;
    cameraComponent.farClip = 75.f;
    auto cameraParent = cameraScene.CreateEntity("camera parent");
    auto& parentTransform = cameraParent.GetComponent<LuxEngine::TransformComponent>();
    parentTransform.position = {10, 2, 0};
    parentTransform.rotation.y = LuxEngine::ToRadians(90);
    parentTransform.scale = {-2, 3, -4};
    camera.GetComponent<LuxEngine::TransformComponent>().position.z = 5;
    Check(cameraScene.SetParent(camera.GetId(), cameraParent.GetId(), false),
          "parented camera hierarchy configures");
    cameraView = LuxEngine::ResolveScenePlayerCamera(cameraScene, {});
    Check(Near(cameraView.position.x, -10) && Near(cameraView.position.y, 2) &&
              cameraView.target.x > cameraView.position.x + .99f && Near(cameraView.up.y, 1),
          "primary camera uses world -Z/+Y axes under a rotated, negatively scaled parent");
    camera.GetComponent<LuxEngine::TransformComponent>().rotation.z = LuxEngine::ToRadians(90);
    cameraView = LuxEngine::ResolveScenePlayerCamera(cameraScene, {});
    Check(cameraView.up.z < -.99f, "camera roll changes its actual view up axis");
    parentTransform.scale = {0, 0, 0};
    cameraView = LuxEngine::ResolveScenePlayerCamera(cameraScene, {});
    const auto finite = [](LuxEngine::Vec3 value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };
    const auto forward = cameraView.target - cameraView.position;
    Check(finite(cameraView.position) && finite(cameraView.target) && finite(cameraView.up) &&
              forward.Dot(forward) > .9f &&
              forward.Cross(cameraView.up).Dot(forward.Cross(cameraView.up)) > .9f,
          "collapsed camera axes receive a finite, non-parallel view basis");
}

void Primary2DCamera() {
    using namespace LuxEngine;
    Scene scene;
    auto parent = scene.CreateEntity("Rig");
    parent.GetComponent<TransformComponent>().position = {1e12, 1e12, 0};
    auto camera = scene.CreateEntity("Primary 2D");
    Check(scene.SetParent(camera.GetId(), parent.GetId(), false), "2D camera parented");
    camera.GetComponent<TransformComponent>().position = {.125, .25, 0};
    camera.GetComponent<TransformComponent>().rotation.z = ToRadians(90);
    camera.AddComponent<CameraComponent>().orthoSize = 2;
    const auto cameras = ResolveScenePlayerCameras(scene, 2, {0, 0, 4.5});
    Check(!cameras.perspective && cameras.camera2D.GetPosition().x == 1e12 + .125 &&
              cameras.camera2D.GetPosition().y == 1e12 + .25 &&
              std::abs(cameras.camera2D.GetRotation() - ToRadians(90)) < 1e-12 &&
              cameras.camera2D.GetProjectionMatrix().elements[0] == .25,
          "primary 2D world position, rotation and size override the editor fallback view");
    parent.GetComponent<TransformComponent>().scale.z = -1;
    const auto mirroredDepth = ResolveScenePlayerCameras(scene, 2, {});
    Check(std::abs(mirroredDepth.camera2D.GetRotation() - ToRadians(90)) < 1e-12,
          "a negative parent Z scale does not rotate a 2D camera");
    camera.GetComponent<TransformComponent>().scale.z = 0;
    const auto flatCamera = ResolveScenePlayerCameras(scene, 2, {});
    Check(std::abs(flatCamera.camera2D.GetRotation() - ToRadians(90)) < 1e-12,
          "a parented camera retains its authored angle with collapsed local depth");
    parent.GetComponent<TransformComponent>().scale = {-1, 1, 0};
    const auto reflectedCamera = ResolveScenePlayerCameras(scene, 2, {});
    Check(std::abs(reflectedCamera.camera2D.GetRotation() + ToRadians(90)) < 1e-12,
          "an XY reflection preserves the reflected angle even when world depth is collapsed");
    camera.GetComponent<CameraComponent>().isPrimary = false;
    const auto fallback = ResolveScenePlayerCameras(scene, 2, {3, 4, 5});
    Check(fallback.camera2D.GetPosition().x == 3 && fallback.camera2D.GetPosition().y == 4 &&
              fallback.camera2D.GetRotation() == 0 &&
              fallback.camera2D.GetProjectionMatrix().elements[0] == .1,
          "the explicit view is used when no primary camera is active");
}

void FitMeshBounds() {
    using namespace LuxEngine;
    RuntimeFiles files;
    const auto writeMesh = [&](const char* name, int farZ) {
        std::ofstream obj(files.root / name);
        obj << "v 100 -1 -700\nv 102 -1 -700\nv 101 1 " << farZ << "\nf 1 2 3\n";
        obj.close();
        Check(static_cast<bool>(obj), "off-center OBJ is written");
    };
    writeMesh("off-center.obj", -700);
    writeMesh("deep.obj", -2700);
    Scene scene;
    const auto path = (files.root / "scene.lux").u8string();
    scene.SetSourcePath({reinterpret_cast<const char*>(path.data()), path.size()});
    auto entity = scene.CreateEntity("Off-center mesh");
    auto& mesh = entity.AddComponent<MeshRendererComponent>();
    mesh.meshPath = "off-center.obj";
    constexpr double aspect = 9.0 / 16;
    const auto fitted = FitSceneView(scene, aspect);
    Check(fitted.centerX == 101, "automatic view centers on the OBJ bounds");

    auto& transform = entity.GetComponent<TransformComponent>();
    transform.rotation.z = ToRadians(90);
    transform.scale = {2, 3, 1};
    const auto view = FitSceneView(scene, aspect);
    const auto cameras = ResolveScenePlayerCameras(scene, aspect, view);
    Check(cameras.perspective && cameras.camera3D.GetTarget().z == -700 &&
              std::abs(view.centerY - 202) < 1e-9,
          "fallback camera uses rotated, scaled mesh bounds in all three dimensions");
    const auto visible = [](const Mat4& matrix, Vec3 p) {
        const auto clip = matrix.TransformPoint(p);
        const auto* m = matrix.elements;
        const auto w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
        return w > 0 && std::abs(clip.x) < w && std::abs(clip.y) < w && clip.z > 0 && clip.z < w;
    };
    const auto& matrix = cameras.camera3D.GetViewProjectionMatrix();
    const auto world = transform.GetTransform();
    for (const Vec3 vertex : {Vec3{100, -1, -700}, Vec3{102, -1, -700}, Vec3{101, 1, -700}})
        Check(visible(matrix, world.TransformPoint(vertex)),
              "every OBJ vertex lies inside the fitted perspective view");

    transform = {};
    mesh.meshPath = "deep.obj";
    const auto deep = ResolveScenePlayerCameras(scene, aspect, FitSceneView(scene, aspect));
    Check(visible(deep.camera3D.GetViewProjectionMatrix(), {100, -1, -700}) &&
              visible(deep.camera3D.GetViewProjectionMatrix(), {101, 1, -2700}),
          "a deep mesh fits from its nearest edge through its farthest vertex");
    mesh.meshPath = "missing.obj";
    const auto fallback = FitSceneView(scene, aspect);
    Check(fallback.centerX == 0 && fallback.centerY == 0 && std::isfinite(fallback.halfHeight),
          "an unavailable mesh uses a finite fallback size");
}

void PrimaryPerspectiveSelection() {
    using namespace LuxEngine;
    Scene scene;
    auto first = scene.CreateEntity("First primary");
    auto second = scene.CreateEntity("Second primary");
    auto& firstCamera = first.AddComponent<CameraComponent>();
    firstCamera.isPerspective = true;
    second.AddComponent<CameraComponent>().isPerspective = true;
    first.GetComponent<TransformComponent>().position = {1e160, 0, 0};
    second.GetComponent<TransformComponent>().position = {42, 0, 5};
    const auto positionFallback = ResolveScenePlayerCameras(scene, 1, {});
    Check(positionFallback.perspective && positionFallback.cameraEntity == second.GetId() &&
              positionFallback.camera3D.GetPosition().x == 42,
          "an unusable primary pose hands both its camera ID and view to the next camera");

    first.GetComponent<TransformComponent>().position = {5, 0, 0};
    firstCamera.fov = .01;
    const auto projectionFallback = ResolveScenePlayerCameras(scene, 1e-37, {});
    Check(projectionFallback.cameraEntity == second.GetId() &&
              projectionFallback.camera3D.GetPosition().x == 42,
          "camera ID and projection use the same aspect-dependent validation");
}

void StreamingObserverSkipsInvalidPlayer() {
    using namespace LuxEngine;
    Scene scene;
    auto invalid = scene.CreateEntity("Player");
    auto parent = scene.CreateEntity("Parent");
    invalid.GetComponent<TransformComponent>().position.x = 1e308;
    parent.GetComponent<TransformComponent>().position.x = 1e308;
    Check(scene.SetParent(invalid.GetId(), parent.GetId(), false), "streaming test hierarchy");
    auto camera = scene.CreateEntity("Camera");
    camera.GetComponent<TransformComponent>().position = {12, 4, 0};
    camera.AddComponent<CameraComponent>();
    const auto fallback = ResolveSceneStreamingObserver(scene, {}, invalid.GetId());
    Check(fallback.x == 12 && fallback.y == 4, "invalid player position falls back to the camera");
    auto valid = scene.CreateEntity("Player");
    valid.GetComponent<TransformComponent>().position = {1, 2, 3};
    const auto observer = ResolveSceneStreamingObserver(scene, {}, invalid.GetId());
    Check(observer.x == 1 && observer.y == 2 && observer.z == 3,
          "a usable tagged player takes precedence over the camera");
}

void FixedUpdateTime() {
    constexpr double step = 1.0 / 30;
    LuxEngine::FixedStepAccumulator clock;
    Check(clock.InterpolationAlpha(step) == 0, "render phase starts at zero");
    clock.AddElapsed(step / 2);
    Check(!clock.Consume(step), "a fractional step waits for more elapsed time");
    Check(clock.InterpolationAlpha(step) == .5, "render phase advances between simulation ticks");
    clock.AddElapsed(step * 3);
    Check(clock.InterpolationAlpha(step) == 1, "render phase clamps pending full ticks");
    for (int i = 0; i < 3; ++i)
        Check(clock.Consume(step), "a delayed frame performs multiple scene ticks");
    Check(!clock.Consume(step), "the remaining fraction does not advance an extra tick");
    Check(std::abs(clock.InterpolationAlpha(step) - .5) < 1e-12,
          "render phase retains the fractional remainder after catch-up ticks");
    clock.AddElapsed(step * .51);
    Check(clock.Consume(step), "substep time is carried across frames");
    clock.AddElapsed(5);
    for (int i = 0; i < clock.MaxUpdatesPerFrame; ++i)
        Check(clock.Consume(step), "the catch-up cap still permits timely simulation");
    clock.DropBacklog(step);
    Check(!clock.Consume(step), "excess backlog is dropped after catch-up");
    Check(clock.InterpolationAlpha(step) == 0, "discarded backlog also resets the render phase");
    Check(clock.InterpolationAlpha(0) == 1 &&
              clock.InterpolationAlpha(std::numeric_limits<double>::quiet_NaN()) == 1,
          "an invalid fixed step uses the current pose");
}

// Writes a small platformer scene for the LuxPlayer smoke test.
void WriteScene(const std::string& path) {
    LuxEngine::Scene scene;
    auto ground = scene.CreateEntity("땅");
    auto& groundTransform = ground.GetComponent<LuxEngine::TransformComponent>();
    groundTransform.position = {0, -3, 0};
    groundTransform.scale = {12, 1, 1};
    ground.AddComponent<LuxEngine::SpriteRendererComponent>().color = {0.3f, 0.6f, 0.3f, 1};
    ground.AddComponent<LuxEngine::Rigidbody2DComponent>().type = LuxEngine::BodyType::Static;
    ground.AddComponent<LuxEngine::BoxCollider2DComponent>();
    auto player = scene.CreateEntity("플레이어");
    player.GetComponent<LuxEngine::TransformComponent>().position = {0, 1, 0};
    player.AddComponent<LuxEngine::SpriteRendererComponent>().color = {0.9f, 0.7f, 0.2f, 1};
    player.AddComponent<LuxEngine::Rigidbody2DComponent>();
    player.AddComponent<LuxEngine::BoxCollider2DComponent>();
    auto map = scene.CreateEntity("타일맵");
    map.GetComponent<LuxEngine::TransformComponent>().position = {-6, -6, 0};
    auto& tiles = map.AddComponent<LuxEngine::TilemapComponent>();
    tiles.Resize(12, 2);
    for (LuxEngine::uint32 x = 0; x < 12; ++x)
        tiles.Set(x, 0, 2);
    auto cube3D = scene.CreateEntity("3D 배경 큐브");
    cube3D.AddComponent<LuxEngine::ScriptComponent>() = {"Unused.DisabledScript", false};
    cube3D.GetComponent<LuxEngine::TransformComponent>().position = {2.5f, 1.5f, -2.0f};
    cube3D.AddComponent<LuxEngine::MeshRendererComponent>().material.albedoColor = {0.4f, 0.7f,
                                                                                    0.9f, 1.0f};
    Check(scene.Serialize(path), "smoke scene written");
    LuxEngine::Scene loaded;
    Check(loaded.Deserialize(path) && loaded.GetEntityCount() == 4, "smoke scene reloads");
}

int CameraClippingGpu(const std::filesystem::path& directory) {
    LuxEngine::Scene scene;
    auto settings = scene.GetSettings();
    settings.clearColor = {.01f, .02f, .04f, 1};
    Check(scene.SetSettings(settings), "camera clip GPU scene settings configure");
    auto camera = scene.CreateEntity("Custom clipping camera");
    auto& component = camera.AddComponent<LuxEngine::CameraComponent>();
    component.isPerspective = true;
    component.fov = 60;
    component.nearClip = 3;
    component.farClip = 5;
    const std::array<LuxEngine::Vec3, 3> positions{{{-.85f, 0, -2}, {0, 0, -4}, {3.4f, 0, -8}}};
    for (size_t index = 0; index < positions.size(); ++index) {
        auto object = scene.CreateEntity("Clip test cube");
        auto& transform = object.GetComponent<LuxEngine::TransformComponent>();
        transform.position = positions[index];
        transform.scale = {.5f, .5f, .5f};
        object.AddComponent<LuxEngine::MeshRendererComponent>().material.albedoColor =
            index == 1 ? LuxEngine::Vec4{.15f, .85f, .2f, 1} : LuxEngine::Vec4{.9f, .1f, .1f, 1};
    }
    const auto file = directory / "camera-clipping.lux";
    const auto encoded = file.u8string();
    const std::string path(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    Check(scene.Serialize(path), "camera clipping scene saves");
    LuxEngine::ScenePlayerOptions options;
    options.scenePath = path;
    options.width = 320;
    options.height = 240;
    LuxEngine::ScenePlayer player(options);
    if (!player.Initialize()) {
        std::cout << "SKIP: scene player camera GPU/window backend is unavailable\n";
        return 77;
    }
    Check(player.LoadedScene(), "runtime custom camera scene loads");
    const auto frame = [&] {
        SDL_Delay(18);
        const auto status = player.RunFrame();
        Check(status == LuxEngine::Application::FrameResult::Continue ||
                  status == LuxEngine::Application::FrameResult::Skipped,
              "runtime custom camera keeps rendering");
    };
    frame();
    Check(player.GetGpuDevice().RequestFrameCapture(), "runtime camera capture queues");
    frame();
    LuxEngine::GpuFrameCapture image;
    Check(player.GetGpuDevice().ReadFrameCapture(image) && image.width >= 32 && image.height >= 24,
          "runtime camera capture completes");
    const auto pixel = [&](float x) {
        const auto offset = (static_cast<size_t>(image.height / 2) * image.width +
                             static_cast<size_t>(x * image.width)) *
                            4;
        return std::array<int, 3>{image.rgba.at(offset), image.rgba.at(offset + 1),
                                  image.rgba.at(offset + 2)};
    };
    const auto center = pixel(.5f), near = pixel(.224f), far = pixel(.776f);
    Check(center[1] > center[0] + 20 && center[1] > center[2] + 20,
          "runtime custom camera renders geometry between the configured clipping planes");
    Check(near[0] < near[1] + 15 && far[0] < far[1] + 15,
          "actual runtime projection ignores custom near/far clipping settings");
    player.Close();
    player.RunFrame();
    return 0;
}

void LegacyLightingAndFrameLimitGpu(const std::filesystem::path& directory) {
    using namespace LuxEngine;
    Scene scene;
    auto settings = scene.GetSettings();
    settings.clearColor = {.01f, .01f, .01f, 1};
    settings.render3D.mode = Render3DMode::Legacy;
    settings.render3D.sun.direction = {0, 0, -1};
    settings.render3D.sun.intensity = 3;
    scene.CreateEntity("Camera").AddComponent<CameraComponent>().isPerspective = true;
    auto cube = scene.CreateEntity("White cube");
    cube.GetComponent<TransformComponent>().position = {0, 0, -3};
    cube.AddComponent<MeshRendererComponent>().material.albedoColor = {.7f, .7f, .7f, 1};
    const auto render = [&](Vec3 color) {
        settings.render3D.sun.color = color;
        Check(scene.SetSettings(settings), "save the authored Legacy light settings");
        const auto file = (directory / "legacy-light.lux").u8string();
        ScenePlayerOptions options;
        options.scenePath.assign(reinterpret_cast<const char*>(file.data()), file.size());
        options.width = 320;
        options.height = 240;
        Check(scene.Serialize(options.scenePath), "write a Legacy lighting fixture");
        ScenePlayer player(options);
        Check(player.Initialize(), "start the Legacy lighting fixture");
        Check(player.GetGpuDevice().RequestFrameCapture(), "queue a Legacy frame capture");
        Check(player.RunFrame() == Application::FrameResult::Continue, "render the Legacy scene");
        GpuFrameCapture image;
        Check(player.GetGpuDevice().ReadFrameCapture(image), "read the Legacy frame");
        const auto offset =
            (static_cast<size_t>(image.height / 2) * image.width + image.width / 2) * 4;
        const std::array<int, 3> rgb{image.rgba.at(offset), image.rgba.at(offset + 1),
                                     image.rgba.at(offset + 2)};
        player.Shutdown();
        return rgb;
    };
    const auto red = render({1, 0, 0});
    const auto blue = render({0, 0, 1});
    Check(red[0] > red[2] + 20 && blue[2] > blue[0] + 20,
          "the standalone Legacy renderer respects authored directional light colors");

    const auto file = (directory / "legacy-light.lux").u8string();
    ScenePlayerOptions options;
    options.scenePath.assign(reinterpret_cast<const char*>(file.data()), file.size());
    options.width = 320;
    options.height = 240;
    options.maxFrames = 1;
    ScenePlayer player(options);
    Check(player.Initialize(), "start the one-frame fixture");
    SDL_Delay(40);
    Check(player.RunFrame() == Application::FrameResult::Stopped &&
              player.GetPresentedFrameCount() == 1,
          "a one-frame budget presents a frame even when physics steps run first");
    Check(player.Initialize() && player.GetPresentedFrameCount() == 0,
          "restarting the player resets its presented-frame count");
    SDL_Delay(40);
    Check(player.RunFrame() == Application::FrameResult::Stopped &&
              player.GetPresentedFrameCount() == 1,
          "the restarted player presents exactly one frame again");
    Check(!player.LoadedScene(), "a stopped player clears its loaded state");
    Check(std::filesystem::remove(directory / "legacy-light.lux"), "remove the restart fixture");
    Check(!player.Initialize() && !player.LoadedScene() && !player.RuntimeFailed(),
          "a failed restart cannot reuse the previous successful scene state");
}

int SceneManagementGpu() {
    namespace fs = std::filesystem;
    RuntimeFiles files;

    if (const int result = CameraClippingGpu(files.root); result != 0)
        return result;
    LegacyLightingAndFrameLimitGpu(files.root);

    const auto text = [](const fs::path& path) {
        const auto bytes = path.u8string();
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };
    LuxEngine::Scene source;
    auto sprite = source.CreateEntity("스트리밍 스프라이트");
    sprite.GetComponent<LuxEngine::TransformComponent>().scale = {2, 2, 1};
    sprite.AddComponent<LuxEngine::SpriteRendererComponent>().color = {.2f, .8f, .3f, 1};
    auto settings = source.GetSettings();
    settings.clearColor = {.04f, .08f, .16f, 1};
    settings.exposure = .5f;
    Check(source.SetSettings(settings), "runtime source settings are valid");
    const auto base = files.root / "base.lux", composed = files.root / "composed.lux";
    Check(source.Serialize(text(base)), "runtime source file serializes");
    LuxEngine::SceneManager manager;
    LuxEngine::Scene target;
    std::string error;
    const auto root =
        manager.AddSceneInstance(target, base, LuxEngine::SceneReferenceMode::Inherited, error);
    Check(root != 0, "runtime inherited scene composes");
    Check(manager.ConfigureStreaming(target, root, {0, 0}, 5, 8, error) &&
              manager.UnloadInstance(target, root, error),
          "runtime source starts as an unloaded streaming region");
    Check(LuxEngine::SceneManager::Save(target, composed, {}, error),
          "runtime composed file saves with portable paths");
    Check(manager.Load(composed).scene != nullptr,
          "runtime composed fixture is valid before GPU startup");
    LuxEngine::ScenePlayerOptions options;
    options.scenePath = text(composed);
    options.width = 320;
    options.height = 240;
    options.view = LuxEngine::ScenePlayerView{0, 0, 4.5f};
    LuxEngine::ScenePlayer player(options);
    if (!player.Initialize()) {
        std::cout << "SKIP: scene player GPU/window backend is unavailable\n";
        return 77;
    }
    Check(player.LoadedScene(), "runtime player opens composed scenes");
    const auto frame = [&] {
        SDL_Delay(18);
        const auto result = player.RunFrame();
        Check(result == LuxEngine::Application::FrameResult::Continue ||
                  result == LuxEngine::Application::FrameResult::Skipped,
              "runtime composed scene keeps rendering");
    };
    bool sawSprite = false;
    const auto deadline = SDL_GetTicks() + 5000;
    LuxEngine::GpuFrameCapture image;
    do {
        frame();
        Check(player.GetGpuDevice().RequestFrameCapture(), "runtime frame capture queues");
        frame();
        Check(player.GetGpuDevice().ReadFrameCapture(image), "runtime frame capture completes");
        Check(image.width > 2 && image.height > 2 &&
                  image.rgba.size() == static_cast<size_t>(image.width) * image.height * 4,
              "runtime frame capture contains a complete image");
        const auto middle =
            (static_cast<size_t>(image.height / 2) * image.width + image.width / 2) * 4;
        sawSprite = image.rgba[middle + 1] > image.rgba[middle] + 20 &&
                    image.rgba[middle + 1] > image.rgba[middle + 2] + 20;
    } while (!sawSprite && SDL_GetTicks() < deadline);
    Check(sawSprite, "runtime player streams inherited scene entities into the actual GPU frame");
    const size_t edge = (static_cast<size_t>(image.width) + 1) * 4;
    Check(image.rgba[edge + 2] > image.rgba[edge + 1] + 10 &&
              image.rgba[edge + 1] > image.rgba[edge] + 3,
          "scene-specific environment color reaches the actual runtime framebuffer");
    player.Close();
    player.RunFrame();
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view suite = argc > 1 ? argv[1] : "parse";
        if (suite == "parse")
            Parse();
        else if (suite == "fit") {
            Fit();
            Primary2DCamera();
            FitMeshBounds();
            PrimaryPerspectiveSelection();
            StreamingObserverSkipsInvalidPlayer();
            FixedUpdateTime();
        } else if (suite == "write-scene" && argc > 2)
            WriteScene(argv[2]);
        else if (suite == "scene-management-gpu")
            return SceneManagementGpu();
        else if (suite == "gltf")
            Gltf::Assets();
        else if (suite == "gltf-gpu")
            Gltf::PlayerGpu();
        else
            throw std::runtime_error("Unknown suite");
        std::cout << suite << ": " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
