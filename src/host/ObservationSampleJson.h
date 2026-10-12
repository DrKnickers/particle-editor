#ifndef OBSERVATION_SAMPLE_JSON_H
#define OBSERVATION_SAMPLE_JSON_H

#include "host/third_party/nlohmann/json.hpp"
#include "simulation/ObservationLifecycle.h"
#include <string>

// One telemetry/frames.jsonl line. Shared with the lifecycle test so the
// collection byte budget is checked against the bytes the runner writes.
inline nlohmann::json ObservationSampleJson(const ObservationSample& s)
{
    return {{"frame", s.frame}, {"instances", s.instances}, {"emitters", s.emitters},
        {"particles", s.particles}, {"perEmitter", s.perEmitter},
        {"rngDrawCount", s.rngDrawCount}, {"rngState", s.rngState},
        {"runtimeGuardRefusals", s.runtimeGuardRefusals}, {"hardGuardRefusals", s.hardGuardRefusals},
        {"indexCapRefusals", s.indexCapRefusals}, {"overloadActive", s.overloadActive}, {"spawnerIdle", s.spawnerIdle}};
}

inline std::string ObservationSampleLine(const ObservationSample& s)
{
    return ObservationSampleJson(s).dump() + "\n";
}

#endif
