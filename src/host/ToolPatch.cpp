// `--tool patch`. See ToolPatch.h for the request and verification contract.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "ToolPatch.h"
#include "ToolOps.h"
#include "ToolIO.h"
#include "ToolSelector.h"
#include "effect/ParticleAuthoring.h"
#include "effect/ParticleSystem.h"
#include "effect/ParticleSystemIO.h"
#include "effect/ParticleSystemJson.h"
#include "effect/PersistedProjection.h"
#include "effect/Rescale.h"

using nlohmann::json;
typedef ParticleSystem::Emitter        Emitter;
typedef ParticleSystem::Emitter::Track Track;

namespace host
{
using namespace tool_ops;

namespace
{
const size_t kMaxMutations = 256;

bool CheckRequest(const json& request, Stop* stop)
{
    if (!request.is_object()) return Fail(stop, kExitUsage, "request-malformed", "the request must be a JSON object");
    for (auto it = request.begin(); it != request.end(); ++it)
        if (it.key() != "schema" && it.key() != "selector" && it.key() != "mutations")
            return Fail(stop, kExitUsage, "request-malformed", "unknown request key '" + it.key() + "'");
    if (!request.contains("schema") || request["schema"] != 1)
        return Fail(stop, kExitUsage, "request-malformed", "the request needs \"schema\": 1");
    if (!request.contains("selector"))
        return Fail(stop, kExitUsage, "request-malformed", "the request needs a selector");
    if (!request.contains("mutations") || !request["mutations"].is_array())
        return Fail(stop, kExitUsage, "request-malformed", "the request needs a mutations array");
    const json& ops = request["mutations"];
    if (ops.size() > kMaxMutations)
        return Fail(stop, kExitUsage, "request-malformed", "the request has more than 256 mutations");
    for (size_t i = 0; i < ops.size(); i++)
        if (!CheckOp(ops[i], static_cast<int>(i), stop)) return false;
    return true;
}

// The emitter positions from `pos` up to its root.
std::vector<json> Ancestry(const json& dump, size_t pos)
{
    std::vector<json> chain;
    json              at = pos;
    while (at.is_number() && at.get<size_t>() < dump["emitters"].size() && chain.size() <= dump["emitters"].size())
    {
        chain.push_back(at);
        at = dump["emitters"][at.get<size_t>()]["parent"];
    }
    return chain;
}

// Verification steps 1-6. Fills `report` as it goes; returns false with `stop`.
bool Patch(const std::string&  source,
           const std::string&  sourceHash,
           const std::string&  requestBytes,
           const std::wstring& inputPath,
           const std::wstring& out,
           const PatchHooks&   hooks,
           json&               report,
           Stop*               stop)
{
    // The request.
    json        request;
    std::string reason;
    if (!tool_selector::ParseStrictJson(requestBytes, &request, &reason))
        return Fail(stop, kExitUsage, "request-malformed", reason);
    if (!CheckRequest(request, stop)) return false;

    // 1. Parse twice: S stays pristine, W is edited.
    std::unique_ptr<ParticleSystem> S = ParseSource(source, &reason);
    if (!S) return Fail(stop, kExitSource, "source-invalid", "the source did not load: " + reason);
    if (!S->getGraphRepairs().empty())
        return Fail(stop, kExitSource, "source-invalid", "the loader repaired the source: " + S->getGraphRepairs()[0]);
    json        dumpS = particle_json::ParticleSystemJson(*S);
    std::string path;
    if (particle_json::FindInvalidInspectValue(dumpS, &path, &reason))
        return Fail(stop, kExitSource, "source-invalid", path + " " + reason);
    dumpS["sourceDocumentHash"]       = sourceHash;
    std::unique_ptr<ParticleSystem> W = ParseSource(source, &reason);
    if (!W) return Fail(stop, kExitInternal, "internal", "the second parse failed: " + reason);

    if (S->getName() != DeriveParticleSystemName(inputPath))
        report["warnings"].push_back(
            {{"code", "internal-name"},
             {"message", "the internal name '" + S->getName() + "' differs from '" +
                             DeriveParticleSystemName(inputPath) +
                             "', the name the input file name gives; the candidate keeps the internal name"}});

    // 2. Resolve, and refuse a linked target.
    const tool_selector::Resolution resolved = tool_selector::ResolveSelector(request["selector"], dumpS);
    report["selector"]                       = {{"candidates", resolved.candidates}};
    if (resolved.refusal != tool_selector::Refusal::None)
        return Fail(stop, kExitSelector, tool_selector::RefusalName(resolved.refusal), resolved.reason);
    const size_t      pos         = resolved.emitter;
    const std::string prefix      = "emitters[" + std::to_string(pos) + "].";
    report["selector"]["emitter"] = pos;
    report["selector"]["name"]    = dumpS["emitters"][pos]["name"];
    Emitter& target               = W->getEmitter(pos);
    if (HasLinkGroupMembership(target))
        return Fail(stop, kExitRule, "linked-target",
                    "the target is in link group " + std::to_string(target.linkGroup) +
                        "; the tool does not edit linked emitters");

    // 3. Apply, op by op, against W's current state.
    std::vector<std::string>                requested, footprint;
    const std::set<std::pair<float, float>> outOfGridBefore = OutOfGrid(S->getEmitter(pos));
    for (size_t i = 0; i < request["mutations"].size(); i++)
    {
        const json& op     = request["mutations"][i];
        const json  before = particle_json::EmitterJson(target, nullptr);
        Touched     touched;
        if (!ApplyOp(*W, target, op, static_cast<int>(i), &touched, stop)) return false;
        const json after   = particle_json::EmitterJson(target, nullptr);
        json       fields  = json::array();
        bool       changed = false;
        for (const std::string& p : touched.requested)
        {
            fields.push_back({{"path", prefix + p}, {"before", *At(before, p)}, {"requested", *At(after, p)}});
            changed = changed || *At(before, p) != *At(after, p);
        }
        json fullFootprint = json::array();
        for (const std::string& p : touched.footprint)
        {
            fullFootprint.push_back(prefix + p);
            Add(footprint, prefix + p);
        }
        for (const std::string& p : touched.requested) Add(requested, prefix + p);
        report["ops"].push_back({{"index", i},
                                 {"op", op["op"]},
                                 {"effect", changed ? "changed" : "unchanged"},
                                 {"fields", fields},
                                 {"footprint", fullFootprint}});
    }
    report["footprint"] = footprint;

    // What a save will make of W, before anything is written.
    json dumpW = particle_json::ParticleSystemJson(*W);
    if (particle_json::FindInvalidInspectValue(dumpW, &path, &reason))
        return Fail(stop, kExitRule, "non-finite-result", "the edits leave " + path + " " + reason);
    json projected;
    if (!particle_json::ProjectPersisted(dumpW, &projected, &reason))
        return Fail(stop, kExitRule, "unsaveable", "the edited document would not reload: " + reason);
    for (const std::string& p : requested)
    {
        const json* was   = At(dumpS, p);
        const json* asked = At(dumpW, p);
        const json* saved = At(projected, p);
        if (was && asked && saved && *asked != *was && *saved == *was)
            return Fail(stop, kExitRule, "value-lost",
                        p + ": the requested " + asked->dump() + " saves as " + saved->dump() +
                            ", the value it already has");
    }
    std::unique_ptr<ParticleSystem> B;
    {
        const std::string baseline = SaveToBytes(*S);
        B                          = ParseSource(baseline, &reason);
        if (!B) return Fail(stop, kExitInternal, "internal", "the unedited source does not reload: " + reason);
    }
    json dumpB = particle_json::ParticleSystemJson(*B);
    {
        json p = projected, b = dumpB;
        for (const std::string& f : footprint)
        {
            Blank(p, f);
            Blank(b, f);
        }
        if (FirstDifference(p, b, "", &path))
            return Fail(stop, kExitRule, "collateral",
                        "saving the edit would also change " + path + ", outside the edit's footprint");
    }
    const std::set<std::pair<float, float>> outOfGridAfter = OutOfGrid(target);
    for (const auto& key : outOfGridAfter)
        if (!outOfGridBefore.count(key))
            report["warnings"].push_back(
                {{"code", "atlas-out-of-grid"},
                 {"message", prefix + "tracks[5] has index " + json(static_cast<double>(key.second)).dump() +
                                 " at time " + json(static_cast<double>(key.first)).dump() +
                                 ", outside the atlas grid; the engine wraps it"}});

    // 4. Stage the candidate.
    const std::string  bytesW = SaveToBytes(*W);
    const std::wstring staged = Stage(out, bytesW);
    if (staged.empty()) return Fail(stop, kExitIo, "io", "the candidate could not be staged next to --out");
    struct StagedFile
    {
        std::wstring path;
        bool         keep = false;
        ~StagedFile()
        {
            if (!keep) DeleteFileW(path.c_str());
        }
    } guard{staged};
    if (hooks.afterStage) hooks.afterStage(staged);

    // 5. Verify the staged file.
    std::string stagedBytes;
    if (!ReadFileBytes(staged, &stagedBytes, &reason))
        return Fail(stop, kExitIo, "io", "the staged candidate could not be read: " + reason);
    report["checks"] = json::object();
    auto postcheck   = [&](int n, bool ok, const std::string& why)
    {
        report["checks"][std::to_string(n)] = ok;
        if (!ok) Fail(stop, kExitPostcheck, "postcondition", "check " + std::to_string(n) + ": " + why);
        return ok;
    };
    if (!postcheck(4, Sha256Hex(stagedBytes) == Sha256Hex(bytesW),
                   "the staged file is not the document that was verified"))
        return false;
    std::unique_ptr<ParticleSystem> C = ParseSource(stagedBytes, &reason);
    if (!postcheck(3, C && SaveToBytes(*C) == stagedBytes,
                   C ? "a save of the reloaded candidate differs from it" : "the candidate does not load: " + reason))
        return false;
    json dumpC = particle_json::ParticleSystemJson(*C);
    {
        std::string why;
        if (!C->getGraphRepairs().empty())
            why = "the loader repaired the candidate: " + C->getGraphRepairs()[0];
        else if (particle_json::FindInvalidInspectValue(dumpC, &path, &reason))
            why = path + " " + reason;
        else if (dumpC["emitters"].size() != dumpW["emitters"].size())
            why = "the emitter count changed";
        else if (dumpC["emitters"][pos]["name"] != dumpW["emitters"][pos]["name"])
            why = "the target's name did not persist";
        else if (dumpC["emitters"][pos]["linkGroup"] != 0)
            why = "the target is in a link group";
        else if (Ancestry(dumpC, pos) != Ancestry(dumpS, pos))
            why = "the target's place in the tree changed";
        if (!postcheck(5, why.empty(), why)) return false;
    }
    for (const std::string& f : footprint)
    {
        const json* got  = At(dumpC, f);
        const json* want = At(projected, f);
        if (!got || !want || *got != *want)
            return postcheck(1, false, f + " saved as " + Shown(got) + ", projected " + Shown(want));
    }
    postcheck(1, true, "");
    {
        json c = dumpC, b = dumpB;
        for (const std::string& f : footprint)
        {
            Blank(c, f);
            Blank(b, f);
        }
        if (!postcheck(2, !FirstDifference(c, b, "", &path), "the candidate also changed " + path)) return false;
    }
    for (json& op : report["ops"])
        for (json& field : op["fields"]) field["persisted"] = *At(dumpC, field["path"].get<std::string>());

    // 6. Commit.
    if (!MoveFileExW(staged.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return Fail(stop, kExitIo, "io",
                    "the candidate could not replace --out (error " + std::to_string(GetLastError()) + ")");
    guard.keep = true;
    std::string written;
    if (!ReadFileBytes(out, &written, &reason))
        return Fail(stop, kExitIo, "io", "--out could not be read back: " + reason);
    report["candidateHash"] = Sha256Hex(written);
    return true;
}
} // namespace

int RunPatch(const std::string&  source,
             const std::string&  sourceHash,
             const std::string&  request,
             const std::wstring& inputPath,
             const std::wstring& out,
             const PatchHooks&   hooks,
             json*               report)
{
    json& r = *report;
    r       = {{"schema", 1},
               {"verb", "patch"},
               {"sourceDocumentHash", sourceHash},
               {"textureResolution", "unverified"},
               {"warnings", json::array()},
               {"ops", json::array()}};
    Stop       stop;
    const bool ok = Patch(source, sourceHash, request, inputPath, out, hooks, r, &stop);
    r["outcome"]  = ok ? "ok" : (stop.code == kExitPostcheck ? "postcondition-failed" : "refused");
    r["exitCode"] = ok ? kExitOk : stop.code;
    if (!ok)
    {
        r["refusal"]       = {{"category", stop.category}, {"reason", stop.reason}};
        r["refusal"]["op"] = stop.op >= 0 ? json(stop.op) : json(nullptr);
    }
    return ok ? kExitOk : stop.code;
}
} // namespace host
