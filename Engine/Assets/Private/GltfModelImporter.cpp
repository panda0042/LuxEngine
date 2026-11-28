#include <LuxEngine/Assets/ModelAsset.hpp>
#include <algorithm>
#include <array>
#include <cgltf.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace LuxEngine {
namespace {
namespace fs = std::filesystem;

constexpr size_t MaxFileBytes = 64 * 1024 * 1024;
constexpr size_t MaxResourceBytes = 128 * 1024 * 1024;
constexpr size_t MaxParserBytes = 128 * 1024 * 1024;
constexpr size_t MaxVertices = 1'000'000;
constexpr size_t MaxIndices = 3'000'000;
constexpr size_t MaxNodes = 100'000;
constexpr size_t MaxSubmeshes = 8192;
constexpr size_t MaxResources = 8192;
constexpr size_t MaxExternalFiles = 128;
constexpr size_t MaxDepth = 256;

void Require(bool condition, const char* reason) {
    if (!condition)
        throw std::runtime_error(reason);
}

fs::path Path(std::string_view path) {
    return fs::path(std::u8string(path.begin(), path.end()));
}

std::string Utf8(const fs::path& path) {
    const auto bytes = path.generic_u8string();
    return {bytes.begin(), bytes.end()};
}

std::string DecodeJsonString(const char* value) {
    if (!value)
        return {};
    std::string text(value);
    text.resize(cgltf_decode_string(text.data()));
    Require(text.find('\0') == std::string::npos, "glTF 문자열에 NUL 문자가 있습니다.");
    return text;
}

struct FileStamp {
    fs::path requested;
    fs::path resolved;
    fs::file_time_type modified;
    uintmax_t size = 0;
};

FileStamp Stamp(const fs::path& path) {
    const auto resolved = fs::canonical(path);
    Require(fs::is_regular_file(resolved), "glTF 리소스가 일반 파일이 아닙니다.");
    const auto size = fs::file_size(resolved);
    Require(size > 0 && size <= MaxFileBytes,
            "glTF 리소스는 비어 있지 않고 64 MiB 이하여야 합니다.");
    return {path, resolved, fs::last_write_time(resolved), size};
}

bool Current(const FileStamp& stamp) {
    std::error_code status;
    const auto resolved = fs::canonical(stamp.requested, status);
    if (status || resolved != stamp.resolved || !fs::is_regular_file(resolved, status) || status)
        return false;
    const auto size = fs::file_size(resolved, status);
    if (status || size != stamp.size)
        return false;
    return fs::last_write_time(resolved, status) == stamp.modified && !status;
}

std::vector<uint8> Snapshot(const FileStamp& stamp) {
    std::ifstream input(stamp.resolved, std::ios::binary);
    Require(static_cast<bool>(input), "glTF 리소스를 열 수 없습니다.");
    std::vector<uint8> data(static_cast<size_t>(stamp.size));
    input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    Require(static_cast<bool>(input) && input.peek() == std::char_traits<char>::eof() &&
                Current(stamp),
            "glTF 리소스를 읽는 동안 파일이 변경되었습니다.");
    return data;
}

int Hex(unsigned char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

fs::path ResourcePath(std::string_view uri) {
    Require(!uri.empty() && uri.size() <= 4096, "glTF 리소스 URI 길이가 올바르지 않습니다.");
    std::string decoded;
    decoded.reserve(uri.size());
    for (size_t index = 0; index < uri.size(); ++index) {
        char c = uri[index];
        if (c == '%') {
            Require(uri.size() - index >= 3, "glTF URI의 퍼센트 이스케이프가 올바르지 않습니다.");
            const int a = Hex(static_cast<unsigned char>(uri[index + 1]));
            const int b = Hex(static_cast<unsigned char>(uri[index + 2]));
            Require(a >= 0 && b >= 0, "glTF URI의 퍼센트 이스케이프가 올바르지 않습니다.");
            c = static_cast<char>(a * 16 + b);
            index += 2;
        }
        Require(static_cast<unsigned char>(c) >= 32 && c != 127 &&
                    std::string_view("\\:?#").find(c) == std::string_view::npos,
                "glTF는 로컬 상대 리소스 URI만 지원합니다.");
        decoded.push_back(c);
    }
    const auto relative = Path(decoded);
    Require(!relative.has_root_path() &&
                std::ranges::none_of(relative, [](const auto& segment) { return segment == ".."; }),
            "glTF 리소스 URI가 모델 폴더를 벗어납니다.");
    return relative.lexically_normal();
}

struct ResourceFiles {
    fs::path root;
    std::vector<FileStamp> stamps;
    size_t bytes = 0;

    void AddBytes(size_t count) {
        Require(count <= MaxResourceBytes - bytes, "glTF 외부·내장 리소스 합계가 너무 큽니다.");
        bytes += count;
    }

    const FileStamp& Add(const fs::path& relative) {
        const auto path = root / relative;
        const auto found = std::ranges::find(stamps, path, &FileStamp::requested);
        if (found != stamps.end())
            return *found;
        Require(stamps.size() <= MaxExternalFiles, "glTF 외부 리소스가 너무 많습니다.");
        auto stamp = Stamp(path);
        const auto inside = stamp.resolved.lexically_relative(root);
        Require(!inside.empty() && !inside.has_root_path() && *inside.begin() != "..",
                "glTF 리소스의 실제 경로가 모델 폴더를 벗어납니다.");
        stamps.push_back(std::move(stamp));
        return stamps.back();
    }
};

int Base64(char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    return c == '/' ? 63 : -1;
}

std::vector<uint8> DataUri(std::string_view uri, bool image, ResourceFiles& files) {
    const auto comma = uri.find(',');
    Require(uri.starts_with("data:") && comma != std::string_view::npos && comma >= 12,
            "glTF 데이터 URI가 올바르지 않습니다.");
    const auto header = uri.substr(5, comma - 5);
    Require(header.ends_with(";base64"), "glTF 데이터 URI는 Base64여야 합니다.");
    const auto mime = header.substr(0, header.size() - 7);
    Require(image ? (mime == "image/png" || mime == "image/jpeg")
                  : (mime == "application/octet-stream" || mime == "application/gltf-buffer"),
            "지원하지 않는 glTF 데이터 URI 미디어 형식입니다.");
    const auto encoded = uri.substr(comma + 1);
    Require(!encoded.empty() && encoded.size() % 4 == 0, "glTF Base64 길이가 올바르지 않습니다.");
    const size_t padding = encoded.ends_with("==") ? 2 : (encoded.ends_with('=') ? 1 : 0);
    const size_t size = encoded.size() / 4 * 3 - padding;
    Require(size > 0 && size <= MaxFileBytes, "glTF 데이터 URI 크기가 한도를 초과했습니다.");
    files.AddBytes(size);
    std::vector<uint8> bytes;
    bytes.reserve(size);
    uint32 accumulator = 0;
    int bits = 0;
    for (const char c : encoded.substr(0, encoded.size() - padding)) {
        const int value = Base64(c);
        Require(value >= 0, "glTF Base64 문자가 올바르지 않습니다.");
        accumulator = (accumulator << 6) | static_cast<uint32>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<uint8>(accumulator >> bits));
        }
    }
    Require(bytes.size() == size, "glTF Base64 디코딩 길이가 올바르지 않습니다.");
    return bytes;
}

struct alignas(std::max_align_t) Allocation {
    size_t bytes;
};

void* Allocate(void* user, size_t size) {
    auto& used = *static_cast<size_t*>(user);
    if (size > MaxParserBytes - used || sizeof(Allocation) > MaxParserBytes - used - size)
        return nullptr;
    auto* memory = static_cast<Allocation*>(std::malloc(sizeof(Allocation) + size));
    if (!memory)
        return nullptr;
    memory->bytes = sizeof(Allocation) + size;
    used += memory->bytes;
    return memory + 1;
}

void Free(void* user, void* data) {
    if (!data)
        return;
    auto* memory = static_cast<Allocation*>(data) - 1;
    *static_cast<size_t*>(user) -= memory->bytes;
    std::free(memory);
}

void LoadBuffers(cgltf_data& document, ResourceFiles& files,
                 std::vector<std::vector<uint8>>& buffers) {
    Require(document.buffers_count > 0 && document.buffers_count <= MaxExternalFiles,
            "glTF 버퍼 개수가 올바르지 않습니다.");
    buffers.resize(document.buffers_count);
    for (size_t index = 0; index < document.buffers_count; ++index) {
        auto& buffer = document.buffers[index];
        Require(buffer.size > 0 && buffer.size <= MaxFileBytes,
                "glTF 버퍼 크기가 올바르지 않습니다.");
        if (!buffer.uri) {
            Require(index == 0 && document.bin && buffer.size <= document.bin_size,
                    "glTF 버퍼에 BIN 청크 또는 URI가 없습니다.");
            buffer.data = const_cast<void*>(document.bin);
        } else {
            const auto uri = DecodeJsonString(buffer.uri);
            auto& bytes = buffers[index];
            if (uri.starts_with("data:"))
                bytes = DataUri(uri, false, files);
            else {
                const auto& file = files.Add(ResourcePath(uri));
                files.AddBytes(static_cast<size_t>(file.size));
                bytes = Snapshot(file);
            }
            Require(bytes.size() >= buffer.size, "glTF 버퍼가 선언한 크기보다 작습니다.");
            buffer.data = bytes.data();
        }
        buffer.data_free_method = cgltf_data_free_method_none;
    }
}

void ValidateDocument(const cgltf_data& document) {
    Require(
        document.asset.version && std::string_view(document.asset.version) == "2.0" &&
            (!document.asset.min_version || std::string_view(document.asset.min_version) == "2.0"),
        "glTF 2.0 파일만 지원합니다.");
    if (document.extensions_required_count)
        throw std::runtime_error(std::string("지원하지 않는 glTF 필수 확장: ") +
                                 document.extensions_required[0]);
    Require(document.animations_count == 0 && document.skins_count == 0,
            "정적 glTF 가져오기는 애니메이션과 스킨을 지원하지 않습니다.");
    Require(document.nodes_count <= MaxNodes && document.materials_count <= MaxResources &&
                document.textures_count <= MaxResources && document.images_count <= MaxResources &&
                document.meshes_count <= MaxSubmeshes,
            "glTF 노드·메시·재질 개수가 한도를 초과했습니다.");
    for (size_t index = 0; index < document.nodes_count; ++index) {
        size_t depth = 0;
        for (auto* parent = document.nodes[index].parent; parent; parent = parent->parent)
            Require(++depth < MaxDepth, "glTF 노드 계층이 너무 깊거나 순환합니다.");
    }
}

void ValidateStorage(cgltf_data& document) {
    for (size_t index = 0; index < document.buffer_views_count; ++index) {
        const auto& view = document.buffer_views[index];
        Require(!view.has_meshopt_compression && view.buffer && view.buffer->data &&
                    view.offset <= view.buffer->size &&
                    view.size <= view.buffer->size - view.offset,
                "glTF 버퍼 뷰 범위 또는 압축 형식이 올바르지 않습니다.");
    }
    for (size_t index = 0; index < document.accessors_count; ++index) {
        const auto& accessor = document.accessors[index];
        Require(!accessor.is_sparse, "정적 glTF 가져오기는 희소 접근자를 지원하지 않습니다.");
        const size_t component = cgltf_component_size(accessor.component_type);
        const size_t item = cgltf_calc_size(accessor.type, accessor.component_type);
        const auto* view = accessor.buffer_view;
        Require(component > 0 && item > 0 && accessor.count > 0 && accessor.count <= MaxIndices &&
                    view && accessor.stride >= item && accessor.stride <= 252 &&
                    accessor.offset <= view->size && item <= view->size - accessor.offset &&
                    accessor.count - 1 <= (view->size - accessor.offset - item) / accessor.stride &&
                    (reinterpret_cast<std::uintptr_t>(view->buffer->data) + view->offset +
                     accessor.offset) %
                            component ==
                        0 &&
                    accessor.stride % component == 0,
                "glTF 접근자 정렬 또는 범위가 올바르지 않습니다.");
    }
    for (size_t mesh = 0; mesh < document.meshes_count; ++mesh)
        for (size_t index = 0; index < document.meshes[mesh].primitives_count; ++index) {
            const auto& primitive = document.meshes[mesh].primitives[index];
            Require(!primitive.has_draco_mesh_compression && primitive.targets_count == 0,
                    "정적 glTF 가져오기는 Draco 메시와 모프 대상을 지원하지 않습니다.");
            if (primitive.indices)
                Require(primitive.indices->stride ==
                            cgltf_component_size(primitive.indices->component_type),
                        "glTF 인덱스 접근자는 조밀해야 합니다.");
        }
    Require(cgltf_validate(&document) == cgltf_result_success, "glTF 구조 검증에 실패했습니다.");
}

std::string Name(const char* value, const char* fallback) {
    const auto name = DecodeJsonString(value);
    Require(name.size() <= 4096, "glTF 이름이 너무 깁니다.");
    return name.empty() ? fallback : name;
}

bool ImageMime(std::string_view type) {
    return type == "image/png" || type == "image/jpeg";
}

void CheckImage(std::span<const uint8> bytes) {
    constexpr std::array<uint8, 8> png{137, 80, 78, 71, 13, 10, 26, 10};
    Require((bytes.size() >= png.size() && std::ranges::equal(bytes.first(png.size()), png)) ||
                (bytes.size() >= 3 && bytes[0] == 255 && bytes[1] == 216 && bytes[2] == 255),
            "glTF 내장 이미지는 PNG 또는 JPEG여야 합니다.");
}

double Finite(double value) {
    Require(std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max(),
            "glTF 값이 유효한 실수 범위를 벗어났습니다.");
    return value;
}

float Unit(float value) {
    Require(value >= 0 && value <= 1, "glTF 재질 값은 0과 1 사이여야 합니다.");
    return value;
}

Vec3 Vector(const float* values) {
    return {Finite(values[0]), Finite(values[1]), Finite(values[2])};
}

double Length(const Vec3& value) {
    return std::hypot(value.x, value.y, value.z);
}

Vec3 Normal(const Vec3& value, Vec3 fallback = {}) {
    const auto length = Length(value);
    if (length > 1e-30)
        return value * (1.0 / length);
    return fallback;
}

ModelTransform LocalTransform(const cgltf_node& node) {
    std::array<float, 16> source{};
    cgltf_node_transform_local(&node, source.data());
    ModelTransform result;
    std::transform(source.begin(), source.end(), result.matrix.elements, Finite);
    const auto& m = result.matrix.elements;
    Require(m[3] == 0 && m[7] == 0 && m[11] == 0 && m[15] == 1,
            "glTF 노드는 아핀 변환이어야 합니다.");
    result.position = {m[12], m[13], m[14]};
    Vec3 x{m[0], m[1], m[2]}, y{m[4], m[5], m[6]}, z{m[8], m[9], m[10]};
    result.scale = {Length(x), Length(y), Length(z)};
    Require(result.scale.x > 0 && result.scale.y > 0 && result.scale.z > 0,
            "glTF 노드의 축 배율은 0일 수 없습니다.");
    x = x * (1 / result.scale.x);
    y = y * (1 / result.scale.y);
    z = z * (1 / result.scale.z);
    Require(std::abs(x.Dot(y)) <= 1e-4 && std::abs(x.Dot(z)) <= 1e-4 && std::abs(y.Dot(z)) <= 1e-4,
            "glTF 노드 행렬에 전단 변환이 있습니다.");
    if (x.Dot(y.Cross(z)) < 0) {
        result.scale.x = -result.scale.x;
        x = x * -1;
    }
    result.rotation.y = std::asin(std::clamp(-x.z, -1.0, 1.0));
    if (std::abs(std::cos(result.rotation.y)) > 1e-5) {
        result.rotation.x = std::atan2(y.z, z.z);
        result.rotation.z = std::atan2(x.y, x.x);
    } else {
        result.rotation.z = std::atan2(-y.x, y.y);
    }
    return result;
}

const cgltf_accessor* Attribute(const cgltf_primitive& primitive, cgltf_attribute_type type) {
    const auto attributes = std::span(primitive.attributes, primitive.attributes_count);
    const auto it = std::ranges::find_if(attributes, [&](const auto& attribute) {
        return attribute.type == type && attribute.index == 0;
    });
    return it == attributes.end() ? nullptr : it->data;
}

struct Importer {
    cgltf_data& document;
    ResourceFiles& files;
    ModelAsset& model;
    std::vector<int32> images;
    std::vector<bool> visited;
    size_t vertices = 0;
    size_t indices = 0;
    int32 defaultMaterial = -1;

    int32 Texture(const cgltf_texture_view& view) {
        if (!view.texture)
            return -1;
        Require(view.texcoord == 0 && view.texture->image,
                "정적 glTF 텍스처는 UV0와 PNG 또는 JPEG 이미지를 사용해야 합니다.");
        const auto& source = *view.texture->image;
        auto& cached = images[cgltf_image_index(&document, &source)];
        if (cached >= 0)
            return cached;
        ModelTexture texture;
        texture.name = Name(source.name, "Image");
        if (source.uri) {
            const auto uri = DecodeJsonString(source.uri);
            if (uri.starts_with("data:")) {
                texture.embedded = DataUri(uri, true, files);
                CheckImage(texture.embedded);
            } else {
                const auto relative = ResourcePath(uri);
                auto extension = Utf8(relative.extension());
                std::ranges::transform(extension, extension.begin(), [](char c) {
                    return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
                });
                Require(extension == ".png" || extension == ".jpg" || extension == ".jpeg",
                        "glTF 외부 텍스처는 PNG 또는 JPEG여야 합니다.");
                const auto& file = files.Add(relative);
                texture.path = Utf8(file.resolved);
                texture.relativePath = Utf8(relative);
            }
        } else {
            Require(source.buffer_view && source.mime_type && ImageMime(source.mime_type),
                    "glTF 내장 텍스처의 버퍼 뷰 또는 미디어 형식이 올바르지 않습니다.");
            const auto& bufferView = *source.buffer_view;
            const auto* bytes =
                static_cast<const uint8*>(bufferView.buffer->data) + bufferView.offset;
            const std::span payload(bytes, bufferView.size);
            CheckImage(payload);
            files.AddBytes(payload.size());
            texture.embedded.assign(payload.begin(), payload.end());
        }
        cached = static_cast<int32>(model.textures.size());
        model.textures.push_back(std::move(texture));
        return cached;
    }

    void ReadMaterials() {
        model.materials.reserve(document.materials_count);
        for (size_t index = 0; index < document.materials_count; ++index) {
            const auto& source = document.materials[index];
            ModelMaterial material;
            material.name = Name(source.name, "Material");
            material.metallic = 1;
            if (source.has_pbr_metallic_roughness) {
                const auto& pbr = source.pbr_metallic_roughness;
                const auto* base = pbr.base_color_factor;
                material.baseColor = {Unit(base[0]), Unit(base[1]), Unit(base[2]), Unit(base[3])};
                material.metallic = Unit(pbr.metallic_factor);
                material.roughness = Unit(pbr.roughness_factor);
                material.baseColorTexture = Texture(pbr.base_color_texture);
                material.metallicTexture = Texture(pbr.metallic_roughness_texture);
                material.roughnessTexture = material.metallicTexture;
            }
            material.emissive = {Unit(source.emissive_factor[0]), Unit(source.emissive_factor[1]),
                                 Unit(source.emissive_factor[2])};
            material.normalTexture = Texture(source.normal_texture);
            if (material.normalTexture >= 0)
                material.normalScale = static_cast<float>(Finite(source.normal_texture.scale));
            material.occlusionTexture = Texture(source.occlusion_texture);
            if (material.occlusionTexture >= 0)
                material.occlusionStrength = Unit(source.occlusion_texture.scale);
            material.emissiveTexture = Texture(source.emissive_texture);
            material.doubleSided = source.double_sided != 0;
            if (source.alpha_mode == cgltf_alpha_mode_mask)
                material.alphaMode = ModelAlphaMode::Mask;
            else if (source.alpha_mode == cgltf_alpha_mode_blend)
                material.alphaMode = ModelAlphaMode::Blend;
            Require(source.alpha_cutoff >= 0 && std::isfinite(source.alpha_cutoff),
                    "glTF 알파 컷오프가 올바르지 않습니다.");
            material.alphaCutoff = source.alpha_cutoff;
            model.materials.push_back(std::move(material));
        }
    }

    int32 Material(const cgltf_primitive& primitive) {
        if (primitive.material)
            return static_cast<int32>(cgltf_material_index(&document, primitive.material));
        if (defaultMaterial < 0) {
            ModelMaterial material;
            material.name = "Default";
            material.metallic = 1;
            defaultMaterial = static_cast<int32>(model.materials.size());
            model.materials.push_back(std::move(material));
        }
        return defaultMaterial;
    }

    void ReadPrimitive(const cgltf_primitive& primitive, uint32 node, const std::string& name) {
        const auto* positions = Attribute(primitive, cgltf_attribute_type_position);
        const auto* normals = Attribute(primitive, cgltf_attribute_type_normal);
        const auto* uv = Attribute(primitive, cgltf_attribute_type_texcoord);
        const auto isFloat3 = [](const cgltf_accessor* attribute) {
            return attribute->type == cgltf_type_vec3 &&
                   attribute->component_type == cgltf_component_type_r_32f &&
                   !attribute->normalized;
        };
        Require(primitive.type == cgltf_primitive_type_triangles && positions &&
                    isFloat3(positions),
                "정적 glTF 가져오기는 위치가 있는 삼각형 메시를 지원합니다.");
        Require(!normals || isFloat3(normals), "glTF 법선 접근자의 형식이 올바르지 않습니다.");
        Require(!uv || (uv->type == cgltf_type_vec2 &&
                        ((!uv->normalized && uv->component_type == cgltf_component_type_r_32f) ||
                         (uv->normalized && (uv->component_type == cgltf_component_type_r_8u ||
                                             uv->component_type == cgltf_component_type_r_16u)))),
                "glTF UV0 접근자의 형식이 올바르지 않습니다.");
        const auto count = positions->count;
        const auto indexCount = primitive.indices ? primitive.indices->count : count;
        const auto vertexCount = normals ? count : indexCount;
        Require(count >= 3 && count <= MaxVertices - vertices && indexCount >= 3 &&
                    indexCount % 3 == 0 && indexCount <= MaxIndices - indices &&
                    vertexCount <= MaxVertices - vertices &&
                    model.submeshes.size() < MaxSubmeshes &&
                    (!primitive.indices || primitive.indices->type == cgltf_type_scalar),
                "glTF 정점·인덱스 또는 서브메시 개수가 올바르지 않습니다.");
        const auto material = Material(primitive);
        const auto& usedMaterial = model.materials[material];
        Require(uv || (usedMaterial.baseColorTexture < 0 && usedMaterial.normalTexture < 0 &&
                       usedMaterial.metallicTexture < 0 && usedMaterial.occlusionTexture < 0 &&
                       usedMaterial.emissiveTexture < 0),
                "텍스처가 있는 glTF 프리미티브에는 UV0가 필요합니다.");
        ModelSubmesh out;
        out.name = name;
        out.node = node;
        out.material = material;
        out.hasTexCoords = uv != nullptr;
        out.vertices.resize(count);
        out.indices.resize(indexCount);
        vertices += vertexCount;
        indices += indexCount;
        std::array<float, 3> value{};
        for (size_t index = 0; index < count; ++index) {
            auto& vertex = out.vertices[index];
            Require(cgltf_accessor_read_float(positions, index, value.data(), 3),
                    "glTF 위치를 읽을 수 없습니다.");
            vertex.position = Vector(value.data());
            if (normals) {
                Require(cgltf_accessor_read_float(normals, index, value.data(), 3),
                        "glTF 법선을 읽을 수 없습니다.");
                vertex.normal = Normal(Vector(value.data()), {0, 1, 0});
            }
            if (uv) {
                Require(cgltf_accessor_read_float(uv, index, value.data(), 2),
                        "glTF UV0를 읽을 수 없습니다.");
                vertex.texCoords = {Finite(value[0]), Finite(value[1])};
            }
        }
        for (size_t index = 0; index < indexCount; ++index) {
            const size_t vertexIndex =
                primitive.indices ? cgltf_accessor_read_index(primitive.indices, index) : index;
            Require(vertexIndex < count, "glTF 프리미티브의 인덱스가 정점 범위를 벗어납니다.");
            out.indices[index] = static_cast<uint32>(vertexIndex);
        }
        if (!normals) {
            if (primitive.indices) {
                std::vector<ModelVertex> flat;
                flat.reserve(indexCount);
                for (auto& index : out.indices) {
                    flat.push_back(out.vertices[index]);
                    index = static_cast<uint32>(flat.size() - 1);
                }
                out.vertices = std::move(flat);
            }
            for (size_t index = 0; index < indexCount; index += 3) {
                auto& a = out.vertices[index];
                auto& b = out.vertices[index + 1];
                auto& c = out.vertices[index + 2];
                const auto normal = (b.position - a.position).Cross(c.position - a.position);
                a.normal = b.normal = c.normal = Normal(normal, {0, 1, 0});
            }
        }
        out.boundsMin = out.boundsMax = out.vertices.front().position;
        for (const auto& vertex : out.vertices) {
            const auto [x, y, z] = vertex.position;
            const auto& min = out.boundsMin;
            const auto& max = out.boundsMax;
            out.boundsMin = {std::min(x, min.x), std::min(y, min.y), std::min(z, min.z)};
            out.boundsMax = {std::max(x, max.x), std::max(y, max.y), std::max(z, max.z)};
        }
        model.nodes[node].submeshes.push_back(static_cast<uint32>(model.submeshes.size()));
        model.submeshes.push_back(std::move(out));
    }

    void Visit(const cgltf_node& source, int32 parent, size_t depth) {
        const size_t sourceIndex = cgltf_node_index(&document, &source);
        Require(depth < MaxDepth && !visited[sourceIndex] && !source.skin &&
                    !source.has_mesh_gpu_instancing,
                "glTF 노드 계층이나 인스턴싱 형식이 지원 범위를 벗어났습니다.");
        visited[sourceIndex] = true;
        const auto index = static_cast<uint32>(model.nodes.size());
        ModelNode node;
        node.name = Name(source.name, "Node");
        node.parent = parent;
        node.localTransform = LocalTransform(source);
        node.worldTransform = parent < 0
                                  ? node.localTransform.matrix
                                  : model.nodes[parent].worldTransform * node.localTransform.matrix;
        for (const auto value : node.worldTransform.elements)
            Finite(value);
        model.nodes.push_back(std::move(node));
        if (source.mesh) {
            const auto name = Name(source.mesh->name, "Mesh");
            for (size_t primitive = 0; primitive < source.mesh->primitives_count; ++primitive)
                ReadPrimitive(source.mesh->primitives[primitive], index, name);
        }
        for (size_t child = 0; child < source.children_count; ++child)
            Visit(*source.children[child], static_cast<int32>(index), depth + 1);
    }

    void ReadNodes() {
        const auto* active = document.scene ? document.scene : document.scenes;
        if (active) {
            for (size_t index = 0; index < active->nodes_count; ++index)
                Visit(*active->nodes[index], -1, 0);
        } else {
            for (size_t index = 0; index < document.nodes_count; ++index)
                if (!document.nodes[index].parent)
                    Visit(document.nodes[index], -1, 0);
        }
        Require(!model.submeshes.empty(), "활성 glTF 씬에 가져올 메시가 없습니다.");
    }
};

struct CachedModel {
    std::weak_ptr<ModelAsset> model;
    std::vector<FileStamp> files;
};

std::mutex cacheMutex;
std::unordered_map<std::string, CachedModel> cache;
std::unordered_map<std::string, std::weak_ptr<ModelAsset>> requestedModels;

void RememberRequestedModel(const std::string& requested, const std::string& canonical,
                            const Ref<ModelAsset>& model) {
    if (requested == canonical) {
        requestedModels.erase(requested);
        return;
    }
    if (!requestedModels.contains(requested) && requestedModels.size() >= MaxExternalFiles) {
        std::erase_if(requestedModels, [](const auto& entry) { return entry.second.expired(); });
        if (requestedModels.size() >= MaxExternalFiles)
            requestedModels.erase(requestedModels.begin());
    }
    requestedModels.insert_or_assign(requested, model);
}

} // namespace

Ref<ModelAsset> ModelAsset::ReadGltf(const std::string& filepath, std::string& error) {
    error.clear();
    Ref<ModelAsset> fallback;
    try {
        Require(!filepath.empty() && filepath.size() <= 4096 &&
                    filepath.find('\0') == std::string::npos,
                "glTF 원본 경로가 올바르지 않습니다.");
        const auto requested = fs::absolute(Path(filepath));
        const auto requestedKey = Utf8(requested);
        {
            const std::lock_guard lock(cacheMutex);
            if (const auto found = requestedModels.find(requestedKey);
                found != requestedModels.end())
                fallback = found->second.lock();
            if (!fallback)
                if (const auto found = cache.find(requestedKey); found != cache.end())
                    fallback = found->second.model.lock();
        }
        auto stamp = Stamp(requested);
        const auto key = Utf8(stamp.resolved);
        {
            const std::lock_guard lock(cacheMutex);
            const auto found = cache.find(key);
            if (found != cache.end()) {
                if (auto model = found->second.model.lock()) {
                    if (std::ranges::all_of(found->second.files, Current)) {
                        RememberRequestedModel(requestedKey, key, model);
                        return model;
                    }
                    if (!fallback)
                        fallback = std::move(model);
                }
            }
        }
        auto bytes = Snapshot(stamp);
        ResourceFiles files{stamp.resolved.parent_path(), {std::move(stamp)}};
        std::vector<std::vector<uint8>> buffers;
        size_t allocated = 0;
        cgltf_options options{};
        options.memory = {Allocate, Free, &allocated};
        cgltf_data* source = nullptr;
        const auto status = cgltf_parse(&options, bytes.data(), bytes.size(), &source);
        const std::unique_ptr<cgltf_data, decltype(&cgltf_free)> document(source, cgltf_free);
        Require(status == cgltf_result_success && document, "glTF 파일을 해석할 수 없습니다.");
        ValidateDocument(*document);
        LoadBuffers(*document, files, buffers);
        ValidateStorage(*document);
        Ref<ModelAsset> model(new ModelAsset);
        model->sourcePath = key;
        model->staticGltf_ = true;
        Importer importer{*document, files, *model, std::vector<int32>(document->images_count, -1),
                          std::vector<bool>(document->nodes_count)};
        importer.ReadMaterials();
        importer.ReadNodes();
        Require(std::ranges::all_of(files.stamps, Current),
                "glTF 가져오기 중 파일이 변경되었습니다.");
        model->sourceDependencies.reserve(files.stamps.size() - 1);
        for (size_t index = 1; index < files.stamps.size(); ++index)
            model->sourceDependencies.push_back(Utf8(files.stamps[index].requested));
        {
            const std::lock_guard lock(cacheMutex);
            const auto found = cache.find(key);
            if (found != cache.end()) {
                if (auto existing = found->second.model.lock();
                    existing && std::ranges::all_of(found->second.files, Current)) {
                    RememberRequestedModel(requestedKey, key, existing);
                    return existing;
                }
            } else if (cache.size() >= MaxExternalFiles) {
                std::erase_if(cache,
                              [](const auto& entry) { return entry.second.model.expired(); });
                if (cache.size() >= MaxExternalFiles)
                    cache.erase(cache.begin());
            }
            cache.insert_or_assign(key, CachedModel{model, std::move(files.stamps)});
            RememberRequestedModel(requestedKey, key, model);
        }
        return model;
    } catch (const std::exception& exception) {
        error = exception.what();
        return fallback;
    }
}

} // namespace LuxEngine
