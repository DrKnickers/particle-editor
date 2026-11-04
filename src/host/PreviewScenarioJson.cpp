#include "PreviewScenarioJson.h"
#include "host/third_party/nlohmann/json.hpp"
#include <windows.h>
#include <climits>
#include <limits>
#include <stdexcept>

namespace
{

using nlohmann::json;

bool KnownField(const std::string& key)
{
    const char* fields[] = {"schemaVersion",
                            "aloPath",
                            "seed",
                            "algorithmVersion",
                            "clockAnchor",
                            "spawnPosition",
                            "spawnVelocity",
                            "maxFrames",
                            "overloadGuardEnabled",
                            "overloadGuardLimit",
                            "estimatedParticlesPerInstance"};
    for (const char* field : fields)
        if (key == field) return true;
    return false;
}

void Require(bool condition, const std::string& field, const char* problem)
{
    if (!condition) throw std::runtime_error(field + " " + problem);
}

template <class T> void ReadInteger(const json& object, const char* field, T& value)
{
    if (!object.contains(field)) return;
    const json& input = object.at(field);
    Require(input.is_number_integer(), field, "must be an integer");
    const double number = input.get<double>();
    Require(number >= (std::numeric_limits<T>::lowest)() && number <= (std::numeric_limits<T>::max)(), field,
            "is out of range");
    value = input.get<T>();
}

template <class T> void ReadNumber(const json& input, const std::string& field, T& value)
{
    Require(input.is_number(), field, "must be a number");
    const double number = input.get<double>();
    Require(std::isfinite(number) && number >= -(std::numeric_limits<T>::max)() &&
                number <= (std::numeric_limits<T>::max)(),
            field, "must be finite and representable");
    value = static_cast<T>(number);
}

void ReadVector(const json& object, const char* field, D3DXVECTOR3& value)
{
    if (!object.contains(field)) return;
    const json& input = object.at(field);
    Require(input.is_array() && input.size() == 3, field, "must be an array of three numbers");
    ReadNumber(input[0], std::string(field) + ".x", value.x);
    ReadNumber(input[1], std::string(field) + ".y", value.y);
    ReadNumber(input[2], std::string(field) + ".z", value.z);
}

std::wstring DecodePath(const std::string& text)
{
    Require(text.size() <= static_cast<std::size_t>(INT_MAX), "aloPath", "is too long");
    if (text.empty()) return L"";
    const int count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), NULL, 0);
    Require(count != 0, "aloPath", "must be valid UTF-8");
    std::wstring result(count, L'\0');
    Require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), &result[0],
                                count) == count,
            "aloPath", "could not be decoded");
    return result;
}

std::string EncodePath(const std::wstring& text)
{
    Require(text.size() <= static_cast<std::size_t>(INT_MAX), "aloPath", "is too long");
    if (text.empty()) return "";
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                          NULL, 0, NULL, NULL);
    Require(count != 0, "aloPath", "must be valid UTF-16");
    std::string result(count, '\0');
    Require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), &result[0],
                                count, NULL, NULL) == count,
            "aloPath", "could not be encoded");
    return result;
}

} // namespace

std::string host::ParsePreviewScenario(const std::string& text, PreviewScenario& scenario)
{
    try
    {
        const json object = json::parse(text);
        Require(object.is_object(), "scenario", "must be a JSON object");
        for (auto it = object.begin(); it != object.end(); ++it)
            Require(KnownField(it.key()), it.key(), "is not a supported scenario field");
        PreviewScenario parsed;
        ReadInteger(object, "schemaVersion", parsed.schemaVersion);
        ReadInteger(object, "seed", parsed.seed);
        ReadInteger(object, "algorithmVersion", parsed.algorithmVersion);
        ReadInteger(object, "maxFrames", parsed.maxFrames);
        ReadInteger(object, "overloadGuardLimit", parsed.overloadGuardLimit);
        if (object.contains("aloPath"))
        {
            Require(object.at("aloPath").is_string(), "aloPath", "must be a string");
            parsed.aloPath = DecodePath(object.at("aloPath").get<std::string>());
        }
        if (object.contains("clockAnchor"))
        {
            ReadNumber(object.at("clockAnchor"), "clockAnchor", parsed.clockAnchor);
            Require(object.at("clockAnchor").get<double>() == 0.0, "clockAnchor", "must be exactly zero");
        }
        ReadVector(object, "spawnPosition", parsed.spawnPosition);
        ReadVector(object, "spawnVelocity", parsed.spawnVelocity);
        if (object.contains("overloadGuardEnabled"))
        {
            Require(object.at("overloadGuardEnabled").is_boolean(), "overloadGuardEnabled", "must be a boolean");
            parsed.overloadGuardEnabled = object.at("overloadGuardEnabled").get<bool>();
        }
        if (object.contains("estimatedParticlesPerInstance"))
            ReadNumber(object.at("estimatedParticlesPerInstance"), "estimatedParticlesPerInstance",
                       parsed.estimatedParticlesPerInstance);
        const std::string problem = ValidatePreviewScenario(parsed);
        if (!problem.empty()) return problem;
        scenario = parsed;
        return "";
    }
    catch (const std::exception& error)
    {
        return error.what();
    }
}

std::string host::SerializePreviewScenario(const PreviewScenario& scenario)
{
    const std::string problem = ValidatePreviewScenario(scenario);
    if (!problem.empty()) throw std::runtime_error(problem);
    // Float-to-double promotion is exact; json's round-trip double formatting
    // preserves every float bit, including negative zero, on readback.
    const json object = {
        {"schemaVersion", scenario.schemaVersion},
        {"aloPath", EncodePath(scenario.aloPath)},
        {"seed", scenario.seed},
        {"algorithmVersion", scenario.algorithmVersion},
        {"clockAnchor", scenario.clockAnchor},
        {"spawnPosition", {scenario.spawnPosition.x, scenario.spawnPosition.y, scenario.spawnPosition.z}},
        {"spawnVelocity", {scenario.spawnVelocity.x, scenario.spawnVelocity.y, scenario.spawnVelocity.z}},
        {"maxFrames", scenario.maxFrames},
        {"overloadGuardEnabled", scenario.overloadGuardEnabled},
        {"overloadGuardLimit", scenario.overloadGuardLimit},
        {"estimatedParticlesPerInstance", scenario.estimatedParticlesPerInstance}};
    return object.dump();
}
