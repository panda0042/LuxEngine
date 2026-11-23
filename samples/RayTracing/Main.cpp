#include <LuxEngine/Application.hpp>
#include <LuxEngine/EntryPoint.hpp>
#include <LuxEngine/Renderer/Renderer3D/Renderer3D.hpp>
#include <LuxEngine/Renderer/Water/WaterSurface.hpp>
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace LuxEngine;

Material3D Opaque(Vec4 color, float metallic = 0, float roughness = .55f) {
    Material3D material;
    material.alphaMode = MaterialAlphaMode::Opaque;
    material.albedoColor = color;
    material.metallic = metallic;
    material.roughness = roughness;
    return material;
}

class RayTracingDemo final : public Application {
public:
    explicit RayTracingDemo(bool smoke)
        : Application(
              {"LuxEngine · Ray Tracing and Fog", smoke ? 384u : 960u, smoke ? 216u : 540u}),
          smoke_(smoke) {}

private:
    void OnInit() override {
        renderer_.Init(&GetGpuDevice());
        const std::array waves{WaterWave{{1, .5}, .018, 1.8, .9}};
        if (!water_.Configure({{6, 5}, 24, 20, -.1}, waves) || !water_.SyncGpu(GetGpuDevice()))
            throw std::runtime_error("Could not create the demo pool");
        const auto& info = GetGpuDevice().GetInfo();
        std::cout << info.deviceName << " / " << info.deviceType << '\n'
                  << (info.rayTracing ? "Hardware ray queries available.\n"
                                      : "Using raster lighting and volumetric fog.\n");
        settings_.sun = {{-.55, -1, -.75}, {1, .92, .8}, 3.4f};
        settings_.environmentColor = {.2, .26, .35};
        settings_.showSkybox = true;
        settings_.shadowRange = 24;
        settings_.rayTracing = {true, true, true, true, 3, 45, .45f};
        settings_.fog = {{.78, .88, .98}, .055f, 45, .2f, .5, .25f, true};
    }

    void OnEvent(Event& event) override {
        Application::OnEvent(event);
        EventDispatcher dispatcher(event);
        dispatcher.Dispatch<KeyPressedEvent>([&](KeyPressedEvent& key) {
            if (key.IsRepeat())
                return false;
            switch (static_cast<LuxKey>(key.GetKeyCode())) {
            case LuxKey::Escape:
                Close();
                break;
            case LuxKey::R: {
                auto& rays = settings_.rayTracing;
                const bool enable = !rays.reflections;
                rays.shadows = rays.reflections = rays.refractions = enable;
                break;
            }
            case LuxKey::G:
                settings_.rayTracing.indirectDiffuse = !settings_.rayTracing.indirectDiffuse;
                break;
            case LuxKey::F:
                settings_.fog.density = settings_.fog.density > 0 ? 0.f : .055f;
                break;
            case LuxKey::T:
                settings_.fog.rayTracedShadows = !settings_.fog.rayTracedShadows;
                break;
            default:
                return false;
            }
            return true;
        });
    }

    void OnRender(float) override {
        uint32 width = 0, height = 0;
        GetWindow().GetFramebufferSize(width, height);
        PerspectiveCamera camera(ToRadians(53), double(std::max(1u, width)) / std::max(1u, height),
                                 .1, 80);
        camera.SetPosition({8.4, 5.4, 11});
        camera.SetTarget({0, .8, .6});
        GetGpuDevice().Clear(.04f, .07f, .11f, 1);
        renderer_.BeginScene(camera, settings_, 1);

        const auto floor = Opaque({.20f, .25f, .28f, 1}, .2f, .25f);
        renderer_.DrawCube({0, -.93, 0}, {26, .3, 24}, floor);
        renderer_.DrawCube({-1, -.57, 1.8}, {1.3, .3, 1.5}, Opaque({.96f, .25f, .07f, 1}));
        renderer_.DrawCube({1.0, -.53, .8}, {1.25, .4, 1.25}, Opaque({.09f, .65f, .8f, 1}));
        renderer_.DrawMesh(water_.GetMesh(), Mat4::Translate({0, 0, 1.25}),
                           WaterSurface::MakeMaterial());

        renderer_.DrawCube({-2.1, .8, -1.4}, {1.2, 3.4, 1.0}, Opaque({.91f, .16f, .08f, 1}));
        renderer_.DrawCube({2.5, .35, -1.25}, {1.35, 2.4, 1.35}, Opaque({.06f, .56f, .19f, 1}));
        renderer_.DrawCube({-.6, 1.22, -5.1}, {5.5, 4, .13},
                           Opaque({.96f, .95f, .91f, 1}, 1, .055f));
        renderer_.DrawCube({3.7, 1.9, -3.85}, {1.2, 5.5, 1.2}, Opaque({.52f, .41f, .32f, 1}));
        renderer_.DrawCube({-4.4, 2.2, -3.8}, {1, 6.0, 1}, Opaque({.50f, .36f, .21f, 1}));
        renderer_.DrawCube({-.25, 3.4, -3.75}, {9.3, .34, 1.05}, Opaque({.44f, .38f, .29f, 1}));
        renderer_.EndScene();
    }

    void OnFramePresented() override {
        if (!smoke_ || ++frames_ < 4)
            return;
        const auto stats = GetGpuDevice().GetFrameStats();
        if (stats.volumetricFogPasses != 1)
            throw std::runtime_error("Froxel fog did not render");
        if (GetGpuDevice().GetInfo().rayTracing &&
            (stats.rayTracedPasses != 1 || stats.accelerationStructureBuilds == 0 ||
             stats.rayVolumetricFogPasses != 1))
            throw std::runtime_error("Hardware ray lighting or ray-traced fog did not render");
        if (GetGpuDevice().GetCounters().validationErrors)
            throw std::runtime_error("GPU validation reported an error");
        Close();
    }

    void OnShutdown() override {
        water_.ReleaseGpu();
        renderer_.Shutdown();
    }

    bool smoke_;
    int frames_ = 0;
    Render3DSettings settings_;
    Renderer3D::Context renderer_;
    WaterSurface water_;
};

int Run(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "SampleRayTracing [--smoke-test]\n"
                     "R: ray shadows/reflection/refraction. G: indirect light.\n"
                     "F: froxel fog. T: ray-traced fog shadows. Escape: quit.\n";
        return 0;
    }
    const bool smoke = argc == 2 && std::string_view(argv[1]) == "--smoke-test";
    if (argc != 1 && !smoke) {
        std::cerr << "Usage: SampleRayTracing [--smoke-test]\n";
        return 2;
    }
    RayTracingDemo app(smoke);
    return app.Initialize() ? app.Run() : 1;
}

} // namespace

LUX_ENGINE_MAIN(Run)
