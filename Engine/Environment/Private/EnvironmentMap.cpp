#include <LuxEngine/Environment/EnvironmentMap.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <numbers>
#include <stb_image.h>
#include <unordered_map>

namespace LuxEngine {
namespace {
constexpr float Pi = std::numbers::pi_v<float>;
constexpr uint32 SpecularLevels = 8;
constexpr uint32 EnvironmentSamples = 64;
constexpr uint32 BrdfSize = 128;
constexpr uint32 BrdfSamples = 128;
std::mutex revisionsMutex;
std::unordered_map<std::string, uint64> revisions;
uint64 nextRevision = 1;

std::string Key(const std::string& path) {
    if (path.empty())
        return {};
    try {
        const auto source = std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
        const auto normalized =
            std::filesystem::weakly_canonical(std::filesystem::absolute(source)).generic_u8string();
        return {reinterpret_cast<const char*>(normalized.data()), normalized.size()};
    } catch (...) {
        return path;
    }
}

float RadicalInverse(uint32 bits) {
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xaaaaaaaau) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xccccccccu) >> 2);
    bits = ((bits & 0x0f0f0f0fu) << 4) | ((bits & 0xf0f0f0f0u) >> 4);
    bits = ((bits & 0x00ff00ffu) << 8) | ((bits & 0xff00ff00u) >> 8);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

struct TangentFrame {
    Vec3 tangent, bitangent, normal;

    explicit TangentFrame(Vec3 n) : normal(n) {
        const Vec3 up = std::abs(n.z) < .999f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
        tangent = up.Cross(n).Normalized();
        bitangent = n.Cross(tangent);
    }

    Vec3 Rotate(const Vec3& sample) const {
        return tangent * sample.x + bitangent * sample.y + normal * sample.z;
    }
};

Vec3 Ggx(float roughness, float u, float v) {
    const float a = roughness * roughness;
    const float cosine = std::sqrt((1 - v) / (1 + (a * a - 1) * v));
    const float sine = std::sqrt(std::max(0.f, 1 - cosine * cosine));
    return {std::cos(2 * Pi * u) * sine, std::sin(2 * Pi * u) * sine, cosine};
}

struct EnvironmentKernels {
    std::array<Vec3, EnvironmentSamples> diffuse;
    std::array<std::array<Vec3, EnvironmentSamples>, SpecularLevels - 1> specular;
    std::array<float, SpecularLevels - 1> specularWeight;
};

const EnvironmentKernels& Kernels() {
    static const auto kernels = [] {
        EnvironmentKernels result{};
        for (uint32 i = 0; i < EnvironmentSamples; ++i) {
            const float u = (i + .5f) / EnvironmentSamples;
            const float v = RadicalInverse(i);
            const float radius = std::sqrt(v);
            result.diffuse[i] = {radius * std::cos(2 * Pi * u), radius * std::sin(2 * Pi * u),
                                 std::sqrt(1 - v)};
            for (uint32 level = 1; level < SpecularLevels; ++level) {
                const auto half = Ggx(static_cast<float>(level) / (SpecularLevels - 1), u, v);
                const Vec3 reflected = half * (2 * half.z) - Vec3{0, 0, 1};
                result.specular[level - 1][i] = reflected;
                result.specularWeight[level - 1] += std::max(0., reflected.z);
            }
        }
        return result;
    }();
    return kernels;
}

const auto& BrdfPixels() {
    // The split-sum BRDF integral depends only on view angle and roughness, not the HDRI.
    static const auto pixels = [] {
        std::array<float, BrdfSize * BrdfSize * 4> image{};
        const TangentFrame frame({0, 0, 1});
        for (uint32 y = 0; y < BrdfSize; ++y) {
            const float rough = (y + .5f) / BrdfSize;
            const float alpha = rough * rough, a2 = alpha * alpha;
            std::array<Vec3, BrdfSamples> halves;
            for (uint32 i = 0; i < BrdfSamples; ++i)
                halves[i] = frame.Rotate(Ggx(rough, (i + .5f) / BrdfSamples, RadicalInverse(i)));
            for (uint32 x = 0; x < BrdfSize; ++x) {
                const float nv = (x + .5f) / BrdfSize;
                const Vec3 v{std::sqrt(1 - nv * nv), 0, nv};
                const float smithView = std::sqrt(nv * nv * (1 - a2) + a2);
                float a = 0, b = 0;
                for (const auto& half : halves) {
                    const double dot = v.Dot(half);
                    const float nl = static_cast<float>(2 * dot * half.z - v.z);
                    if (nl <= 0)
                        continue;
                    const float nh = std::max(static_cast<float>(half.z), 1e-5f);
                    const float vh = std::max(static_cast<float>(dot), 0.f);
                    const float visibility =
                        .5f /
                        std::max(nl * smithView + nv * std::sqrt(nl * nl * (1 - a2) + a2), 1e-5f);
                    const float g = 4 * nl * visibility * vh / nh;
                    const float d = 1 - vh, d2 = d * d;
                    const float f = d2 * d2 * d;
                    a += (1 - f) * g;
                    b += f * g;
                }
                const auto index = (y * BrdfSize + x) * 4;
                image[index] = a / BrdfSamples;
                image[index + 1] = b / BrdfSamples;
                image[index + 3] = 1;
            }
        }
        return image;
    }();
    return pixels;
}

Vec3 CubeDirection(uint32 face, float u, float v) {
    switch (face) {
    case 0:
        return Vec3{1, -v, -u}.Normalized();
    case 1:
        return Vec3{-1, -v, u}.Normalized();
    case 2:
        return Vec3{u, 1, v}.Normalized();
    case 3:
        return Vec3{u, -1, -v}.Normalized();
    case 4:
        return Vec3{u, -v, 1}.Normalized();
    default:
        return Vec3{-u, -v, -1}.Normalized();
    }
}

using HdrPixels = std::unique_ptr<float, decltype(&stbi_image_free)>;

bool Decode(const std::string& path, int& width, int& height, HdrPixels& pixels,
            std::string& error) {
    try {
        const auto file = std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
        auto extension = file.extension().u8string();
        for (auto& c : extension)
            if (c >= u8'A' && c <= u8'Z')
                c += u8'a' - u8'A';
        if (path.find('\0') != std::string::npos || extension != u8".hdr")
            throw std::runtime_error("Environment must be a Radiance .hdr image");
        const auto size = std::filesystem::file_size(file);
        const auto time = std::filesystem::last_write_time(file);
        if (!size || size > (64ull << 20))
            throw std::runtime_error("Environment exceeds 64 MiB");
        std::vector<unsigned char> bytes(static_cast<size_t>(size));
        std::ifstream input(file, std::ios::binary);
        if (!input.read(reinterpret_cast<char*>(bytes.data()),
                        static_cast<std::streamsize>(size)) ||
            std::filesystem::file_size(file) != size ||
            std::filesystem::last_write_time(file) != time)
            throw std::runtime_error("Environment changed while reading");
        int channels = 0;
        if (!stbi_is_hdr_from_memory(bytes.data(), static_cast<int>(size)) ||
            !stbi_info_from_memory(bytes.data(), static_cast<int>(size), &width, &height,
                                   &channels) ||
            width < 2 || height < 1 || width > 8192 || height > 4096 || width != 2 * height ||
            static_cast<uint64>(width) * height * 3 * sizeof(float) > (256ull << 20))
            throw std::runtime_error("Invalid 2:1 HDR dimensions or decoded size");
        pixels.reset(stbi_loadf_from_memory(bytes.data(), static_cast<int>(size), &width, &height,
                                            &channels, 3));
        if (!pixels)
            throw std::runtime_error("HDR decode failed");
        const float* const end = pixels.get() + static_cast<size_t>(width) * height * 3;
        for (auto* p = pixels.get(); p != end; ++p) {
            auto& v = *p;
            if (!std::isfinite(v) || v < 0)
                throw std::runtime_error("Nonfinite or negative HDR sample");
            v = std::min(v, 65504.f);
        }
        return true;
    } catch (const std::exception& e) {
        error = std::string(e.what()) + ": " + path;
        pixels.reset();
        return false;
    }
}

struct Source {
    int width = 0, height = 0;
    HdrPixels pixels{nullptr, &stbi_image_free};

    Vec3 Sample(Vec3 d) const {
        if (!pixels) {
            const float sky = std::max<double>(0.f, d.y);
            static const Vec3 sun = Vec3{-.5f, 1, .5f}.Normalized();
            const float highlight = 6 * std::pow(std::max<double>(0.f, d.Dot(sun)), 64.f);
            return Vec3{.18f + .45f * sky, .2f + .6f * sky, .25f + .9f * sky} +
                   Vec3{1, .85f, .65f} * highlight;
        }
        const float u = (std::atan2(d.z, d.x) / (2 * Pi) + .5f) * width - .5f;
        const float v = (std::acos(std::clamp<double>(d.y, -1.f, 1.f)) / Pi) * height - .5f;
        const int x = static_cast<int>(std::floor(u)), y = static_cast<int>(std::floor(v));
        const auto at = [&](int px, int py) {
            const size_t i = (static_cast<size_t>(std::clamp(py, 0, height - 1)) * width +
                              (px % width + width) % width) *
                             3;
            const float* sample = pixels.get() + i;
            return Vec3{sample[0], sample[1], sample[2]};
        };
        const float fx = u - std::floor(u), fy = v - std::floor(v);
        return (at(x, y) * (1 - fx) + at(x + 1, y) * fx) * (1 - fy) +
               (at(x, y + 1) * (1 - fx) + at(x + 1, y + 1) * fx) * fy;
    }
};

template <class Sample> EnvironmentImage Cube(uint32 size, uint32 levels, Sample sample) {
    EnvironmentImage image{size, levels, 6, {}};
    size_t count = 0;
    for (uint32 level = 0; level < levels; ++level) {
        const uint32 side = std::max(1u, size >> level);
        count += static_cast<size_t>(side) * side * 6 * 4;
    }
    image.pixels.reserve(count);
    for (uint32 level = 0; level < levels; ++level) {
        const uint32 side = std::max(1u, size >> level);
        for (uint32 face = 0; face < 6; ++face)
            for (uint32 y = 0; y < side; ++y)
                for (uint32 x = 0; x < side; ++x) {
                    const Vec3 d =
                        CubeDirection(face, 2 * (x + .5f) / side - 1, 2 * (y + .5f) / side - 1);
                    const Vec3 color = sample(d, level);
                    image.pixels.insert(image.pixels.end(),
                                        {static_cast<float>(color.x), static_cast<float>(color.y),
                                         static_cast<float>(color.z), 1});
                }
    }
    return image;
}
} // namespace

std::shared_ptr<EnvironmentData> EnvironmentData::Neutral() {
    auto data = std::make_shared<EnvironmentData>();
    data->sky = Cube(1, 1, [](Vec3, uint32) { return Vec3{1, 1, 1}; });
    data->diffuse = data->sky;
    data->specular = data->sky;
    data->brdf = {1, 1, 1, {1, 0, 0, 1}};
    return data;
}

std::shared_ptr<EnvironmentData> EnvironmentData::Prepare(const std::string& path) {
    const auto start = std::chrono::steady_clock::now();
    auto data = std::make_shared<EnvironmentData>();
    try {
        Source source;
        if (!path.empty() && !Decode(path, source.width, source.height, source.pixels, data->error))
            return data;
        data->sky = Cube(256, 1, [&](Vec3 n, uint32) { return source.Sample(n); });
        const auto& kernels = Kernels();
        data->diffuse = Cube(32, 1, [&](Vec3 n, uint32) {
            const TangentFrame frame(n);
            Vec3 sum{};
            for (const auto& direction : kernels.diffuse)
                sum += source.Sample(frame.Rotate(direction));
            return sum * (1.f / EnvironmentSamples);
        });
        data->specular = Cube(128, SpecularLevels, [&](Vec3 n, uint32 level) {
            if (!level)
                return source.Sample(n);
            const TangentFrame frame(n);
            Vec3 sum{};
            for (const auto& direction : kernels.specular[level - 1])
                if (direction.z > 0)
                    sum += source.Sample(frame.Rotate(direction)) * direction.z;
            return sum * (1 / std::max(kernels.specularWeight[level - 1], 1e-5f));
        });
        const auto& brdf = BrdfPixels();
        data->brdf = {BrdfSize, 1, 1, {brdf.begin(), brdf.end()}};
        const size_t bytes = (data->sky.pixels.size() + data->diffuse.pixels.size() +
                              data->specular.pixels.size() + data->brdf.pixels.size()) *
                             sizeof(float);
        if (bytes > (128ull << 20))
            throw std::runtime_error("Environment prefilter budget exceeded");
    } catch (const std::exception& e) {
        data = std::make_shared<EnvironmentData>();
        data->error = e.what();
    }
    data->preparationMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return data;
}

bool EnvironmentData::ValidateFile(const std::string& path, std::string& error) {
    int w = 0, h = 0;
    HdrPixels pixels{nullptr, &stbi_image_free};
    error.clear();
    return Decode(path, w, h, pixels, error);
}

uint64 EnvironmentData::Revision(const std::string& path) {
    std::lock_guard lock(revisionsMutex);
    auto it = revisions.find(Key(path));
    return it == revisions.end() ? 0 : it->second;
}

bool EnvironmentData::Reload(const std::string& path, std::string& error) {
    if (!path.empty() && !ValidateFile(path, error))
        return false;
    std::lock_guard lock(revisionsMutex);
    if (revisions.size() >= 128 && !revisions.contains(Key(path)))
        revisions.clear();
    revisions[Key(path)] = nextRevision++;
    return true;
}
} // namespace LuxEngine
