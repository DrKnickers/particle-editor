#ifndef TOOL_OPS_H
#define TOOL_OPS_H

// The mutation ops shared by `--tool patch` (ToolPatch.h) and `--tool build`
// (ToolBuild.h): their request shape, how each applies to one emitter under
// the editor's rules, and the inspect-path and staging helpers both verify
// with. Window-free apart from the staged file.

#include <set>
#include <string>
#include <utility>
#include <vector>
#include "host/third_party/nlohmann/json.hpp"
#include "effect/ParticleSystem.h"
#include "ToolIO.h"

namespace host
{
namespace tool_ops
{
// Why a verb stopped: its exit code, a report category, the reason, and the
// mutation it stopped at (-1 when none).
struct Stop
{
    int         code = kExitOk;
    std::string category;
    std::string reason;
    int         op = -1;
};

// Fills `stop` and returns false.
bool Fail(Stop* stop, int code, const std::string& category, const std::string& reason, int op = -1);

// ---- Inspect paths ----------------------------------------------------------
// "emitters[3].groups[1].minX": object keys separated by dots, array indices
// in brackets.

// The value at `path`, or nullptr.
const nlohmann::json* At(const nlohmann::json& root, const std::string& path);
// Sets the value at `path`, if there is one, to null.
void Blank(nlohmann::json& root, const std::string& path);
// The first path at which `a` and `b` differ (numbers compare by value).
bool FirstDifference(const nlohmann::json& a, const nlohmann::json& b, const std::string& path, std::string* where);
// `value` as JSON text, or "(absent)".
std::string Shown(const nlohmann::json* value);
// Appends `path` unless `to` has it.
void Add(std::vector<std::string>& to, const std::string& path);

// ---- Ops ---------------------------------------------------------------------
// The ops, with their keys:
//   setProperty      field (an inspect path, e.g. "groups[0].maxY"), value (number or boolean)
//   setBlendMode     value
//   setColorTexture  name          setNormalTexture  name          rename  name
//   rescale          timeScale, sizeScale (multipliers, as DoRescaleEmitter takes them)
//   setInterpolation track, interpolation ("linear" | "smooth" | "step")
//   replaceTrack     track, keys ([[time, value], ...], endpoints at 0 and 100)
//   setTrackKey      track, time, value, optional newTime (moves an interior key)
//   addTrackKey      track, time, value
//   setTrackLock     track, optional lockTo (an earlier colour channel; absent unlocks)
// The four key and interpolation ops take an optional `dependents`: the colour channels
// locked to `track`, which the edit changes too. It is required, and must
// list them exactly, when there are any.

// Checks one op's name, keys and their kinds (exit 2), as op `at`.
bool CheckOp(const nlohmann::json& op, int at, Stop* stop);

// What one applied op touched, as inspect paths relative to the emitter:
// `requested` are the fields it asked for; `footprint` adds what a save
// derives from them.
struct Touched
{
    std::vector<std::string> requested;
    std::vector<std::string> footprint;
};

// Applies a checked op to `e`, a member of `system`, under the editor's rules
// (exit 5 when a rule refuses it).
bool ApplyOp(
    ParticleSystem& system, ParticleSystem::Emitter& e, const nlohmann::json& op, int at, Touched* touched, Stop* stop);

// Creates a unique sibling of `out` holding `bytes`, flushed. "" on failure.
std::wstring Stage(const std::wstring& out, const std::string& bytes);

// The atlas index keys of `e` outside its grid, by time and value.
std::set<std::pair<float, float>> OutOfGrid(const ParticleSystem::Emitter& e);
} // namespace tool_ops
} // namespace host

#endif // TOOL_OPS_H
