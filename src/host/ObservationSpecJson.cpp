#include "ObservationSpecJson.h"
#include "PreviewScenarioJson.h"
#include "host/third_party/nlohmann/json.hpp"
#include <limits>
#include <stdexcept>

namespace {

using nlohmann::json;

bool KnownField(const std::string& key, const std::string& level)
{
    if (level == "observation")
        return key == "schemaVersion" || key == "scenario" || key == "contentLayers" || key == "lifecycle" ||
            key == "output";
    if (level == "lifecycle")
        return key == "type" || key == "quietTailFrames" || key == "warmupFrames" || key == "reviewFrames";
    return key == "strideInterval" || key == "playbackStride" || key == "maxBytes";
}

void Require(bool condition, const std::string& field, const char* problem)
{
    if (!condition) throw std::runtime_error(field + " " + problem);
}

void CheckFields(const json& object, const std::string& level)
{
    Require(object.is_object(), level, "must be a JSON object");
    for (auto it = object.begin(); it != object.end(); ++it)
        Require(KnownField(it.key(), level), level + "." + it.key(), "is not a supported observation field");
}

template<class T>
void ReadInteger(const json& object, const char* field, const char* level, T& value)
{
    if (!object.contains(field)) return;
    const json& input = object.at(field);
    const std::string name = std::string(level) + "." + field;
    Require(input.is_number_integer(), name, "must be an integer");
    // Comparing integer domains avoids rounding UINT64_MAX up to 2^64.
    if (input.is_number_unsigned())
        Require(input.get<std::uint64_t>() <= static_cast<std::uint64_t>((std::numeric_limits<T>::max)()),
                name, "is out of range");
    else
    {
        const std::int64_t number = input.get<std::int64_t>();
        if constexpr (std::numeric_limits<T>::is_signed)
            Require(number >= (std::numeric_limits<T>::lowest)() &&
                    number <= (std::numeric_limits<T>::max)(), name, "is out of range");
        else
            Require(number >= 0, name, "is out of range");
    }
    value = input.get<T>();
}

} // namespace

std::string host::ParseObservationSpec(const std::string& text, ObservationSpec& spec)
{
    try
    {
        const json object = json::parse(text);
        CheckFields(object, "observation");
        ObservationSpec parsed;
        Require(object.contains("scenario"), "scenario", "must be a JSON object");
        const std::string scenarioProblem = ParsePreviewScenario(object.at("scenario").dump(), parsed.scenario);
        if (!scenarioProblem.empty()) return scenarioProblem;
        ReadInteger(object, "schemaVersion", "observation", parsed.schemaVersion);
        if (object.contains("contentLayers"))
        {
            const json& layers = object.at("contentLayers");
            Require(layers.is_array(), "contentLayers", "must be an array of folder paths");
            Require(layers.size() <= 16, "contentLayers", "allows at most 16 layers");
            for (size_t i = 0; i < layers.size(); ++i)
            {
                Require(layers[i].is_string(), "contentLayers[" + std::to_string(i) + "]", "must be a string");
                parsed.contentLayers.push_back(layers[i].get<std::string>());
            }
            parsed.hasContentLayers = true;
        }
        if (object.contains("lifecycle"))
        {
            const json& lifecycle = object.at("lifecycle");
            CheckFields(lifecycle, "lifecycle");
            if (lifecycle.contains("type"))
            {
                Require(lifecycle.at("type").is_string(), "lifecycle.type", "must be a string");
                const std::string type = lifecycle.at("type").get<std::string>();
                Require(type == "oneshot" || type == "continuous", "lifecycle.type", "must be oneshot or continuous");
                parsed.lifecycle.type = type == "oneshot" ? ObservationSpec::Lifecycle::Type::Oneshot
                    : ObservationSpec::Lifecycle::Type::Continuous;
            }
            ReadInteger(lifecycle, "quietTailFrames", "lifecycle", parsed.lifecycle.quietTailFrames);
            ReadInteger(lifecycle, "warmupFrames", "lifecycle", parsed.lifecycle.warmupFrames);
            ReadInteger(lifecycle, "reviewFrames", "lifecycle", parsed.lifecycle.reviewFrames);
        }
        if (object.contains("output"))
        {
            const json& output = object.at("output");
            CheckFields(output, "output");
            ReadInteger(output, "strideInterval", "output", parsed.output.strideInterval);
            ReadInteger(output, "playbackStride", "output", parsed.output.playbackStride);
            ReadInteger(output, "maxBytes", "output", parsed.output.maxBytes);
        }
        const std::string problem = ValidateObservationSpec(parsed);
        if (!problem.empty()) return problem;
        spec = parsed;
        return "";
    }
    catch (const std::exception& error)
    {
        return error.what();
    }
}

std::string host::SerializeObservationSpec(const ObservationSpec& spec)
{
    const std::string problem = ValidateObservationSpec(spec);
    if (!problem.empty()) throw std::runtime_error(problem);
    json object = {
        { "schemaVersion", spec.schemaVersion },
        { "scenario", json::parse(SerializePreviewScenario(spec.scenario)) },
        { "lifecycle", {
            { "type", spec.lifecycle.type == ObservationSpec::Lifecycle::Type::Oneshot ? "oneshot" : "continuous" },
            { "quietTailFrames", spec.lifecycle.quietTailFrames },
            { "warmupFrames", spec.lifecycle.warmupFrames },
            { "reviewFrames", spec.lifecycle.reviewFrames }
        } },
        { "output", {
            { "strideInterval", spec.output.strideInterval },
            { "playbackStride", spec.output.playbackStride },
            { "maxBytes", spec.output.maxBytes }
        } }
    };
    if (spec.hasContentLayers) object["contentLayers"] = spec.contentLayers;
    return object.dump();
}
