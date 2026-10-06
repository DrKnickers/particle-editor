#ifndef OBSERVATION_SPEC_H
#define OBSERVATION_SPEC_H

#include "PreviewScenario.h"
#include <cstdint>

struct ObservationSpec
{
    int schemaVersion = 1;
    PreviewScenario scenario;
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

inline std::string ValidateObservationSpec(const ObservationSpec& spec)
{
    const std::string problem = ValidatePreviewScenario(spec.scenario);
    if (!problem.empty()) return problem;
    if (spec.schemaVersion != 1) return "schemaVersion must be 1";
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
