#pragma once

#include <LuxEngine/Renderer/Renderer3D/Renderer3D.hpp>
#include <algorithm>
#include <cmath>

namespace LuxEngine {

struct ShadowCascades {
    Mat4 projection;
    // XY crop of the shared outer light volume and view-space far split.
    std::array<std::array<float, 4>, 4> crops{};
};

inline ShadowCascades FitShadowCascades(const PerspectiveCamera& camera,
                                        const Render3DSettings& settings) {
    const auto& p = camera.GetProjectionMatrix().elements;
    const double end = settings.shadowRange;
    const double nearZ = std::clamp(p[14] / p[10], .001, end * .99);
    const auto eye = camera.GetPosition();
    const auto forward = (camera.GetTarget() - eye).Normalized();
    const auto sun = settings.sun.direction.Normalized();
    auto basis = Mat4::LookAt({}, sun, std::abs(sun.y) > .99 ? Vec3{0, 0, 1} : Vec3{0, 1, 0});
    const Vec3 side{basis.elements[0], basis.elements[4], basis.elements[8]};
    const Vec3 up{basis.elements[1], basis.elements[5], basis.elements[9]};
    const double diagonal2 = 1.0 / (p[0] * p[0]) + 1.0 / (p[5] * p[5]);
    ShadowCascades out{};
    std::array<double, 4> radii{}, centerX{}, centerY{};
    double previous = nearZ, overlapStart = nearZ;
    for (unsigned i = 0; i < 4; ++i) {
        const double t = (i + 1) / 4.;
        const double split =
            std::lerp(std::lerp(nearZ, end, t), nearZ * std::pow(end / nearZ, t), .65);
        // The outer volume also encloses all receivers for fog and offscreen caster culling.
        const double start = i == 3 ? nearZ : overlapStart;
        const double half = .5 * (split - start);
        const double radius =
            std::ceil(std::sqrt(half * half + split * split * diagonal2) * 16.) / 16.;
        const double texel = 2. * radius / settings.shadowResolution;
        const auto center = eye + forward * (start + half);
        centerX[i] = std::round(center.Dot(side) / texel) * texel;
        centerY[i] = std::round(center.Dot(up) / texel) * texel;
        radii[i] = radius;
        out.crops[i][3] = float(split);
        overlapStart = std::lerp(previous, split, .85);
        previous = split;
    }
    for (unsigned i = 0; i < 4; ++i) {
        out.crops[i][0] = float(radii[3] / radii[i]);
        out.crops[i][1] = float((centerX[3] - centerX[i]) / radii[i]);
        out.crops[i][2] = float((centerY[3] - centerY[i]) / radii[i]);
    }
    // Work in camera-relative space. Snap the light basis in world space before converting to
    // float.
    basis.elements[12] = eye.Dot(side) - centerX[3];
    basis.elements[13] = eye.Dot(up) - centerY[3];
    basis.elements[14] = .5 * (nearZ + end) * forward.Dot(sun) - 2. * radii[3];
    const double radius = radii[3];
    out.projection = Mat4::Orthographic(-radius, radius, -radius, radius, .1, 4 * radius) * basis;
    return out;
}

inline Mat4 SceneShadowProjection(const PerspectiveCamera& camera,
                                  const Render3DSettings& settings) {
    if (settings.cascadedShadows)
        return FitShadowCascades(camera, settings).projection;
    const auto direction = settings.sun.direction.Normalized();
    const auto target = camera.GetTarget();
    auto lightView = Mat4::LookAt(target - direction * (settings.shadowRange * 2), target,
                                  std::abs(direction.y) > .99 ? Vec3{0, 0, 1} : Vec3{0, 1, 0});
    const double texel = settings.shadowRange / settings.shadowResolution;
    lightView.elements[12] = std::round(lightView.elements[12] / texel) * texel;
    lightView.elements[13] = std::round(lightView.elements[13] / texel) * texel;
    const double half = settings.shadowRange * .5;
    return Mat4::Orthographic(-half, half, -half, half, .1, settings.shadowRange * 4) * lightView *
           Mat4::Translate(camera.GetPosition());
}

} // namespace LuxEngine
