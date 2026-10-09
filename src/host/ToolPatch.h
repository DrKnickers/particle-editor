#ifndef TOOL_PATCH_H
#define TOOL_PATCH_H

// `--tool patch`: apply a request's mutations to one emitter and write a
// verified candidate .alo (tasks/2026-10-07-phase4b-tool-verbs-plan.md §3.5).
//
// The request (UTF-8 JSON, at most 16 MiB and 256 mutations):
//
//   { "schema": 1,
//     "selector": { "sourceDocumentHash": "...", "rolePath": [...], "expected": {...} },
//     "mutations": [ { "op": "setProperty", "field": "lifetime", "value": 1.5 }, ... ] }
//
// Every mutation edits the selected emitter. The ops, with their keys:
//   setProperty      field (an inspect path, e.g. "groups[0].maxY"), value (number or boolean)
//   setBlendMode     value
//   setColorTexture  name          setNormalTexture  name          rename  name
//   rescale          timeScale, sizeScale (multipliers, as DoRescaleEmitter takes them)
//   setInterpolation track, interpolation ("linear" | "smooth" | "step")
//   replaceTrack     track, keys ([[time, value], ...], endpoints at 0 and 100)
//   setTrackKey      track, time, value, optional newTime (moves an interior key)
//   addTrackKey      track, time, value
// The four track ops take an optional `dependents`: the colour channels
// locked to `track`, which the edit changes too. It is required, and must
// list them exactly, when there are any.
//
// Outcomes the editor would adjust silently are refused (owner decision 7):
// a set or add whose key is not found, a border key whose time would move, an
// add the dedup would nudge, and a value that differs from the current one
// but saves as it. Setting a field to its current value is `unchanged`.
//
// The candidate is written to a unique staged sibling of --out, verified
// there (checks 1-5), and only then renamed over --out.

#include <string>
#include "host/third_party/nlohmann/json.hpp"

namespace host
{
struct PatchHooks
{
    // Test seam: called with the staged file's path after it is written and
    // before it is verified.
    void (*afterStage)(const std::wstring& staged) = nullptr;
};

// Runs a patch of `source` (its bytes and their SHA-256) with `request`'s
// bytes, writing the candidate to `out` on success. `inputPath` names the
// source for the internal-name warning. Fills `report` (§3.6) whatever the
// outcome and returns the exit code (ToolIO.h).
int RunPatch(const std::string& source, const std::string& sourceHash, const std::string& request,
             const std::wstring& inputPath, const std::wstring& out, const PatchHooks& hooks,
             nlohmann::json* report);
}

#endif // TOOL_PATCH_H
