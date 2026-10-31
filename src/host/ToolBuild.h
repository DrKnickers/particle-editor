#ifndef TOOL_BUILD_H
#define TOOL_BUILD_H

// `--tool build`: a new particle system from a spec, written as a verified
// .alo.
//
// The spec (UTF-8 JSON, at most 16 MiB, 64 entries and 256 mutations an
// emitter):
//
//   { "schema": 1,
//     "emitters": [ { "name": "CORE_T1_SMOKE_EARLY", "mutations": [ ... ],
//                     "lifeChild": { "name": "...", "mutations": [ ... ] } }, ... ] }
//
// Each entry is a root emitter, in order, followed by its lifetime child if
// it has one (a child has no children of its own). Every emitter starts as
// the editor's new emitter, named by its entry; its mutations are patch's
// ops (ToolOps.h) but `rename`, applied in order. Names are distinct. The
// internal name is the --out file's, as a save stamps it.
//
// Refused before anything is written (exit 5): a refused op, a value that is
// not finite, a document that would not reload, a requested value that saves
// as the new emitter's, and a save that would change a field outside the
// edits' footprint. The candidate is staged next to --out and verified there:
// 4 the staged bytes are the verified document, 3 it reloads and saves back
// byte for byte, 5 its emitters, names and tree are the spec's, with no
// loader repair and no link group, 1 every edited field persisted as
// projected, 2 every other field is a new emitter's. Then it replaces --out.

#include <string>
#include "host/third_party/nlohmann/json.hpp"

namespace host
{
struct BuildHooks
{
    // Test seam: called with the staged file's path after it is written and
    // before it is verified.
    void (*afterStage)(const std::wstring& staged) = nullptr;
};

// Builds the system `spec` (its bytes) describes, writing it to `out` on
// success. Fills `report` whatever the outcome and returns the exit code
// (ToolIO.h).
int RunBuild(const std::string& spec, const std::wstring& out, const BuildHooks& hooks, nlohmann::json* report);
}

#endif // TOOL_BUILD_H
