#include "Benchmark.hpp"
#include "Characters.hpp"
#include "Game.hpp"
#include "PropModels.hpp"
#include <Common/Artwork.hpp>
#include <Common/SampleApplication.hpp>
#include <LuxEngine/Application.hpp>
#include <LuxEngine/EntryPoint.hpp>
#include <LuxEngine/Renderer/Water/WaterSurface.hpp>
#include <SDL3/SDL.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <iostream>
#include <map>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace Frontier {
namespace {
using namespace LuxEngine;
constexpr double Tau = 2 * std::numbers::pi;

struct Quality {
    std::string_view name;
    unsigned trees, shadow, fog;
    int radius;
    bool virtualShadows;
};

constexpr std::array Qualities{
    Quality{"low", 96, 512, 55, 1, false},         Quality{"medium", 160, 1024, 80, 1, false},
    Quality{"high", 256, 1024, 120, 2, false},     Quality{"ultra", 352, 2048, 150, 2, true},
    Quality{"cinematic", 512, 2048, 190, 2, true}, Quality{"extreme", 1024, 4096, 230, 3, true},
};
constexpr std::array<std::string_view, 5> Scenarios{"town", "drive", "forest", "water", "weather"};

struct Options {
    Quality quality = Qualities[2];
    std::string_view scenario = "water";
    std::filesystem::path output = "frontier-benchmark", capture;
    unsigned seed = 4805, width = 1280, height = 720, warmup = 60, frames = 180, daySeconds = 3600;
    bool smoke = false, benchmark = false, rayTracing = false, help = false, edgeAA = true;
    bool temporal = true;
};

unsigned Integer(std::string_view text, unsigned low, unsigned high) {
    unsigned value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value < low ||
        value > high)
        throw std::runtime_error("Invalid numeric option: " + std::string(text));
    return value;
}

Options Parse(int argc, char** argv) {
    Options options;
    unsigned vegetation = 0;
    bool resolution = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto next = [&]() -> std::string_view {
            if (++i == argc)
                throw std::runtime_error("Missing value for " + std::string(arg));
            return argv[i];
        };
        if (arg == "--help")
            options.help = true;
        else if (arg == "--smoke-test")
            options.smoke = true;
        else if (arg == "--benchmark")
            options.benchmark = true;
        else if (arg == "--rt")
            options.rayTracing = true;
        else if (arg == "--no-taa")
            options.temporal = false;
        else if (arg == "--no-edge-aa")
            options.edgeAA = false;
        else if (arg == "--output")
            options.output = next();
        else if (arg == "--capture")
            options.capture = next();
        else if (arg == "--seed")
            options.seed = Integer(next(), 0, 0xffffffff);
        else if (arg == "--day-seconds")
            options.daySeconds = Integer(next(), 60, 864000);
        else if (arg == "--vegetation")
            vegetation = Integer(next(), 1, 4096);
        else if (arg == "--warmup")
            options.warmup = Integer(next(), 1, 1000000);
        else if (arg == "--frames")
            options.frames = Integer(next(), 1, 1000000);
        else if (arg == "--scenario") {
            options.scenario = next();
            if (std::ranges::find(Scenarios, options.scenario) == Scenarios.end())
                throw std::runtime_error("Unknown scenario");
        } else if (arg == "--preset") {
            const auto value = next();
            const auto preset = std::ranges::find(Qualities, value, &Quality::name);
            if (preset == Qualities.end())
                throw std::runtime_error("Unknown graphics preset");
            options.quality = *preset;
        } else if (arg == "--resolution") {
            const auto value = next();
            const auto split = value.find('x');
            if (split == std::string_view::npos)
                throw std::runtime_error("Expected WIDTHxHEIGHT");
            options.width = Integer(value.substr(0, split), 640, 7680);
            options.height = Integer(value.substr(split + 1), 360, 4320);
            resolution = true;
        } else {
            throw std::runtime_error("Unknown option: " + std::string(arg));
        }
    }
    if (options.smoke && (options.benchmark || !options.capture.empty()))
        throw std::runtime_error("Choose one automated mode");
    if (options.smoke)
        options.scenario = "town";
    if (!resolution && options.smoke) {
        options.width = 640;
        options.height = 360;
    } else if (!resolution && options.benchmark) {
        options.width = 1920;
        options.height = 1080;
    }
    if (vegetation)
        options.quality.trees = vegetation;
    return options;
}

std::string Utf8Path(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

class FrontierApp final : public Application {
public:
    explicit FrontierApp(Options options)
        : Application({"Frontier County", options.width, options.height,
                       !options.benchmark && !options.smoke && options.capture.empty()}),
          options_(std::move(options)), game_(options_.seed, options_.daySeconds),
          recorder_(options_.warmup, options_.benchmark ? options_.frames : 0) {}

    int Run() {
        const int result = Application::Run();
        return result == 0 && (options_.smoke || options_.benchmark || !options_.capture.empty()) &&
                       !done_
                   ? 1
                   : result;
    }

private:
    void OnInit() override;
    void OnUpdate(double dt) override;
    void OnRender(float) override;
    void OnFramePresented() override;
    void OnShutdown() override;
    void Step(const Controls& controls, double dt);
    void SmokeStep();
    void UpdateLight();
    void DrawActors();
    void DrawVegetation(Vec3 eye);
    void DrawHud();
    Vec2 ScenarioStart() const;

    Options options_;
    Game game_;
    World world_;
    Benchmark recorder_;
    Renderer3D::Context renderer_;
    WaterSurface lake_;
    PropModels props_;
    std::map<ChunkKey, std::vector<PropModels::Part>> groundCover_;
    std::vector<Plant> nearTrees_;
    Characters characters_;
    Material3D ground_, scenery_, foliage_, road_;
    Render3DSettings settings_;
    std::unique_ptr<LuxSamples::Artwork> artwork_;
    std::filesystem::path assets_;
    std::chrono::steady_clock::time_point automationStart_, lastRender_;
    Vec3 driveStart_;
    double time_ = 0, updateMs_ = 0;
    unsigned smokeStage_ = 0, frameWidth_ = 0, frameHeight_ = 0;
    bool sampledTemporalHistory_ = false;
    bool rt_ = false, controlsVisible_ = false, showStats_ = false, done_ = false,
         capturePending_ = false;
};

Vec2 FrontierApp::ScenarioStart() const {
    if (options_.scenario == "forest")
        return {-587, -128};
    if (options_.scenario == "water")
        return {225, 326};
    if (options_.scenario == "drive")
        return {237, 1.85};
    return Spawn;
}

void FrontierApp::OnInit() {
    auto& gpu = GetGpuDevice();
    const char* base = SDL_GetBasePath();
    if (!base)
        throw std::runtime_error("Could not locate sample assets");
    assets_ =
        std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(base))) / "assets";
    artwork_ = std::make_unique<LuxSamples::Artwork>(gpu, assets_);
    renderer_.Init(&gpu);
    if (options_.benchmark && !gpu.SetVSync(false))
        throw std::runtime_error("Cannot disable VSync for benchmark");
    const auto initial = ScenarioStart();
    world_.Start(options_.seed, options_.quality.trees, options_.quality.radius, initial, gpu);
    game_.Start(world_);
    game_.StartAt(initial, options_.scenario == "drive");
    if (options_.scenario == "water")
        game_.Face(LakeCenter);
    else if (options_.scenario == "forest")
        game_.Face(Regions[1].center);
    if (options_.scenario == "weather")
        game_.SetWeather(Weather::Thunder);
    const std::array waves{
        WaterWave{{1, -.4}, .094, 27.0, 2.9, .15, .46},
        WaterWave{{-.34, .92}, .046, 16.0, 2.05, 1.2, .4},
        WaterWave{{.74, .18}, .020, 10.0, 1.4, .9, .25},
    };
    if (!lake_.Configure({LakeSize, 64, 56, 0}, waves))
        throw std::runtime_error("Cannot initialize Silverlake waves");
    props_.Load(gpu, assets_);
    characters_.Load(assets_);
    if (options_.smoke && !characters_.VerifyClips())
        throw std::runtime_error("Character mesh did not deform during a motion clip");

    const auto texture = [&](const char* name) {
        const auto path = assets_ / "FrontierCounty" / name;
        if (!std::filesystem::is_regular_file(path))
            throw std::runtime_error("Required texture missing: " + Utf8Path(path));
        return Utf8Path(path);
    };
    ground_.alphaMode = MaterialAlphaMode::Opaque;
    ground_.baseColorTexture = texture("gravel_albedo.jpg");
    ground_.normalTexture = texture("gravel_normal.png");
    ground_.roughnessTexture = texture("gravel_roughness.png");
    scenery_.alphaMode = MaterialAlphaMode::Opaque;
    const auto meadow = assets_ / "FrontierMeadow";
    ground_.terrainLayer =
        TerrainLayer3D{Utf8Path(meadow / "albedo.jpg"), Utf8Path(meadow / "normal.png"),
                       Utf8Path(meadow / "roughness.png")};
    scenery_.roughness = .78f;
    const auto asphalt = assets_ / "FrontierAsphalt";
    road_.alphaMode = MaterialAlphaMode::Opaque;
    road_.textureSampling.anisotropic = true;
    road_.baseColorTexture = Utf8Path(asphalt / "albedo.jpg");
    road_.normalTexture = Utf8Path(asphalt / "normal.png");
    road_.roughnessTexture = Utf8Path(asphalt / "roughness.png");
    foliage_.baseColorTexture = Utf8Path(assets_ / "FrontierFoliage" / "spruce.png");
    foliage_.alphaMode = MaterialAlphaMode::Mask;
    foliage_.alphaCutoff = .42f;
    foliage_.roughness = .92f;
    foliage_.textureSampling = {true, TextureAddressMode::Clamp, true, false};
    settings_.showSkybox = true;
    settings_.temporalAntialiasing = options_.quality.radius > 1 && options_.temporal;
    settings_.screenSpaceAntialiasing = !settings_.temporalAntialiasing && options_.edgeAA;
    settings_.environmentPath = Utf8Path(assets_ / "LakesideOcean" / "lakeside_physical_noon.hdr");
    settings_.shadowResolution = options_.quality.shadow;
    settings_.shadowRange = 120;
    settings_.shadowNormalBias = .049f;
    settings_.virtualShadows = options_.quality.virtualShadows;
    settings_.waterReflection = {
        Utf8Path(assets_ / "LakesideSunrise" / "lakeside_sunrise_water.hdr"), -52, 1.9f, 128};
    settings_.fog.heightFalloff = .095f;
    settings_.fog.maxDistance = float(options_.quality.fog);
    rt_ = options_.rayTracing && gpu.GetInfo().rayTracing;
    const std::array bindings{
        std::pair{"forward", LuxKey::W},     std::pair{"reverse", LuxKey::S},
        std::pair{"left", LuxKey::A},        std::pair{"right", LuxKey::D},
        std::pair{"run", LuxKey::LeftShift}, std::pair{"jump", LuxKey::Space},
        std::pair{"interact", LuxKey::E},    std::pair{"crouch", LuxKey::C},
        std::pair{"camera", LuxKey::V},      std::pair{"weather", LuxKey::T},
        std::pair{"controls", LuxKey::F1},   std::pair{"stats", LuxKey::F3},

        std::pair{"ray", LuxKey::G},         std::pair{"aa", LuxKey::H},
        std::pair{"pause", LuxKey::P},       std::pair{"quit", LuxKey::Escape},
    };
    for (auto [action, key] : bindings)
        Input::BindAction(action, key);
    if (!options_.smoke && !options_.benchmark && options_.capture.empty() &&
        !GetWindow().SetRelativeMouseMode(true))
        throw std::runtime_error("Cannot capture relative mouse input");
    Input::ConsumeMouseDelta();
    automationStart_ = std::chrono::steady_clock::now();
}

void FrontierApp::Step(const Controls& controls, double dt) {
    const auto start = std::chrono::steady_clock::now();
    game_.Step(controls, world_, dt);
    if (!game_.Paused())
        time_ += dt;
    updateMs_ +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void FrontierApp::OnUpdate(double dt) {
    if (options_.smoke || options_.benchmark || !options_.capture.empty())
        return;
    if (Input::WasActionPressed("quit"))
        Close();
    if (Input::WasActionPressed("pause")) {
        game_.TogglePause();
        GetWindow().SetRelativeMouseMode(!game_.Paused());
        Input::ConsumeMouseDelta();
    }
    if (Input::WasActionPressed("controls"))
        controlsVisible_ = !controlsVisible_;
    if (Input::WasActionPressed("stats"))
        showStats_ = !showStats_;
    if (Input::WasActionPressed("aa")) {
        settings_.temporalAntialiasing = !settings_.temporalAntialiasing;
        settings_.screenSpaceAntialiasing = !settings_.temporalAntialiasing && options_.edgeAA;
    }
    if (Input::WasActionPressed("ray")) {
        if (GetGpuDevice().GetInfo().rayTracing)
            rt_ = !rt_;
        else
            Log::Info("This graphics device does not expose ray tracing.");
    }
    const auto axis = [](const char* plus, const char* minus) {
        return double(Input::IsActionDown(plus)) - double(Input::IsActionDown(minus));
    };
    const auto [mouseX, mouseY] = Input::ConsumeMouseDelta();
    Step({axis("forward", "reverse"), axis("right", "left"), mouseX, mouseY,
          Input::IsActionDown("run"), Input::WasActionPressed("jump"),
          Input::WasActionPressed("crouch"), Input::WasActionPressed("interact"),
          Input::WasActionPressed("camera"), Input::WasActionPressed("weather")},
         dt);
}

void FrontierApp::SmokeStep() {
    constexpr double dt = 1. / 60;
    if (smokeStage_ == 0) {
        game_.StartAt({10, 4.7}, false);
        game_.Face({10, -100});
        const auto baseline = game_.Look();
        Step({.mouseX = 30}, dt);
        const auto right = game_.Look();
        Step({.mouseY = -30}, dt);
        const auto up = game_.Look();
        if (right.x <= baseline.x || std::abs(right.y - baseline.y) > 1e-8 || up.y <= right.y ||
            std::abs(up.x / up.z - right.x / right.z) > 1e-8)
            throw std::runtime_error("Mouse axes are swapped or inverted");
        Step({.interact = true}, dt);
        if (!game_.Driving())
            throw std::runtime_error("Pickup entry failed");
        driveStart_ = game_.Truck();
        ++smokeStage_;
    }
    if (smokeStage_ != 1)
        return;
    for (int i = 0; i < 12; ++i)
        Step({.throttle = 1}, dt);
    if (Distance(Horizontal(driveStart_), Horizontal(game_.Truck())) <= 4)
        return;
    game_.StartAt({820, 24}, false);
    ++smokeStage_;
}

void FrontierApp::UpdateLight() {
    const double angle = Tau * (game_.Hour() - 6) / 24;
    const double height = std::sin(angle);
    const double daylight = std::clamp(height * 4 + .12, 0., 1.);
    const double dusk = std::max(0., 1 - 4 * std::abs(height)) * daylight;
    const float clouds = game_.CloudCover();
    settings_.sun.direction =
        Vec3{-.78 * std::cos(angle) - .29, -std::max(.025, height), -.64}.Normalized();
    settings_.sun.color = {1, .95 - .3 * dusk, .89 - .48 * dusk};
    settings_.sun.intensity =
        float(daylight * (2.9 - 2.3 * clouds) + .035 + game_.StormFlash() * 6);
    settings_.environmentColor = {.115 + daylight * .24, .143 + daylight * .24,
                                  .22 + daylight * .27};
    settings_.environmentIntensity = float(.24 + daylight * (.7 - clouds * .27));
    settings_.fog.color = {.47 + daylight * .30, .59 + daylight * .23, .78 + daylight * .14};
    settings_.fog.baseHeight = game_.Player().y - .9;
    settings_.fog.density = game_.Fog();
    settings_.fog.rayTracedShadows = rt_;
    settings_.waterReflection.intensity = float(.14 + 1.76 * daylight);
    settings_.rayTracing.shadows = rt_;
    settings_.rayTracing.reflections = rt_;
    settings_.rayTracing.refractions = rt_;
    settings_.rayTracing.indirectDiffuse = rt_;
    settings_.rayTracing.maxDistance = 95;
    settings_.rayTracing.maxBounces = 2;
    ground_.roughness = float(1 - .48 * game_.Wetness());
}

void FrontierApp::DrawVegetation(Vec3 eye) {
    for (const auto& tree : nearTrees_)
        props_.Draw(renderer_, "norway-spruce", tree.ground, tree.dimensions, tree.yaw);

    const ChunkKey center{int(std::floor(eye.x / CoverTileSize)),
                          int(std::floor(eye.z / CoverTileSize))};
    const int radius = options_.quality.radius > 1 ? 2 : 1;
    const auto away = [&](ChunkKey tile) {
        return std::max(std::abs(tile.x - center.x), std::abs(tile.z - center.z));
    };
    std::erase_if(groundCover_, [&](const auto& entry) { return away(entry.first) > radius; });
    unsigned uploads = 0;
    for (int ring = 0; ring <= radius && uploads < 3; ++ring)
        for (int z = -ring; z <= ring && uploads < 3; ++z)
            for (int x = -ring; x <= ring && uploads < 3; ++x) {
                const ChunkKey tile{center.x + x, center.z + z};
                if (away(tile) != ring || groundCover_.contains(tile))
                    continue;
                const Vec3 origin{double(tile.x * CoverTileSize), 0,
                                  double(tile.z * CoverTileSize)};
                if (!world_.HasChunk(ChunkAt(Horizontal(origin))))
                    continue;
                const auto plants = GroundCover(tile, options_.seed);
                auto& parts = groundCover_[tile];
                constexpr std::array names{"grass-clump", "sedge-tuft"};
                for (size_t i = 0; i < names.size(); ++i) {
                    auto batch = props_.Batch(GetGpuDevice(), names[i], plants[i], origin);
                    for (auto& part : batch)
                        parts.push_back(std::move(part));
                }
                ++uploads;
            }
    for (const auto& [tile, parts] : groundCover_) {
        const auto transform =
            Mat4::Translate({double(tile.x * CoverTileSize), 0, double(tile.z * CoverTileSize)});
        for (const auto& part : parts)
            renderer_.DrawMesh(*part.mesh, transform, part.material);
    }
}

void FrontierApp::DrawActors() {
    const auto player = game_.Player(), truck = game_.Truck();
    const auto nearby = [&](Vec3 point, double range) {
        return Distance(Horizontal(player), Horizontal(point)) < range;
    };
    if (nearby(truck, 440))
        props_.Draw(renderer_, "pickup-truck", truck - Vec3{0, .81, 0}, {1.9, 1.77, 4.53},
                    std::numbers::pi - game_.TruckYaw());
    for (const auto& structure : Structures()) {
        const auto base = OnGround(structure.position, options_.seed, -.07);
        if (nearby(base, 550) && world_.HasChunk(ChunkAt(structure.position)))
            props_.Draw(renderer_, structure.model, base, structure.size, structure.yaw);
    }
    const auto people = game_.Pedestrians();
    for (size_t i = 0; i < people.size(); ++i) {
        if (!nearby(people[i], 180))
            continue;
        const auto movement = game_.PedestrianVelocity(i);
        const auto speed = Distance(movement, {});
        const double yaw = speed > .14 ? std::atan2(movement.x, -movement.y) : .6 * double(i);
        characters_.Draw(renderer_, i, people[i], yaw, false, speed, time_);
    }
    if (game_.ShowPlayer())
        characters_.Draw(renderer_, 3, player, game_.PlayerYaw(), game_.Crouched(),
                         Distance(game_.Velocity(), {}), time_);
}

void FrontierApp::DrawHud() {
    constexpr Vec4 paper{.91f, .89f, .82f, 1}, subtle{.77f, .73f, .63f, 1};
    constexpr Vec4 shadow{.025f, .028f, .025f, .86f};
    const double edge = 6 * double(frameWidth_) / std::max(1u, frameHeight_), left = -edge + .38;
    if (game_.Wetness() > .25 && game_.WeatherName() != "Overcast") {
        const bool snow = game_.WeatherName() == "Snow" || game_.WeatherName() == "Blizzard";
        for (unsigned i = 0; i < unsigned(42 * game_.Wetness()); ++i) {
            const double phase = std::fmod(double(i) * .7723 + time_ * (snow ? .17 : 1.7), 1.);
            const double x = std::fmod(double(i) * .6180339 + time_ * .052, 1.) * 2 * edge - edge;
            artwork_->Quad({x, 6 - phase * 12}, snow ? Vec2{.026, .034} : Vec2{.012, .23},
                           {.72f, .83f, .91f, snow ? .26f : .15f}, snow ? 0.f : -.34f);
        }
    }
    const auto text = [&](std::string_view value, Vec2 at, float size, Vec4 color) {
        artwork_->Text(value, at + Vec2{.012, -.015}, size, shadow);
        artwork_->Text(value, at, size, color);
    };
    if (!options_.benchmark && options_.capture.empty()) {
        if (controlsVisible_) {
            const auto point = Horizontal(game_.Player());
            const unsigned minutes = unsigned(game_.Hour() * 60);
            artwork_->Quad({left + 4.85, 3.97}, {10.15, 3.19}, {.026f, .028f, .025f, .87f});
            artwork_->Text(std::format("{} / {:02}:{:02} / {}", RegionAt(point).name, minutes / 60,
                                       minutes % 60, game_.WeatherName()),
                           {left, 5.26}, .048f, paper);
            artwork_->Text("WASD move / drive    Mouse look    E enter / exit", {left, 4.56}, .035f,
                           subtle);
            artwork_->Text("V camera    T weather    G ray tracing    H TAA", {left, 4.02}, .035f,
                           subtle);
            artwork_->Text("F3 diagnostics    F1 close    Esc quit", {left, 3.45}, .035f, subtle);
        }
        if (game_.Paused())
            text("Paused", {-.75, .2}, .062f, paper);
    }
    if (showStats_) {
        const auto chunks = world_.GetStats();
        const auto gpu = GetGpuDevice().GetFrameStats();
        const auto memory = GetGpuDevice().GetMemoryStats();
        const double top = controlsVisible_ ? 1.9 : 5.2;
        artwork_->Quad({left + 4.05, top - .55}, {8.7, 1.63}, {.023f, .029f, .027f, .88f});
        artwork_->Text(std::format("{:.1f} ms present   {} draws   {} dispatches   {} bodies",
                                   recorder_.LastIntervalMs(), gpu.drawCalls, gpu.computeDispatches,
                                   game_.Bodies()),
                       {left, top}, .036f, paper);
        artwork_->Text(std::format("Chunks {} + {} pending   Trees {}   GPU {:.0f} MiB",
                                   chunks.loaded, chunks.pending, chunks.trees,
                                   double(memory.allocationBytes) / 1048576),
                       {left, top - .47}, .036f, paper);
        artwork_->Text(rt_ ? "Hardware RT requested" : "Raster lighting", {left, top - .92}, .036f,
                       subtle);
    }
    artwork_->Flush(Mat4::Orthographic(-edge, edge, -6, 6, -1, 1));
}

void FrontierApp::OnRender(float) {
    auto& gpu = GetGpuDevice();
    auto& sample = recorder_.BeginFrame();
    if (options_.smoke)
        SmokeStep();
    if (options_.benchmark)
        Step({.throttle = options_.scenario == "drive" ? 1. : 0.}, GetFixedUpdateStep());
    world_.Stream(Horizontal(game_.Player()), game_.Velocity(), gpu);
    sample.chunks = world_.GetStats();
    sample.physicsAndGameMs = std::exchange(updateMs_, 0);
    sample.bodies = game_.Bodies();

    GetWindow().GetFramebufferSize(frameWidth_, frameHeight_);
    PerspectiveCamera camera(ToRadians(game_.Driving() ? 62 : 56),
                             double(std::max(1u, frameWidth_)) / std::max(1u, frameHeight_), .09,
                             1400);
    const auto now = std::chrono::steady_clock::now();
    const double cameraDt =
        lastRender_ == std::chrono::steady_clock::time_point{} || options_.benchmark ||
                options_.smoke
            ? GetFixedUpdateStep()
            : std::clamp(std::chrono::duration<double>(now - lastRender_).count(), .001, .12);
    lastRender_ = now;
    camera.SetPosition(game_.Camera(cameraDt));
    camera.SetTarget(game_.CameraTarget());
    const bool fixedView = options_.benchmark || !options_.capture.empty();
    if (fixedView && options_.scenario == "water") {
        camera.SetPosition({222, 19.8, 310});
        camera.SetTarget({389, LakeLevel + 3.2, 449});
    } else if (fixedView && options_.scenario == "forest") {
        camera.SetPosition({-587, Height({-587, -128}, options_.seed) + 6, -128});
        camera.SetTarget({-735, Height({-735, -208}, options_.seed) + 8, -208});
    }

    UpdateLight();
    if (!gpu.Clear(.22f, .35f, .49f, 1))
        throw std::runtime_error(gpu.GetLastError());
    const bool drawWater = Distance(Horizontal(camera.GetPosition()), LakeCenter) < 390;
    if (drawWater && (!lake_.SetTime(time_) || !lake_.SyncGpu(gpu)))
        throw std::runtime_error("Silverlake GPU upload failed: " + gpu.GetLastError());
    const auto hour = game_.Hour();
    const float lamp = float(42 * std::clamp((std::abs(hour - 12) - 5.) / 2., 0., 1.));
    const std::array lights{PointLight{OnGround(Depot, options_.seed, 5), {1, .69, .42}, lamp, 26}};
    renderer_.BeginScene(camera, settings_, .98f, lights);
    nearTrees_.clear();
    world_.Draw(renderer_, ground_, scenery_, foliage_, road_, camera.GetPosition(), nearTrees_);
    DrawVegetation(camera.GetPosition());
    DrawActors();
    if (drawWater) {
        auto water = WaterSurface::MakeMaterial();
        water.roughness = float(.07 + .12 * game_.Wetness());
        water.water->absorption = {.42, .18, .097};
        renderer_.DrawMesh(lake_.GetMesh(),
                           Mat4::Translate({LakeCenter.x, LakeLevel, LakeCenter.y}), water);
    }
    renderer_.EndScene();
    DrawHud();
    if (!options_.capture.empty() && !capturePending_ && recorder_.PresentedFrames() > 10 &&
        world_.GetStats().pending == 0 && renderer_.IsEnvironmentReady() &&
        (!drawWater || gpu.GetFrameStats().waterReflectionPasses > 0)) {
        if (!gpu.RequestFrameCapture())
            throw std::runtime_error("Could not start frame capture");
        capturePending_ = true;
    }
}

void FrontierApp::OnFramePresented() {
    const auto& gpu = GetGpuDevice();
    const bool finished = recorder_.EndFrame(gpu, frameWidth_, frameHeight_);
    sampledTemporalHistory_ |= gpu.GetFrameStats().temporalHistoryPasses != 0;
    if (options_.benchmark || options_.smoke || !options_.capture.empty()) {
        if (gpu.GetCounters().validationErrors)
            throw std::runtime_error("GPU validation reported errors");
        if (!renderer_.GetLastAssetError().empty())
            throw std::runtime_error(renderer_.GetLastAssetError());
        if (!options_.benchmark &&
            std::chrono::steady_clock::now() - automationStart_ > std::chrono::seconds(45))
            throw std::runtime_error(
                std::format("Automated run timed out at stage {}, with {} chunks", smokeStage_,
                            world_.GetStats().loaded));
    }
    if (capturePending_) {
        LuxSamples::WritePpmCapture(GetGpuDevice(), options_.capture);
        if (!options_.benchmark) {
            done_ = true;
            Close();
            return;
        }
        capturePending_ = false;
        options_.capture.clear();
    }
    if (options_.smoke && smokeStage_ == 2 &&
        world_.HasChunk(ChunkAt(Horizontal(game_.Player()))) && world_.GetStats().evicted > 0 &&
        gpu.GetFrameStats().drawCalls > 0 &&
        (!settings_.temporalAntialiasing || sampledTemporalHistory_)) {
        std::cout << "Frontier County smoke passed: FBX motion, Jolt drive, chunk eviction and GPU "
                     "draw\n";
        done_ = true;
        Close();
    }
    if (finished) {
        recorder_.Write(options_.output, options_.scenario, gpu.GetInfo().deviceName,
                        options_.quality.name, options_.seed);
        done_ = options_.capture.empty();
        Close();
    }
}

void FrontierApp::OnShutdown() {
    world_.Stop();
    artwork_.reset();
    lake_.ReleaseGpu();
    groundCover_.clear();
    props_.Release();
    renderer_.Shutdown();
}
} // namespace
} // namespace Frontier

int RunFrontier(int argc, char** argv) {
    try {
        auto options = Frontier::Parse(argc, argv);
        if (options.help) {
            std::cout
                << "Frontier County\n"
                   "  WASD move/drive  mouse look  Shift jog/brake  Space jump  C crouch\n"
                   "  E enter/exit  V camera  T weather  F1 "
                   "controls\n"
                   "  G hardware RT  H TAA  P pause  F3 stats  Esc exit\n"
                   "  --preset low|medium|high|ultra|cinematic|extreme  --resolution WIDTHxHEIGHT\n"
                   "  --seed N  --day-seconds N  --vegetation N  --rt  --no-taa  --no-edge-aa\n"
                   "  --smoke-test | --benchmark [--scenario town|drive|forest|water|weather]\n"
                   "  --warmup N  --frames N  --output PREFIX  --capture FRAME.ppm\n";
            return 0;
        }
        Frontier::FrontierApp app(std::move(options));
        return app.Initialize() ? app.Run() : 1;
    } catch (const std::exception& error) {
        std::cerr << "Frontier County: " << error.what() << '\n';
        return 1;
    }
}

LUX_ENGINE_MAIN(RunFrontier)
