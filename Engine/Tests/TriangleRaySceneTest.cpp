#include <LuxEngine/Renderer/Lighting/TriangleRayScene.hpp>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace LuxEngine;

void Check(bool condition, const char* reason) {
    if (!condition)
        throw std::runtime_error(reason);
}

bool Near(double a, double b) {
    return std::abs(a - b) < 1e-8;
}

void Plane(std::vector<RayTriangle>& geometry, Vec3 center, Vec3 u, Vec3 v, uint32 material = 0) {
    const auto a = center - u - v;
    const auto b = center + u - v;
    const auto c = center + u + v;
    const auto d = center - u + v;
    geometry.push_back({a, b, c, material});
    geometry.push_back({a, c, d, material});
}

void Intersection() {
    constexpr double offset = 1e12;
    std::vector<RayTriangle> triangles;
    for (int i = 0; i < 5; ++i)
        Plane(triangles, {offset, offset, -10.0 + i * 2}, {1, 0, 0}, {0, 1, 0});
    triangles.push_back({{offset, offset, 0}, {offset, offset, 0}, {offset, offset, 0}});
    TriangleRayScene scene;
    Check(scene.Build(triangles), "BVH accepts a bounded world-space triangle snapshot");
    TriangleRay ray{{offset, offset, 0}, {0, 0, -10}};
    const auto hit = scene.ClosestHit(ray);
    Check(hit && hit->frontFace && hit->triangle == 8 && Near(hit->position.z, -2) &&
              Near(hit->distance, 2) && Near(hit->normal.z, 1) &&
              Near(hit->barycentric.x + hit->barycentric.y + hit->barycentric.z, 1),
          "closest shared-edge hit is stable across BVH nodes at large coordinates");
    ray.maximumDistance = 1.9;
    Check(!scene.Occluded(ray), "finite ray segment excludes farther blockers");
    ray.maximumDistance = 2.1;
    Check(scene.Occluded(ray), "finite ray segment includes the closest blocker");
    ray.origin.x += 2;
    Check(!scene.Occluded(ray), "parallel box slabs reject an origin outside the bounds");
    ray.origin.x = offset;
    triangles.front().material = 99;
    Check(!scene.Build(triangles) && scene.ClosestHit(ray).has_value(),
          "invalid snapshot keeps the existing acceleration structure");
}

void Shadows() {
    TriangleRayScene scene;
    std::vector<RayTriangle> geometry;
    Plane(geometry, {}, {2, 0, 0}, {0, 2, 0});
    const TriangleRay ray{{0, 0, 2}, {0, 0, -1}};
    RayLighting lights;
    lights.directionToLight = {0, 1, 1};
    lights.directionalIrradiance = {1, 1, 1};
    lights.maxBounces = 0;
    Check(scene.Build(geometry), "receiver geometry builds");
    const auto lit = scene.TraceRadiance(ray, lights);
    Check(lit.x > .15 && Near(lit.x, lit.y) && Near(lit.x, lit.z),
          "unoccluded directional irradiance reaches the receiver");
    Plane(geometry, {0, 1, 1}, {.25, 0, 0}, {0, .25, 0});
    Check(scene.Build(geometry) && Near(scene.TraceRadiance(ray, lights).x, 0),
          "a triangle off the camera ray still casts a ray-traced shadow");
}

void Bounces() {
    TriangleRayScene scene;
    std::vector<RayTriangle> geometry;
    const std::array<RaySurface, 3> materials{{
        {.albedo = {1, 1, 1}, .emission = {}, .metallic = 1},
        {.albedo = {1, 1, 1}, .emission = {1, 0, 0}},
        {.albedo = {1, 1, 1}, .emission = {}, .transmission = 1},
    }};
    const TriangleRay ray{{0, 0, 1}, {0, 0, -1}};
    Plane(geometry, {}, {.5, 0, -.5}, {0, .7, 0});
    Plane(geometry, {2, 0, 0}, {.5, 0, .5}, {0, .7, 0});
    Plane(geometry, {2, 0, 2}, {.6, 0, 0}, {0, .6, 0}, 1);
    Check(scene.Build(geometry, materials), "multi-bounce scene builds");
    RayLighting lights;
    lights.maxBounces = 1;
    Check(Near(scene.TraceRadiance(ray, lights).x, 0),
          "one mirror bounce stops on the second mirror");
    lights.maxBounces = 2;
    const auto reflected = scene.TraceRadiance(ray, lights);
    Check(Near(reflected.x, 1) && Near(reflected.y, 0),
          "two mirror bounces reach the emissive triangle");

    // Oblique incidence bends the ray toward the normal, reaching a small target that a
    // straight-through transparency approximation would miss.
    geometry.clear();
    Plane(geometry, {}, {2, 0, 0}, {0, 2, 0}, 2);
    const TriangleRay slanted{{-.6, 0, 1}, {.6, 0, -1}};
    const double sine = .6 / std::sqrt(1.36) / 1.5;
    const double targetX = sine / std::sqrt(1 - sine * sine);
    Plane(geometry, {targetX, 0, -1}, {.04, 0, 0}, {0, .04, 0}, 1);
    Check(scene.Build(geometry, materials), "dielectric scene builds");
    const auto refracted = scene.TraceRadiance(slanted, lights);
    Check(refracted.x > .9 && Near(refracted.y, 0),
          "Snell refraction reaches a target missed by the incoming direction");
}
} // namespace

int main() {
    try {
        Intersection();
        Shadows();
        Bounces();
        std::cout << "Triangle BVH, ray shadows and reflection/refraction bounces passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
