#include <LuxEngine/Runtime/ProjectManifest.hpp>
#include <array>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>

namespace LuxEngine {
namespace {
bool ParseDependencyVersion(std::string_view text, std::array<unsigned, 4>& version) {
    for (auto& number : version) {
        const auto dot = text.find('.');
        const auto part = text.substr(0, dot);
        if (part.empty())
            return false;
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), number);
        if (error != std::errc{} || end != part.data() + part.size())
            return false;
        if (dot == std::string_view::npos)
            return true;
        text.remove_prefix(dot + 1);
    }
    return false;
}
} // namespace

bool IsValidProjectDependencyVersion(std::string_view version) {
    if (version.empty())
        return true;
    const auto range = version.find("...");
    std::array<unsigned, 4> lower{}, upper{};
    return ParseDependencyVersion(version.substr(0, range), lower) &&
           (range == std::string_view::npos ||
            (ParseDependencyVersion(version.substr(range + 3), upper) && upper >= lower));
}

bool IsValidProjectDependency(std::string_view package, std::string_view version) {
    const auto letter = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    };
    if (package.empty() || package.size() > 64 || !letter(package.front()))
        return false;
    for (const char c : package)
        if (!letter(c) && (c < '0' || c > '9') && c != '_')
            return false;
    return IsValidProjectDependencyVersion(version);
}

std::string_view ProjectManifestContent(std::string_view data) {
    if (data.starts_with("\xef\xbb\xbf"))
        data.remove_prefix(3);
    const auto first = data.find_first_not_of(" \t\r\n");
    return first == std::string_view::npos ? std::string_view{} : data.substr(first);
}

bool ParseProjectManifestJson(std::string_view data, ProjectManifestData& manifest) {
    using Json = nlohmann::json;
    if (data.empty() || data.size() > 65536)
        return false;
    try {
        std::vector<std::set<std::string>> keys;
        const auto root =
            Json::parse(data, [&](int depth, Json::parse_event_t event, Json& parsed) {
                if (depth > 16)
                    throw std::runtime_error("Project JSON nesting limit exceeded");
                if (event == Json::parse_event_t::object_start)
                    keys.emplace_back();
                else if (event == Json::parse_event_t::key &&
                         (keys.empty() || !keys.back().insert(parsed.get<std::string>()).second))
                    throw std::runtime_error("Duplicate project JSON field");
                else if (event == Json::parse_event_t::object_end)
                    keys.pop_back();
                return true;
            });
        if (!root.is_object() || root.at("format") != "LuxEngine.Project" ||
            !root.at("version").is_number_integer() || root.at("version") != 1)
            return false;

        ProjectManifestData parsed;
        parsed.name = root.at("name").get<std::string>();
        parsed.engineVersion = root.at("engine_version").get<std::string>();
        parsed.projectVersion = root.at("project_version").get<std::string>();
        parsed.templateName = root.at("template").get<std::string>();
        parsed.startScene = root.at("start_scene").get<std::string>();
        const auto& settings = root.at("settings");
        parsed.icon = settings.at("icon").get<std::string>();
        parsed.snapToGrid = settings.at("snap_to_grid").get<bool>();
        const auto readUnsigned = [](const Json& value, unsigned max, unsigned& output) {
            if (!value.is_number_integer() || (!value.is_number_unsigned() && value < 0) ||
                value > max)
                return false;
            output = value.get<unsigned>();
            return true;
        };
        if (!readUnsigned(settings.at("autosave_seconds"), 3600, parsed.autosaveSeconds) ||
            !readUnsigned(settings.at("backup_limit"), 100, parsed.backupLimit) ||
            parsed.backupLimit == 0)
            return false;

        const auto& dependencies = root.at("dependencies");
        if (!dependencies.is_array() || dependencies.size() > 64)
            return false;
        for (const auto& dependency : dependencies) {
            auto package = dependency.at("package").get<std::string>();
            auto version = dependency.at("version").get<std::string>();
            if (!IsValidProjectDependency(package, version))
                return false;
            parsed.dependencies.emplace_back(std::move(package), std::move(version));
        }
        manifest = std::move(parsed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool ReadProjectStartScene(const std::filesystem::path& contentRoot,
                           std::filesystem::path& sceneFile, std::string& error) {
    error.clear();
    try {
        namespace fs = std::filesystem;
        std::error_code ec;
        auto manifest = contentRoot / "project.luxproject";
        if (!fs::exists(manifest, ec))
            manifest = contentRoot / "project.luxproj";
        const auto size = fs::file_size(manifest, ec);
        if (ec || size == 0 || size > 65536)
            throw std::runtime_error("프로젝트 실행 정보를 읽을 수 없습니다.");
        std::ifstream input(manifest, std::ios::binary);
        std::string data(static_cast<size_t>(size), '\0');
        if (!input.read(data.data(), static_cast<std::streamsize>(data.size())) ||
            input.peek() != std::char_traits<char>::eof())
            throw std::runtime_error("프로젝트 실행 정보를 읽을 수 없습니다.");
        std::string scene;
        const auto text = ProjectManifestContent(data);
        if (text.starts_with('{')) {
            ProjectManifestData project;
            if (!ParseProjectManifestJson(data, project))
                throw std::runtime_error("프로젝트 실행 정보 형식이 올바르지 않습니다.");
            scene = std::move(project.startScene);
        } else {
            std::istringstream legacy{std::string(text)};
            std::string magic;
            int version = 0;
            if (!(legacy >> magic >> version) || magic != "LUXPROJECT" ||
                (version != 1 && version != 2))
                throw std::runtime_error("프로젝트 실행 정보 형식이 올바르지 않습니다.");
            std::string line;
            bool found = false;
            while (std::getline(legacy, line)) {
                std::istringstream field(line);
                std::string key, trailing;
                if (!(field >> key) || key != "start_scene")
                    continue;
                if (found || !(field >> std::quoted(scene)) || field >> trailing)
                    throw std::runtime_error("시작 씬 설정이 올바르지 않습니다.");
                found = true;
            }
            if (!found || legacy.bad())
                throw std::runtime_error("시작 씬을 지정해 주세요.");
        }
        const fs::path relative(
            std::u8string(reinterpret_cast<const char8_t*>(scene.data()), scene.size()));
        if (relative.empty() || relative.is_absolute() || relative.extension() != ".lux" ||
            scene.find('\\') != std::string::npos || scene.find(':') != std::string::npos ||
            scene.find('\0') != std::string::npos)
            throw std::runtime_error("시작 씬 경로가 올바르지 않습니다.");
        for (const auto& part : relative)
            if (part == ".." || part == ".")
                throw std::runtime_error("시작 씬 경로가 콘텐츠 폴더를 벗어났습니다.");
        const auto root = fs::canonical(contentRoot);
        const auto target = fs::canonical(root / relative);
        const auto resolved = target.lexically_relative(root);
        if (resolved.empty() || !fs::is_regular_file(target))
            throw std::runtime_error("시작 씬 파일을 찾을 수 없습니다.");
        for (const auto& part : resolved)
            if (part == "..")
                throw std::runtime_error("시작 씬 경로가 콘텐츠 폴더를 벗어났습니다.");
        sceneFile = target;
        return true;
    } catch (const std::exception&) {
        error = "프로젝트 실행 정보 또는 시작 씬 경로가 올바르지 않습니다.";
        return false;
    }
}
} // namespace LuxEngine
