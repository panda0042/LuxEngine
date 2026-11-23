#include <LuxEngine/Renderer/Water/OceanSpectrum.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <numbers>
#include <random>
#include <vector>

namespace LuxEngine {
namespace {

using Complex = std::complex<double>;
constexpr Complex I{0, 1};
constexpr double Pi = std::numbers::pi;
constexpr double Tau = 2 * Pi;
constexpr double Gravity = 9.81;

struct WaveMode {
    Complex seed;
    Vec2 choppyDirection;
    uint32 frequency = 0;
};

// A complex inverse transform can yield two real fields. Each input field has
// Hermitian symmetry, so the complex result's real and imaginary parts separate them.
using WaveFields = std::array<Complex, 3>;

struct DisplacementSlope {
    Vec3 x, z;
};

bool InRange(double value, double min, double max) {
    return value >= min && value <= max;
}

bool Finite(Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

double Wrap(double value, double period) {
    double result = std::fmod(value, period);
    return result < 0 ? result + period : result;
}

Vec3 UpwardNormal(const Vec3& direction) {
    const double length = std::hypot(direction.x, direction.y, direction.z);
    if (length <= 1e-10)
        return {0, 1, 0};
    return direction * (direction.y < 0 ? -1 / length : 1 / length);
}

bool Valid(const OceanSpectrumSettings& s) {
    return s.resolution >= 8 && s.resolution <= 256 && std::has_single_bit(s.resolution) &&
           InRange(s.patchLength, 1, 10000) && InRange(s.windSpeed, 0, 80) &&
           InRange(s.spectrumAmplitude, 0, .04) && InRange(s.choppiness, 0, 4) &&
           InRange(s.foamThreshold, 0, 1) && InRange(s.foamSoftness, .001, 1) &&
           InRange(std::hypot(s.windDirection.x, s.windDirection.y), 1e-6, 1e6);
}

} // namespace

struct OceanSpectrum::State {
    explicit State(const OceanSpectrumSettings& config)
        : settings(config), n(settings.resolution), mask(n - 1), spacing(settings.patchLength / n),
          modes(n * n), angularFrequencies(n * n / 2 + 1), phases(angularFrequencies.size()),
          roots(n), bitReverse(n), fields(n * n), column(n), surface(n * n) {
        for (uint32 i = 0; i < n; ++i) {
            roots[i] = std::polar(1.0, Tau * i / n);
            for (uint32 bit = 1, value = i; bit < n; bit <<= 1, value >>= 1)
                bitReverse[i] = (bitReverse[i] << 1) | (value & 1);
        }

        if (settings.windSpeed == 0 || settings.spectrumAmplitude == 0)
            return;
        const double step = Tau / settings.patchLength;
        const Vec2 wind = settings.windDirection *
                          (1 / std::hypot(settings.windDirection.x, settings.windDirection.y));
        const double windLength = settings.windSpeed * settings.windSpeed / Gravity;
        const double damping = .1 * spacing;
        std::mt19937 random(settings.seed);
        const auto uniform = [&]() { return (random() + .5) / 4294967296.0; };
        const auto waveNumber = [this](uint32 i) {
            return i <= n / 2 ? static_cast<int>(i) : static_cast<int>(i) - static_cast<int>(n);
        };
        for (uint32 z = 0; z < n; ++z) {
            const int iz = waveNumber(z);
            for (uint32 x = 0; x < n; ++x) {
                const int ix = waveNumber(x);
                const auto radiusSquared = static_cast<uint32>(ix * ix + iz * iz);
                if (!radiusSquared)
                    continue;
                const double magnitude = step * std::sqrt(radiusSquared);
                angularFrequencies[radiusSquared] = std::sqrt(Gravity * magnitude);
                const Vec2 direction{step * ix / magnitude, step * iz / magnitude};
                const auto alongWind = direction.x * wind.x + direction.y * wind.y;
                const double largeWave = magnitude * windLength;
                const double smallWave = magnitude * damping;
                const double spectrum =
                    settings.spectrumAmplitude * alongWind * alongWind *
                    std::exp(-1 / (largeWave * largeWave) - smallWave * smallWave) /
                    std::pow(magnitude, 4) * (alongWind < 0 ? .07 : 1);
                const double radius = std::sqrt(-std::log(uniform()) * spectrum) * step;
                auto& mode = modes[Index(x, z)];
                mode.seed = std::polar(radius, Tau * uniform());
                mode.frequency = radiusSquared;
                // The Nyquist coordinate mirrors onto itself. A derivative multiplier on
                // that axis would break Hermitian symmetry and leak into the packed field.
                mode.choppyDirection = {x == n / 2 ? 0 : direction.x * settings.choppiness,
                                        z == n / 2 ? 0 : direction.y * settings.choppiness};
            }
        }
    }

    size_t Index(uint32 x, uint32 z) const {
        return static_cast<size_t>(z & mask) * n + (x & mask);
    }

    void Transform(std::span<WaveFields> line) const {
        for (uint32 i = 0; i < n; ++i)
            if (i < bitReverse[i])
                std::swap(line[i], line[bitReverse[i]]);
        for (uint32 length = 2; length <= n; length <<= 1) {
            const uint32 half = length / 2, stride = n / length;
            for (uint32 group = 0; group < n; group += length)
                for (uint32 i = 0; i < half; ++i) {
                    auto& a = line[group + i];
                    auto& b = line[group + i + half];
                    const auto root = roots[i * stride];
                    for (size_t field = 0; field < a.size(); ++field) {
                        const Complex rotated = b[field] * root;
                        b[field] = a[field] - rotated;
                        a[field] += rotated;
                    }
                }
        }
    }

    void Evolve(double time) {
        if (settings.windSpeed == 0 || settings.spectrumAmplitude == 0)
            return;
        for (size_t i = 0; i < angularFrequencies.size(); ++i) {
            const double frequency = angularFrequencies[i];
            if (frequency == 0)
                continue;
            const double period = Tau / frequency;
            phases[i] = std::polar(1.0, std::remainder(time, period) * frequency);
        }
        for (uint32 z = 0; z < n; ++z)
            for (uint32 x = 0; x < n; ++x) {
                const size_t index = Index(x, z);
                const auto& mode = modes[index];
                const auto phase = phases[mode.frequency];
                const auto forward = mode.seed * phase;
                const auto backward = std::conj(modes[Index(n - x, n - z)].seed * phase);
                const auto height = forward + backward;
                const auto heightRate =
                    I * angularFrequencies[mode.frequency] * (forward - backward);
                const auto displacementX = I * mode.choppyDirection.x * height;
                const auto displacementZ = I * mode.choppyDirection.y * height;
                const auto velocityX = I * mode.choppyDirection.x * heightRate;
                const auto velocityZ = I * mode.choppyDirection.y * heightRate;
                // Coefficients carry the frequency-domain sample area, so the inverse
                // sum is unnormalized and amplitude does not shrink with grid size.
                fields[index] = {height + I * displacementX, displacementZ + I * heightRate,
                                 velocityX + I * velocityZ};
            }
        for (uint32 z = 0; z < n; ++z)
            Transform(std::span(fields).subspan(Index(0, z), n));
        for (uint32 x = 0; x < n; ++x) {
            for (uint32 z = 0; z < n; ++z)
                column[z] = fields[Index(x, z)];
            Transform(column);
            for (uint32 z = 0; z < n; ++z)
                fields[Index(x, z)] = column[z];
        }

        for (size_t i = 0; i < fields.size(); ++i) {
            const auto& value = fields[i];
            surface[i].displacement = {value[0].imag(), value[0].real(), value[1].real()};
            surface[i].velocity = {value[2].real(), value[1].imag(), value[2].imag()};
        }
        for (uint32 z = 0; z < n; ++z)
            for (uint32 x = 0; x < n; ++x) {
                const Vec3 dx = (surface[Index(x + 1, z)].displacement -
                                 surface[Index(x - 1, z)].displacement) *
                                (.5 / spacing);
                const Vec3 dz = (surface[Index(x, z + 1)].displacement -
                                 surface[Index(x, z - 1)].displacement) *
                                (.5 / spacing);
                auto& sample = surface[Index(x, z)];
                sample.jacobian = (1 + dx.x) * (1 + dz.z) - dz.x * dx.z;
                sample.foam = std::clamp(
                    (settings.foamThreshold - sample.jacobian) / settings.foamSoftness, 0.0, 1.0);
                sample.normal = UpwardNormal((dz + Vec3{0, 0, 1}).Cross(dx + Vec3{1, 0, 0}));
            }
    }

    OceanSample At(Vec2 point, DisplacementSlope* slope = nullptr) const {
        const double x = Wrap(point.x, settings.patchLength) / spacing;
        const double z = Wrap(point.y, settings.patchLength) / spacing;
        const auto ix = static_cast<uint32>(x), iz = static_cast<uint32>(z);
        const double u = x - ix, v = z - iz;
        const auto& a = surface[Index(ix, iz)];
        const auto& b = surface[Index(ix + 1, iz)];
        const auto& c = surface[Index(ix, iz + 1)];
        const auto& d = surface[Index(ix + 1, iz + 1)];
        if (slope) {
            slope->x = ((b.displacement - a.displacement) * (1 - v) +
                        (d.displacement - c.displacement) * v) *
                       (1 / spacing);
            slope->z = ((c.displacement - a.displacement) * (1 - u) +
                        (d.displacement - b.displacement) * u) *
                       (1 / spacing);
        }
        const auto mix = [&](auto channel) {
            return (a.*channel * (1 - u) + b.*channel * u) * (1 - v) +
                   (c.*channel * (1 - u) + d.*channel * u) * v;
        };
        return {mix(&OceanSample::displacement), UpwardNormal(mix(&OceanSample::normal)),
                mix(&OceanSample::velocity), mix(&OceanSample::jacobian), mix(&OceanSample::foam)};
    }

    OceanSpectrumSettings settings;
    uint32 n, mask;
    double spacing;
    std::vector<WaveMode> modes;
    std::vector<double> angularFrequencies;
    std::vector<Complex> phases, roots;
    std::vector<uint32> bitReverse;
    std::vector<WaveFields> fields, column;
    std::vector<OceanSample> surface;
};

OceanSpectrum::OceanSpectrum() = default;
OceanSpectrum::~OceanSpectrum() = default;
OceanSpectrum::OceanSpectrum(OceanSpectrum&&) noexcept = default;
OceanSpectrum& OceanSpectrum::operator=(OceanSpectrum&&) noexcept = default;

bool OceanSpectrum::Initialize(const OceanSpectrumSettings& settings) {
    if (!Valid(settings))
        return false;
    auto candidate = std::make_unique<State>(settings);
    candidate->Evolve(0);
    state_ = std::move(candidate);
    return true;
}

bool OceanSpectrum::Update(double timeSeconds) {
    if (!state_ || !std::isfinite(timeSeconds))
        return false;
    state_->Evolve(timeSeconds);
    return true;
}

std::span<const OceanSample> OceanSpectrum::Grid() const {
    return state_ ? std::span<const OceanSample>(state_->surface) : std::span<const OceanSample>{};
}

std::optional<OceanSample> OceanSpectrum::Sample(Vec2 localXZ) const {
    if (!state_ || !std::isfinite(localXZ.x) || !std::isfinite(localXZ.y))
        return std::nullopt;
    return state_->At(localXZ);
}

std::optional<OceanSample> OceanSpectrum::SampleAtSurface(Vec2 localXZ) const {
    if (!state_ || !std::isfinite(localXZ.x) || !std::isfinite(localXZ.y))
        return std::nullopt;
    const double period = state_->settings.patchLength;
    const Vec2 target{Wrap(localXZ.x, period), Wrap(localXZ.y, period)};
    Vec2 point = target;
    DisplacementSlope slope;
    auto best = state_->At(point, &slope);
    Vec2 error{best.displacement.x, best.displacement.z};
    double distance = std::hypot(error.x, error.y);
    for (int i = 0; i < 8 && distance > state_->spacing * 1e-4; ++i) {
        const double determinant = (1 + slope.x.x) * (1 + slope.z.z) - slope.x.z * slope.z.x;
        Vec2 adjustment = error;
        if (std::abs(determinant) > 1e-6)
            adjustment = Vec2{(1 + slope.z.z) * error.x - slope.z.x * error.y,
                              (1 + slope.x.x) * error.y - slope.x.z * error.x} *
                         (1 / determinant);
        const double length = std::hypot(adjustment.x, adjustment.y);
        if (length > 4 * state_->spacing)
            adjustment = adjustment * (4 * state_->spacing / length);
        bool improved = false;
        for (int backtrack = 0; backtrack < 4; ++backtrack) {
            const Vec2 next = point - adjustment;
            DisplacementSlope nextSlope;
            const auto candidate = state_->At(next, &nextSlope);
            const Vec2 nextError =
                next + Vec2{candidate.displacement.x, candidate.displacement.z} - target;
            const double nextDistance = std::hypot(nextError.x, nextError.y);
            if (nextDistance < distance) {
                point = next;
                best = candidate;
                slope = nextSlope;
                error = nextError;
                distance = nextDistance;
                improved = true;
                break;
            }
            adjustment = adjustment * .5;
        }
        if (!improved)
            break;
    }
    return best;
}

Vec3 OceanBuoyancyImpulse(const OceanSample& water, double bodyHeight, Vec3 bodyVelocity,
                          double bodyMass, double dt, const OceanBuoyancySettings& s) {
    if (!std::isfinite(water.displacement.y) || !Finite(water.velocity) ||
        !std::isfinite(bodyHeight) || !Finite(bodyVelocity) || !InRange(bodyMass, 1e-6, 1e9) ||
        !InRange(dt, 0, .25) || !InRange(s.radius, 1e-4, 50) ||
        !InRange(s.fluidDensity, 0, 100000) || !InRange(s.gravity, 0, 1000) ||
        !InRange(s.drag, 0, 1000))
        return {};
    const double depth = std::clamp(water.displacement.y - bodyHeight + s.radius, 0., 2 * s.radius);
    const double submergedVolume = Pi * depth * depth * (s.radius - depth / 3);
    const double fullVolume = 4.0 / 3.0 * Pi * s.radius * s.radius * s.radius;
    const double dragFraction = -std::expm1(-s.drag * submergedVolume / fullVolume * dt);
    const Vec3 drag = (water.velocity - bodyVelocity) * (bodyMass * dragFraction);
    const Vec3 impulse = drag + Vec3{0, s.fluidDensity * s.gravity * submergedVolume * dt, 0};
    return Finite(impulse) ? impulse : Vec3{};
}

} // namespace LuxEngine
