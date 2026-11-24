#include "PropModels.hpp"

#include <LuxEngine/Assets/ModelAsset.hpp>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <stdexcept>

namespace Frontier {
namespace {
using namespace LuxEngine;

std::string Utf8Path(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::map<std::string, Material3D, std::less<>> ReadMaterials(const std::filesystem::path& root) {
    std::ifstream file(root / "materials.txt");
    if (!file)
        throw std::runtime_error("Missing prop materials: " + Utf8Path(root));
    std::map<std::string, Material3D, std::less<>> materials;
    std::string name, base, normal, rough, alpha;
    Material3D material;
    while (file >> std::quoted(name) >> std::quoted(base) >> std::quoted(normal) >>
           std::quoted(rough) >> material.albedoColor.x >> material.albedoColor.y >>
           material.albedoColor.z >> material.albedoColor.w >> material.metallic >>
           material.roughness >> alpha >> material.alphaCutoff) {
        material.alphaMode = alpha == "MASK" ? MaterialAlphaMode::Mask : MaterialAlphaMode::Opaque;
        material.baseColorTexture = base.empty() ? "" : Utf8Path(root / base);
        material.normalTexture = normal.empty() ? "" : Utf8Path(root / normal);
        material.roughnessTexture = rough.empty() ? "" : Utf8Path(root / rough);
        material.textureSampling.anisotropic = true;
        const auto [entry, inserted] = materials.emplace(name, material);
        if (!inserted)
            throw std::runtime_error("Duplicate prop material: " + name);
    }
    if (!file.eof() || materials.empty())
        throw std::runtime_error("Invalid prop material table: " + Utf8Path(root));
    return materials;
}
} // namespace

void PropModels::Load(GpuDevice& gpu, const std::filesystem::path& assets) {
    std::set<std::string_view> names{"pickup-truck", "norway-spruce", "grass-clump", "sedge-tuft"};
    for (const auto& structure : Structures())
        names.insert(structure.model);
    for (const auto name : names) {
        const bool cover = name == "grass-clump" || name == "sedge-tuft";
        const bool vegetation = cover || name == "norway-spruce";
        const auto root = assets / (vegetation ? "FrontierVegetation" : "FrontierProps") / name;
        const auto materials = ReadMaterials(root);
        const auto path = Utf8Path(root / "model.fbx");
        std::string error;
        const auto asset = ModelAsset::LoadFbx(path, error);
        if (!asset)
            throw std::runtime_error("Prop asset " + path + ": " + error);
        Model result;
        const auto infinity = std::numeric_limits<double>::infinity();
        Vec3 low{infinity, infinity, infinity}, high{-infinity, -infinity, -infinity};
        for (const auto& submesh : asset->submeshes) {
            const auto& node = asset->nodes.at(submesh.node).worldTransform;
            const auto found = materials.find(asset->materials.at(size_t(submesh.material)).name);
            if (found == materials.end())
                throw std::runtime_error("Prop material missing for " + path + " (" +
                                         asset->materials.at(size_t(submesh.material)).name + ")");
            std::vector<Vertex3D> vertices;
            vertices.reserve(submesh.vertices.size());
            for (const auto& source : submesh.vertices) {
                const auto position = node.TransformPoint(source.position);
                low = {std::min(low.x, position.x), std::min(low.y, position.y),
                       std::min(low.z, position.z)};
                high = {std::max(high.x, position.x), std::max(high.y, position.y),
                        std::max(high.z, position.z)};
                vertices.push_back({position, node.TransformDirection(source.normal).Normalized(),
                                    source.texCoords});
            }
            if (cover) {
                result.batchGeometry.push_back(
                    {{std::move(vertices), submesh.indices}, found->second});
            } else {
                auto mesh = std::make_unique<Mesh3D>();
                if (!mesh->Upload(&gpu, vertices, submesh.indices))
                    throw std::runtime_error("Could not upload prop: " + path);
                result.parts.push_back({std::move(mesh), found->second});
            }
        }
        result.dimensions = high - low;
        if (result.dimensions.x < .001 || result.dimensions.y < .001 || result.dimensions.z < .001)
            throw std::runtime_error("Prop has a degenerate bounding volume: " + path);
        result.center = {(low.x + high.x) * .5, low.y, (low.z + high.z) * .5};
        models_.emplace(name, std::move(result));
    }
}

void PropModels::Draw(Renderer3D::Context& renderer, std::string_view key, Vec3 ground,
                      Vec3 dimensions, double yaw) const {
    const auto model = models_.find(key);
    if (model == models_.end())
        throw std::runtime_error("Unknown prop asset: " + std::string(key));
    const auto& source = model->second;
    const Vec3 scale{dimensions.x / source.dimensions.x, dimensions.y / source.dimensions.y,
                     dimensions.z / source.dimensions.z};
    const auto transform = Mat4::Translate(ground) * Mat4::RotateY(yaw) * Mat4::Scale(scale) *
                           Mat4::Translate(source.center * -1);
    for (const auto& part : source.parts)
        renderer.DrawMesh(*part.mesh, transform, part.material);
}

std::vector<PropModels::Part> PropModels::Batch(GpuDevice& gpu, std::string_view key,
                                                std::span<const Plant> plants, Vec3 origin) const {
    std::vector<Part> parts;
    if (plants.empty())
        return parts;
    const auto found = models_.find(key);
    if (found == models_.end())
        throw std::runtime_error("Unknown foliage asset: " + std::string(key));
    const auto& model = found->second;
    for (const auto& source : model.batchGeometry) {
        MeshData data;
        data.vertices.reserve(plants.size() * source.mesh.vertices.size());
        data.indices.reserve(plants.size() * source.mesh.indices.size());
        for (const auto& plant : plants) {
            const Vec3 scale{plant.dimensions.x / model.dimensions.x,
                             plant.dimensions.y / model.dimensions.y,
                             plant.dimensions.z / model.dimensions.z};
            const auto rotation = Mat4::RotateY(plant.yaw);
            const auto transform = Mat4::Translate(plant.ground - origin) * rotation *
                                   Mat4::Scale(scale) * Mat4::Translate(model.center * -1);
            const uint32 base = uint32(data.vertices.size());
            for (const auto& vertex : source.mesh.vertices) {
                const auto normal = vertex.normal;
                const auto direction =
                    rotation
                        .TransformDirection(
                            {normal.x / scale.x, normal.y / scale.y, normal.z / scale.z})
                        .Normalized();
                data.vertices.push_back({transform.TransformPoint(vertex.position), direction,
                                         vertex.texCoords, vertex.color});
            }
            for (const auto index : source.mesh.indices)
                data.indices.push_back(base + index);
        }
        auto mesh = std::make_unique<Mesh3D>();
        data.Upload(gpu, *mesh);
        parts.push_back({std::move(mesh), source.material});
    }
    return parts;
}

void PropModels::Release() {
    models_.clear();
}
} // namespace Frontier
