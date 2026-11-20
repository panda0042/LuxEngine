#pragma once

#include <LuxEngine/Renderer/Renderer3D/Renderer3D.hpp>
#include <cmath>

namespace LuxEngine {

inline Mat4 SceneShadowProjection(const PerspectiveCamera& camera,
                                  const Render3DSettings& settings) {
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
