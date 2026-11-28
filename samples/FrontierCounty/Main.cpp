#include "Characters.hpp"
#include "Game.hpp"
#include "PropModels.hpp"
#include "RenderDebugPanel.hpp"
#include "SurfaceMarks.hpp"
#include <Common/Artwork.hpp>
#include <LuxEngine/Application.hpp>
#include <LuxEngine/EntryPoint.hpp>
#include <LuxEngine/Renderer/Water/WaterSurface.hpp>
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <format>
#include <future>
#include <iostream>
#include <map>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <utility>

namespace Frontier {
namespace {
using namespace LuxEngine;
constexpr double Tau = 2 * std::numbers::pi;
constexpr unsigned WorldSeed = 4805;
constexpr unsigned TreeCandidates = 1024;
constexpr int StreamRadius = 2;

std::filesystem::path PathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string Utf8Path(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

bool CoverTerrainReady(const World& world, ChunkKey tile) {
    const int left = std::max(-WorldHalf, tile.x * CoverTileSize);
    const int top = std::max(-WorldHalf, tile.z * CoverTileSize);
    const int right = std::min(WorldHalf - 1, (tile.x + 1) * CoverTileSize - 1);
    const int bottom = std::min(WorldHalf - 1, (tile.z + 1) * CoverTileSize - 1);
    if (left > right || top > bottom)
        return false;
    const auto first = ChunkAt({double(left), double(top)});
    const auto last = ChunkAt({double(right), double(bottom)});
    for (int z = first.z; z <= last.z; ++z)
        for (int x = first.x; x <= last.x; ++x)
            if (!world.HasChunk({x, z}))
                return false;
    return true;
}

class FrontierApp final : public Application {
public:
    FrontierApp() : Application({"Frontier County", 1280, 720, true}), game_(WorldSeed, 3600) {}

private:
    void OnInit() override;
    void OnEvent(Event& event) override;
    void OnUpdate(double dt) override;
    void OnRender(float) override;
    void OnFramePresented() override;
    void OnShutdown() override;
    void Step(const Controls& controls, double dt);
    void UpdateLight();
    void DrawActors(const TransformComponent& truckPose);
    void DrawVegetation(Vec3 eye, bool allowUploads);
    void DrawHud();
    void SetRenderPanelOpen(bool open);

    Game game_;
    World world_;
    Renderer3D::Context renderer_;
    WaterSurface lake_;
    PropModels props_;

    struct CoverTile {
        std::deque<PropModels::Geometry> pending;
        std::vector<PropModels::Part> parts;
    };

    struct CoverBuild {
        ChunkKey tile;
        std::future<std::deque<PropModels::Geometry>> result;
    };

    std::map<ChunkKey, CoverTile> groundCover_;
    std::optional<CoverBuild> coverBuild_;
    std::vector<Plant> nearTrees_;
    Characters characters_;
    Material3D ground_, scenery_, foliage_, road_;
    SurfaceMarks surfaceMarks_;
    Render3DSettings settings_;
    std::unique_ptr<LuxSamples::Artwork> artwork_;
    std::filesystem::path assets_;
    std::chrono::steady_clock::time_point lastRender_, lastPresent_;
    RenderDebugPanel renderPanel_;
    double time_ = 0, lastWake_ = -1, presentMs_ = 0;
    unsigned frameWidth_ = 0, frameHeight_ = 0;
    bool terrainUploaded_ = false, coverUploadReady_ = false;
    bool rt_ = false, controlsVisible_ = false, showStats_ = false, renderPanelOpen_ = true;
};

void FrontierApp::OnInit() {
    auto& gpu = GetGpuDevice();
    const char* base = SDL_GetBasePath();
    if (!base)
        throw std::runtime_error("Could not locate sample assets");
    assets_ = PathFromUtf8(base) / "assets";
    artwork_ = std::make_unique<LuxSamples::Artwork>(gpu, assets_);
    renderer_.Init(&gpu);
    const Vec2 initial{225, 326};
    world_.Start(WorldSeed, TreeCandidates, StreamRadius, initial, gpu);
    game_.Start(world_);
    game_.StartAt(initial, false);
    game_.Face(LakeCenter);
    const std::array waves{
        WaterWave{{.63, -.78}, .045, 17.7, 2.6, .15, .42},
        WaterWave{{-.94, .38}, .032, 11.7, 2.05, 1.2, .4},
        WaterWave{{.39, .92}, .019, 7.4, 1.8, .9, .25},
        WaterWave{{-.44, -.90}, .009, 4.7, 1.2, .56, .29},
    };
    if (!lake_.Configure({LakeSize, 128, 106, 0}, waves))
        throw std::runtime_error("Cannot initialize Silverlake waves");
    props_.Load(gpu, assets_);
    characters_.Load(assets_);
    ground_.alphaMode = MaterialAlphaMode::Opaque;
    ground_.textureSampling.anisotropic = true;
    ground_.parallaxDepth = .036f;
    scenery_.alphaMode = MaterialAlphaMode::Opaque;
    const auto palette = assets_ / "FrontierTerrain";
    ground_.terrainLayer = TerrainLayer3D{
        Utf8Path(palette / "albedo.jpg"), "", Utf8Path(palette / "surface.png"), "", .8f, true};
    scenery_.roughness = .78f;
    const auto asphalt = assets_ / "FrontierAsphalt";
    road_.alphaMode = MaterialAlphaMode::Opaque;
    road_.textureSampling.anisotropic = true;
    road_.baseColorTexture = Utf8Path(asphalt / "albedo.jpg");
    road_.normalTexture = Utf8Path(asphalt / "normal.png");
    road_.roughnessTexture = Utf8Path(asphalt / "roughness.png");
    const auto spruce = assets_ / "FrontierVegetation" / "norway-spruce";
    foliage_.baseColorTexture = Utf8Path(spruce / "spruce-views-color.png");
    foliage_.normalTexture = Utf8Path(spruce / "spruce-views-normal-depth.png");
    foliage_.alphaMode = MaterialAlphaMode::Mask;
    foliage_.alphaCutoff = .42f;
    foliage_.roughness = .92f;
    foliage_.textureSampling = {true, TextureAddressMode::Clamp, true, false};
    settings_.showSkybox = true;
    surfaceMarks_.Initialize(assets_);
    settings_.temporalAntialiasing = true;
    settings_.environmentPath = Utf8Path(assets_ / "LakesideOcean" / "lakeside_physical_noon.hdr");
    settings_.shadowResolution = 1024;
    settings_.gtaoRadius = 1.25f;
    settings_.contactShadowDistance = .6f;
    settings_.shadowLightAngularRadius = .00465f;
    settings_.hierarchicalReflections = true;
    settings_.dynamicAtmosphere = true;
    settings_.dynamicSkyLighting = true;
    settings_.cloudShadowStrength = 1.f;
    settings_.bloomIntensity = .12f;
    settings_.physicallyBasedBloom = true;
    settings_.autoExposure = true;
    settings_.localExposure = .25f;
    // Scene-linear units are scaled to keep daylight radiances inside the FP16 scene buffer.
    settings_.physicalCamera.exposure = true;
    settings_.physicalCamera.iso = 100;
    settings_.physicalCamera.fNumber = 16.f;
    settings_.physicalCamera.shutterSeconds = 1.f / 125.f;
    settings_.physicalCamera.sensorHeightMm = 24;
    settings_.physicalCamera.luminanceScale = 25000; // cd/m² per scene-linear radiance unit.
    settings_.metering.lowPercentile = .18f;
    settings_.metering.highPercentile = .85f;
    settings_.metering.minEV = -2.5f;
    settings_.metering.maxEV = 10.f;
    settings_.focusDistance = 30.f;
    settings_.colorGradingLut = Utf8Path(assets_ / "FrontierCounty" / "noon.cube");
    settings_.colorGradingStrength = .25f;
    {
        // Ashford's workshop bay opens into the yard. Cross the covered threshold
        // gradually as the camera passes the building's perimeter.
        PostProcessVolume workshop;
        workshop.center = {-57, Height({-57, -19}, WorldSeed) + 2.5, -19};
        workshop.halfExtent = {10, 2.8, 6};
        workshop.blendDistance = 3;
        workshop.priority = 10;
        workshop.compensationEV = .4f;
        workshop.localExposure = .35f;
        settings_.postVolumes.push_back(workshop);
    }
    foliage_.windAmplitude = .32f;
    ground_.roughness = 1;
    ground_.wettingResponse = 1;
    road_.wettingResponse = .95f;
    foliage_.wettingResponse = .6f;
    foliage_.subsurfaceRadius = {.00035, .0009, .00022};
    foliage_.subsurfaceThickness = .00038f;
    auto& needles = foliage_.surface.emplace();
    needles.model = SurfaceModel::Foliage;
    needles.impostorDepth = true;
    needles.indexOfRefraction = 1.43f;
    needles.transmission = .48f;
    needles.anisotropy = .28f;
    settings_.cascadedShadows = true;
    settings_.shadowRange = 120;
    settings_.shadowNormalBias = .049f;
    settings_.virtualShadows = false;
    if (!settings_.dynamicSkyLighting)
        settings_.waterReflection = {
            Utf8Path(assets_ / "LakesideSunrise" / "lakeside_sunrise_water.hdr"), -52, 1.9f, 128};
    settings_.fog.heightFalloff = .095f;
    settings_.fog.densityVariation = .4f;
    settings_.fog.multipleScattering = .22f;
    settings_.fog.historyWeight = .74f;
    settings_.fog.maxDistance = 120.f;
    const std::array bindings{
        std::pair{"forward", LuxKey::W},     std::pair{"reverse", LuxKey::S},
        std::pair{"left", LuxKey::A},        std::pair{"right", LuxKey::D},
        std::pair{"run", LuxKey::LeftShift}, std::pair{"jump", LuxKey::Space},
        std::pair{"interact", LuxKey::E},    std::pair{"crouch", LuxKey::C},
        std::pair{"camera", LuxKey::V},      std::pair{"weather", LuxKey::T},
        std::pair{"controls", LuxKey::F1},   std::pair{"stats", LuxKey::F3},

        std::pair{"ray", LuxKey::G},         std::pair{"render-panel", LuxKey::Tab},
        std::pair{"pause", LuxKey::P},       std::pair{"quit", LuxKey::Escape},
    };
    for (auto [action, key] : bindings)
        Input::BindAction(action, key);
    renderPanel_.Initialize(gpu, settings_);
    SetRenderPanelOpen(true);
}

void FrontierApp::OnEvent(Event& event) {
    Application::OnEvent(event);
    if (renderPanelOpen_)
        renderPanel_.FeedEvent(event);
}

void FrontierApp::SetRenderPanelOpen(bool open) {
    renderPanelOpen_ = open;
    if (!GetWindow().SetRelativeMouseMode(!open && !game_.Paused()))
        throw std::runtime_error("Could not change mouse capture");
    Input::ConsumeMouseDelta();
}

void FrontierApp::Step(const Controls& controls, double dt) {
    game_.Step(controls, world_, dt);
    if (!game_.Paused())
        time_ += dt;
}

void FrontierApp::OnUpdate(double dt) {
    if (Input::WasActionPressed("quit")) {
        if (renderPanelOpen_)
            SetRenderPanelOpen(false);
        else
            Close();
        return;
    }
    if (Input::WasActionPressed("render-panel")) {
        SetRenderPanelOpen(!renderPanelOpen_);
        return;
    }
    if (Input::WasActionPressed("pause")) {
        game_.TogglePause();
        GetWindow().SetRelativeMouseMode(!renderPanelOpen_ && !game_.Paused());
        Input::ConsumeMouseDelta();
    }
    if (renderPanelOpen_) {
        Step({}, dt);
        return;
    }
    if (Input::WasActionPressed("controls"))
        controlsVisible_ = !controlsVisible_;
    if (Input::WasActionPressed("stats"))
        showStats_ = !showStats_;
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

void FrontierApp::UpdateLight() {
    const double angle = Tau * (game_.Hour() - 6) / 24;
    const double height = std::sin(angle);
    const double daylight = std::clamp(height * 4 + .12, 0., 1.);
    const double dusk = std::max(0., 1 - 4 * std::abs(height)) * daylight;
    const float clouds = game_.CloudCover();
    settings_.weatherTime = time_;
    settings_.aerosolDensity = .85f + clouds * 1.4f;
    settings_.cloudCoverage = .25f + .68f * clouds;
    settings_.windVelocity = {2.5 + 7. * clouds, 1.2 + 3. * clouds};
    settings_.sun.direction = Vec3{-.78 * std::cos(angle) - .29, -height, -.64}.Normalized();
    const Vec3 solarColor{1, .97 - .32 * dusk, .93 - .5 * dusk};
    settings_.sun.color = solarColor * (1. / solarColor.Dot({.2126, .7152, .0722}));
    // Authored clear-sky direct-normal illuminance is 100 klx at noon. Camera scaling
    // converts reflected radiance back to cd/m² once, after integration in the BRDF.
    const double airMass = std::clamp(1. / std::max(.04, height), 1., 25.);
    settings_.atmosphereSolarIrradiance = float(118000. / settings_.physicalCamera.luminanceScale);
    const double directLux = height > 0 ? 100000. * std::exp(-.16 * (airMass - 1.)) : 0.;
    settings_.sun.intensity =
        float((directLux * (1. - .88 * clouds) + game_.StormFlash() * 65000.) /
              settings_.physicalCamera.luminanceScale);
    settings_.colorGrade.temperatureK = float(6500. + 160. * dusk - 180. * clouds);
    settings_.colorGrade.vibrance = float(.025 * daylight * (1 - clouds));
    settings_.environmentColor = {.115 + daylight * .24, .143 + daylight * .24,
                                  .22 + daylight * .27};
    settings_.environmentIntensity = float(.24 + daylight * (.7 - clouds * .27));
    settings_.fog.color = {.47 + daylight * .30, .59 + daylight * .23, .78 + daylight * .14};
    settings_.fog.baseHeight = LakeLevel;
    settings_.fog.density = game_.Fog();
    settings_.fog.rayTracedShadows = rt_;
    settings_.waterReflection.intensity = float(.14 + 1.76 * daylight);
    settings_.rayTracing.shadows = rt_;
    settings_.rayTracing.reflections = rt_;
    settings_.rayTracing.refractions = rt_;
    settings_.rayTracing.indirectDiffuse = rt_;
    settings_.rayTracing.restirDirect = rt_;
    settings_.rayTracing.restirIndirect = rt_;
    settings_.rayTracing.denoiser = rt_;
    settings_.hierarchicalReflections = !rt_;
    settings_.rayTracing.maxDistance = 95;
    settings_.rayTracing.maxBounces = 2;
    settings_.rainfall = static_cast<float>(game_.Wetness());
    settings_.surfaceWetness = static_cast<float>(game_.SurfaceWetness());
}

void FrontierApp::DrawVegetation(Vec3 eye, bool allowUploads) {
    for (const auto& tree : nearTrees_)
        props_.Draw(renderer_, "norway-spruce", tree.ground, tree.dimensions, tree.yaw,
                    tree.lodTransition);

    const ChunkKey center{int(std::floor(eye.x / CoverTileSize)),
                          int(std::floor(eye.z / CoverTileSize))};
    const int radius = StreamRadius;
    const auto away = [&](ChunkKey tile) {
        return std::max(std::abs(tile.x - center.x), std::abs(tile.z - center.z));
    };
    std::erase_if(groundCover_, [&](const auto& entry) {
        const int distance = away(entry.first);
        return distance > radius + 1 || (distance > radius && !entry.second.pending.empty()) ||
               !CoverTerrainReady(world_, entry.first);
    });
    if (coverBuild_ &&
        coverBuild_->result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const auto tile = coverBuild_->tile;
        auto geometry = coverBuild_->result.get();
        coverBuild_.reset();
        if (away(tile) <= radius && CoverTerrainReady(world_, tile))
            groundCover_.try_emplace(tile, CoverTile{std::move(geometry), {}});
    }
    unsigned drafts = 0;
    std::optional<ChunkKey> prepare;
    for (int ring = 0; ring <= radius; ++ring)
        for (int z = -ring; z <= ring; ++z)
            for (int x = -ring; x <= ring; ++x) {
                const ChunkKey tile{center.x + x, center.z + z};
                if (away(tile) != ring)
                    continue;
                const auto found = groundCover_.find(tile);
                if (found == groundCover_.end()) {
                    if (CoverTerrainReady(world_, tile)) {
                        if (!prepare)
                            prepare = tile;
                    }
                    continue;
                }
                auto& cover = found->second;
                if (cover.pending.empty())
                    continue;
                if (allowUploads) {
                    auto& geometry = cover.pending.front();
                    auto mesh = std::make_unique<Mesh3D>();
                    geometry.mesh.Upload(GetGpuDevice(), *mesh);
                    cover.parts.push_back({std::move(mesh), std::move(geometry.material)});
                    cover.pending.pop_front();
                    allowUploads = false;
                }
                drafts += unsigned(!cover.pending.empty());
            }
    if (prepare && !coverBuild_ && drafts < 2)
        coverBuild_.emplace(
            CoverBuild{*prepare, std::async(std::launch::async, &PropModels::BuildGroundCover,
                                            &props_, *prepare, WorldSeed)});
    coverUploadReady_ = drafts != 0;
    for (const auto& [tile, cover] : groundCover_) {
        if (away(tile) > radius)
            continue;
        const auto transform =
            Mat4::Translate({double(tile.x * CoverTileSize), 0, double(tile.z * CoverTileSize)});
        for (const auto& part : cover.parts)
            renderer_.DrawMesh(*part.mesh, transform, part.material);
    }
}

void FrontierApp::DrawActors(const TransformComponent& truckPose) {
    const auto player = game_.Player();
    const auto nearby = [&](Vec3 point, double range) {
        return Distance(Horizontal(player), Horizontal(point)) < range;
    };
    if (nearby(truckPose.position, 440)) {
        const auto placement = truckPose.GetTransform() * Mat4::Translate({0, -.81, 0}) *
                               Mat4::RotateY(std::numbers::pi);
        props_.Draw(renderer_, "pickup-truck", placement, {1.9, 1.77, 4.53}, 0x10000u);
    }
    for (const auto& structure : Structures()) {
        const auto base = OnGround(structure.position, WorldSeed, -.07);
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
    {
        if (controlsVisible_) {
            const auto point = Horizontal(game_.Player());
            const unsigned minutes = unsigned(game_.Hour() * 60);
            artwork_->Quad({left + 4.85, 3.97}, {10.15, 3.19}, {.026f, .028f, .025f, .87f});
            artwork_->Text(std::format("{} / {:02}:{:02} / {}", RegionAt(point).name, minutes / 60,
                                       minutes % 60, game_.WeatherName()),
                           {left, 5.26}, .048f, paper);
            artwork_->Text("WASD move / drive    Mouse look    E enter / exit", {left, 4.56}, .035f,
                           subtle);
            artwork_->Text("V camera    T weather    G ray tracing    Tab render debug",
                           {left, 4.02}, .035f, subtle);
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
                                   presentMs_, gpu.drawCalls, gpu.computeDispatches,
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
    const bool reserveCover =
        terrainUploaded_ &&
        (coverUploadReady_ || (coverBuild_ && coverBuild_->result.wait_for(std::chrono::seconds(
                                                  0)) == std::future_status::ready));
    world_.Stream(Horizontal(game_.Player()), game_.Velocity(), gpu, !reserveCover);
    terrainUploaded_ = world_.GetStats().uploadedMeshes != 0;

    GetWindow().GetFramebufferSize(frameWidth_, frameHeight_);
    PerspectiveCamera camera(ToRadians(game_.Driving() ? 62 : 56),
                             double(std::max(1u, frameWidth_)) / std::max(1u, frameHeight_), .09,
                             1400);
    const auto now = std::chrono::steady_clock::now();
    const double cameraDt =
        lastRender_ == std::chrono::steady_clock::time_point{}
            ? GetFixedUpdateStep()
            : std::clamp(std::chrono::duration<double>(now - lastRender_).count(), .001, .12);
    lastRender_ = now;
    const auto view = game_.GetRenderView(cameraDt, GetRenderInterpolationAlpha());
    camera.SetPosition(view.cameraPosition);
    camera.SetTarget(view.cameraTarget);
    UpdateLight();
    if (!gpu.Clear(.22f, .35f, .49f, 1))
        throw std::runtime_error(gpu.GetLastError());
    const bool drawWater = Distance(Horizontal(camera.GetPosition()), LakeCenter) < 390;
    const auto contact = game_.Driving() ? game_.Truck() : game_.Player();
    const auto contactXZ = Horizontal(contact);
    if (drawWater && InLake(contactXZ) && contact.y < LakeLevel + 1.2 && time_ - lastWake_ >= .32 &&
        std::hypot(game_.Velocity().x, game_.Velocity().y) > .15) {
        lake_.AddDisturbance(contactXZ - LakeCenter, time_, game_.Driving() ? .18 : .055,
                             game_.Driving() ? 7.5 : 3.8);
        lastWake_ = time_;
    }
    if (drawWater && (!lake_.SetTime(time_) || !lake_.SyncGpu(gpu)))
        throw std::runtime_error("Silverlake GPU upload failed: " + gpu.GetLastError());
    const auto hour = game_.Hour();
    const float lamp = float(42 * std::clamp((std::abs(hour - 12) - 5.) / 2., 0., 1.));
    const std::array lights{PointLight{OnGround(Depot, WorldSeed, 5), {1, .69, .42}, lamp, 26}};
    const auto pedestrian = game_.Player(), truck = game_.Truck();
    settings_.foliageInteractions = {
        FoliageInteraction{{pedestrian.x, pedestrian.z}, .60f, game_.Driving() ? 0.f : .9f},
        FoliageInteraction{{truck.x, truck.z}, 2.0f, 1.f}};
    surfaceMarks_.Update(gpu, camera.GetPosition(),
                         game_.Driving() ? game_.Truck() : game_.Player(), game_.Driving(),
                         game_.SurfaceWetness(), WorldSeed);
    auto renderSettings = settings_;
    renderPanel_.Apply(renderSettings);
    renderer_.BeginScene(camera, renderSettings, 1.f, lights);
    nearTrees_.clear();
    world_.Draw(renderer_, ground_, scenery_, foliage_, road_, camera.GetPosition(), nearTrees_);
    DrawVegetation(camera.GetPosition(), !terrainUploaded_);
    DrawActors(view.truck);
    surfaceMarks_.Draw(renderer_);
    if (drawWater) {
        auto water = WaterSurface::MakeMaterial();
        water.roughness = float(.07 + .12 * game_.Wetness());
        water.water->absorption = {.42, .18, .097};
        water.water->rippleStrength = .8f;
        renderer_.DrawMesh(lake_.GetMesh(),
                           Mat4::Translate({LakeCenter.x, LakeLevel, LakeCenter.y}), water);
    }
    renderer_.EndScene();
    DrawHud();
    if (renderPanel_.Draw(GetWindow(), GetGpuDevice(), renderPanelOpen_, game_.Paused(), cameraDt))
        SetRenderPanelOpen(!renderPanelOpen_);
}

void FrontierApp::OnFramePresented() {
    const auto now = std::chrono::steady_clock::now();
    if (lastPresent_ != std::chrono::steady_clock::time_point{})
        presentMs_ = std::chrono::duration<double, std::milli>(now - lastPresent_).count();
    lastPresent_ = now;
}

void FrontierApp::OnShutdown() {
    coverBuild_.reset();
    world_.Stop();
    artwork_.reset();
    surfaceMarks_.Release();
    renderPanel_.Shutdown();
    lake_.ReleaseGpu();
    groundCover_.clear();
    props_.Release();
    renderer_.Shutdown();
}
} // namespace
} // namespace Frontier

int RunFrontier(int, char**) {
    try {
        Frontier::FrontierApp app;
        return app.Initialize() ? app.Run() : 1;
    } catch (const std::exception& error) {
        std::cerr << "Frontier County: " << error.what() << '\n';
        return 1;
    }
}

LUX_ENGINE_MAIN(RunFrontier)
