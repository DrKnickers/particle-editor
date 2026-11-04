#pragma once

#include "simulation/PreviewScenario.h"

namespace host
{
// Leaves the output untouched on failure; returns the first problem.
std::string ParsePreviewScenario(const std::string& text, PreviewScenario& scenario);
std::string SerializePreviewScenario(const PreviewScenario& scenario);
} // namespace host
