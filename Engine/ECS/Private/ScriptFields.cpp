#include <LuxEngine/ECS/ScriptFields.hpp>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <type_traits>
#include <vector>

namespace LuxEngine {
namespace {
using Json = nlohmann::ordered_json;

Json Value(const ScriptFieldValue& value) {
    return std::visit(
        [](const auto& item) -> Json {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, Vec2>)
                return Json::array({item.x, item.y});
            else if constexpr (std::is_same_v<T, Vec3>)
                return Json::array({item.x, item.y, item.z});
            else if constexpr (std::is_same_v<T, Vec4>)
                return Json::array({item.x, item.y, item.z, item.w});
            else if constexpr (std::is_same_v<T, ScriptEntityReference>)
                return item.identity ? Json{{"id", item.id}, {"identity", item.identity}}
                                     : Json(item.id);
            else
                return item;
        },
        value);
}

double Number(const Json& value) {
    if (!value.is_number())
        throw std::invalid_argument("Script field must be numeric");
    const double number = value.get<double>();
    if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<double>::max())
        throw std::invalid_argument("Script field must be finite");
    return number;
}

float Number32(const Json& value) {
    const double number = Number(value);
    if (std::abs(number) > std::numeric_limits<float>::max())
        throw std::invalid_argument("Script float field exceeds its range");
    return static_cast<float>(number);
}

ScriptFieldValue Read(const Json& field) {
    if (!field.is_object() || field.size() != 2)
        throw std::invalid_argument("Invalid script field record");
    const auto& type = field.at("type");
    const auto& value = field.at("value");
    if (!type.is_number_integer() || type.get<double>() < 0 || type.get<double>() > 7)
        throw std::invalid_argument("Invalid script field type");
    switch (type.get<int>()) {
    case 0:
        if (!value.is_boolean())
            break;
        return value.get<bool>();
    case 1:
        if (!value.is_number_integer() || value.get<double>() < std::numeric_limits<int32>::min() ||
            value.get<double>() > std::numeric_limits<int32>::max())
            break;
        return value.get<int32>();
    case 2:
        return Number32(value);
    case 3:
        if (!value.is_string() || value.get_ref<const std::string&>().size() > 16384 ||
            value.get_ref<const std::string&>().find('\0') != std::string::npos)
            break;
        return value.get<std::string>();
    case 4:
        if (!value.is_array() || value.size() != 2)
            break;
        return Vec2{Number(value[0]), Number(value[1])};
    case 5:
        if (!value.is_array() || value.size() != 3)
            break;
        return Vec3{Number(value[0]), Number(value[1]), Number(value[2])};
    case 6:
        if (!value.is_array() || value.size() != 4)
            break;
        return Vec4{Number32(value[0]), Number32(value[1]), Number32(value[2]), Number32(value[3])};
    case 7: {
        const auto validId = [](const Json& id) {
            return id.is_number_integer() && id.get<double>() >= 0 &&
                   id.get<double>() <= std::numeric_limits<uint32>::max();
        };
        if (validId(value))
            return ScriptEntityReference{value.get<uint32>()};
        if (value.is_object() && value.size() == 2 && validId(value.at("id")) &&
            value.at("identity").is_number_unsigned() && value.at("identity").get<uint64>())
            return ScriptEntityReference{value.at("id").get<uint32>(),
                                         value.at("identity").get<uint64>()};
        break;
    }
    }
    throw std::invalid_argument("Invalid script field value");
}
} // namespace

bool DeserializeScriptFields(const std::string& input, ScriptFieldMap& fields) {
    try {
        if (input.size() > 65536)
            return false;
        std::vector<std::set<std::string>> keys;
        const auto json =
            Json::parse(input, [&](int depth, Json::parse_event_t event, Json& parsed) {
                if (depth > 8)
                    throw std::invalid_argument("Script field data is too deeply nested");
                if (event == Json::parse_event_t::object_start)
                    keys.emplace_back();
                else if (event == Json::parse_event_t::key) {
                    if (keys.empty() || !keys.back().insert(parsed.get<std::string>()).second)
                        throw std::invalid_argument("Duplicate script field key");
                } else if (event == Json::parse_event_t::object_end)
                    keys.pop_back();
                return true;
            });
        if (!json.is_object() || json.size() > 128)
            return false;
        ScriptFieldMap next;
        for (const auto& [name, value] : json.items()) {
            if (name.empty() || name.size() > 256 || name.find('\0') != std::string::npos)
                return false;
            next.emplace(name, Read(value));
        }
        fields = std::move(next);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool SerializeScriptFields(const ScriptFieldMap& fields, std::string& output) {
    try {
        Json json = Json::object();
        for (const auto& [name, value] : fields)
            json[name] = {{"type", value.index()}, {"value", Value(value)}};
        const auto serialized = json.dump();
        ScriptFieldMap validated;
        if (!DeserializeScriptFields(serialized, validated))
            return false;
        output = serialized;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
} // namespace LuxEngine
