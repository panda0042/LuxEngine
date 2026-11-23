#pragma once

#include <LuxEngine/Core/Math.hpp>
#include <limits>
#include <memory>
#include <optional>
#include <span>

namespace LuxEngine {

struct TriangleRay {
    Vec3 origin;
    Vec3 direction{0, 0, -1}; // Normalized by the ray query.
    // Distances are measured in world units regardless of the direction length.
    double minimumDistance = 0;
    double maximumDistance = std::numeric_limits<double>::infinity();
};

struct RayTriangle {
    Vec3 a, b, c;
    uint32 material = 0;
};

struct TriangleRayHit {
    double distance = 0;
    Vec3 position;
    Vec3 normal; // Geometric normal oriented against the incoming ray.
    Vec3 barycentric;
    uint32 triangle = 0;
    uint32 material = 0;
    bool frontFace = false;
};

struct RaySurface {
    Vec3 albedo{.8, .8, .8};
    Vec3 emission;
    double metallic = 0;
    double transmission = 0;
    double indexOfRefraction = 1.5;
};

struct RayLighting {
    Vec3 directionToLight{0, 1, 0};
    Vec3 directionalIrradiance;
    Vec3 environmentRadiance;
    uint32 maxBounces = 2;
};

// Software ray queries over a world-space triangle snapshot, independent of the graphics device.
// Queries can run concurrently with other queries. Call Build between query batches.
class LUX_API TriangleRayScene {
public:
    TriangleRayScene();
    ~TriangleRayScene();
    TriangleRayScene(TriangleRayScene&&) noexcept;
    TriangleRayScene& operator=(TriangleRayScene&&) noexcept;
    TriangleRayScene(const TriangleRayScene&) = delete;
    TriangleRayScene& operator=(const TriangleRayScene&) = delete;

    // An empty material span supplies the default surface at index 0. Degenerate primitives are
    // ignored. Invalid input leaves the previous snapshot available.
    bool Build(std::span<const RayTriangle> triangles, std::span<const RaySurface> materials = {});
    std::optional<TriangleRayHit> ClosestHit(const TriangleRay& ray) const;
    bool Occluded(const TriangleRay& ray) const;

    // Linear radiance from emission, one directional light with ray visibility, and up to eight
    // ideal specular or dielectric interfaces. Dielectrics use an air/material boundary.
    Vec3 TraceRadiance(const TriangleRay& ray, const RayLighting& lighting) const;

private:
    struct Data;
    std::unique_ptr<Data> data_;
};

} // namespace LuxEngine
