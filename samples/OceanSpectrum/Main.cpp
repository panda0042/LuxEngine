#include <LuxEngine/Application.hpp>
#include <LuxEngine/EntryPoint.hpp>
#include <LuxEngine/Renderer/Water/OceanSpectrum.hpp>
#include <LuxEngine/Renderer/Water/WaterSurface.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxEngine/Scene/Scene.hpp>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

using namespace LuxEngine;
constexpr uint32 Resolution = 64;
constexpr double PatchLength = 32;
constexpr double HalfPatch = PatchLength * .5;
constexpr double BuoyRadius = .38;

class OceanDemo final : public Application {
public:
    explicit OceanDemo(bool smoke)
        : Application({"LuxEngine · Spectral Ocean", 1280, 720}), smoke_(smoke) {}

private:
    void OnInit() override {
        OceanSpectrumSettings spectrum;
        spectrum.resolution = Resolution;
        spectrum.patchLength = PatchLength;
        spectrum.spectrumAmplitude = .018;
        spectrum.foamThreshold = .96;
        if (!ocean_.Initialize(spectrum))
            throw std::runtime_error("could not initialize ocean spectrum");

        if (!mesh_.UploadOcean(&GetGpuDevice(), ocean_))
            throw std::runtime_error("could not create GPU ocean: " +
                                     GetGpuDevice().GetLastError());

        for (auto point : {Vec2{-5, -2}, Vec2{2, 0}, Vec2{7, 4}, Vec2{0, 7}}) {
            auto entity = scene_.CreateEntity("Buoy");
            entity.GetComponent<TransformComponent>().position = {point.x, .4, point.y};
            auto& body = entity.AddComponent<Rigidbody3DComponent>();
            body.mass = 65;
            entity.AddComponent<BoxCollider3DComponent>().size = Vec3{1, 1, 1} * (2 * BuoyRadius);
        }
        Renderer3D::Init(&GetGpuDevice());
    }

    void OnUpdate(double dt) override {
        if (Input::IsKeyPressed(LuxKey::Escape)) {
            Close();
            return;
        }
        time_ += dt;
        if (!ocean_.Update(static_cast<float>(time_)))
            throw std::runtime_error("ocean update failed");
        const OceanBuoyancySettings buoyancy{.radius = BuoyRadius, .drag = 5};
        for (auto entity : scene_.GetEntities()) {
            const auto& t = entity.GetComponent<TransformComponent>();
            const auto& body = entity.GetComponent<Rigidbody3DComponent>();
            const auto sample =
                ocean_.SampleAtSurface({t.position.x + HalfPatch, t.position.z + HalfPatch});
            if (sample &&
                !scene_.AddImpulse3D(entity.GetId(),
                                     OceanBuoyancyImpulse(*sample, t.position.y, body.velocity,
                                                          body.mass, dt, buoyancy)))
                throw std::runtime_error("ocean buoyancy impulse was rejected");
        }
        scene_.OnUpdate(dt);
    }

    void OnRender(float) override {
        if (!mesh_.UpdateOcean(static_cast<float>(time_)))
            throw std::runtime_error("could not dispatch GPU ocean: " +
                                     GetGpuDevice().GetLastError());
        uint32 width = 0, height = 0;
        GetWindow().GetFramebufferSize(width, height);
        const double aspect = height ? static_cast<double>(width) / height : 16. / 9;
        PerspectiveCamera camera(ToRadians(55), aspect, .1, 140);
        camera.SetPosition({16, 8, 16});
        camera.SetTarget({.5, 0, 1});
        Render3DSettings render;
        render.showSkybox = true;
        render.sun.direction = {-.6, -1, -.75};
        render.sun.intensity = 2;
        render.shadowRange = 24;
        GetGpuDevice().Clear(.09f, .16f, .25f, 1);
        Renderer3D::BeginScene(camera, render, 1);
        Material3D bottom;
        bottom.albedoColor = {.11f, .16f, .16f, 1};
        Renderer3D::DrawCube({0, -6, 0}, {96, 1, 96}, bottom);
        Material3D buoy;
        buoy.albedoColor = {.88f, .25f, .035f, 1};
        buoy.roughness = .43f;
        for (auto entity : scene_.GetEntities()) {
            const auto& p = entity.GetComponent<TransformComponent>().position;
            Renderer3D::DrawCube(p, {.6, .85, .6}, buoy);
        }

        const auto material = WaterSurface::MakeMaterial();
        for (int z = -1; z <= 1; ++z)
            for (int x = -1; x <= 1; ++x)
                Renderer3D::DrawMesh(mesh_, Mat4::Translate({x * PatchLength, 0, z * PatchLength}),
                                     material);
        Renderer3D::EndScene();
    }

    void OnFramePresented() override {
        if (smoke_) {
            const auto dispatches = GetGpuDevice().GetFrameStats().computeDispatches;
            if (dispatches == 3)
                ++simulatedFrames_;
            if (++frames_ == 12) {
                if (simulatedFrames_ < 2)
                    throw std::runtime_error("GPU ocean did not advance");
                Close();
            }
        }
    }

    void OnShutdown() override {
        mesh_.Release();
    }

    bool smoke_;
    int frames_ = 0;
    int simulatedFrames_ = 0;
    double time_ = 0;
    OceanSpectrum ocean_;
    Scene scene_;
    Mesh3D mesh_;
};

int Run(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "SampleOceanSpectrum [--smoke-test]\n"
                     "GPU inverse FFT ocean with Jacobian foam and Jolt buoys. Escape quits.\n";
        return 0;
    }
    const bool smoke = argc == 2 && std::string_view(argv[1]) == "--smoke-test";
    if (argc != 1 && !smoke) {
        std::cerr << "Usage: SampleOceanSpectrum [--smoke-test]\n";
        return 2;
    }
    OceanDemo app(smoke);
    if (!app.Initialize())
        return 1;
    return app.Run();
}

} // namespace

LUX_ENGINE_MAIN(Run)
