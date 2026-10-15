#ifndef OBSERVATION_SPEC_H
#define OBSERVATION_SPEC_H

#include "PreviewScenario.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

struct ObservationSpec
{
    int schemaVersion = 1;
    PreviewScenario scenario;
    // Schema 2 only: the content-layer stack for this run (absolute folders,
    // UTF-8, front = highest precedence). It replaces whatever stack the run
    // would otherwise use and is never persisted. Present and empty = unmodded.
    bool hasContentLayers = false;
    std::vector<std::string> contentLayers;
    bool hasCamera = false;
    struct Camera
    {
        std::array<float, 3> position = { 0, -250, 125 };
        std::array<float, 3> target = { 0, 0, 0 };
        std::array<float, 3> up = { 0, 0, 1 };
    } camera;
    struct Lifecycle
    {
        enum class Type { Oneshot, Continuous };
        Type type = Type::Oneshot;
        int quietTailFrames = 15;
        int warmupFrames = 0;
        int reviewFrames = 1;
    } lifecycle;
    struct Output
    {
        int strideInterval = 15;
        int playbackStride = 0;
        std::uint64_t maxBytes = 200ull * 1024 * 1024;
    } output;
};

// The caller validates before using the continuous sum as an int cap.
inline int ObservationRunCap(const ObservationSpec& spec)
{
    return spec.lifecycle.type == ObservationSpec::Lifecycle::Type::Continuous
        ? spec.lifecycle.warmupFrames + spec.lifecycle.reviewFrames : spec.scenario.maxFrames;
}

// Schema 2 is the schema that carries contentLayers, so each implies the
// other. Each layer is a drive-rooted folder path; whether it exists is the
// launcher's check, since this one stays file-system free.
inline std::string ValidateObservationContentLayers(const ObservationSpec& spec)
{
    if (spec.hasContentLayers != (spec.schemaVersion == 2))
        return spec.hasContentLayers ? "contentLayers requires schemaVersion 2"
                                     : "schemaVersion 2 requires contentLayers";
    if (spec.contentLayers.size() > 16) return "contentLayers allows at most 16 layers";
    if (!spec.hasContentLayers && !spec.contentLayers.empty()) return "contentLayers is set without hasContentLayers";
    const auto lower = [](std::string text)
    {
        for (char& c : text)
        {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c == '/') c = '\\';
        }
        while (text.size() > 3 && text.back() == '\\') text.pop_back();
        return text;
    };
    for (size_t i = 0; i < spec.contentLayers.size(); ++i)
    {
        const std::string& layer = spec.contentLayers[i];
        const std::string field = "contentLayers[" + std::to_string(i) + "]";
        // Short enough that a layer + Data\Art\Textures\ + a texture name stays
        // inside MAX_PATH, the limit the content lookups run under.
        if (layer.size() < 3 || layer.size() > 160) return field + " must be a folder path of 3 to 160 bytes";
        const bool drive = ((layer[0] >= 'A' && layer[0] <= 'Z') || (layer[0] >= 'a' && layer[0] <= 'z')) &&
            layer[1] == ':' && (layer[2] == '\\' || layer[2] == '/');
        if (!drive) return field + " must be an absolute drive path";
        for (const char c : layer)
            if (static_cast<unsigned char>(c) < 0x20) return field + " must not contain control characters";
        // Win32 drops a component's trailing dots and spaces when it opens the
        // folder itself but not a file below it, so "D:\gen ." passes a
        // folder check yet resolves no content. "." and ".." alias too.
        size_t start = 3;
        while (start < layer.size())
        {
            size_t end = layer.find_first_of("\\/", start);
            if (end == std::string::npos) end = layer.size();
            const std::string part = layer.substr(start, end - start);
            if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ')
                return field + " has an empty, dot or space-ended folder name";
            start = end + 1;
        }
        for (size_t j = 0; j < i; ++j)
            if (lower(spec.contentLayers[j]) == lower(layer)) return field + " repeats an earlier layer";
    }
    return "";
}

// The engine builds its view in floats, so a camera must stay well inside
// float range and clear of a degenerate basis: bounded components, a real
// distance, an up of real length, and an up at least ~0.06 degrees off the view.
inline std::string ValidateObservationCamera(const ObservationSpec& spec)
{
    if (!spec.hasCamera) return "";
    const double kLimit = 1.0e6, kMinDistance = 0.01, kMinUp = 1.0e-6, kMinSine = 1.0e-3;
    const auto& camera = spec.camera;
    for (size_t i = 0; i < 3; ++i)
    {
        if (!std::isfinite(camera.position[i])) return "camera.position must be finite";
        if (!std::isfinite(camera.target[i])) return "camera.target must be finite";
        if (!std::isfinite(camera.up[i])) return "camera.up must be finite";
        if (std::fabs(camera.position[i]) > kLimit) return "camera.position must be within 1000000 of the origin";
        if (std::fabs(camera.target[i]) > kLimit) return "camera.target must be within 1000000 of the origin";
        if (std::fabs(camera.up[i]) > kLimit) return "camera.up must be within 1000000 of the origin";
    }
    double d[3], u[3];
    for (size_t i = 0; i < 3; ++i)
    {
        d[i] = static_cast<double>(camera.target[i]) - camera.position[i];
        u[i] = camera.up[i];
    }
    const double distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const double upLength = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (distance < kMinDistance) return "camera.position and camera.target must differ by at least 0.01";
    if (upLength < kMinUp) return "camera.up must have nonzero length";
    const double cx = d[1] * u[2] - d[2] * u[1], cy = d[2] * u[0] - d[0] * u[2], cz = d[0] * u[1] - d[1] * u[0];
    if (std::sqrt(cx * cx + cy * cy + cz * cz) < kMinSine * distance * upLength)
        return "camera.up must not be parallel to the view direction";
    return "";
}

inline std::string ValidateObservationSpec(const ObservationSpec& spec)
{
    const std::string problem = ValidatePreviewScenario(spec.scenario);
    if (!problem.empty()) return problem;
    if (spec.schemaVersion != 1 && spec.schemaVersion != 2) return "schemaVersion must be 1 or 2";
    const std::string layersProblem = ValidateObservationContentLayers(spec);
    if (!layersProblem.empty()) return layersProblem;
    const std::string cameraProblem = ValidateObservationCamera(spec);
    if (!cameraProblem.empty()) return cameraProblem;
    if (spec.scenario.maxFrames > 3600) return "scenario.maxFrames must be within [1, 3600]";
    if (spec.lifecycle.type != ObservationSpec::Lifecycle::Type::Oneshot &&
        spec.lifecycle.type != ObservationSpec::Lifecycle::Type::Continuous)
        return "lifecycle.type must be oneshot or continuous";
    if (spec.lifecycle.quietTailFrames < 1 || spec.lifecycle.quietTailFrames > 600)
        return "lifecycle.quietTailFrames must be within [1, 600]";
    if (spec.lifecycle.type == ObservationSpec::Lifecycle::Type::Continuous)
    {
        if (spec.lifecycle.warmupFrames < 0) return "lifecycle.warmupFrames must not be negative";
        if (spec.lifecycle.reviewFrames < 1) return "lifecycle.reviewFrames must be at least 1";
        if (static_cast<std::int64_t>(spec.lifecycle.warmupFrames) + spec.lifecycle.reviewFrames > spec.scenario.maxFrames)
            return "lifecycle.warmupFrames + lifecycle.reviewFrames must not exceed scenario.maxFrames";
    }
    if (spec.output.strideInterval < 1 || spec.output.strideInterval > 600)
        return "output.strideInterval must be within [1, 600]";
    if (spec.output.playbackStride < 0 || spec.output.playbackStride > 4)
        return "output.playbackStride must be 0 or within [1, 4]";
    if (spec.output.maxBytes < 1024ull * 1024 || spec.output.maxBytes > 2ull * 1024 * 1024 * 1024)
        return "output.maxBytes must be within [1 MiB, 2 GiB]";
    const int windowMax = spec.lifecycle.type == ObservationSpec::Lifecycle::Type::Continuous
        ? spec.lifecycle.reviewFrames : ObservationRunCap(spec);
    if (windowMax / spec.output.strideInterval + 2 > 64)
        return "output.strideInterval allows more than 64 stride frames";
    return "";
}

#endif
