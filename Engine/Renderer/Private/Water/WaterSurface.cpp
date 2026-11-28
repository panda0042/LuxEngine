#include <LuxEngine/Renderer/Water/WaterSurface.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace LuxEngine {
namespace {
constexpr double Tau = 2 * std::numbers::pi;
constexpr double LocalLimit = 1e9;

bool Bounded(double value, double limit) {
    return std::isfinite(value) && std::abs(value) <= limit;
}

Vec2 Phase(double radians) {
    return {std::sin(radians), std::cos(radians)};
}
} // namespace

bool WaterSurface::Configure(const WaterSurfaceDesc& desc, std::span<const WaterWave> waves,
                             std::span<const double> rowCenters) {
    if (!Bounded(desc.size.x, 1e6) || !Bounded(desc.size.y, 1e6) || desc.size.x < 1e-3 ||
        desc.size.y < 1e-3 || !Bounded(desc.level, LocalLimit) || desc.columns == 0 ||
        desc.columns > MaxSegments || desc.rows == 0 || desc.rows > MaxSegments ||
        waves.size() > MaxWaves || (!rowCenters.empty() && rowCenters.size() != desc.rows + 1))
        return false;
    for (double center : rowCenters)
        if (!Bounded(center, 1e6))
            return false;

    WaterSurface prepared;
    prepared.level_ = desc.level;
    prepared.size_ = desc.size;
    prepared.rowCenters_.assign(rowCenters.begin(), rowCenters.end());
    prepared.waveCount_ = waves.size();
    double maxSlope = 0;
    for (size_t i = 0; i < waves.size(); ++i) {
        const auto& wave = waves[i];
        const double length = std::hypot(wave.direction.x, wave.direction.y);
        if (!std::isfinite(length) || length <= 1e-12 || !Bounded(wave.amplitude, 1e4) ||
            wave.amplitude < 0 || !Bounded(wave.wavelength, 1e6) || wave.wavelength < 1e-3 ||
            !Bounded(wave.speed, 1e4) || !std::isfinite(wave.phase) ||
            !Bounded(wave.steepness, 1) || wave.steepness < 0)
            return false;
        const double frequency = Tau / wave.wavelength;
        const Vec2 direction{wave.direction.x / length, wave.direction.y / length};
        const double horizontal = wave.amplitude * wave.steepness;
        maxSlope += horizontal * frequency;
        auto& target = prepared.waves_[i];
        target = {direction * frequency,
                  direction * horizontal,
                  wave.amplitude,
                  -frequency * wave.speed,
                  wave.speed == 0 ? 0 : wave.wavelength / std::abs(wave.speed),
                  std::remainder(wave.phase, Tau)};
    }
    // Bound the maximum horizontal compression. This keeps the surface from folding
    // and makes inverse mapping to a unique support height a contraction.
    if (maxSlope > .8)
        return false;

    const uint32 stride = desc.columns + 1;
    const auto vertexCount = static_cast<size_t>(stride) * (desc.rows + 1);
    prepared.vertices_.reserve(vertexCount);
    prepared.grid_.resize(vertexCount);
    prepared.indices_.reserve(static_cast<size_t>(desc.columns) * desc.rows * 6);
    for (uint32 row = 0; row <= desc.rows; ++row) {
        const double v = static_cast<double>(row) / desc.rows;
        const double center = rowCenters.empty() ? 0 : rowCenters[row];
        const double z = (v - .5) * desc.size.y;
        for (uint32 column = 0; column <= desc.columns; ++column) {
            const double u = static_cast<double>(column) / desc.columns;
            const double x = center + (u - .5) * desc.size.x;
            auto& grid = prepared.grid_[prepared.vertices_.size()];
            grid.planar = {x, z};
            for (size_t i = 0; i < waves.size(); ++i) {
                const auto& wave = prepared.waves_[i];
                grid.phases[i] = Phase(wave.waveVector.x * x + wave.waveVector.y * z + wave.phase);
            }
            prepared.vertices_.push_back({{x, desc.level, z}, {0, 1, 0}, {u, v}});
            if (row < desc.rows && column < desc.columns) {
                const uint32 top = row * stride + column;
                const uint32 bottom = top + stride;
                for (uint32 index : {top, bottom, bottom + 1, top, bottom + 1, top + 1})
                    prepared.indices_.push_back(index);
            }
        }
    }
    prepared.SetTime(0);
    *this = std::move(prepared);
    return true;
}

bool WaterSurface::SetTime(double seconds) {
    if (vertices_.empty() || !std::isfinite(seconds))
        return false;
    if (time_ && *time_ == seconds && !disturbancesChanged_)
        return true;
    // Only the temporal part needs trigonometry each frame. Precomputed spatial
    // phases let vertices reuse the same factors via the angle addition identities.
    bool changed = !time_ || disturbancesChanged_ || !disturbances_.empty();
    disturbancesChanged_ = false;
    std::erase_if(disturbances_,
                  [&](const auto& impulse) { return seconds - impulse.start > 12.; });
    for (size_t i = 0; i < waveCount_; ++i) {
        const auto& wave = waves_[i];
        const double angle =
            wave.period > 0 ? std::remainder(seconds, wave.period) * wave.angularVelocity : 0;
        const auto phase = Phase(angle);
        changed |= wave.amplitude != 0 && (phase.x != temporal_[i].x || phase.y != temporal_[i].y);
        temporal_[i] = phase;
    }
    time_ = seconds;
    if (!changed)
        return true;
    for (size_t i = 0; i < vertices_.size(); ++i) {
        const auto& point = grid_[i];
        const auto value = Evaluate(point.phases, point.planar);
        const auto location = point.planar + value.displacement;
        auto& vertex = vertices_[i];
        vertex.position = {location.x, value.sample.height, location.y};
        vertex.normal = value.sample.normal;
        vertex.color.w = static_cast<float>(1 - value.sample.foam);
    }
    gpuDirty_ = true;
    return true;
}

bool WaterSurface::AddDisturbance(Vec2 center, double start, double amplitude, double wavelength) {
    if (vertices_.empty() || !Bounded(center.x, LocalLimit) || !Bounded(center.y, LocalLimit) ||
        !std::isfinite(start) || !Bounded(amplitude, 10) || !std::isfinite(wavelength) ||
        wavelength < .05 || wavelength > 1000)
        return false;
    if (disturbances_.size() >= 32)
        disturbances_.erase(disturbances_.begin());
    disturbances_.push_back({center, start, amplitude, wavelength});
    disturbancesChanged_ = true;
    return true;
}

WaterSurface::Evaluation WaterSurface::Evaluate(const std::array<Vec2, MaxWaves>& spatial,
                                                Vec2 position) const {
    Evaluation value;
    value.sample.height = level_;
    Vec3 tangentX{1, 0, 0}, tangentZ{0, 0, 1};
    for (size_t i = 0; i < waveCount_; ++i) {
        const auto& wave = waves_[i];
        const auto& s = spatial[i];
        const auto& t = temporal_[i];
        const double sine = s.x * t.y + s.y * t.x;
        const double cosine = s.y * t.y - s.x * t.x;
        value.displacement += wave.horizontalAmplitude * cosine;
        value.sample.height += wave.amplitude * sine;
        const Vec3 slope{-wave.horizontalAmplitude.x * sine, wave.amplitude * cosine,
                         -wave.horizontalAmplitude.y * sine};
        tangentX += slope * wave.waveVector.x;
        tangentZ += slope * wave.waveVector.y;
        value.sample.velocity += slope * wave.angularVelocity;
    }
    for (const auto& pulse : disturbances_) {
        const double age = time_.value_or(pulse.start) - pulse.start;
        if (age < 0 || age >= 12.)
            continue;
        const Vec2 offset = position + value.displacement - pulse.center;
        const double radius = std::hypot(offset.x, offset.y);
        const double k = Tau / pulse.wavelength, speed = std::sqrt(9.81 / k);
        const double u = (radius - speed * age) / pulse.wavelength;
        if (std::abs(u) > 3.)
            continue;
        const double damping =
            std::exp(-2. * u * u - .44 * age) / std::sqrt(1. + radius / pulse.wavelength);
        const double phase = Tau * u;
        const double height = pulse.amplitude * damping * std::cos(phase);
        const double profile = pulse.amplitude * damping *
                               (-4. * u * std::cos(phase) - Tau * std::sin(phase)) /
                               pulse.wavelength;
        const double radial = profile - .5 * height / (pulse.wavelength + radius);
        tangentX.y += radial * offset.x / std::max(radius, .001);
        tangentZ.y += radial * offset.y / std::max(radius, .001);
        value.sample.height += height;
        value.sample.velocity.y += -speed * profile - .44 * height;
    }
    const Vec3 normal = tangentZ.Cross(tangentX);
    value.sample.jacobian = normal.y;
    value.sample.normal = normal.Normalized();
    const double crest = std::clamp((.96 - value.sample.jacobian) / .42, 0.0, 1.0);
    value.sample.foam = crest * crest * (3 - 2 * crest);
    return value;
}

std::optional<WaterSample> WaterSurface::Sample(double x, double z) const {
    if (vertices_.empty() || !Bounded(x, LocalLimit) || !Bounded(z, LocalLimit))
        return std::nullopt;
    const Vec2 target{x, z};
    Vec2 planar = target;
    std::array<Vec2, MaxWaves> spatial{};
    const double tolerance = 1e-12 * std::max({1.0, std::abs(x), std::abs(z)});
    for (unsigned i = 0; i < 192; ++i) {
        for (size_t j = 0; j < waveCount_; ++j) {
            const auto& wave = waves_[j];
            spatial[j] =
                Phase(wave.waveVector.x * planar.x + wave.waveVector.y * planar.y + wave.phase);
        }
        const auto value = Evaluate(spatial, planar);
        // Solve displaced XZ = rest XZ + horizontal displacement for the rest point.
        const Vec2 next = target - value.displacement;
        if (std::abs(next.x - planar.x) <= tolerance && std::abs(next.y - planar.y) <= tolerance) {
            // A residual bounds the position error by 1/(1 - .8); include that
            // error when a query lands at the floating-point mesh boundary.
            const double edgeTolerance = tolerance * 5;
            if (std::abs(planar.y) > size_.y * .5 + edgeTolerance)
                return std::nullopt;
            double center = 0;
            if (!rowCenters_.empty()) {
                const double v = std::clamp(planar.y / size_.y + .5, 0.0, 1.0);
                const double row = v * (rowCenters_.size() - 1);
                const auto lower = std::min(static_cast<size_t>(row), rowCenters_.size() - 2);
                center = std::lerp(rowCenters_[lower], rowCenters_[lower + 1], row - lower);
            }
            if (std::abs(planar.x - center) > size_.x * .5 + edgeTolerance)
                return std::nullopt;
            return value.sample;
        }
        planar = next;
    }
    return std::nullopt;
}

std::span<const Vertex3D> WaterSurface::GetVertices() const {
    return vertices_;
}

std::span<const uint32> WaterSurface::GetIndices() const {
    return indices_;
}

bool WaterSurface::SyncGpu(GpuDevice& device) {
    if (vertices_.empty())
        return false;
    if (meshDevice_ == &device && mesh_.IsValid()) {
        if (gpuDirty_ && !mesh_.UpdateVertices(vertices_))
            return false;
    } else {
        Mesh3D uploaded;
        if (!uploaded.Upload(&device, vertices_, indices_))
            return false;
        mesh_ = std::move(uploaded);
        meshDevice_ = &device;
    }
    gpuDirty_ = false;
    return true;
}

void WaterSurface::ReleaseGpu() {
    mesh_.Release();
    meshDevice_ = nullptr;
    gpuDirty_ = true;
}

const Mesh3D& WaterSurface::GetMesh() const {
    return mesh_;
}

Material3D WaterSurface::MakeMaterial() {
    Material3D material;
    material.albedoColor = {.018f, .058f, .075f, 1};
    material.metallic = 0;
    material.roughness = .075f;
    material.alphaMode = MaterialAlphaMode::Opaque;
    material.water = WaterMaterial3D{};
    material.doubleSided = false;
    return material;
}

} // namespace LuxEngine
