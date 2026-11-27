#include "Swimmer.hpp"
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
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <locale>
#include <numbers>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace LuxEngine;

constexpr double Patch = 64;
constexpr double HalfPatch = Patch / 2;
constexpr double BuoyRadius = .21;
constexpr double Tau = 2 * std::numbers::pi;
constexpr int FirstWaterRow = -2, LastCoastalWaterRow = 1;
constexpr double CoastHorizon = 1800;
constexpr double CoastalSeaLevel = .16;

enum class CameraView { Default, Shore, Waterline, Swimmer };

struct Options {
    bool smoke = false;
    bool benchmark = false;
    bool pacedBenchmark = false;
    bool rayTracing = true;
    bool fixedWindow = false;
    std::optional<bool> temporalAntialiasing;
    std::filesystem::path capture;
    std::filesystem::path sequence;
    std::filesystem::path swimmer;
    std::optional<double> capturePitch;
    unsigned tick = 120;
    unsigned sequenceLength = 180;
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
        inland < 0 ? std::max(-7.5, .25 * inland) : inland * (.10 + .025 * std::sin(z * .057));
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
            const Vec3 sand{.89, .91, .85};
            const Vec3 moss{.30, .49, .22}, stone{.66, .67, .63};
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

void MakeHorizon(GpuDevice& device, Mesh3D& mesh) {
    constexpr double left = -Patch - HalfPatch, right = Patch + HalfPatch;
    constexpr double back = FirstWaterRow * Patch - HalfPatch;
    constexpr double front = LastCoastalWaterRow * Patch + HalfPatch;
    std::vector<Vertex3D> vertices;
    std::vector<uint32> indices;
    const auto strip = [&](double x0, double z0, double x1, double z1) {
        const auto first = static_cast<uint32>(vertices.size());
        for (const auto p : {Vec2{x0, z0}, Vec2{x0, z1}, Vec2{x1, z0}, Vec2{x1, z1}})
            vertices.push_back({{p.x, -.11, p.y}, {0, 1, 0}, {p.x / Patch, p.y / Patch}});
        indices.insert(indices.end(),
                       {first, first + 1, first + 2, first + 2, first + 1, first + 3});
    };
    constexpr double overlap = 5;
    strip(-CoastHorizon, -CoastHorizon, CoastHorizon, back + overlap);
    strip(-CoastHorizon, front - overlap, CoastHorizon, CoastHorizon);
    strip(-CoastHorizon, back + overlap, left + overlap, front - overlap);
    strip(right - overlap, back + overlap, CoastHorizon, front - overlap);
    if (!mesh.Upload(&device, vertices, indices))
        throw std::runtime_error("Could not upload the ocean horizon");
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

double WaterTextureNoise(double u, double v, uint32 width, uint32 depth, uint32 seed) {
    const double x = u * width, z = v * depth;
    const int ix = static_cast<int>(std::floor(x)), iz = static_cast<int>(std::floor(z));
    const double fx = x - ix, fz = z - iz;
    constexpr std::array<Vec2, 8> gradients{{{1, 0},
                                             {0, 1},
                                             {-1, 0},
                                             {0, -1},
                                             {.7071, .7071},
                                             {-.7071, .7071},
                                             {-.7071, -.7071},
                                             {.7071, -.7071}}};
    const auto corner = [&](int dx, int dz) {
        const uint32 px = static_cast<uint32>(ix + dx) & (width - 1);
        const uint32 pz = static_cast<uint32>(iz + dz) & (depth - 1);
        uint32 hash = (px * 0x85ebca6bu) ^ (pz * 0xc2b2ae35u) ^ seed;
        hash ^= hash >> 16u;
        hash *= 0x7feb352du;
        hash ^= hash >> 15u;
        hash *= 0x846ca68bu;
        hash ^= hash >> 16u;
        const auto direction = gradients[hash & 7u];
        return direction.x * (fx - dx) + direction.y * (fz - dz);
    };
    const auto fade = [](double t) { return t * t * t * (t * (6 * t - 15) + 10); };
    return .5 + .76 * std::lerp(std::lerp(corner(0, 0), corner(1, 0), fade(fx)),
                                std::lerp(corner(0, 1), corner(1, 1), fade(fx)), fade(fz));
}

std::vector<uint8> MakeBubbleCoverage(uint32 size) {
    std::vector<uint8> coverage(size_t(size) * size, 13);
    uint32 state = 0xd06597e1u;
    const auto jitter = [&] {
        state ^= state << 13u;
        state ^= state >> 17u;
        state ^= state << 5u;
        return double(state >> 8u) / 16777216.;
    };
    const uint32 mask = size - 1;
    const auto grain = [&](double x, double y) {
        const double pick = jitter();
        const double radius = .74 + 1.57 * pick * pick;
        const double width = radius * (.85 + .50 * jitter());
        const double depth = radius * (.86 + .40 * jitter());
        const double lean = (jitter() - .5) * .60;
        const double density = .35 + .49 * jitter();
        const double inverseWidth = 1. / width, inverseDepth = 1. / depth;
        const double span = width * (1 + std::abs(lean));
        for (int py = static_cast<int>(std::floor(y - depth));
             py <= static_cast<int>(std::ceil(y + depth)); ++py) {
            const double v = (py + .5 - y) * inverseDepth;
            const double rowWeight = 1. - v * v;
            for (int px = static_cast<int>(std::floor(x - span));
                 px <= static_cast<int>(std::ceil(x + span)); ++px) {
                const double u = (px + .5 - x) * inverseWidth + lean * v;
                const double body = std::max(0., rowWeight - u * u);
                const double amount = density * body * (.56 + .44 * body);
                const auto index = size_t(static_cast<uint32>(py) & mask) * size +
                                   (static_cast<uint32>(px) & mask);
                coverage[index] += static_cast<uint8>((255 - coverage[index]) * amount);
            }
        }
    };
    const size_t groups = coverage.size() / 63;
    for (size_t i = 0; i < groups; ++i) {
        const double x = jitter() * size, y = jitter() * size;
        const double width = 4.0 + 3.5 * jitter(), depth = 3.5 + 3.2 * jitter();
        for (int member = 0; member < 6; ++member) {
            const double offsetX = (jitter() + jitter() - 1.) * width;
            const double offsetY = (jitter() + jitter() - 1.) * depth;
            grain(x + offsetX, y + offsetY);
        }
        const double fleckX = jitter() * size, fleckY = jitter() * size;
        grain(fleckX, fleckY);
    }
    return coverage;
}

void MakeCapillaryNormal(GpuDevice& device, GpuTexture& texture, bool coast) {
    const uint32 size = coast ? 2048 : 256;
    const auto foamPattern = coast ? MakeBubbleCoverage(size) : std::vector<uint8>{};
    constexpr std::array waves{Vec3{6, 17, .012},   Vec3{13, -11, .016}, Vec3{21, 8, .010},
                               Vec3{-24, 16, .006}, Vec3{37, 73, .0031}, Vec3{81, -51, .0023},
                               Vec3{-93, 61, .0018}};

    struct Ripple {
        double x, z, steepness, phase;
    };

    std::array ripples{
        Ripple{26, 31, .13, .8},      Ripple{-24, 61, .115, 2.1},   Ripple{58, -35, .12, 4.6},
        Ripple{57, 86, .105, 1.5},    Ripple{113, 41, .10, 5.7},    Ripple{-99, 57, .092, 3.9},
        Ripple{134, -89, .083, 1.3},  Ripple{49, 189, .08, 4.1},    Ripple{247, 67, .073, 2.7},
        Ripple{-239, 189, .064, 5.3}, Ripple{317, -259, .053, 2.2}, Ripple{102, 443, .05, 3.2},
        Ripple{443, 271, .044, 4.7},  Ripple{-545, 273, .039, .4},  Ripple{633, -315, .03, 2.6}};
    for (auto& ripple : ripples)
        ripple.steepness /= std::hypot(ripple.x, ripple.z);
    std::vector<uint8> pixels(size * size * 4);
    const auto encode = [](double v) { return static_cast<uint8>(std::lround(127.5 * (v + 1))); };
    for (uint32 y = 0; y < size; ++y)
        for (uint32 x = 0; x < size; ++x) {
            const double u = Tau * x / size, v = Tau * y / size;
            Vec3 n{0, 0, 1};
            if (coast) {
                const double warpX = .022 * std::sin(13 * u + 9 * v);
                const double warpZ = .019 * std::cos(7 * u - 15 * v);
                for (const auto& ripple : ripples) {
                    const double slope =
                        ripple.steepness *
                        std::cos(ripple.x * (u + warpX) + ripple.z * (v + warpZ) + ripple.phase);
                    n.x += ripple.x * slope;
                    n.y += ripple.z * slope;
                }
            } else {
                for (const auto& wave : waves) {
                    const double slope = wave.z * std::cos(wave.x * u + wave.y * v + wave.z * 47);
                    n.x += wave.x * slope;
                    n.y += wave.y * slope;
                }
            }
            n = n.Normalized();
            const size_t index = (y * size + x) * 4;
            pixels[index] = encode(n.x);
            pixels[index + 1] = encode(n.y);
            pixels[index + 2] = encode(n.z);
            pixels[index + 3] = coast ? foamPattern[index / 4] : 255;
        }
    TextureSampling sampling{true, TextureAddressMode::Repeat, true, true};
    if (!texture.Initialize(&device, size, size, pixels.data(), TextureFilter::Linear,
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

std::filesystem::path PathFromUtf8(std::string_view text) {
    return std::filesystem::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

class FrameBufferedMesh {
public:
    bool Initialize(GpuDevice& gpu, std::span<const Vertex3D> vertices,
                    std::span<const uint32> indices) {
        std::vector<Mesh3D> buffers(std::max(2u, gpu.GetInfo().framesInFlight));
        for (auto& mesh : buffers)
            if (!mesh.Upload(&gpu, vertices, indices))
                return false;
        buffers_ = std::move(buffers);
        active_ = 0;
        return true;
    }

    // Stage at most once per render frame. The current mesh remains readable until publication.
    bool Stage(std::span<const Vertex3D> vertices) {
        return buffers_[Next()].UpdateVertices(vertices);
    }

    void Publish() {
        active_ = Next();
    }

    const Mesh3D& Current() const {
        return buffers_[active_];
    }

    void Release() {
        buffers_.clear();
    }

private:
    size_t Next() const {
        return (active_ + 1) % buffers_.size();
    }

    std::vector<Mesh3D> buffers_;
    size_t active_ = 0;
};

class LakesideDemo final : public Application {
public:
    explicit LakesideDemo(Options options)
        : Application({options.swimmer.empty() ? "LuxEngine · Lakeside at Noon"
                                               : "LuxEngine · Haerang Swim",
                       options.smoke ? 640u : 1440u, options.smoke ? 360u : 810u,
                       !options.fixedWindow}),
          options_(std::move(options)) {}

    int Run() {
        const int result = Application::Run();
        return result == 0 && (options_.benchmark || IsCapturing()) && !runFinished_ ? 1 : result;
    }

private:
    bool IsOpenOcean() const {
        return !options_.swimmer.empty();
    }

    bool IsCapturing() const {
        return !options_.capture.empty() || !options_.sequence.empty();
    }

    void ConfigureFramePacing() {
        if (!IsOpenOcean() || IsCapturing() || options_.smoke ||
            (options_.benchmark && !options_.pacedBenchmark))
            return;
        const auto display = SDL_GetDisplayForWindow(GetWindow().GetNativeHandle());
        const auto* mode = SDL_GetCurrentDisplayMode(display);
        const double refresh = mode && mode->refresh_rate > 0 ? mode->refresh_rate : 60;
        const auto interval = static_cast<uint32>(std::max(1., std::ceil(refresh / 90.5)));
        const double target = refresh / interval;
        auto& gpu = GetGpuDevice();
        const bool nativePacing = interval <= gpu.GetInfo().maxVSyncInterval;
        if (nativePacing) {
            if (!gpu.SetVSyncInterval(interval))
                throw std::runtime_error("Could not set the lakeside VSync interval");
        } else {
            framePeriod_ = static_cast<uint64>(std::lround(1e9 / target));
        }
        std::cout << "Display " << refresh << " Hz | frame target " << target << " FPS | "
                  << (nativePacing ? "VSync interval " + std::to_string(interval)
                                   : "software pacing")
                  << '\n';
    }

    void PaceFrame(uint64 now) {
        if (!framePeriod_)
            return;
        if (!frameDeadline_ || now > frameDeadline_ + framePeriod_)
            frameDeadline_ = now;
        if (now < frameDeadline_)
            SDL_DelayPrecise(frameDeadline_ - now);
        frameDeadline_ += framePeriod_;
    }

    void ResetOrbit() {
        orbitYaw_ = -1.07;
        orbitPitch_ = .23;
        orbitDistance_ = 4.35;
    }

    void OnInit() override {
        auto& gpu = GetGpuDevice();
        if (options_.benchmark &&
            (!gpu.GetInfo().gpuTimestamps || !gpu.SetVSync(options_.pacedBenchmark)))
            throw std::runtime_error("Could not configure the lakeside benchmark");
        ConfigureFramePacing();
        renderer_.Init(&gpu);
        if (!options_.swimmer.empty())
            swimmer_.Load(gpu, options_.swimmer, !options_.sequence.empty());
        MakeCapillaryNormal(gpu, capillary_, IsOpenOcean());
        if (IsOpenOcean()) {
            MakeHorizon(gpu, horizon_);
            MakeSurfEffects(gpu);
        } else {
            MakeTerrain(gpu, terrain_);
            const std::array stones{
                Ring{-.32, .18, {.12f, .11f, .09f, 1}}, Ring{-.09, .52, {.19f, .17f, .15f, 1}},
                Ring{.14, .48, {.25f, .22f, .19f, 1}}, Ring{.33, .25, {.29f, .26f, .22f, 1}},
                Ring{.37, .03, {.23f, .20f, .17f, 1}}};
            const Vec4 bark{.13f, .084f, .047f, 1};
            const std::array trunk{Ring{0, .061, bark}, Ring{.5, .039, bark},
                                   Ring{1.4, .009, bark}};
            MakeLathe(gpu, rock_, stones, 13, .24);
            MakeLathe(gpu, trunk_, trunk, 9, .13);
            MakeConifer(gpu, tree_);
        }
        const std::array marker{
            Ring{-.20, .035, {.026f, .033f, .037f, 1}}, Ring{-.15, .18, {.034f, .042f, .046f, 1}},
            Ring{-.03, .22, {.058f, .062f, .052f, 1}},  Ring{.01, .225, {.72f, .255f, .036f, 1}},
            Ring{.13, .196, {.84f, .35f, .064f, 1}},    Ring{.22, .12, {.83f, .51f, .19f, 1}},
            Ring{.265, .04, {.12f, .11f, .075f, 1}},    Ring{.34, .035, {.44f, .34f, .18f, 1}}};
        MakeLathe(gpu, buoy_, marker, 28, .01);

        OceanSpectrumSettings spectrum;
        spectrum.resolution = 128;
        spectrum.patchLength = Patch;
        spectrum.windDirection = {-.2, -1};
        spectrum.windSpeed = 6.8;
        spectrum.spectrumAmplitude = .0009;
        spectrum.choppiness = .8;
        spectrum.foamThreshold = .96;
        spectrum.seed = 48;
        if (IsOpenOcean()) {
            spectrum.resolution = 256;
            spectrum.windDirection = {-.91, -.4};
            spectrum.windSpeed = 7.4;
            spectrum.spectrumAmplitude = .00011;
            spectrum.choppiness = 1.22;
            spectrum.foamThreshold = .947;
            spectrum.foamSoftness = .105;
            spectrum.swells = {{{4, 1, .37, .85}, {6, -2, .19, 2.2}, {8, 2, .082, -.65}}};
        }
        if (!ocean_.Initialize(spectrum) || !water_.UploadOcean(&gpu, ocean_))
            throw std::runtime_error("Could not create the GPU FFT lake");

        for (const auto p : {Vec3{-1.9, .2, 4.6}, Vec3{3.3, .2, -5.4}, Vec3{10.5, .2, -18.8}}) {
            auto buoy = scene_.CreateEntity("Buoy");
            buoy.GetComponent<TransformComponent>().position =
                p + Vec3{0, IsOpenOcean() ? CoastalSeaLevel : 0, 0};
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
        settings_.fog = {{.76, .85, .96}, .0025f, 155, .095f, .1, .35f, false};
        if (IsOpenOcean()) {
            settings_.environmentRotation = 145;
            settings_.environmentPath = AssetPath("LakesideOcean", "coastal_clouds.hdr");
            settings_.waterReflection = {settings_.environmentPath, 145, 1, 512};
            coastalBreakerHeight_ = 3.60f;
            settings_.sun = {{0, -.618, -.786}, {1, .98, .94}, 3.2f};
            settings_.environmentColor = {.25, .31, .40};
            settings_.fog = {{.66, .79, .90}, .0035f, 650, .16f, .1 + CoastalSeaLevel, .27f, false};
        } else {
            shore_.alphaMode = MaterialAlphaMode::Opaque;
            shore_.roughness = 1;
            shore_.normalScale = .65f;
            shore_.baseColorTexture = AssetPath("RockyGravel", "albedo.jpg");
            shore_.normalTexture = AssetPath("RockyGravel", "normal.png");
            shore_.roughnessTexture = AssetPath("RockyGravel", "roughness.png");
        }
        settings_.screenSpaceAntialiasing = true;
        settings_.temporalAntialiasing = options_.temporalAntialiasing.value_or(IsOpenOcean());
        settings_.rayTracing.waterReflections = settings_.rayTracing.refractions =
            options_.rayTracing;
        settings_.rayTracing.maxDistance = 48;
        settings_.rayTracing.maxBounces = 1;
        fogDensity_ = settings_.fog.density;
        ResetOrbit();
        if (options_.capturePitch)
            orbitPitch_ = *options_.capturePitch;
        if (swimmer_.IsLoaded()) {
            if (options_.benchmark)
                swimmerYaw_ = 1.85;
            StepSwimmer(0);
            previousSwimmerPosition_ = swimmerPosition_;
            MakeWake(gpu);
        }
        if (!options_.sequence.empty())
            std::filesystem::create_directories(options_.sequence);
        if (IsCapturing())
            for (unsigned tick = 0; tick < options_.tick; ++tick)
                StepLake(GetFixedUpdateStep());
        start_ = std::chrono::steady_clock::now();
    }

    void OnEvent(Event& event) override {
        Application::OnEvent(event);
        EventDispatcher dispatcher(event);
        if (options_.view == CameraView::Swimmer && !IsScripted()) {
            dispatcher.Dispatch<MouseButtonPressedEvent>([&](const MouseButtonPressedEvent& mouse) {
                if (mouse.GetMouseButton() != static_cast<int32>(LuxMouseButton::Right))
                    return false;
                Input::ConsumeMouseDelta();
                GetWindow().SetRelativeMouseMode(true);
                return true;
            });
            dispatcher.Dispatch<MouseButtonReleasedEvent>(
                [&](const MouseButtonReleasedEvent& mouse) {
                    if (mouse.GetMouseButton() != static_cast<int32>(LuxMouseButton::Right))
                        return false;
                    GetWindow().SetRelativeMouseMode(false);
                    return true;
                });
            dispatcher.Dispatch<MouseScrolledEvent>([&](const MouseScrolledEvent& wheel) {
                orbitDistance_ =
                    std::clamp(orbitDistance_ * std::exp(-.14 * wheel.GetYOffset()), 2.15, 14.);
                return true;
            });
        }
        dispatcher.Dispatch<KeyPressedEvent>([&](KeyPressedEvent& key) {
            if (key.IsRepeat())
                return false;
            if (IsScripted() && key.GetKeyCode() != static_cast<int>(LuxKey::Escape))
                return false;
            switch (static_cast<LuxKey>(key.GetKeyCode())) {
            case LuxKey::Escape:
                Close();
                break;
            case LuxKey::Space:
                paused_ = !paused_;
                break;
            case LuxKey::F:
                settings_.fog.density = settings_.fog.density > 0 ? 0.f : fogDensity_;
                break;
            case LuxKey::Home:
                ResetOrbit();
                if (IsOpenOcean()) {
                    GetWindow().SetRelativeMouseMode(false);
                    options_.view = CameraView::Swimmer;
                }
                break;
            case LuxKey::V:
                if (!IsOpenOcean())
                    return false;
                GetWindow().SetRelativeMouseMode(false);
                options_.view =
                    options_.view == CameraView::Shore ? CameraView::Swimmer : CameraView::Shore;
                break;
            case LuxKey::R: {
                auto& ray = settings_.rayTracing;
                ray.refractions = ray.waterReflections = !ray.waterReflections;
                break;
            }
            case LuxKey::C:
                if (!GetGpuDevice().GetInfo().rayTracingCompute)
                    return false;
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
        if (!paused_ && !options_.benchmark && !IsCapturing())
            StepLake(dt);
    }

    bool IsScripted() const {
        return options_.benchmark || options_.smoke || IsCapturing();
    }

    void StepLake(double dt) {
        time_ += dt;
        if (time_ - waveSampleTime_ >= 1.0 / 30.0 - 1e-8) {
            if (!ocean_.Update(static_cast<float>(time_)))
                throw std::runtime_error("Could not sample the animated lake");
            waveSampleTime_ = time_;
        }
        const OceanBuoyancySettings buoyancy{.radius = BuoyRadius, .drag = 5.5};
        for (auto entity : scene_.GetEntities()) {
            const auto& position = entity.GetComponent<TransformComponent>().position;
            const auto& body = entity.GetComponent<Rigidbody3DComponent>();
            auto sample = LakeSurface(position);
            if (!sample)
                continue;
            if (IsOpenOcean())
                sample->velocity.y = std::clamp(sample->velocity.y, -2.4, 2.4);
            auto impulse =
                OceanBuoyancyImpulse(*sample, position.y, body.velocity, body.mass, dt, buoyancy);
            if (IsOpenOcean()) {
                const double liftBudget =
                    body.mass * std::max(0., 2.7 - body.velocity.y + 9.81 * dt);
                impulse.y = std::min(impulse.y, liftBudget);
            }
            if (!scene_.AddImpulse3D(entity.GetId(), impulse))
                throw std::runtime_error("Could not apply buoyancy");
        }
        scene_.OnUpdate(dt);
        if (swimmer_.IsLoaded()) {
            StepSwimmer(dt);
            StepSurfEffects(dt);
        }
    }

    OceanSample BreakerAtSurface(Vec3 point) const {
        const float height = coastalBreakerHeight_;
        if (height <= 0 || point.x < -14 - 1.1 * height || point.x > 16 || std::abs(point.z) >= 31)
            return {};
        const auto separation = [&](double x) {
            return x + SampleCoastalBreakerHorizontal({x, point.z}, time_, height) - point.x;
        };
        constexpr int intervals = 7;
        double bestHeight = -1;
        OceanSample best;
        double previousResidual = separation(point.x);
        for (int i = 0; i < intervals; ++i) {
            double left = point.x + 1.1 * height * i / intervals;
            double right = point.x + 1.1 * height * (i + 1) / intervals;
            const double a = previousResidual, b = separation(right);
            previousResidual = b;
            if ((a < 0) != (b < 0)) {
                for (int step = 0; step < 10; ++step) {
                    const double middle = (left + right) * .5;
                    if ((separation(middle) < 0) == (a < 0))
                        left = middle;
                    else
                        right = middle;
                }
            } else if (a == 0) {
                right = left;
            } else if (b == 0) {
                left = right;
            } else {
                continue;
            }
            const auto wave = SampleCoastalBreaker({(left + right) * .5, point.z}, time_, height);
            if (wave.displacement.y > bestHeight) {
                bestHeight = wave.displacement.y;
                best = wave;
            }
        }
        return best;
    }

    std::optional<OceanSample> LakeSurface(Vec3 point) const {
        const auto breaker = BreakerAtSurface(point);
        auto wave = ocean_.SampleAtSurface(
            {point.x - breaker.displacement.x + HalfPatch, point.z + HalfPatch});
        if (wave) {
            wave->displacement.y += wave->velocity.y * (time_ - waveSampleTime_);
            if (IsOpenOcean()) {
                wave->displacement.y += CoastalSeaLevel;
                wave->displacement += breaker.displacement;
                wave->velocity += breaker.velocity;
                wave->normal = (wave->normal + breaker.normal - Vec3{0, 1, 0}).Normalized();
                wave->foam = std::max(wave->foam, breaker.foam);
            }
        }
        return wave;
    }

    void StepSwimmer(double dt) {
        previousSwimmerPosition_ = swimmerPosition_;
        previousSwimmerYaw_ = swimmerYaw_;
        const bool scripted = IsScripted();
        Vec3 direction;
        if (scripted) {
            direction = {std::sin(swimmerYaw_), 0, std::cos(swimmerYaw_)};
        } else {
            const auto key = [](LuxKey code) { return int(Input::IsKeyPressed(code)); };
            const Vec3 forward{-std::sin(orbitYaw_), 0, -std::cos(orbitYaw_)};
            const Vec3 right{-forward.z, 0, forward.x};
            direction = forward * double(key(LuxKey::W) - key(LuxKey::S)) +
                        right * double(key(LuxKey::D) - key(LuxKey::A));
        }
        const bool moving = direction.Dot(direction) > 0;
        const auto wave = LakeSurface(swimmerPosition_).value_or(OceanSample{});
        const double stroke = .88 + .16 * std::sin(time_ * Tau / 1.4);
        if (moving) {
            direction = direction.Normalized();
            if (!scripted) {
                const auto target = std::atan2(direction.x, direction.z);
                swimmerYaw_ += std::remainder(target - swimmerYaw_, Tau) * (1 - std::exp(-11 * dt));
            }
            swimmerPosition_ += direction * (stroke * dt);
        }
        swimmerPosition_ += Vec3{wave.velocity.x, 0, wave.velocity.z} * (.22 * dt);
        const auto surface = LakeSurface(swimmerPosition_).value_or(OceanSample{});
        const double height = surface.displacement.y - (moving ? .18 : .55);
        const double weight = dt > 0 ? 1 - std::exp(-10 * dt) : 1;
        swimmerPosition_.y = std::lerp(swimmerPosition_.y, height, weight);
        const auto motion = moving ? Swimmer::Motion::Swim : Swimmer::Motion::Tread;
        swimmer_.Advance(motion, dt, 1);
        swimmingSpeed_ = moving ? stroke : 0;
    }

    void MakeWake(GpuDevice& gpu) {
        std::vector<uint32> indices;
        for (uint32 band = 0; band < WakeBands; ++band) {
            for (uint32 segment = 0; segment < WakeSegments; ++segment) {
                const uint32 a = WakeRows * (band * (WakeSegments + 1) + segment);
                for (uint32 row = 0; row + 1 < WakeRows; ++row) {
                    const uint32 start = a + row, next = start + WakeRows;
                    indices.insert(indices.end(),
                                   {start, start + 1, next, start + 1, next + 1, next});
                }
            }
        }
        UpdateWakeVertices();
        if (!wake_.Initialize(gpu, wakeVertices_, indices))
            throw std::runtime_error("Could not upload the water wake");
    }

    static void MakeSurfTexture(GpuDevice& gpu, GpuTexture& texture, bool surfaceFoam) {
        constexpr uint32 size = 128;
        const auto bubbles = surfaceFoam ? MakeBubbleCoverage(size) : std::vector<uint8>{};
        std::array<uint8, size * size * 4> pixels{};
        for (uint32 y = 0; y < size; ++y)
            for (uint32 x = 0; x < size; ++x) {
                const double u = (2. * x + 1) / size - 1, v = (2. * y + 1) / size - 1;
                const double edge = std::max(0., 1 - u * u - v * v);
                const double body = WaterTextureNoise(u, v, 8, 8, 7091);
                const double flecks = WaterTextureNoise(u + v, v - u, 32, 32, 3157);
                const size_t i = (size_t(y) * size + x) * 4;
                const double grains = surfaceFoam
                                          ? bubbles[i / 4] / 255. * (.62 + .38 * body)
                                          : .45 + .55 * std::clamp(body + .3 * flecks, 0., 1.);
                pixels[i] = pixels[i + 1] = pixels[i + 2] = 255;
                pixels[i + 3] = static_cast<uint8>(255 * edge * edge * grains);
            }
        if (!texture.Initialize(&gpu, size, size, pixels.data(), TextureFilter::Linear,
                                TextureColorSpace::Linear, {.mipmaps = true}))
            throw std::runtime_error("Could not create the surf effects texture");
    }

    void MakeSurfEffects(GpuDevice& gpu) {
        MakeSurfTexture(gpu, sprayTexture_, false);
        MakeSurfTexture(gpu, surfaceFoamTexture_, true);
        std::vector<uint32> indices;
        const auto upload = [&](FrameBufferedMesh& mesh, std::span<const Vertex3D> vertices) {
            indices.clear();
            for (uint32 i = 0; i < vertices.size(); i += 4)
                indices.insert(indices.end(), {i, i + 1, i + 2, i + 2, i + 1, i + 3});
            return mesh.Initialize(gpu, vertices, indices);
        };
        if (!upload(spray_, sprayVertices_) || !upload(surfaceFoam_, surfaceFoamVertices_))
            throw std::runtime_error("Could not upload the surf effects meshes");
    }

    double SurfRandom() {
        surfSeed_ ^= surfSeed_ << 13u;
        surfSeed_ ^= surfSeed_ >> 17u;
        surfSeed_ ^= surfSeed_ << 5u;
        return (surfSeed_ >> 8u) / 16777216.;
    }

    std::optional<Vec3> FindBreakerTip(double z) const {
        const float height = coastalBreakerHeight_;
        constexpr double start = -14, stride = .42;
        const auto displacedX = [&](double x) {
            return x + SampleCoastalBreakerHorizontal({x, z}, time_, height);
        };
        double before = start, middle = displacedX(start + stride);
        for (double x = start + 2 * stride; x <= 9; x += stride) {
            const double after = displacedX(x);
            if (middle < before && middle <= after) {
                const double adjust = (before - after) / (2 * (before - 2 * middle + after));
                const double tipX = x - stride + std::clamp(adjust, -.5, .5) * stride;
                const auto lip = SampleCoastalBreaker({tipX, z}, time_, height);
                const auto wave = ocean_.SampleAtSurface({tipX + HalfPatch, z + HalfPatch});
                if (!wave || lip.foam < .28)
                    break;
                return Vec3{tipX + lip.displacement.x,
                            CoastalSeaLevel + lip.displacement.y + wave->displacement.y +
                                wave->velocity.y * (time_ - waveSampleTime_),
                            z};
            }
            before = middle;
            middle = after;
        }
        return std::nullopt;
    }

    void EmitSurfSpray(double dt) {
        for (size_t site = 0; site < breakerSites_.size(); ++site) {
            auto& emitter = breakerSites_[site];
            const double z = -23.5 + site * 2.12 + .38 * std::sin(3.73 * site);
            const auto tip = FindBreakerTip(z);
            const double fall = tip && emitter.tipHeight
                                    ? std::clamp((*emitter.tipHeight - tip->y) / (3 * dt), 0., 1.)
                                    : 0.;
            emitter.tipHeight = tip ? std::optional{tip->y} : std::nullopt;
            emitter.contact += emitter.drift * dt;
            emitter.impact *= std::exp(-6.2 * dt);
            if (tip && fall > .12) {
                const Vec3 front = *tip - Vec3{.24, 0, 0};
                if (const auto water = LakeSurface(front)) {
                    const double gap = tip->y - water->displacement.y;
                    emitter.lip += dt * 28. * fall * std::clamp(gap * 1.7, 0., 1.);
                    while (emitter.lip >= 1) {
                        emitter.lip -= 1;
                        SpawnSurfDrop(*tip, false);
                    }
                    const double impact = fall * std::max(0., 1. - std::abs(gap - .035) / .43);
                    if (impact > .25 && impact >= emitter.impact) {
                        emitter.impact = impact;
                        emitter.contact = {front.x, water->displacement.y, front.z};
                        emitter.drift = {-.8 + .22 * water->velocity.x, 0, .24 * water->velocity.z};
                    }
                }
            }
            if (emitter.impact < .06)
                continue;
            if (const auto water = LakeSurface(emitter.contact))
                emitter.contact.y = water->displacement.y;
            emitter.splash += dt * 165. * emitter.impact;
            emitter.foam += dt * 48. * emitter.impact;
            while (emitter.splash >= 1) {
                emitter.splash -= 1;
                SpawnSurfDrop(emitter.contact, true);
            }
            while (emitter.foam >= 1) {
                emitter.foam -= 1;
                const Vec3 origin = emitter.contact +
                                    Vec3{-.10 - .34 * SurfRandom(), 0, 1.70 * (SurfRandom() - .5)};
                auto& patch = foamPatches_[nextFoam_++ % foamPatches_.size()];
                patch = {origin,
                         emitter.drift + Vec3{-.65 * SurfRandom(), 0, .68 * (SurfRandom() - .5)},
                         0,
                         2.4 + 1.7 * SurfRandom(),
                         .75 + 1.05 * SurfRandom(),
                         Tau * .25 + .95 * (SurfRandom() - .5),
                         .83 + .16 * SurfRandom()};
            }
        }
    }

    void SpawnSurfDrop(Vec3 source, bool splash) {
        const double size = SurfRandom();
        const bool plume = splash && size > .70;
        auto& drop = surfDrops_[nextDrop_++ % surfDrops_.size()];
        drop = {source + Vec3{-.16 - (splash ? .50 : .18) * SurfRandom(),
                              .07 + (plume ? .28 : .12) * SurfRandom(),
                              (splash ? 1.52 : .88) * (SurfRandom() - .5)},
                {-.62 - (splash ? 3.4 : 1.55) * SurfRandom(),
                 splash ? 1.85 + 4.15 * SurfRandom() : -.55 + 1.3 * SurfRandom(),
                 (splash ? 2.65 : 1.25) * (SurfRandom() - .5)},
                0,
                .63 + .55 * SurfRandom(),
                plume ? .30 + .37 * SurfRandom() : .035 + size * size * (splash ? .24 : .13),
                .48 + .5 * SurfRandom()};
    }

    void StepSurfEffects(double dt) {
        for (auto& patch : foamPatches_) {
            if (patch.age >= patch.lifetime)
                continue;
            patch.age += dt;
            patch.position += patch.velocity * dt;
            patch.velocity = patch.velocity * std::exp(-.82 * dt);
        }
        for (auto& drop : surfDrops_) {
            if (drop.age >= drop.lifetime)
                continue;
            drop.age += dt;
            drop.velocity.y -= 9.81 * dt;
            drop.position += drop.velocity * dt;
            if (drop.velocity.y >= 0 ||
                drop.position.y > CoastalSeaLevel + coastalBreakerHeight_ * 1.6)
                continue;
            const auto surface = LakeSurface(drop.position);
            if (!surface || drop.position.y > surface->displacement.y)
                continue;

            auto& patch = foamPatches_[nextFoam_++ % foamPatches_.size()];
            const Vec3 contact{drop.position.x, surface->displacement.y, drop.position.z};
            const Vec3 drift{
                std::clamp(drop.velocity.x * .22 + surface->velocity.x * .28, -2.1, 2.1), 0,
                std::clamp(drop.velocity.z * .22 + surface->velocity.z * .28, -2.1, 2.1)};
            patch = {contact,
                     drift,
                     0,
                     1.65 + 1.7 * SurfRandom(),
                     .135 + .28 * SurfRandom(),
                     Tau * SurfRandom(),
                     drop.opacity};
            drop.age = drop.lifetime;
        }
        sprayStep_ += dt;
        if (sprayStep_ >= 1. / 30. - 1e-8) {
            EmitSurfSpray(sprayStep_);
            sprayStep_ = 0.;
        }
    }

    void DrawSurfEffects(const PerspectiveCamera& camera, double renderTime) {
        const auto tick = std::floor(renderTime * 30);
        const auto face = (camera.GetPosition() - camera.GetTarget()).Normalized();
        if (tick != sprayTick_ || face.Dot(sprayFacing_) < .999) {
            const Vec3 right = Vec3{0, 1, 0}.Cross(face).Normalized();
            const Vec3 up = face.Cross(right);
            constexpr std::array<Vec2, 4> corners{{{0, 0}, {0, 1}, {1, 0}, {1, 1}}};
            for (size_t i = 0; i < surfDrops_.size(); ++i) {
                const auto& drop = surfDrops_[i];
                const bool alive = drop.age < drop.lifetime;
                const double amount = alive ? drop.age / drop.lifetime : 1;
                const double scale = drop.radius * (1 + amount * .7);
                const double fadeIn = std::min(1., drop.age * 37.);
                const float alpha = static_cast<float>(drop.opacity * fadeIn * (1 - amount));
                for (size_t c = 0; c < corners.size(); ++c) {
                    const auto uv = corners[c];
                    const auto p = alive ? drop.position + right * (scale * (uv.x - .5)) +
                                               up * (1.5 * scale * (uv.y - .5))
                                         : Vec3{};
                    sprayVertices_[i * 4 + c] = {p, face, uv, {.94f, .97f, 1, alpha}};
                }
            }
            for (size_t i = 0; i < foamPatches_.size(); ++i) {
                const auto& patch = foamPatches_[i];
                const bool alive = patch.age < patch.lifetime;
                const double amount = alive ? patch.age / patch.lifetime : 1;
                const auto wave = alive ? LakeSurface(patch.position) : std::nullopt;
                const Vec3 normal = wave ? wave->normal : Vec3{0, 1, 0};
                const double layFlat = std::clamp((normal.y - .25) * 2.2, 0., 1.);
                const float alpha = static_cast<float>(.54 * patch.opacity * (1 - amount) *
                                                       std::min(patch.age * 15, 1.) * layFlat);
                const double size = patch.radius * (.75 + 1.35 * amount);
                const Vec3 along{std::cos(patch.angle), 0, std::sin(patch.angle)};
                const Vec3 across{-along.z, 0, along.x};
                for (size_t c = 0; c < corners.size(); ++c) {
                    const auto uv = corners[c];
                    const Vec3 offset =
                        along * (size * (uv.x - .5)) + across * (size * .62 * (uv.y - .5));
                    auto p = alive ? patch.position + offset : Vec3{};
                    if (wave)
                        p.y = wave->displacement.y + .016 -
                              offset.Dot(normal) / std::max(.48, normal.y);
                    surfaceFoamVertices_[i * 4 + c] = {p, normal, uv, {.83f, .9f, .96f, alpha}};
                }
            }
            if (spray_.Stage(sprayVertices_) && surfaceFoam_.Stage(surfaceFoamVertices_)) {
                spray_.Publish();
                surfaceFoam_.Publish();
                sprayTick_ = tick;
                sprayFacing_ = face;
            } else if (!GetGpuDevice().IsReady()) {
                throw std::runtime_error("Could not update the surf effects");
            }
        }
        Material3D mist;
        mist.alphaMode = MaterialAlphaMode::Blend;
        mist.roughness = 1;
        renderer_.DrawMeshWithTextures(surfaceFoam_.Current(), Mat4::Identity(), mist,
                                       {.baseColor = &surfaceFoamTexture_});
        renderer_.DrawMeshWithTextures(spray_.Current(), Mat4::Identity(), mist,
                                       {.baseColor = &sprayTexture_});
    }

    void UpdateWakeVertices() {
        const Vec3 front{std::sin(swimmerYaw_), 0, std::cos(swimmerYaw_)};
        const Vec3 side{front.z, 0, -front.x};
        for (uint32 band = 0; band < WakeBands; ++band) {
            const double travel = std::fmod(time_ * .9 + double(band) / WakeBands, 1.);
            const double flow = std::clamp(swimmingSpeed_, 0., 1.);
            const double reach = .40 + travel * (.63 + flow * .59);
            for (uint32 segment = 0; segment <= WakeSegments; ++segment) {
                const double fraction = double(segment) / WakeSegments;
                const double angle = (fraction - .5) * (Tau * .88);
                const Vec3 direction = (front * std::cos(angle) + side * std::sin(angle));
                const double ripple = .072 * std::sin(5 * angle - 5 * travel + band) +
                                      .027 * std::sin(11 * angle + 3 * travel);
                const double width =
                    (.063 + .055 * travel) * (.78 + .22 * std::sin(4 * angle + band * 1.7));
                auto center =
                    swimmerPosition_ + direction * (reach + ripple) - front * (flow * travel * .95);
                const auto surface = LakeSurface(center);
                const Vec3 normal = surface ? surface->normal : Vec3{0, 1, 0};
                center.y = (surface ? surface->displacement.y : CoastalSeaLevel) + .018;
                const double breakup = std::clamp(.62 + .31 * std::sin(6 * angle + 7 * travel) +
                                                      .18 * std::sin(13 * angle - band),
                                                  .14, 1.);
                const double opacity = (1 - travel) * std::sin(fraction * std::numbers::pi) *
                                       breakup * (.16 + flow * .36) *
                                       std::clamp((normal.y - .32) * 2.1, 0., 1.);
                for (uint32 row = 0; row < WakeRows; ++row) {
                    const auto offset = direction * (width * (double(row) / (WakeRows - 1) - .5));
                    auto point = center + offset;
                    point.y -= offset.Dot(normal) / std::max(.48, normal.y);
                    const auto index = WakeRows * (band * (WakeSegments + 1) + segment) + row;
                    wakeVertices_[index] = {
                        point, normal, {}, {.74f, .86f, .90f, row == 1 ? float(opacity) : 0.f}};
                }
            }
        }
    }

    void OnRender(float) override {
        auto& gpu = GetGpuDevice();
        if (options_.benchmark && benchmarkFrames_)
            StepLake(GetFixedUpdateStep());
        const double blend = IsScripted() || paused_ ? 1. : GetRenderInterpolationAlpha();
        const double renderTime = std::max(0., time_ - (1 - blend) * GetFixedUpdateStep());
        const auto actorPosition =
            previousSwimmerPosition_ + (swimmerPosition_ - previousSwimmerPosition_) * blend;
        const double actorYaw = std::lerp(previousSwimmerYaw_, swimmerYaw_, blend);
        if (!water_.UpdateOcean(static_cast<float>(renderTime)))
            throw std::runtime_error("GPU FFT dispatch failed: " + gpu.GetLastError());
        uint32 width = 0, height = 0;
        GetWindow().GetFramebufferSize(width, height);
        const double fov = options_.view == CameraView::Swimmer ? 49 : 58;
        PerspectiveCamera camera(ToRadians(fov), double(std::max(1u, width)) / std::max(1u, height),
                                 .08, IsOpenOcean() ? CoastHorizon : 240);
        const double drift = .35 * std::sin(.11 * time_);
        camera.SetPosition({3.9 + drift, 1.53 + .09 * std::sin(.23 * time_), 12.7});
        camera.SetTarget({-4, 1.25, -47});
        if (options_.view == CameraView::Shore) {
            camera.SetPosition(IsOpenOcean() ? Vec3{-13.2, 1.85 + CoastalSeaLevel, -16.5}
                                             : Vec3{-7, 2.2, 7});
            camera.SetTarget(IsOpenOcean() ? Vec3{-9.95, .38, 7.5} : Vec3{-15, -.15, -12});
        } else if (options_.view == CameraView::Waterline) {
            camera.SetPosition({4, .48, 12.7});
            camera.SetTarget({-4, .60, -47});
        } else if (options_.view == CameraView::Swimmer) {
            const auto [dx, dy] = Input::ConsumeMouseDelta();
            if (!IsScripted() && Input::IsMouseButtonPressed(LuxMouseButton::Right)) {
                orbitYaw_ = std::remainder(orbitYaw_ - dx * .005, Tau);
                orbitPitch_ = std::clamp(orbitPitch_ + dy * .004, -.04, 1.15);
            }
            const double followHeight =
                .42 + CoastalSeaLevel + .74 * std::max(0., actorPosition.y - CoastalSeaLevel - .06);
            const Vec3 center{actorPosition.x, followHeight, actorPosition.z};
            const double shotYaw = orbitYaw_ + .0045 * sequenceFrame_;
            const auto radius = orbitDistance_ * std::cos(orbitPitch_);
            auto eye =
                center + Vec3{std::sin(shotYaw) * radius, std::sin(orbitPitch_) * orbitDistance_,
                              std::cos(shotYaw) * radius};
            const double surface = LakeSurface(eye).value_or(OceanSample{}).displacement.y;
            eye.y = std::max(eye.y, surface + .20);
            camera.SetPosition(eye);
            camera.SetTarget(center);
        }
        if (!gpu.Clear(.22f, .31f, .40f, 1))
            throw std::runtime_error("Could not clear the lakeside frame: " + gpu.GetLastError());
        std::optional<CoastalWaterFrame> coastalWater;
        if (IsOpenOcean())
            coastalWater = CoastalWaterFrame{static_cast<float>(std::fmod(renderTime, 256.)),
                                             coastalBreakerHeight_};
        renderer_.BeginScene(camera, settings_, .9f, {}, coastalWater);
        if (!IsOpenOcean()) {
            Material3D earth;
            earth.alphaMode = MaterialAlphaMode::Opaque;
            earth.roughness = .83f;
            renderer_.DrawMesh(terrain_, Mat4::Identity(), shore_);
            for (int i = 0; i < 35; ++i) {
                const double z = 2.5 - i * 2.8;
                const double x = Shore(z) + 1.3 * std::sin(2.4 * i);
                const double size = .55 + .8 * (.5 + .5 * std::sin(i * 2.283));
                renderer_.DrawMesh(rock_,
                                   Mat4::Translate({x, LakeBed(x, z), z}) *
                                       Mat4::RotateY(i * 2.399) *
                                       Mat4::Scale({size * 1.35, size, size}),
                                   earth);
            }
            for (int i = 0; i < 55; ++i) {
                const double scatter = .5 + .5 * std::sin(i * 2.399);
                const double z =
                    i < 27 ? 5 - (i * 3.8) : FarShore(i * 3.5 - 115) - 5 - scatter * 24;
                const double x = i < 27 ? Shore(z) - 5 - scatter * 13 : i * 3.5 - 115;
                const double h = 2.15 + 2.9 * scatter;
                const auto transform = Mat4::Translate({x, LakeBed(x, z), z}) *
                                       Mat4::RotateY(i * 1.89) * Mat4::Scale({.75 * h, h, .75 * h});
                renderer_.DrawMesh(trunk_, transform, earth);
                renderer_.DrawMesh(tree_, transform, earth);
            }
        }
        Material3D marker;
        marker.alphaMode = MaterialAlphaMode::Opaque;
        marker.roughness = .33f;
        for (auto entity : scene_.GetEntities()) {
            const auto& p = entity.GetComponent<TransformComponent>().position;
            const auto sample = LakeSurface(p);
            const Vec3 n = sample ? sample->normal : Vec3{0, 1, 0};
            renderer_.DrawMesh(buoy_,
                               Mat4::Translate(p) * Mat4::RotateX(std::atan2(n.z, n.y)) *
                                   Mat4::RotateZ(-std::atan2(n.x, n.y)),
                               marker);
        }
        if (swimmer_.IsLoaded())
            swimmer_.Draw(renderer_, actorPosition, actorYaw);
        auto water = WaterSurface::MakeMaterial();
        water.water->absorption = {.42, .145, .086};
        water.roughness = .055f;
        water.normalScale = .22f;
        Vec3 waterCenter;
        if (IsOpenOcean()) {
            const auto snap = [](double value) { return std::floor(value / Patch + .5) * Patch; };
            waterCenter = {snap(camera.GetPosition().x), CoastalSeaLevel,
                           snap(camera.GetPosition().z)};
            water.albedoColor = {.034f, .22f, .40f, .12f};
            water.water->absorption = {.64, .23, .15};
            water.roughness = .096f;
            water.normalScale = .34f;
            auto distant = water;
            distant.roughness = .10f;
            renderer_.DrawMesh(horizon_, Mat4::Translate(waterCenter), distant);
            water.doubleSided = true;
        }
        for (int row = FirstWaterRow; row <= (IsOpenOcean() ? LastCoastalWaterRow : 0); ++row)
            for (int column = -1; column <= 1; ++column)
                renderer_.DrawMeshWithTextures(
                    water_, Mat4::Translate(waterCenter + Vec3{column * Patch, 0, row * Patch}),
                    water, {.normal = &capillary_});
        if (swimmer_.IsLoaded()) {
            const double tick = std::floor(time_ * 30);
            if (tick != wakeTick_) {
                UpdateWakeVertices();
                if (wake_.Stage(wakeVertices_)) {
                    wake_.Publish();
                    wakeTick_ = tick;
                } else if (!gpu.IsReady()) {
                    throw std::runtime_error("Could not update the water wake");
                }
            }
            Material3D foam;
            foam.alphaMode = MaterialAlphaMode::Blend;
            foam.roughness = .82f;
            renderer_.DrawMesh(wake_.Current(), Mat4::Identity(), foam);
        }
        if (IsOpenOcean())
            DrawSurfEffects(camera, renderTime);
        renderer_.EndScene();
        if (IsCapturing() && renderer_.IsEnvironmentReady() &&
            gpu.GetFrameStats().waterReflectionPasses == 1) {
            if (!gpu.RequestFrameCapture())
                throw std::runtime_error("Could not request a lakeside capture");
            captureRequested_ = true;
        }
    }

    void OnFramePresented() override {
        const uint64 now = SDL_GetTicksNS();
        if (!IsScripted()) {
            PaceFrame(now);
            return;
        }
        auto& gpu = GetGpuDevice();
        const auto stats = gpu.GetFrameStats();
        const bool environmentReady =
            stats.waterReflectionPasses == 1 && renderer_.IsEnvironmentReady();
        if (options_.benchmark && environmentReady) {
            RecordBenchmark(stats, now);
            PaceFrame(now);
            return;
        }
        if (stats.computeDispatches == 3 + stats.volumetricFogPasses)
            ++waveUpdates_;
        ++frames_;
        const bool smokeReady =
            options_.smoke && frames_ >= 5 && waveUpdates_ >= 2 && environmentReady;
        if (captureRequested_ || smokeReady) {
            if (stats.volumetricFogPasses != 1 || gpu.GetCounters().validationErrors)
                throw std::runtime_error("Lakeside frame did not render the expected passes");
            if (stats.rayTracedPasses != unsigned(options_.rayTracing && gpu.GetInfo().rayTracing))
                throw std::runtime_error(
                    "Lakeside water ray tracing did not match the requested mode");
            if (!renderer_.GetLastAssetError().empty())
                throw std::runtime_error("Lakeside asset error: " + renderer_.GetLastAssetError());
            if (captureRequested_) {
                if (!options_.sequence.empty()) {
                    char filename[24];
                    std::snprintf(filename, sizeof(filename), "%04u.ppm", sequenceFrame_);
                    LuxSamples::WritePpmCapture(gpu, options_.sequence / filename);
                    captureRequested_ = false;
                    if (++sequenceFrame_ < options_.sequenceLength) {
                        StepLake(1.0 / 60.0);
                        return;
                    }
                } else {
                    LuxSamples::WritePpmCapture(gpu, options_.capture);
                }
                runFinished_ = true;
                std::cout << gpu.GetInfo().deviceName << " | Lakeside tick " << options_.tick
                          << " | virtual shadow pages " << stats.virtualShadowPages
                          << " | ray-traced passes " << stats.rayTracedPasses << " | AS builds "
                          << stats.accelerationStructureBuilds << " | temporal resolve "
                          << stats.temporalResolvePasses << " history "
                          << stats.temporalHistoryPasses << '\n';
            }
            Close();
        } else if (std::chrono::steady_clock::now() - start_ > std::chrono::seconds(20)) {
            throw std::runtime_error(
                "Lakeside validation timed out after " + std::to_string(frames_) + " frames and " +
                std::to_string(waveUpdates_) + " FFT updates (environment " +
                (renderer_.IsEnvironmentReady() ? "ready" : "pending") +
                ", water reflection passes " + std::to_string(stats.waterReflectionPasses) +
                ", dispatches " + std::to_string(stats.computeDispatches) + ", ray passes " +
                std::to_string(stats.rayTracedPasses) + "): " + renderer_.GetLastAssetError());
        }
    }

    void RecordBenchmark(const GpuFrameStats& stats, uint64 presented) {
        constexpr unsigned WarmupFrames = 120, Samples = 240;
        auto& gpu = GetGpuDevice();
        if (gpu.GetCounters().validationErrors || !renderer_.GetLastAssetError().empty() ||
            stats.volumetricFogPasses != 1 ||
            stats.rayTracedPasses != unsigned(options_.rayTracing && gpu.GetInfo().rayTracing))
            throw std::runtime_error("Lakeside benchmark did not render the expected frame");
        if (++benchmarkFrames_ < WarmupFrames)
            return;
        if (stats.gpuTimingFrame > lastTimingFrame_) {
            lastTimingFrame_ = stats.gpuTimingFrame;
            if (stats.gpuTimeMs > 0) {
                if (frameTimes_.empty())
                    firstTimingSample_ = std::chrono::steady_clock::now();
                else
                    presentTimes_.push_back(double(presented - lastPresented_) / 1e6);
                lastPresented_ = presented;
                frameTimes_.push_back(stats.gpuTimeMs);
                shadowTimes_.push_back(stats.gpuShadowTimeMs);
            }
        }
        if (frameTimes_.size() == Samples) {
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - firstTimingSample_)
                    .count();
            std::sort(frameTimes_.begin(), frameTimes_.end());
            std::sort(shadowTimes_.begin(), shadowTimes_.end());
            std::sort(presentTimes_.begin(), presentTimes_.end());
            uint32 width = 0, height = 0;
            GetWindow().GetFramebufferSize(width, height);
            std::cout << "Lakeside " << gpu.GetInfo().deviceName << " | " << width << 'x' << height
                      << " | " << Samples << " GPU frames | " << (Samples - 1) / seconds
                      << " frames/s | GPU frame ms p50 " << frameTimes_[Samples / 2 - 1] << " p95 "
                      << frameTimes_[Samples * 95 / 100 - 1] << " | shadow ms p50 "
                      << shadowTimes_[Samples / 2 - 1] << " p95 "
                      << shadowTimes_[Samples * 95 / 100 - 1] << " | virtual shadow pages "
                      << stats.virtualShadowPages << '/' << stats.requestedShadowPages
                      << " | draws " << stats.drawCalls << " | ray-traced passes "
                      << stats.rayTracedPasses << " | AS builds "
                      << stats.accelerationStructureBuilds << " | temporal resolve "
                      << stats.temporalResolvePasses << " history " << stats.temporalHistoryPasses
                      << " | present ms p95 " << presentTimes_[presentTimes_.size() * 95 / 100 - 1]
                      << " p99 " << presentTimes_[presentTimes_.size() * 99 / 100 - 1] << '\n';
            runFinished_ = true;
            Close();
        } else if (std::chrono::steady_clock::now() - start_ > std::chrono::seconds(60)) {
            throw std::runtime_error("Lakeside GPU timestamp benchmark timed out");
        }
    }

    void OnShutdown() override {
        renderer_.Shutdown();
        swimmer_.Release();
        wake_.Release();
        capillary_.Release();
        spray_.Release();
        sprayTexture_.Release();
        surfaceFoam_.Release();
        surfaceFoamTexture_.Release();
        horizon_.Release();
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
    uint64 lastTimingFrame_ = 0, lastPresented_ = 0, framePeriod_ = 0, frameDeadline_ = 0;
    std::vector<double> frameTimes_, shadowTimes_, presentTimes_;
    double time_ = 0, waveSampleTime_ = 0;
    float coastalBreakerHeight_ = 0;
    std::chrono::steady_clock::time_point start_, firstTimingSample_;
    Render3DSettings settings_;
    Material3D shore_;
    Renderer3D::Context renderer_;
    Scene scene_;
    OceanSpectrum ocean_;
    Swimmer swimmer_;
    static constexpr uint32 WakeBands = 3, WakeSegments = 24, WakeRows = 3;
    std::array<Vertex3D, WakeRows * WakeBands*(WakeSegments + 1)> wakeVertices_;
    FrameBufferedMesh wake_;

    struct SurfDrop {
        Vec3 position, velocity;
        double age = 0, lifetime = 0, radius = 0, opacity = 0;
    };

    struct FoamPatch {
        Vec3 position, velocity;
        double age = 0, lifetime = 0, radius = 0, angle = 0, opacity = 0;
    };

    struct BreakerSite {
        std::optional<double> tipHeight;
        Vec3 contact, drift;
        double impact = 0, lip = 0, splash = 0, foam = 0;
    };

    static constexpr size_t SprayCapacity = 512, FoamCapacity = 144;
    std::array<SurfDrop, SprayCapacity> surfDrops_{};
    std::array<FoamPatch, FoamCapacity> foamPatches_{};
    std::array<BreakerSite, 23> breakerSites_{};
    std::array<Vertex3D, SprayCapacity * 4> sprayVertices_{};
    std::array<Vertex3D, FoamCapacity * 4> surfaceFoamVertices_{};
    size_t nextDrop_ = 0, nextFoam_ = 0;
    uint32 surfSeed_ = 0xe15a38b7u;
    double sprayStep_ = 0.;
    double sprayTick_ = -1;
    Vec3 sprayFacing_;
    FrameBufferedMesh spray_, surfaceFoam_;
    Vec3 swimmerPosition_{-2.85, 0, 2.7};
    Vec3 previousSwimmerPosition_ = swimmerPosition_;
    double swimmerYaw_ = -1.36;
    double previousSwimmerYaw_ = swimmerYaw_;
    double orbitYaw_ = 0, orbitPitch_ = 0, orbitDistance_ = 0;
    float fogDensity_ = 0;
    unsigned sequenceFrame_ = 0;
    double swimmingSpeed_ = 0, wakeTick_ = -1;
    Mesh3D water_, horizon_, terrain_, rock_, tree_, trunk_, buoy_;
    GpuTexture capillary_, sprayTexture_, surfaceFoamTexture_;
};

int Run(int argc, char** argv) {
    constexpr auto usage =
        "SampleLakesideOcean [--smoke-test | --benchmark | --benchmark-paced | "
        "--capture PATH.ppm | --sequence DIRECTORY [--frames COUNT]] "
        "[--tick 0..600] [--view default|shore|waterline|swimmer] [--pitch RADIANS] "
        "[--rt on|off] [--taa on|off] [--fixed-window] [--swimmer PATH.fbx]\n";
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << usage
                  << "WASD: swim and walk. Right drag: orbit. Wheel: zoom. Home: reset camera. "
                     "V: wave camera. Space: pause. F: fog. R: water rays. "
                     "C: radiance cascades. Escape: quit.\n";
        return 0;
    }
    const auto invalidOptions = [usage] {
        std::cerr << usage;
        return 2;
    };
    Options options;
    bool explicitView = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view name(argv[index]);
        if (name == "--fixed-window") {
            options.fixedWindow = true;
            continue;
        }
        if (name == "--smoke-test" || name == "--benchmark" || name == "--benchmark-paced") {
            options.smoke |= name == "--smoke-test";
            options.benchmark |= name != "--smoke-test";
            options.pacedBenchmark |= name == "--benchmark-paced";
            continue;
        }
        if (index + 1 == argc)
            return invalidOptions();
        const std::string_view value(argv[++index]);
        if (name == "--tick") {
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), options.tick);
            if (error != std::errc{} || end != value.data() + value.size() || options.tick > 600)
                return invalidOptions();
        } else if (name == "--frames") {
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), options.sequenceLength);
            if (error != std::errc{} || end != value.data() + value.size() ||
                options.sequenceLength == 0 || options.sequenceLength > 600)
                return invalidOptions();
        } else if (name == "--pitch") {
            double pitch = 0;
            std::istringstream input{std::string(value)};
            input.imbue(std::locale::classic());
            input >> std::noskipws >> pitch;
            if (value.empty() || value.front() == '+' ||
                value.find_first_not_of("0123456789.eE+-") != std::string_view::npos ||
                input.fail() || !input.eof() || !std::isfinite(pitch) || pitch < -.04 ||
                pitch > 1.15)
                return invalidOptions();
            options.capturePitch = pitch;
        } else if (name == "--view") {
            explicitView = true;
            if (value == "default")
                options.view = CameraView::Default;
            else if (value == "shore")
                options.view = CameraView::Shore;
            else if (value == "waterline")
                options.view = CameraView::Waterline;
            else if (value == "swimmer")
                options.view = CameraView::Swimmer;
            else
                return invalidOptions();
        } else if (name == "--capture") {
            options.capture = PathFromUtf8(value);
            if (options.capture.extension() != ".ppm")
                return invalidOptions();
        } else if (name == "--sequence") {
            options.sequence = PathFromUtf8(value);
            if (options.sequence.empty())
                return invalidOptions();
        } else if (name == "--swimmer") {
            options.swimmer = PathFromUtf8(value);
            if (options.swimmer.extension() != ".fbx")
                return invalidOptions();
        } else if (name == "--rt" && (value == "on" || value == "off")) {
            options.rayTracing = value == "on";
        } else if (name == "--taa" && (value == "on" || value == "off")) {
            options.temporalAntialiasing = value == "on";
        } else {
            return invalidOptions();
        }
    }
    if (unsigned(options.smoke) + unsigned(options.benchmark) + unsigned(!options.capture.empty()) +
            unsigned(!options.sequence.empty()) >
        1)
        return invalidOptions();
    if (options.swimmer.empty()) {
        const auto bundled = PathFromUtf8(AssetPath("HaerangSwim", "swimmer.fbx"));
        if (std::filesystem::is_regular_file(bundled))
            options.swimmer = bundled;
    }
    if (!options.swimmer.empty() && !explicitView)
        options.view = CameraView::Swimmer;
    if (options.capturePitch && (options.view != CameraView::Swimmer ||
                                 (options.capture.empty() && options.sequence.empty())))
        return invalidOptions();
    if (options.sequence.empty() && options.sequenceLength != 180)
        return invalidOptions();
    if (options.view == CameraView::Swimmer && options.swimmer.empty()) {
        std::cerr << "The swimmer camera requires the bundled model or --swimmer PATH.fbx.\n";
        return 2;
    }
    LakesideDemo app(std::move(options));
    return app.Initialize() ? app.Run() : 1;
}
} // namespace

LUX_ENGINE_MAIN(Run)
