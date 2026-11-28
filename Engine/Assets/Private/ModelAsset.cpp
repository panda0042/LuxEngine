#include <LuxEngine/Assets/ModelAsset.hpp>
#include <algorithm>
#include <string_view>

namespace LuxEngine {

bool ModelAsset::IsGltfPath(const std::string& filepath) {
    const auto dot = filepath.find_last_of("./\\");
    if (dot == std::string::npos || filepath[dot] != '.')
        return false;
    const auto extension = std::string_view(filepath).substr(dot);
    const auto matches = [&](std::string_view target) {
        return std::ranges::equal(extension, target, [](char a, char b) {
            return (a >= 'A' && a <= 'Z' ? a + ('a' - 'A') : a) == b;
        });
    };
    return matches(".gltf") || matches(".glb");
}

Ref<ModelAsset> ModelAsset::Load(const std::string& filepath, std::string& error) {
    return IsGltfPath(filepath) ? ReadGltf(filepath, error) : LoadFbx(filepath, error);
}

ModelAsset::ModelAsset() = default;

ModelAsset::~ModelAsset() = default;

} // namespace LuxEngine
