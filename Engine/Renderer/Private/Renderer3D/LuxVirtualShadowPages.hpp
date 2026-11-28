#pragma once

#include "../Forge/Shaders/LuxVirtualShadowLayout.h"
#include <LuxEngine/Core/Math.hpp>
#include <algorithm>
#include <cmath>

namespace LuxEngine::Rhi {

inline uint32 VirtualShadowPageMask(const Mat4& shadowProjection, const Mat4& relativeModel,
                                    Vec3 minimum, Vec3 maximum, uint32 resolution,
                                    double normalBias, double filterTexels = 2.0) {
    // The directional projection is orthographic. Bound the projected local box using the
    // affine interval transform; unusual projective models request all pages.
    const Mat4 clip = shadowProjection * relativeModel;
    const auto& m = clip.elements;
    const Vec3 center = (minimum + maximum) * .5;
    const Vec3 extent = (maximum - minimum) * .5;
    const auto projectCenter = [&](int row) {
        return m[row] * center.x + m[row + 4] * center.y + m[row + 8] * center.z + m[row + 12];
    };
    const auto projectExtent = [&](int row) {
        return std::abs(m[row]) * extent.x + std::abs(m[row + 4]) * extent.y +
               std::abs(m[row + 8]) * extent.z;
    };
    const double w = projectCenter(3);
    if (!(w > 0) || !std::isfinite(w) || projectExtent(3) != 0)
        return LUX_VIRTUAL_SHADOW_ALL_PAGES;

    // Include the receiver normal offset, the PCF kernel radius and rounding at page edges.
    const double centerX = .5 + .5 * projectCenter(0) / w;
    const double centerY = .5 - .5 * projectCenter(1) / w;
    const auto radius = [&](int row) {
        const auto& s = shadowProjection.elements;
        const double bias = normalBias * std::hypot(s[row], s[row + 4], s[row + 8]);
        return .5 * (projectExtent(row) + bias) / w + filterTexels / resolution;
    };
    const double radiusX = radius(0), radiusY = radius(1);
    if (!std::isfinite(centerX + radiusX) || !std::isfinite(centerY + radiusY))
        return LUX_VIRTUAL_SHADOW_ALL_PAGES;
    const double minX = centerX - radiusX, maxX = centerX + radiusX;
    const double minY = centerY - radiusY, maxY = centerY + radiusY;
    if (maxX < 0 || maxY < 0 || minX > 1 || minY > 1)
        return 0;
    const auto tile = [](double uv) {
        constexpr int Grid = LUX_VIRTUAL_SHADOW_PAGE_GRID;
        return std::min(Grid - 1, static_cast<int>(std::clamp(uv, 0., 1.) * Grid));
    };
    uint32 mask = 0;
    for (int y = tile(minY); y <= tile(maxY); ++y)
        for (int x = tile(minX); x <= tile(maxX); ++x)
            mask |= 1u << (y * LUX_VIRTUAL_SHADOW_PAGE_GRID + x);
    return mask;
}

} // namespace LuxEngine::Rhi
