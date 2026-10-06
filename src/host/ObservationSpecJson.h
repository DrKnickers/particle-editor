#pragma once

#include "simulation/ObservationSpec.h"

namespace host
{
    // Leaves the output untouched on failure; returns the first problem.
    std::string ParseObservationSpec(const std::string& text, ObservationSpec& spec);
    std::string SerializeObservationSpec(const ObservationSpec& spec);
}
