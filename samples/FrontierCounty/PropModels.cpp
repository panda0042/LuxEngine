#include "PropModels.hpp"

#include <LuxEngine/Assets/AssetManager.hpp>
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
        double rootY = infinity, crownY = -infinity;
        if (vegetation) {
            for (const auto& part : asset->submeshes) {
                const auto& node = asset->nodes.at(part.node).worldTransform;
                for (const auto& vertex : part.vertices) {
                    const double y = node.TransformPoint(vertex.position).y;
                    rootY = std::min(rootY, y);
                    crownY = std::max(crownY, y);
                }
            }
        }
        for (const auto& submesh : asset->submeshes) {
            const auto& node = asset->nodes.at(submesh.node).worldTransform;
            const auto found = materials.find(asset->materials.at(size_t(submesh.material)).name);
            if (found == materials.end())
                throw std::runtime_error("Prop material missing for " + path + " (" +
                                         asset->materials.at(size_t(submesh.material)).name + ")");
            auto material = found->second;
            material.wettingResponse = vegetation ? .62f : .8f;
            if (vegetation) {
                material.windAmplitude = cover ? .12f : .32f;
                if (material.alphaMode == MaterialAlphaMode::Mask) {
                    material.subsurfaceRadius = {.00035, .0009, .00022};
                    material.subsurfaceThickness = cover ? .00024f : .00038f;
                    auto& leaf = material.surface.emplace();
                    leaf.model = SurfaceModel::Foliage;
                    leaf.indexOfRefraction = 1.43f;
                    leaf.transmission = cover ? .67f : .48f;
                    leaf.anisotropy = cover ? .42f : .28f;
                }
            }
            std::vector<Vertex3D> vertices;
            vertices.reserve(submesh.vertices.size());
            for (const auto& source : submesh.vertices) {
                const auto position = node.TransformPoint(source.position);
                low = {std::min(low.x, position.x), std::min(low.y, position.y),
                       std::min(low.z, position.z)};
                high = {std::max(high.x, position.x), std::max(high.y, position.y),
                        std::max(high.z, position.z)};
                vertices.push_back(
                    {position,
                     node.TransformDirection(source.normal).Normalized(),
                     source.texCoords,
                     {1, 1, 1, 1},
                     vegetation
                         ? float(std::clamp((position.y - rootY) / std::max(crownY - rootY, .001),
                                            0., 1.))
                         : 0.f});
            }
            if (cover) {
                result.batchGeometry.push_back({{std::move(vertices), submesh.indices}, material});
            } else {
                auto mesh = std::make_unique<Mesh3D>();
                if (!mesh->Upload(&gpu, vertices, submesh.indices))
                    throw std::runtime_error("Could not upload prop: " + path);
                result.parts.push_back({std::move(mesh), material});
            }
        }
        result.dimensions = high - low;
        if (result.dimensions.x < .001 || result.dimensions.y < .001 || result.dimensions.z < .001)
            throw std::runtime_error("Prop has a degenerate bounding volume: " + path);
        result.center = {(low.x + high.x) * .5, low.y, (low.z + high.z) * .5};
        models_.emplace(name, std::move(result));
    }
    LoadGroundScans(assets);
}

void PropModels::LoadGroundScans(const std::filesystem::path& assets) {
    struct PartSource {
        GroundSpecies species;
        const char* mesh;
        const char* texture;
        bool foliage;
    };

    const std::array sources{
        PartSource{GroundSpecies::Fern, "Models/fern_b.obj", "Textures/fern.png", true},
        PartSource{GroundSpecies::Sapling, "Models/pine_bark_far.obj", "Textures/pine_bark.png",
                   false},
        PartSource{GroundSpecies::Sapling, "Models/pine_needles_far.obj",
                   "Textures/pine_needles.png", true},
        PartSource{GroundSpecies::Deadwood, "Models/trunk_far.obj", "Textures/trunk.png", false},
        PartSource{GroundSpecies::Dandelion, "flower.obj", "albedo.png", true}};
    for (const auto& part : sources) {
        const auto key = std::string(GroundSpeciesNames[size_t(part.species)]);
        const bool flower = part.species == GroundSpecies::Dandelion;
        const auto root = flower ? assets / "FrontierFlowers" : assets / "rainy-creek";
        const auto path = Utf8Path(root / part.mesh);
        RawMeshData raw;
        if (!AssetManager::LoadMesh(path, raw) || !raw.hasTexCoords || raw.indices.empty())
            throw std::runtime_error("Could not load scanned ground cover: " + path);
        auto& model = models_[key];
        Vec3 low = raw.boundsMin, high = raw.boundsMax;
        if (!model.batchGeometry.empty()) {
            const auto hw = Vec3{model.dimensions.x * .5, 0, model.dimensions.z * .5};
            const auto lo = model.center - hw, hi = lo + model.dimensions;
            low = {std::min(low.x, lo.x), std::min(low.y, lo.y), std::min(low.z, lo.z)};
            high = {std::max(high.x, hi.x), std::max(high.y, hi.y), std::max(high.z, hi.z)};
        }
        model.center = {(low.x + high.x) * .5, low.y, (low.z + high.z) * .5};
        model.dimensions = high - low;
        auto& geometry = model.batchGeometry.emplace_back();
        auto& mat = geometry.material;
        mat.baseColorTexture = Utf8Path(root / part.texture);
        mat.textureSampling = {true, TextureAddressMode::Clamp, true, false};
        mat.wettingResponse = .7f;
        mat.roughness = part.foliage ? .71f : .86f;
        if (flower) {
            mat.normalTexture = Utf8Path(root / "normal.png");
            mat.roughnessTexture = Utf8Path(root / "roughness.png");
            mat.roughness = 1;
        }
        if (part.foliage) {
            mat.alphaMode = MaterialAlphaMode::Mask;
            mat.alphaCutoff = .45f;
            mat.doubleSided = true;
            mat.windAmplitude = part.species == GroundSpecies::Sapling ? .09f : .045f;
            mat.subsurfaceRadius = {.0004, .0007, .0003};
            mat.subsurfaceThickness = .00028f;
            auto& leaf = mat.surface.emplace();
            leaf.model = SurfaceModel::Foliage;
            leaf.transmission = .52f;
            leaf.indexOfRefraction = 1.43f;
        }
        geometry.mesh.indices = std::move(raw.indices);
        geometry.mesh.vertices.reserve(raw.vertices.size() / 3);
        for (size_t i = 0; i < raw.vertices.size() / 3; ++i) {
            const Vec3 p{raw.vertices[i * 3], raw.vertices[i * 3 + 1], raw.vertices[i * 3 + 2]};
            const float flexibility =
                part.foliage
                    ? float(std::clamp((p.y - model.center.y) / std::max(.001, model.dimensions.y),
                                       0., 1.))
                    : 0.f;
            geometry.mesh.vertices.push_back(
                {p,
                 {raw.normals[i * 3], raw.normals[i * 3 + 1], raw.normals[i * 3 + 2]},
                 {raw.texCoords[i * 2], raw.texCoords[i * 2 + 1]},
                 {1, 1, 1, 1},
                 flexibility});
        }
    }
}

void PropModels::Draw(Renderer3D::Context& renderer, std::string_view key, Vec3 ground,
                      Vec3 dimensions, double yaw, float lodTransition) const {
    Draw(renderer, key, Mat4::Translate(ground) * Mat4::RotateY(yaw), dimensions, 0, lodTransition);
}

void PropModels::Draw(Renderer3D::Context& renderer, std::string_view key, const Mat4& placement,
                      Vec3 dimensions, uint64_t motionInstance, float lodTransition) const {
    const auto model = models_.find(key);
    if (model == models_.end())
        throw std::runtime_error("Unknown prop asset: " + std::string(key));
    const auto& source = model->second;
    const Vec3 scale{dimensions.x / source.dimensions.x, dimensions.y / source.dimensions.y,
                     dimensions.z / source.dimensions.z};
    const auto transform = placement * Mat4::Scale(scale) * Mat4::Translate(source.center * -1);
    for (const auto& part : source.parts) {
        if (motionInstance || lodTransition != 0) {
            auto material = part.material;
            material.motionInstance = motionInstance;
            material.lodTransition = lodTransition;
            renderer.DrawMesh(*part.mesh, transform, material);
        } else {
            renderer.DrawMesh(*part.mesh, transform, part.material);
        }
    }
}

std::deque<PropModels::Geometry> PropModels::BuildGroundCover(ChunkKey tile, unsigned seed) const {
    const auto plants = GroundCover(tile, seed);
    const Vec3 origin{double(tile.x * CoverTileSize), 0, double(tile.z * CoverTileSize)};
    std::deque<Geometry> batches;
    for (size_t i = 0; i < GroundSpeciesNames.size(); ++i)
        BuildBatches(GroundSpeciesNames[i], plants[i], origin, batches);
    return batches;
}

void PropModels::BuildBatches(std::string_view key, std::span<const Plant> plants, Vec3 origin,
                              std::deque<Geometry>& batches) const {
    if (plants.empty())
        return;
    const auto found = models_.find(key);
    if (found == models_.end())
        throw std::runtime_error("Unknown foliage asset: " + std::string(key));
    const auto& model = found->second;
    constexpr uint64 ByteBudget = 12ull * 1024 * 1024;
    for (const auto& source : model.batchGeometry) {
        const auto bytesPerPlant = source.mesh.UploadBytes();
        if (!bytesPerPlant)
            continue;
        const size_t maxPlants = size_t(std::max<uint64>(1, ByteBudget / bytesPerPlant));
        for (auto remaining = plants; !remaining.empty();) {
            const auto batch = remaining.first(std::min(remaining.size(), maxPlants));
            remaining = remaining.subspan(batch.size());
            MeshData data;
            data.vertices.reserve(batch.size() * source.mesh.vertices.size());
            data.indices.reserve(batch.size() * source.mesh.indices.size());
            for (const auto& plant : batch) {
                const Vec3 scale{plant.dimensions.x / model.dimensions.x,
                                 plant.dimensions.y / model.dimensions.y,
                                 plant.dimensions.z / model.dimensions.z};
                const auto rotation =
                    Mat4::RotateY(plant.yaw) * Mat4::RotateZ(plant.longitudinalTilt);
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
                                             vertex.texCoords, vertex.color, vertex.windWeight});
                }
                for (const auto index : source.mesh.indices)
                    data.indices.push_back(base + index);
            }
            batches.push_back({std::move(data), source.material});
        }
    }
}

void PropModels::Release() {
    models_.clear();
}
} // namespace Frontier
