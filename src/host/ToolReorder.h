#ifndef TOOL_REORDER_H
#define TOOL_REORDER_H

// `--tool reorder`: move one root emitter (with its subtree) to another root
// position and write the saved result. It authors the defect benchmark's
// draw-order cases (docs/VFX_BENCHMARK.md); it is not a `--tool patch` op,
// whose vocabulary leaves structural edits out.
//
// The request (UTF-8 JSON):
//
//   { "schema": 1, "sourceDocumentHash": "...",
//     "move": { "index": 2, "name": "Smoke" }, "toRoot": 0 }
//
// `move` names the root by its emitter position and name; `toRoot` is the gap
// it moves to among the roots, as the editor's drag-reorder takes it (gap K is
// before root K; the root count is below the last). The move goes through
// ParticleSystem::moveEmitterToRootIndex, the editor's own reorder.
//
// The candidate is checked before --out is written: it reloads without a
// graph repair, a save of the reload reproduces it, and it is exactly the
// permutation the report's `map` (source position -> candidate position)
// describes: every emitter equal by identity, with its index, parent and
// spawn slots mapped. A target that is not a root, is linked, or would not
// move is refused.

#include <string>
#include "host/third_party/nlohmann/json.hpp"

namespace host
{
// Runs a reorder of `source` (its bytes and their SHA-256) with `request`'s
// bytes, writing the candidate to `out` on success. Fills `report` whatever
// the outcome and returns the exit code (ToolIO.h).
int RunReorder(const std::string& source, const std::string& sourceHash, const std::string& request,
               const std::wstring& out, nlohmann::json* report);
}

#endif // TOOL_REORDER_H
