#include <Common/SampleApplication.hpp>
#include <LuxEngine/Application.hpp>
#include <LuxEngine/EntryPoint.hpp>
#include <LuxEngine/Renderer/Water/OceanSpectrum.hpp>
#include <LuxEngine/Renderer/Water/WaterSurface.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxEngine/Scene/Scene.hpp>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace LuxEngine;

constexpr double Patch = 64;
constexpr double HalfPatch = Patch / 2;
constexpr double BuoyRadius = .21;
constexpr double Tau = 2 * std::numbers::pi;

enum class CameraView { Default, Shore, Waterline };

struct Options {
    bool smoke = false;
    bool benchmark = false;
    std::filesystem::path capture;
    unsigned tick = 120;
    CameraView view = CameraView::Default;
};

double Shore(double z) {
    return -12 + 4 * std::sin(z * .071) + 2.5 * std::sin(z * .148);
}

double FarShore(double x) {
    return -89 + 5 * std::sin(x * .048) - 2 * std::cos(x * .13);
}

double Relief(double x, double z) {
    return .54 * std::sin(x * .29 + z * .18) + .3 * std::cos(z * .49 - x * .23) +
           .16 * std::sin(x * 1.08 + z * .88);
}

double LakeBed(double x, double z) {
    const double inland = std::max(Shore(z) - x, FarShore(x) - z);
    const double slope =
        inland < 0 ? std::max(-7.5, inland * .25) : inland * (.10 + .025 * std::sin(z * .057));
    const double hills = std::clamp((inland - 4) * .12, 0., 6.);
    const double ridge =
        .6 + .52 * std::sin(x * .069 + z * .041) + .19 * std::cos(x * .16 - z * .08);
    return slope + Relief(x, z) * .21 + hills * ridge;
}

void MakeTerrain(GpuDevice& device, Mesh3D& mesh) {
    constexpr uint32 Width = 193, Depth = 169;
    std::vector<Vertex3D> vertices;
    std::vector<uint32> indices;
    vertices.reserve(Width * Depth);
    indices.reserve((Width - 1) * (Depth - 1) * 6);
    for (uint32 row = 0; row < Depth; ++row)
        for (uint32 column = 0; column < Width; ++column) {
            const double x = -128 + 4. * column / 3, z = -168 + 4. * row / 3;
            const double y = LakeBed(x, z);
            const auto normal = Vec3{LakeBed(x - .2, z) - LakeBed(x + .2, z), .4,
                                     LakeBed(x, z - .2) - LakeBed(x, z + .2)}
                                    .Normalized();
            const double noise = .5 + .5 * Relief(x * 1.7, z * 1.7);
            const double grass = std::clamp((y - .15) * 1.1 + noise * .27, 0., 1.);
            const double rock = std::clamp((.9 - normal.y) * 5, 0., 1.);
            const Vec3 sand{.89, .91, .85}, moss{.30, .49, .22}, stone{.66, .67, .63};
            const auto ground = (sand * (1 - grass) + moss * grass) * (1 - rock) + stone * rock;
            const auto color = ground * (.72 + .48 * noise);
            vertices.push_back({{x, y, z},
                                normal,
                                {x / 1.8, z / 1.8},
                                {float(color.x), float(color.y), float(color.z), 1}});
            if (column + 1 < Width && row + 1 < Depth) {
                const auto first = row * Width + column;
                const auto next = first + Width;
                indices.insert(indices.end(), {first, next, first + 1, first + 1, next, next + 1});
            }
        }
    if (!mesh.Upload(&device, vertices, indices))
        throw std::runtime_error("Could not upload the lakeshore");
}

struct Ring {
    double height, radius;
    Vec4 color;
};

void MakeLathe(GpuDevice& device, Mesh3D& mesh, std::span<const Ring> profile, uint32 segments,
               double irregularity) {
    const uint32 stride = segments + 1;
    std::vector<Vertex3D> vertices;
    std::vector<uint32> indices;
    for (uint32 row = 0; row < profile.size(); ++row) {
        const auto& ring = profile[row];
        const auto& lower = profile[row ? row - 1 : row];
        const auto& upper = profile[std::min<size_t>(row + 1, profile.size() - 1)];
        const double slope = (lower.radius - upper.radius) / (upper.height - lower.height);
        for (uint32 i = 0; i < stride; ++i) {
            const double u = double(i) / segments, angle = Tau * u;
            const double x = std::cos(angle), z = std::sin(angle);
            const double shape = 1 + irregularity * (.55 * std::sin(3 * angle + .65 * row) +
                                                     .45 * std::cos(5 * angle - .4 * row));
            const auto normal = Vec3{x, slope, z}.Normalized();
            vertices.push_back({{x * ring.radius * shape, ring.height, z * ring.radius * shape},
                                normal,
                                {u, ring.height},
                                ring.color});
            if (i < segments && row + 1 < profile.size()) {
                const auto a = row * stride + i, b = a + stride;
                indices.insert(indices.end(), {a, b, a + 1, a + 1, b, b + 1});
            }
        }
    }
    if (!mesh.Upload(&device, vertices, indices))
        throw std::runtime_error("Could not upload the lakeshore props");
}

void MakeConifer(GpuDevice& device, Mesh3D& mesh) {
    std::vector<Vertex3D> vertices;
    std::vector<uint32> indices;
    constexpr uint32 Rings = 7, Branches = 7;
    for (uint32 level = 0; level < Rings; ++level) {
        const double h = .29 + level * .16;
        for (uint32 branch = 0; branch < Branches; ++branch) {
            const double seed = 41.9 * branch + level * 19.47;
            const double angle = Tau * branch / Branches + .736 * level + .34 * std::sin(seed);
            const Vec3 direction{std::cos(angle), 0, std::sin(angle)};
            const Vec3 across{-direction.z, 0, direction.x};
            const double reach = (.57 - .068 * level) * (.8 + .28 * std::cos(seed));
            const double width = reach * .25;
            const Vec3 start{.022 * h, h, 0};
            const auto a = static_cast<uint32>(vertices.size());
            const float green = .062f + static_cast<float>(.017 * std::sin(seed));
            const Vec4 dark{green * .52f, green * .73f, green * .31f, 1};
            const Vec4 lit{green * .77f, green, green * .44f, 1};
            vertices.push_back({start - direction * .03, {0, 1, 0}, {}, dark});
            vertices.push_back(
                {start + direction * (reach * .46) + across * width, Vec3{0, 1, 0}, {}, dark});
            vertices.push_back({start + direction * (reach * .41) + Vec3{0, .13, 0},
                                (direction * .4 + Vec3{0, 1, 0}).Normalized(),
                                {},
                                lit});
            vertices.push_back({start + direction * reach - Vec3{0, .05, 0}, direction, {}, lit});
            vertices.push_back(
                {start + direction * (reach * .56) - across * width, Vec3{0, 1, 0}, {}, dark});
            indices.insert(indices.end(),
                           {a, a + 2, a + 1, a + 1, a + 2, a + 3, a + 3, a + 2, a + 4, a + 4, a + 2,
                            a, a, a + 1, a + 3, a, a + 3, a + 4});
        }
    }
    if (!mesh.Upload(&device, vertices, indices))
        throw std::runtime_error("Could not upload the conifer canopy");
}

void MakeCapillaryNormal(GpuDevice& device, GpuTexture& texture) {
    constexpr uint32 Size = 128;
    constexpr std::array waves{Vec3{6, 17, .018}, Vec3{13, -11, .023}, Vec3{21, 8, .014},
                               Vec3{-24, 16, .009}};
    std::vector<uint8> pixels(Size * Size * 4);
    const auto encode = [](double v) { return static_cast<uint8>(std::lround(127.5 * (v + 1))); };
    for (uint32 y = 0; y < Size; ++y)
        for (uint32 x = 0; x < Size; ++x) {
            Vec3 n{0, 0, 1};
            for (const auto& wave : waves) {
                const double phase = Tau * (wave.x * x + wave.y * y) / Size + wave.z * 47;
                n.x += wave.z * wave.x * std::cos(phase);
                n.y += wave.z * wave.y * std::cos(phase);
            }
            n = n.Normalized();
            const size_t index = (y * Size + x) * 4;
            pixels[index] = encode(n.x);
            pixels[index + 1] = encode(n.y);
            pixels[index + 2] = encode(n.z);
            pixels[index + 3] = 255;
        }
    TextureSampling sampling{true, TextureAddressMode::Repeat, true, true};
    if (!texture.Initialize(&device, Size, Size, pixels.data(), TextureFilter::Linear,
                            TextureColorSpace::Linear, sampling))
        throw std::runtime_error("Could not upload capillary wave normals");
}

std::string AssetPath(const char* group, const char* filename) {
    const char* executable = SDL_GetBasePath();
    if (!executable)
        throw std::runtime_error("Could not locate the lakeside reflection asset");
    const auto directory =
        std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(executable)));
    const auto path = (directory / "assets" / group / filename).u8string();
    return {reinterpret_cast<const char*>(path.data()), path.size()};
}

class LakesideDemo final : public Application {
public:
    explicit LakesideDemo(Options options)
        : Application({"LuxEngine · Lakeside at Noon", options.smoke ? 640u : 1440u,
                       options.smoke ? 360u : 810u}),
          options_(std::move(options)) {}

    int Run() {
        const int result = Application::Run();
        return result == 0 && (options_.benchmark || !options_.capture.empty()) && !runFinished_
                   ? 1 : result;
    }

private:
    void OnInit() override {
        auto& gpu = GetGpuDevice();
        if (options_.benchmark && (!gpu.GetInfo().gpuTimestamps || !gpu.SetVSync(false)))
            throw std::runtime_error("Lakeside benchmark needs GPU timestamps and disabled VSync");
        renderer_.Init(&gpu);
        MakeTerrain(gpu, terrain_);
        MakeCapillaryNormal(gpu, capillary_);
        const std::array stones{
            Ring{-.32, .18, {.12f, .11f, .09f, 1}}, Ring{-.09, .52, {.19f, .17f, .15f, 1}},
            Ring{.14, .48, {.25f, .22f, .19f, 1}}, Ring{.33, .25, {.29f, .26f, .22f, 1}},
            Ring{.37, .03, {.23f, .20f, .17f, 1}}};
        const Vec4 bark{.13f, .084f, .047f, 1};
        const std::array trunk{Ring{0, .061, bark}, Ring{.5, .039, bark}, Ring{1.4, .009, bark}};
        const std::array marker{
            Ring{-.20, .035, {.026f, .033f, .037f, 1}}, Ring{-.15, .18, {.034f, .042f, .046f, 1}},
            Ring{-.03, .22, {.058f, .062f, .052f, 1}},  Ring{.01, .225, {.72f, .255f, .036f, 1}},
            Ring{.13, .196, {.84f, .35f, .064f, 1}},    Ring{.22, .12, {.83f, .51f, .19f, 1}},
            Ring{.265, .04, {.12f, .11f, .075f, 1}},    Ring{.34, .035, {.44f, .34f, .18f, 1}}};
        MakeLathe(gpu, rock_, stones, 13, .24);
        MakeLathe(gpu, trunk_, trunk, 9, .13);
        MakeConifer(gpu, tree_);
        MakeLathe(gpu, buoy_, marker, 28, .01);

        OceanSpectrumSettings spectrum;
        spectrum.resolution = 64;
        spectrum.patchLength = Patch;
        spectrum.windDirection = {-.2, -1};
        spectrum.windSpeed = 6.8;
        spectrum.spectrumAmplitude = .0009;
        spectrum.choppiness = .8;
        spectrum.foamThreshold = .96;
        spectrum.seed = 48;
        if (!ocean_.Initialize(spectrum) || !water_.UploadOcean(&gpu, ocean_))
            throw std::runtime_error("Could not create the GPU FFT lake");

        for (const auto p : {Vec3{-1.9, .2, 4.6}, Vec3{3.3, .2, -5.4}, Vec3{10.5, .2, -18.8}}) {
            auto buoy = scene_.CreateEntity("Lake buoy");
            buoy.GetComponent<TransformComponent>().position = p;
            buoy.AddComponent<Rigidbody3DComponent>().mass = 12;
            buoy.AddComponent<BoxCollider3DComponent>().size = {BuoyRadius * 2, .3, BuoyRadius * 2};
        }
        settings_.showSkybox = true;
        settings_.sun = {{-.451, -.618, .644}, {1, .98, .94}, 3.2f};
        settings_.environmentPath = AssetPath("LakesideOcean", "lakeside_physical_noon.hdr");
        settings_.environmentIntensity = 1;
        settings_.environmentColor = {.23, .29, .38};
        settings_.shadowRange = 110;
        settings_.shadowNormalBias = .05f;
        settings_.virtualShadows = true;
        settings_.waterReflection = {AssetPath("RadkowLake", "radkow_lake_water.hdr"), -91, 4, 256};
        shore_.alphaMode = MaterialAlphaMode::Opaque;
        shore_.roughness = 1;
        shore_.normalScale = .65f;
        shore_.baseColorTexture = AssetPath("RockyGravel", "albedo.jpg");
        shore_.normalTexture = AssetPath("RockyGravel", "normal.png");
        shore_.roughnessTexture = AssetPath("RockyGravel", "roughness.png");
        settings_.fog = {{.76, .85, .96}, .0025f, 155, .095f, .1, .35f, true};
        settings_.rayTracing.shadows = true;
        settings_.rayTracing.maxDistance = 145;
        settings_.rayTracing.maxBounces = 2;
        if (!options_.capture.empty())
            for (unsigned tick = 0; tick < options_.tick; ++tick)
                StepLake(GetFixedUpdateStep());
        start_ = std::chrono::steady_clock::now();
    }

    void OnEvent(Event& event) override {
        Application::OnEvent(event);
        EventDispatcher dispatcher(event);
        dispatcher.Dispatch<KeyPressedEvent>([&](KeyPressedEvent& key) {
            if (key.IsRepeat())
                return false;
            if ((options_.smoke || options_.benchmark || !options_.capture.empty()) &&
                key.GetKeyCode() != static_cast<int>(LuxKey::Escape))
                return false;
            switch (static_cast<LuxKey>(key.GetKeyCode())) {
            case LuxKey::Escape:
                Close();
                break;
            case LuxKey::Space:
                paused_ = !paused_;
                break;
            case LuxKey::F:
                settings_.fog.density = settings_.fog.density > 0 ? 0.f : .0025f;
                break;
            case LuxKey::R: {
                auto& ray = settings_.rayTracing;
                ray.refractions = ray.shadows = ray.reflections = !ray.reflections;
                settings_.fog.rayTracedShadows = ray.shadows;
                break;
            }
            case LuxKey::C:
                settings_.radianceCascades = {settings_.radianceCascades.strength > 0 ? 0.f : .6f,
                                              5};
                break;
            default:
                return false;
            }
            return true;
        });
    }

    void OnUpdate(double dt) override {
        if (!paused_ && !options_.benchmark && options_.capture.empty())
            StepLake(dt);
    }

    void StepLake(double dt) {
        time_ += dt;
        if (!ocean_.Update(static_cast<float>(time_)))
            throw std::runtime_error("Could not sample the animated lake");
        const OceanBuoyancySettings buoyancy{.radius = BuoyRadius, .drag = 5.5};
        for (auto entity : scene_.GetEntities()) {
            const auto& position = entity.GetComponent<TransformComponent>().position;
            const auto& body = entity.GetComponent<Rigidbody3DComponent>();
            const auto sample =
                ocean_.SampleAtSurface({position.x + HalfPatch, position.z + HalfPatch});
            if (sample &&
                !scene_.AddImpulse3D(entity.GetId(),
                                     OceanBuoyancyImpulse(*sample, position.y, body.velocity,
                                                          body.mass, dt, buoyancy)))
                throw std::runtime_error("Could not apply buoyancy");
        }
        scene_.OnUpdate(dt);
    }

    void OnRender(float) override {
        auto& gpu = GetGpuDevice();
        if (options_.benchmark && benchmarkFrames_)
            StepLake(GetFixedUpdateStep());
        if (!water_.UpdateOcean(static_cast<float>(time_)))
            throw std::runtime_error("GPU FFT dispatch failed: " + gpu.GetLastError());
        uint32 width = 0, height = 0;
        GetWindow().GetFramebufferSize(width, height);
        PerspectiveCamera camera(ToRadians(58), double(std::max(1u, width)) / std::max(1u, height),
                                 .08, 240);
        const double drift = .35 * std::sin(.11 * time_);
        camera.SetPosition({3.9 + drift, 1.53 + .09 * std::sin(.23 * time_), 12.7});
        camera.SetTarget({-4, 1.25, -47});
        if (options_.view == CameraView::Shore) {
            camera.SetPosition({-7, 2.2, 7});
            camera.SetTarget({-15, -.15, -12});
        } else if (options_.view == CameraView::Waterline) {
            camera.SetPosition({4, .48, 12.7});
            camera.SetTarget({-4, .60, -47});
        }
        if (!gpu.Clear(.22f, .31f, .40f, 1))
            throw std::runtime_error("Could not clear the lakeside frame: " + gpu.GetLastError());
        renderer_.BeginScene(camera, settings_, .9f);
        Material3D earth;
        earth.alphaMode = MaterialAlphaMode::Opaque;
        earth.roughness = .83f;
        renderer_.DrawMesh(terrain_, Mat4::Identity(), shore_);
        for (int i = 0; i < 35; ++i) {
            const double z = 2.5 - i * 2.8;
            const double x = Shore(z) + 1.3 * std::sin(2.4 * i);
            const double size = .55 + .8 * (.5 + .5 * std::sin(i * 2.283));
            renderer_.DrawMesh(rock_,
                               Mat4::Translate({x, LakeBed(x, z), z}) * Mat4::RotateY(i * 2.399) *
                                   Mat4::Scale({size * 1.35, size, size}),
                               earth);
        }
        for (int i = 0; i < 55; ++i) {
            const double scatter = .5 + .5 * std::sin(i * 2.399);
            const double z = i < 27 ? 5 - (i * 3.8) : FarShore(i * 3.5 - 115) - 5 - scatter * 24;
            const double x = i < 27 ? Shore(z) - 5 - scatter * 13 : i * 3.5 - 115;
            const double h = 2.15 + 2.9 * scatter;
            const auto transform = Mat4::Translate({x, LakeBed(x, z), z}) *
                                   Mat4::RotateY(i * 1.89) * Mat4::Scale({.75 * h, h, .75 * h});
            renderer_.DrawMesh(trunk_, transform, earth);
            renderer_.DrawMesh(tree_, transform, earth);
        }
        Material3D marker;
        marker.alphaMode = MaterialAlphaMode::Opaque;
        marker.roughness = .33f;
        for (auto entity : scene_.GetEntities()) {
            const auto& p = entity.GetComponent<TransformComponent>().position;
            const auto sample = ocean_.SampleAtSurface({p.x + HalfPatch, p.z + HalfPatch});
            const Vec3 n = sample ? sample->normal : Vec3{0, 1, 0};
            renderer_.DrawMesh(buoy_,
                               Mat4::Translate(p) * Mat4::RotateX(std::atan2(n.z, n.y)) *
                                   Mat4::RotateZ(-std::atan2(n.x, n.y)),
                               marker);
        }
        auto water = WaterSurface::MakeMaterial();
        water.water->absorption = {.48, .18, .105};
        water.roughness = .065f;
        water.normalScale = .26f;
        for (int row = -2; row <= 0; ++row)
            for (int column = -1; column <= 1; ++column)
                renderer_.DrawMeshWithTextures(water_,
                                               Mat4::Translate({column * Patch, 0, row * Patch}),
                                               water, {.normal = &capillary_});
        renderer_.EndScene();
        if (!options_.capture.empty() && renderer_.IsEnvironmentReady() &&
            gpu.GetFrameStats().waterReflectionPasses == 1) {
            if (!gpu.RequestFrameCapture())
                throw std::runtime_error("Could not request a lakeside capture");
            captureRequested_ = true;
        }
    }

    void OnFramePresented() override {
        if (!options_.smoke && !options_.benchmark && options_.capture.empty())
            return;
        auto& gpu = GetGpuDevice();
        const auto stats = gpu.GetFrameStats();
        const bool environmentReady =
            stats.waterReflectionPasses == 1 && renderer_.IsEnvironmentReady();
        if (options_.benchmark && environmentReady) {
            RecordBenchmark(stats);
            return;
        }
        if (stats.computeDispatches == 3 + stats.volumetricFogPasses)
            ++waveUpdates_;
        ++frames_;
        const bool smokeReady = options_.smoke && frames_ >= 5 && waveUpdates_ >= 2 &&
                                environmentReady;
        if (captureRequested_ || smokeReady) {
            if (stats.volumetricFogPasses != 1 || gpu.GetCounters().validationErrors)
                throw std::runtime_error("Lakeside frame did not render the expected passes");
            if (!renderer_.GetLastAssetError().empty())
                throw std::runtime_error("Lakeside asset error: " + renderer_.GetLastAssetError());
            if (captureRequested_) {
                LuxSamples::WritePpmCapture(gpu, options_.capture);
                runFinished_ = true;
                std::cout << "Lakeside tick " << options_.tick
                          << " | virtual shadow pages " << stats.virtualShadowPages
                          << " | ray-traced passes " << stats.rayTracedPasses << '\n';
            }
            Close();
        } else if (std::chrono::steady_clock::now() - start_ > std::chrono::seconds(20)) {
            throw std::runtime_error("Lakeside validation timed out after " +
                                     std::to_string(waveUpdates_) + " FFT updates: " +
                                     renderer_.GetLastAssetError());
        }
    }

    void RecordBenchmark(const GpuFrameStats& stats) {
        constexpr unsigned WarmupFrames = 60, Samples = 240;
        auto& gpu = GetGpuDevice();
        if (gpu.GetCounters().validationErrors || !renderer_.GetLastAssetError().empty() ||
            stats.volumetricFogPasses != 1)
            throw std::runtime_error("Lakeside benchmark did not render the expected frame");
        if (++benchmarkFrames_ < WarmupFrames)
            return;
        if (stats.gpuTimingFrame > lastTimingFrame_) {
            lastTimingFrame_ = stats.gpuTimingFrame;
            if (stats.gpuTimeMs > 0 && stats.gpuShadowTimeMs > 0) {
                frameTimes_.push_back(stats.gpuTimeMs);
                shadowTimes_.push_back(stats.gpuShadowTimeMs);
            }
        }
        if (frameTimes_.size() == Samples) {
            std::sort(frameTimes_.begin(), frameTimes_.end());
            std::sort(shadowTimes_.begin(), shadowTimes_.end());
            uint32 width = 0, height = 0;
            GetWindow().GetFramebufferSize(width, height);
            std::cout << "Lakeside " << gpu.GetInfo().deviceName << " | " << width << 'x'
                      << height << " | " << Samples << " GPU frames | frame ms p50 "
                      << frameTimes_[Samples / 2 - 1] << " p95 "
                      << frameTimes_[Samples * 95 / 100 - 1] << " | shadow ms p50 "
                      << shadowTimes_[Samples / 2 - 1] << " p95 "
                      << shadowTimes_[Samples * 95 / 100 - 1] << " | virtual shadow pages "
                      << stats.virtualShadowPages << '/' << stats.requestedShadowPages
                      << " | draws " << stats.drawCalls << '\n';
            runFinished_ = true;
            Close();
        } else if (std::chrono::steady_clock::now() - start_ > std::chrono::seconds(60)) {
            throw std::runtime_error("Lakeside GPU timestamp benchmark timed out");
        }
    }

    void OnShutdown() override {
        renderer_.Shutdown();
        capillary_.Release();
        water_.Release();
        buoy_.Release();
        trunk_.Release();
        tree_.Release();
        rock_.Release();
        terrain_.Release();
    }

    Options options_;
    bool paused_ = false, captureRequested_ = false, runFinished_ = false;
    unsigned frames_ = 0, waveUpdates_ = 0, benchmarkFrames_ = 0;
    uint64 lastTimingFrame_ = 0;
    std::vector<double> frameTimes_, shadowTimes_;
    double time_ = 0;
    std::chrono::steady_clock::time_point start_;
    Render3DSettings settings_;
    Material3D shore_;
    Renderer3D::Context renderer_;
    Scene scene_;
    OceanSpectrum ocean_;
    Mesh3D water_, terrain_, rock_, tree_, trunk_, buoy_;
    GpuTexture capillary_;
};

int Run(int argc, char** argv) {
    constexpr auto usage = "SampleLakesideOcean [--smoke-test | --benchmark | "
                           "--capture PATH.ppm [--tick 0..600] [--view default|shore|waterline]]\n";
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << usage <<
                     "Space: pause waves. F: fog. R: hardware rays. C: radiance cascades.\n"
                     "Escape: quit.\n";
        return 0;
    }
    const auto invalidOptions = [usage] {
        std::cerr << usage;
        return 2;
    };
    Options options;
    if (argc == 2 && std::string_view(argv[1]) == "--smoke-test") {
        options.smoke = true;
    } else if (argc == 2 && std::string_view(argv[1]) == "--benchmark") {
        options.benchmark = true;
    } else if (argc > 1) {
        if (argc < 3 || std::string_view(argv[1]) != "--capture")
            return invalidOptions();
        options.capture = std::filesystem::path(
            std::u8string_view(reinterpret_cast<const char8_t*>(argv[2])));
        if (options.capture.extension() != ".ppm")
            return invalidOptions();
        for (int index = 3; index < argc; index += 2) {
            if (index + 1 == argc)
                return invalidOptions();
            const std::string_view name(argv[index]);
            const std::string_view value(argv[index + 1]);
            if (name == "--tick") {
                const auto [end, error] =
                    std::from_chars(value.data(), value.data() + value.size(), options.tick);
                if (error != std::errc{} || end != value.data() + value.size() || options.tick > 600)
                    return invalidOptions();
            } else if (name == "--view" && value == "default") {
                options.view = CameraView::Default;
            } else if (name == "--view" && value == "shore") {
                options.view = CameraView::Shore;
            } else if (name == "--view" && value == "waterline") {
                options.view = CameraView::Waterline;
            } else {
                return invalidOptions();
            }
        }
    }
    LakesideDemo app(std::move(options));
    return app.Initialize() ? app.Run() : 1;
}
} // namespace

LUX_ENGINE_MAIN(Run)
