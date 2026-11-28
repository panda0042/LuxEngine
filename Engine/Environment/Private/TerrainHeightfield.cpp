#include <LuxEngine/Environment/TerrainHeightfield.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace LuxEngine {
TerrainHeightfield::TerrainHeightfield(uint32 columns, uint32 rows, double spacing, Vec2 origin,
                                       std::span<const float> elevations)
    : columns_(columns), rows_(rows), spacing_(spacing), origin_(origin) {
    if (columns < 2 || rows < 2 || !std::isfinite(spacing) || spacing <= 0 ||
        !std::isfinite(origin.x) || !std::isfinite(origin.y) ||
        uint64(columns) * rows != elevations.size() ||
        !std::ranges::all_of(elevations, [](float h) { return std::isfinite(h); }))
        throw std::invalid_argument("Invalid terrain heightfield dimensions or samples");
    heights_.assign(elevations.begin(), elevations.end());
    drainage_.resize(heights_.size());
}

double TerrainHeightfield::Sample(std::span<const float> values, Vec2 p) const {
    if (!std::isfinite(p.x) || !std::isfinite(p.y))
        throw std::invalid_argument("Invalid terrain sample location");
    const double x = std::clamp((p.x - origin_.x) / spacing_, 0., double(columns_ - 1));
    const double y = std::clamp((p.y - origin_.y) / spacing_, 0., double(rows_ - 1));
    const auto i = std::min(uint32(x), columns_ - 2), j = std::min(uint32(y), rows_ - 2);
    const size_t at = size_t(j) * columns_ + i;
    return std::lerp(
        std::lerp(double(values[at]), double(values[at + 1]), x - i),
        std::lerp(double(values[at + columns_]), double(values[at + columns_ + 1]), x - i), y - j);
}

double TerrainHeightfield::Height(Vec2 p) const {
    return Sample(heights_, p);
}

double TerrainHeightfield::Drainage(Vec2 p) const {
    return Sample(drainage_, p);
}

Vec3 TerrainHeightfield::Normal(Vec2 p) const {
    return Vec3{Height({p.x - spacing_, p.y}) - Height({p.x + spacing_, p.y}), 2 * spacing_,
                Height({p.x, p.y - spacing_}) - Height({p.x, p.y + spacing_})}
        .Normalized();
}

void TerrainHeightfield::Erode(const TerrainErosionSettings& s) {
    const auto bounded = [](double x, double lo, double hi) {
        return std::isfinite(x) && x >= lo && x <= hi;
    };
    if (s.iterations > 4096 || !bounded(s.rainfall, 0, 10) || !bounded(s.evaporation, 0, 1) ||
        !bounded(s.erosionRate, 0, 1) || !bounded(s.talusSlope, 0, 10))
        throw std::invalid_argument("Invalid terrain erosion settings");
    const size_t count = heights_.size();
    std::vector<double> water(count), sediment(count), waterDelta(count), sedimentDelta(count),
        bedDelta(count);
    std::fill(drainage_.begin(), drainage_.end(), 0.f);
    for (unsigned tick = 0; tick < s.iterations; ++tick) {
        for (auto& w : water)
            w += s.rainfall;
        std::fill(waterDelta.begin(), waterDelta.end(), 0.);
        std::fill(sedimentDelta.begin(), sedimentDelta.end(), 0.);
        std::fill(bedDelta.begin(), bedDelta.end(), 0.);
        for (uint32 z = 0; z < rows_; ++z)
            for (uint32 x = 0; x < columns_; ++x) {
                const size_t i = size_t(z) * columns_ + x;
                const std::array<size_t, 4> neighbors = {
                    x ? i - 1 : i, x + 1 < columns_ ? i + 1 : i, z ? i - columns_ : i,
                    z + 1 < rows_ ? i + columns_ : i};
                double headSum = 0, steepest = 0;
                std::array<double, 4> heads{};
                for (unsigned k = 0; k < 4; ++k) {
                    const auto j = neighbors[k];
                    const double drop = double(heights_[i]) - heights_[j];
                    heads[k] = std::max(0., drop + water[i] - water[j]);
                    headSum += heads[k];
                    steepest = std::max(steepest, drop / spacing_);
                    const double slide = .10 * std::max(0., drop - s.talusSlope * spacing_);
                    bedDelta[i] -= slide;
                    bedDelta[j] += slide;
                }
                const double flow = std::min(water[i], headSum * .25);
                const double exchange = s.erosionRate * (flow * steepest * 2. - sediment[i]);
                bedDelta[i] -= exchange;
                const double suspended = sediment[i] + exchange;
                sedimentDelta[i] += exchange;
                if (flow <= 0)
                    continue;
                drainage_[i] += float(flow);
                for (unsigned k = 0; k < 4; ++k) {
                    const size_t j = neighbors[k];
                    const double fraction = flow * heads[k] / headSum;
                    const double transported = suspended * fraction / water[i];
                    waterDelta[i] -= fraction;
                    waterDelta[j] += fraction;
                    sedimentDelta[i] -= transported;
                    sedimentDelta[j] += transported;
                }
            }
        for (size_t i = 0; i < count; ++i) {
            water[i] = std::max(0., water[i] + waterDelta[i]) * (1. - s.evaporation);
            sediment[i] = std::max(0., sediment[i] + sedimentDelta[i]);
            heights_[i] += float(bedDelta[i]);
        }
    }
    for (size_t i = 0; i < count; ++i)
        heights_[i] += float(sediment[i]);
}
} // namespace LuxEngine
