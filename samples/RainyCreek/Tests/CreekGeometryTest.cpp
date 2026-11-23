#include <LuxEngine/EntryPoint.hpp>
#include <RainyCreek/CreekGeometry.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
namespace le = LuxEngine;
namespace rc = RainyCreek;

void Check(bool condition, const std::string& description) {
    if (!condition)
        throw std::runtime_error(description);
}

bool Near(float left, float right, float tolerance = 1e-6f) {
    return std::abs(left - right) <= tolerance;
}

void Validate(const rc::CpuMesh& mesh) {
    Check(!mesh.vertices.empty() && !mesh.indices.empty(), "Mesh must not be empty");
    Check(mesh.indices.size() % 3 == 0, "Indices must form triangles");
    for (const auto& vertex : mesh.vertices) {
        for (const auto value :
             {vertex.position.x, vertex.position.y, vertex.position.z, vertex.normal.x,
              vertex.normal.y, vertex.normal.z, vertex.texCoords.x, vertex.texCoords.y})
            Check(std::isfinite(value), "Mesh contains non-finite data");
        const auto normal = vertex.normal;
        Check(Near(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z, 1.0f, 2e-4f),
              "Mesh normals must be unit length");
        Check(vertex.texCoords.x >= 0.0f && vertex.texCoords.x <= 1.0f &&
                  vertex.texCoords.y >= 0.0f && vertex.texCoords.y <= 1.0f,
              "UVs must fit the current clamped texture sampler");
    }
    for (const auto index : mesh.indices)
        Check(index < mesh.vertices.size(), "Index references a missing vertex");
    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
        const auto& a = mesh.vertices[mesh.indices[i]];
        const auto& b = mesh.vertices[mesh.indices[i + 1]];
        const auto& c = mesh.vertices[mesh.indices[i + 2]];
        const float ux = b.position.x - a.position.x;
        const float uy = b.position.y - a.position.y;
        const float uz = b.position.z - a.position.z;
        const float vx = c.position.x - a.position.x;
        const float vy = c.position.y - a.position.y;
        const float vz = c.position.z - a.position.z;
        const le::Vec3 face{uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx};
        Check(face.x * face.x + face.y * face.y + face.z * face.z > 1e-12f,
              "Mesh contains a degenerate triangle");
        Check(face.x * a.normal.x + face.y * a.normal.y + face.z * a.normal.z > 0.0f,
              "Triangle winding disagrees with its normal");
    }
}

void SameMesh(const rc::CpuMesh& left, const rc::CpuMesh& right) {
    Check(left.indices == right.indices && left.vertices.size() == right.vertices.size(),
          "Animation loop changed topology");
    for (size_t i = 0; i < left.vertices.size(); ++i) {
        const auto& a = left.vertices[i];
        const auto& b = right.vertices[i];
        Check(Near(a.position.x, b.position.x) && Near(a.position.y, b.position.y) &&
                  Near(a.position.z, b.position.z) && Near(a.normal.x, b.normal.x) &&
                  Near(a.normal.y, b.normal.y) && Near(a.normal.z, b.normal.z),
              "Animation phase does not wrap deterministically");
    }
}

void Terrain() {
    const auto mesh = rc::BuildTerrain();
    Validate(mesh);
    Check(mesh.vertices.size() == 72u * 88u * 4u, "Terrain resolution changed unexpectedly");
    float minimumX = 0.0f, maximumX = 0.0f, minimumZ = 0.0f, maximumZ = 0.0f;
    for (const auto& vertex : mesh.vertices) {
        minimumX = std::min<double>(minimumX, vertex.position.x);
        maximumX = std::max<double>(maximumX, vertex.position.x);
        minimumZ = std::min<double>(minimumZ, vertex.position.z);
        maximumZ = std::max<double>(maximumZ, vertex.position.z);
        Check(Near(vertex.position.y, rc::TerrainHeight(vertex.position.x, vertex.position.z)),
              "Terrain mesh differs from placement height queries");
        Check(vertex.normal.y > 0.0f, "Terrain normal points below the surface");
    }
    Check(Near(minimumX, -18.0f) && Near(maximumX, 18.0f) && Near(minimumZ, -22.0f) &&
              Near(maximumZ, 22.0f),
          "Terrain must cover 36 by 44 metres");
    for (int i = -44; i <= 44; ++i) {
        const float z = static_cast<float>(i) * .5f;
        const float center = rc::StreamCenter(z);
        Check(std::abs(center) <= 2.21f, "Creek escaped its intended centre corridor");
        Check(rc::TerrainHeight(center, z) < -.22f, "Creek bed is not below the water");
        for (const float side : {-1.0f, 1.0f}) {
            const float bank = rc::TerrainHeight(center + side * 3.0f, z);
            Check(bank >= .2f && bank <= .7f, "Bank height is outside the placement range");
        }
    }
}

void Water() {
    auto water = rc::BuildWater();
    const auto mesh = [](const le::WaterSurface& surface) {
        return rc::CpuMesh{{surface.GetVertices().begin(), surface.GetVertices().end()},
                           {surface.GetIndices().begin(), surface.GetIndices().end()}};
    };
    const auto first = mesh(water);
    Check(first.vertices.size() == 9u * 177u, "Water exceeds its creek mesh budget");
    Validate(first);
    Check(water.SetTime(1), "Water must accept one full wave period");
    SameMesh(first, mesh(water));
    for (double time : {0.03, 0.07, .257, .83}) {
        Check(water.SetTime(time), "Water must accept fractional seconds");
        const auto moved = mesh(water);
        Validate(moved);
        Check(moved.indices == first.indices, "Water animation changed its topology");
        bool animated = false;
        for (size_t i = 0; i < moved.vertices.size(); ++i) {
            const auto& vertex = moved.vertices[i];
            Check(std::abs(vertex.position.x - rc::StreamCenter(vertex.position.z)) < 1.162f,
                  "Water escaped the creek banks");
            Check(std::abs(vertex.position.y - rc::WaterHeight()) <= .01801,
                  "Wave height is outside its bounded range");
            const auto sample = water.Sample(vertex.position.x, vertex.position.z);
            Check(sample && std::abs(sample->height - vertex.position.y) <= 1e-10 &&
                      sample->normal.Dot(vertex.normal) >= 1 - 1e-10 &&
                      std::abs(sample->foam - (1 - vertex.color.w)) < 1e-6 &&
                      sample->jacobian > 0 && std::isfinite(sample->velocity.y),
                  "Height and slope queries differ from the mesh");
            animated |= !Near(vertex.position.y, first.vertices[i].position.y);
        }
        Check(animated, "Fractional time must animate the water");
    }

    Check(!water.Sample(4, 0) && !water.Sample(0, 24),
          "Height queries outside the winding water surface must fail");

    const double infinity = std::numeric_limits<double>::infinity();
    const auto lastGood = mesh(water);
    Check(!water.SetTime(infinity) && !water.Sample(infinity, 0),
          "Nonfinite water inputs accepted");
    Check(!water.Configure({{10, 10}, 257, 1}, {}), "Unbounded mesh grid accepted");
    const std::array<le::WaterWave, 1> invalid{{{{0, 0}}}};
    Check(!water.Configure({}, invalid), "Wave without direction accepted");
    const std::array<le::WaterWave, 1> folded{{{{1, 0}, 1, 1, 1, 0, 1}}};
    Check(!water.Configure({}, folded), "Self-intersecting displacement accepted");
    SameMesh(lastGood, mesh(water));

    le::WaterSurface swell;
    const std::array<le::WaterWave, 1> surf{{{{1, 0}, .3, 4, 1, 0, 1}}};
    Check(swell.Configure({{4, 4}, 4, 4}, surf), "Safe Gerstner steepness rejected");
    const auto crest = swell.Sample(1, 0), trough = swell.Sample(-1, 0);
    Check(crest && trough && Near(crest->height, .3f) && Near(trough->height, -.3f) &&
              crest->jacobian < 1 && trough->jacobian > 1 && crest->foam > .9 &&
              trough->foam == 0 && crest->velocity.x > 0 && trough->velocity.x < 0,
          "Gerstner crest foam or buoyancy information is inconsistent");
    const auto displaced = swell.Sample(.3, 0);
    Check(displaced && Near(displaced->height, 0),
          "Inverse XZ sampling ignored horizontal displacement");

    const auto material = le::WaterSurface::MakeMaterial();
    Check(material.water && material.metallic == 0, "Water must use a dielectric water material");
}

void Rain() {
    const auto first = rc::BuildRain(0);
    SameMesh(first, rc::BuildRain(24));
    SameMesh(first, rc::BuildRain(48));
    Check(first.vertices.size() <= 350u * 8u, "Rain exceeded its bounded particle budget");
    bool moved = false;
    for (unsigned phase = 0; phase < 24; ++phase) {
        const auto mesh = rc::BuildRain(phase);
        Validate(mesh);
        Check(mesh.vertices.size() <= 350u * 8u, "Rain exceeded its bounded particle budget");
        for (const auto& vertex : mesh.vertices) {
            Check(std::abs(vertex.position.x) <= 18.0f && std::abs(vertex.position.z) <= 22.0f &&
                      vertex.position.y <= 8.0f,
                  "Rain escaped its scene volume");
            Check(vertex.position.y >= rc::TerrainHeight(vertex.position.x, vertex.position.z),
                  "Rain streak continues below the terrain");
            if (std::abs(vertex.position.x - rc::StreamCenter(vertex.position.z)) <= 1.14f)
                Check(vertex.position.y >= rc::WaterHeight(),
                      "Rain streak continues below the water");
        }
        moved = moved || mesh.vertices.size() != first.vertices.size() ||
                !Near(mesh.vertices.front().position.y, first.vertices.front().position.y);
    }
    Check(moved, "Rain phases must move the streaks");
    SameMesh(rc::BuildRain(1, 7), rc::BuildRain(8, 7));
}

void Lamps() {
    const auto post = rc::BuildLampPost();
    const auto glass = rc::BuildLampGlass();
    Validate(post);
    Validate(glass);
    float maximum = 0.0f;
    for (const auto& vertex : post.vertices) {
        Check(vertex.position.y >= 0.0f, "Lamp post extends below its placement origin");
        maximum = std::max<double>(maximum, vertex.position.y);
    }
    Check(Near(maximum, 2.61f), "Lamp post height does not match the scene scale");
    for (const auto& vertex : glass.vertices)
        Check(vertex.position.y >= 2.10f && vertex.position.y <= 2.40f,
              "Lamp glass does not surround the light position");
}

void InvalidPhases() {
    bool rainRejected = false;
    try {
        (void)rc::BuildRain(0, 0);
    } catch (const std::invalid_argument&) {
        rainRejected = true;
    }
    Check(rainRejected, "A zero rain phase count must be rejected");
    Validate(rc::BuildRain(std::numeric_limits<unsigned>::max()));
}

int RunGeometryTests(int, char**) {
    try {
        Terrain();
        Water();
        Rain();
        Lamps();
        InvalidPhases();
        std::cout << "Rainy Creek geometry tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

} // namespace

LUX_ENGINE_MAIN(RunGeometryTests)
