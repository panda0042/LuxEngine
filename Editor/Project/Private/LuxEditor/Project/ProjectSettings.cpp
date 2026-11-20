#include <LuxEditor/Project/EditorProject.hpp>
#include <LuxEditor/Project/PathEncoding.hpp>
#include <LuxEditor/Scene/SceneAuthoringIO.hpp>
#include <LuxUI/Widgets.hpp>

#include <LuxEngine/Scene/Scene.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <string_view>
#include <system_error>

namespace LuxEditor {
namespace {
bool WithinProject(const std::filesystem::path& relative) {
    if (relative.empty() || relative.is_absolute())
        return false;
    const auto first = PathUtf8(*relative.begin());
    if (first == "build" || first == "out" || first.starts_with("cmake-build"))
        return false;
    for (const auto& part : relative)
        if (part == ".." || part == ".git" || part == ".lux")
            return false;
    return true;
}

uint32_t IcnsLength(const std::array<unsigned char, 8>& header) {
    return (uint32_t(header[4]) << 24) | (uint32_t(header[5]) << 16) | (uint32_t(header[6]) << 8) |
           uint32_t(header[7]);
}

bool ValidateIcns(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    std::array<unsigned char, 8> header{};
    input.seekg(0);
    const auto readHeader = [&] {
        return bool(input.read(reinterpret_cast<char*>(header.data()), header.size()));
    };
    if (!readHeader() ||
        std::string_view(reinterpret_cast<const char*>(header.data()), 4) != "icns")
        return false;
    const auto declared = IcnsLength(header);
    if (declared < 16 || size != static_cast<std::streamoff>(declared))
        return false;

    LuxUI::ImageReader reader(LuxUI::String::fromUtf8(PathUtf8(path)));
    reader.setDecideFormatFromContent(true);
    bool hasImage = !reader.read().isNull();
    constexpr std::array imageChunks{"icp4", "icp5", "icp6", "ic07", "ic08", "ic09",
                                     "ic10", "ic11", "ic12", "ic13", "ic14"};
    uint32_t remaining = declared - 8;
    while (remaining) {
        if (remaining < 8 || !readHeader())
            return false;
        const auto chunk = IcnsLength(header);
        if (chunk < 8 || chunk > remaining)
            return false;
        remaining -= chunk;
        const auto payload = chunk - 8;
        const std::string_view type(reinterpret_cast<const char*>(header.data()), 4);
        if (!hasImage && ((type == "ICON" && payload == 128) ||
                          (type == "ICN#" && payload == 256) ||
                          (type == "ics#" && payload == 64) ||
                          (type == "ich#" && payload == 576)))
            hasImage = true;
        if (!hasImage && payload > 8 && payload <= 16 * 1024 * 1024 &&
            std::find(imageChunks.begin(), imageChunks.end(), type) != imageChunks.end()) {
            LuxUI::ByteArray encoded(payload, '\0');
            if (!input.read(encoded.data(), payload))
                return false;
            hasImage = !LuxUI::Image::fromData(encoded).isNull();
        } else {
            input.seekg(payload, std::ios::cur);
        }
        if (!input)
            return false;
    }
    return hasImage;
}
} // namespace

bool EditorProject::SetIcon(const std::filesystem::path& image, std::string& error) {
    auto settings = settings_;
    if (image.empty())
        settings.icon.clear();
    return UpdateSettings(settings, image, error);
}

bool EditorProject::UpdateSettings(const ProjectSettings& requested,
                                   const std::filesystem::path& image, std::string& error) {
    if (image.empty())
        return UpdateSettings(requested, error);
    auto settings = requested;
    error.clear();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(image, ec)) {
        error = "아이콘 이미지 파일을 찾을 수 없습니다.";
        return false;
    }
    const auto extension = PathUtf8(image.extension());
    if (extension != ".png" && extension != ".jpg" && extension != ".jpeg" && extension != ".ico" &&
        extension != ".icns") {
        error = "PNG, JPEG, ICO 또는 ICNS 이미지를 선택해 주세요.";
        return false;
    }
    const auto destination =
        file_.parent_path() / "Assets" /
        ("ProjectIcon-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + extension);
    auto temporary = destination;
    temporary += ".tmp";
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) {
        error = "아이콘을 저장할 Assets 폴더를 만들 수 없습니다.";
        return false;
    }
    std::filesystem::copy_file(image, temporary, std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        error = "아이콘 이미지를 프로젝트에 복사할 수 없습니다.";
        return false;
    }
    const bool valid = [&] {
        if (extension == ".icns")
            return ValidateIcns(temporary);
        LuxUI::ImageReader reader(LuxUI::String::fromUtf8(PathUtf8(temporary)));
        reader.setDecideFormatFromContent(true);
        return !reader.read().isNull();
    }();
    if (!valid) {
        std::filesystem::remove(temporary, ec);
        error = "아이콘 이미지 파일을 읽을 수 없습니다.";
        return false;
    }
    std::filesystem::rename(temporary, destination, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        error = "아이콘 이미지를 프로젝트에 저장할 수 없습니다.";
        return false;
    }
    settings.icon = std::filesystem::path("Assets") / destination.filename();
    if (UpdateSettings(settings, error))
        return true;
    std::filesystem::remove(destination, ec);
    return false;
}

bool EditorProject::BumpVersion(int part, std::string& error) {
    error.clear();
    if (part < 0 || part > 2) {
        error = "버전 항목이 올바르지 않습니다.";
        return false;
    }
    std::array<unsigned, 3> values{};
    std::string_view text = settings_.version;
    for (size_t index = 0; index < values.size(); ++index) {
        const auto dot = text.find('.');
        const auto token = text.substr(0, dot);
        const auto [end, ec] =
            std::from_chars(token.data(), token.data() + token.size(), values[index]);
        if (ec != std::errc{} || end != token.data() + token.size()) {
            error = "프로젝트 버전이 올바르지 않습니다.";
            return false;
        }
        text.remove_prefix(token.size() + (dot != std::string_view::npos ? 1 : 0));
    }
    if (values[static_cast<size_t>(part)] >= 9999) {
        error = "프로젝트 버전을 더 올릴 수 없습니다.";
        return false;
    }
    ++values[static_cast<size_t>(part)];
    for (size_t index = static_cast<size_t>(part) + 1; index < values.size(); ++index)
        values[index] = 0;
    auto settings = settings_;
    settings.version = std::to_string(values[0]) + "." + std::to_string(values[1]) + "." +
                       std::to_string(values[2]);
    return UpdateSettings(settings, error);
}

bool EditorProject::ContainsScene(const std::filesystem::path& sceneFile) const {
    std::error_code ec;
    const auto relative = std::filesystem::relative(sceneFile, file_.parent_path(), ec);
    return !ec && WithinProject(relative) && relative.extension() == ".lux";
}

std::filesystem::path EditorProject::GetAutosaveFile(const std::filesystem::path& sceneFile) const {
    std::error_code ec;
    const auto root = std::filesystem::canonical(file_.parent_path(), ec);
    if (ec)
        return {};
    const auto relative = std::filesystem::relative(sceneFile, root, ec);
    if (ec || !WithinProject(relative) || relative.extension() != ".lux")
        return {};
    const auto targetRelative = std::filesystem::path(".lux") / "autosave" / relative;
    const auto target = root / targetRelative;
    const auto resolved = std::filesystem::weakly_canonical(target, ec);
    // Metadata links must not redirect an autosave onto a source scene or another user's file.
    if (ec || resolved.lexically_relative(root) != targetRelative)
        return {};
    return target;
}

bool EditorProject::SaveAutosave(const std::filesystem::path& sceneFile, LuxEngine::Scene& scene,
                                 std::string& error) const {
    error.clear();
    const auto target = GetAutosaveFile(sceneFile);
    if (target.empty()) {
        error = "프로젝트 안의 장면만 자동 저장할 수 있습니다.";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        error = "장면 자동 저장 파일을 만들 수 없습니다.";
        return false;
    }
    // The scene writer already uses atomic replacement and preserves the previous file on error.
    return SaveAuthoredScene(scene, target, sceneFile, error);
}

bool EditorProject::HasNewerAutosave(const std::filesystem::path& sceneFile) const {
    const auto autosave = GetAutosaveFile(sceneFile);
    std::error_code ec;
    if (autosave.empty() || !std::filesystem::is_regular_file(autosave, ec))
        return false;
    const auto autosaveTime = std::filesystem::last_write_time(autosave, ec);
    if (ec)
        return false;
    const auto sourceTime = std::filesystem::last_write_time(sceneFile, ec);
    if (ec == std::errc::no_such_file_or_directory) {
        LuxEngine::Scene recovery;
        return recovery.Deserialize(PathUtf8(autosave));
    }
    return !ec && autosaveTime > sourceTime;
}

bool EditorProject::ClearAutosave(const std::filesystem::path& sceneFile) const {
    const auto autosave = GetAutosaveFile(sceneFile);
    if (autosave.empty())
        return false;
    std::error_code ec;
    std::filesystem::remove(autosave, ec);
    return !ec;
}

size_t EditorProject::RescanResources(std::string& error) const {
    error.clear();
    const auto root = file_.parent_path() / "Assets";
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path() / ".lux", ec);
    if (ec) {
        error = "리소스 색인 폴더를 만들 수 없습니다.";
        return 0;
    }
    const auto target = file_.parent_path() / ".lux" / "assets.index";
    auto temporary = target;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
        error = "리소스 색인을 저장할 수 없습니다.";
        return 0;
    }
    size_t count = 0;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && iterator != end) {
        if (iterator->is_regular_file(ec) && !iterator->is_symlink(ec)) {
            const auto relative = GenericPathUtf8(iterator->path().lexically_relative(root));
            output << std::quoted(relative) << ' ' << iterator->file_size(ec) << '\n';
            ++count;
        }
        iterator.increment(ec);
    }
    output.close();
    if (ec || !output) {
        error = "리소스를 재검색하지 못했습니다.";
        return 0;
    }
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        std::filesystem::remove(target, ec);
        ec.clear();
        std::filesystem::rename(temporary, target, ec);
    }
    if (ec) {
        error = "리소스 색인을 갱신하지 못했습니다.";
        return 0;
    }
    return count;
}

} // namespace LuxEditor
