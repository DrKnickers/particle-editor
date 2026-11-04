// `--tool build`. See ToolBuild.h for the spec and the checks.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "ToolBuild.h"
#include "ToolOps.h"
#include "ToolIO.h"
#include "ToolSelector.h"
#include "effect/ParticleAuthoring.h"
#include "effect/ParticleSystem.h"
#include "effect/ParticleSystemIO.h"
#include "effect/ParticleSystemJson.h"
#include "effect/PersistedProjection.h"

using nlohmann::json;
typedef ParticleSystem::Emitter Emitter;

namespace host
{
using namespace tool_ops;

namespace
{
const size_t kMaxEntries   = 64;
const size_t kMaxMutations = 256;

// One emitter's spec ({name, mutations}, and lifeChild when `root`), as
// emitter `at` of the built list. Its names go into `names`.
bool CheckEmitter(const json& entry, bool root, int at, std::set<std::string>* names, Stop* stop)
{
    const std::string what = "emitter " + std::to_string(at);
    if (!entry.is_object()) return Fail(stop, kExitUsage, "spec-malformed", what + " must be an object");
    for (auto it = entry.begin(); it != entry.end(); ++it)
        if (it.key() != "name" && it.key() != "mutations" && (it.key() != "lifeChild" || !root))
            return Fail(stop, kExitUsage, "spec-malformed",
                        what + " has no key '" + it.key() + "'" +
                            (it.key() == "lifeChild" ? " (a lifetime child has no children)" : ""));
    if (!entry.contains("name") || !entry["name"].is_string() || entry["name"].get<std::string>().empty())
        return Fail(stop, kExitUsage, "spec-malformed", what + " needs a name");
    const std::string name = entry["name"].get<std::string>();
    if (name.find('\0') != std::string::npos || !IsValidUtf8(name))
        return Fail(stop, kExitUsage, "spec-malformed", what + "'s name must be UTF-8 without a NUL");
    if (!names->insert(name).second)
        return Fail(stop, kExitUsage, "spec-malformed", "two emitters are named '" + name + "'");
    if (!entry.contains("mutations") || !entry["mutations"].is_array())
        return Fail(stop, kExitUsage, "spec-malformed", what + " needs a mutations array");
    const json& ops = entry["mutations"];
    if (ops.size() > kMaxMutations)
        return Fail(stop, kExitUsage, "spec-malformed", what + " has more than 256 mutations");
    for (size_t i = 0; i < ops.size(); i++)
    {
        if (!CheckOp(ops[i], static_cast<int>(i), stop))
            return Fail(stop, kExitUsage, "spec-malformed", what + ": " + stop->reason, stop->op);
        if (ops[i]["op"] == "rename")
            return Fail(stop, kExitUsage, "spec-malformed", what + ": an emitter takes its entry's name, not a rename",
                        static_cast<int>(i));
    }
    return true;
}

bool CheckSpec(const json& spec, Stop* stop, int* failedEmitter)
{
    if (!spec.is_object()) return Fail(stop, kExitUsage, "spec-malformed", "the spec must be a JSON object");
    for (auto it = spec.begin(); it != spec.end(); ++it)
        if (it.key() != "schema" && it.key() != "emitters")
            return Fail(stop, kExitUsage, "spec-malformed", "unknown spec key '" + it.key() + "'");
    if (!spec.contains("schema") || spec["schema"] != 1)
        return Fail(stop, kExitUsage, "spec-malformed", "the spec needs \"schema\": 1");
    if (!spec.contains("emitters") || !spec["emitters"].is_array() || spec["emitters"].empty())
        return Fail(stop, kExitUsage, "spec-malformed", "the spec needs a non-empty emitters array");
    if (spec["emitters"].size() > kMaxEntries)
        return Fail(stop, kExitUsage, "spec-malformed", "the spec has more than 64 entries");
    std::set<std::string> names;
    int                   at = 0;
    for (const json& entry : spec["emitters"])
    {
        *failedEmitter = at;
        if (!CheckEmitter(entry, true, at++, &names, stop)) return false;
        *failedEmitter = at;
        if (entry.contains("lifeChild") && !CheckEmitter(entry["lifeChild"], false, at++, &names, stop)) return false;
    }
    return true;
}

// The spec's emitters, as the editor makes new ones, named and in the tree,
// with no edits; each emitter's spec entry goes into `entries`.
std::unique_ptr<ParticleSystem> Skeleton(const json& spec, const std::string& name, std::vector<const json*>* entries)
{
    std::unique_ptr<ParticleSystem> system(new ParticleSystem());
    system->setName(name);
    entries->clear();
    for (const json& entry : spec["emitters"])
    {
        Emitter* root = system->addRootEmitter();
        root->name    = entry["name"].get<std::string>();
        entries->push_back(&entry);
        if (!entry.contains("lifeChild")) continue;
        Emitter* child = system->addLifetimeEmitter(root);
        child->name    = entry["lifeChild"]["name"].get<std::string>();
        entries->push_back(&entry["lifeChild"]);
    }
    return system;
}

// The parent positions of every emitter in a dump.
json Tree(const json& dump)
{
    json parents = json::array();
    for (const json& e : dump["emitters"]) parents.push_back(e["parent"]);
    return parents;
}

json Names(const json& dump)
{
    json names = json::array();
    for (const json& e : dump["emitters"]) names.push_back(e["name"]);
    return names;
}

// Builds and verifies. Fills `report` as it goes; returns false with `stop`,
// and the emitter it stopped at in `failedEmitter` (-1 when none).
bool Build(const std::string&  specBytes,
           const std::wstring& out,
           const BuildHooks&   hooks,
           json&               report,
           Stop*               stop,
           int*                failedEmitter)
{
    // The spec.
    json        spec;
    std::string reason, path;
    if (!tool_selector::ParseStrictJson(specBytes, &spec, &reason))
        return Fail(stop, kExitUsage, "spec-malformed", reason);
    if (!CheckSpec(spec, stop, failedEmitter)) return false;
    *failedEmitter = -1;

    // 1. K is the skeleton the edits are measured against; W is edited.
    const std::string systemName = DeriveParticleSystemName(out);
    report["name"]               = systemName;
    std::vector<const json*>        entries;
    std::unique_ptr<ParticleSystem> K     = Skeleton(spec, systemName, &entries);
    std::unique_ptr<ParticleSystem> W     = Skeleton(spec, systemName, &entries);
    const json                      dumpK = particle_json::ParticleSystemJson(*K);

    // 2. Apply each emitter's ops, op by op, against its current state.
    std::vector<std::string> requested, footprint;
    for (size_t pos = 0; pos < entries.size(); pos++)
    {
        Emitter&          target        = W->getEmitter(pos);
        const std::string prefix        = "emitters[" + std::to_string(pos) + "].";
        json              emitterReport = {{"position", pos},
                                           {"name", target.name},
                                           {"parent", dumpK["emitters"][pos]["parent"]},
                                           {"ops", json::array()}};
        const json&       ops           = (*entries[pos])["mutations"];
        for (size_t i = 0; i < ops.size(); i++)
        {
            const json before = particle_json::EmitterJson(target, nullptr);
            Touched    touched;
            if (!ApplyOp(*W, target, ops[i], static_cast<int>(i), &touched, stop))
            {
                *failedEmitter = static_cast<int>(pos);
                return false;
            }
            const json after   = particle_json::EmitterJson(target, nullptr);
            json       fields  = json::array();
            bool       changed = false;
            for (const std::string& p : touched.requested)
            {
                fields.push_back({{"path", prefix + p}, {"before", *At(before, p)}, {"requested", *At(after, p)}});
                changed = changed || *At(before, p) != *At(after, p);
            }
            json opFootprint = json::array();
            for (const std::string& p : touched.footprint)
            {
                opFootprint.push_back(prefix + p);
                Add(footprint, prefix + p);
            }
            for (const std::string& p : touched.requested) Add(requested, prefix + p);
            emitterReport["ops"].push_back({{"index", i},
                                            {"op", ops[i]["op"]},
                                            {"effect", changed ? "changed" : "unchanged"},
                                            {"fields", fields},
                                            {"footprint", opFootprint}});
        }
        for (const auto& key : OutOfGrid(target))
            report["warnings"].push_back(
                {{"code", "atlas-out-of-grid"},
                 {"message", prefix + "tracks[5] has index " + json(static_cast<double>(key.second)).dump() +
                                 " at time " + json(static_cast<double>(key.first)).dump() +
                                 ", outside the atlas grid; the engine wraps it"}});
        report["emitters"].push_back(emitterReport);
    }
    report["footprint"] = footprint;

    // 3. What a save will make of W, before anything is written.
    json dumpW = particle_json::ParticleSystemJson(*W);
    if (particle_json::FindInvalidInspectValue(dumpW, &path, &reason))
        return Fail(stop, kExitRule, "non-finite-result", "the edits leave " + path + " " + reason);
    json projected;
    if (!particle_json::ProjectPersisted(dumpW, &projected, &reason))
        return Fail(stop, kExitRule, "unsaveable", "the built document would not reload: " + reason);
    for (const std::string& p : requested)
    {
        const json* was   = At(dumpK, p);
        const json* asked = At(dumpW, p);
        const json* saved = At(projected, p);
        if (was && asked && saved && *asked != *was && *saved == *was)
            return Fail(stop, kExitRule, "value-lost",
                        p + ": the requested " + asked->dump() + " saves as " + saved->dump() +
                            ", a new emitter's value");
    }
    std::unique_ptr<ParticleSystem> B = ParseSource(SaveToBytes(*K), &reason);
    if (!B) return Fail(stop, kExitInternal, "internal", "the unedited skeleton does not reload: " + reason);
    const json dumpB = particle_json::ParticleSystemJson(*B);
    {
        json p = projected, b = dumpB;
        for (const std::string& f : footprint)
        {
            Blank(p, f);
            Blank(b, f);
        }
        if (FirstDifference(p, b, "", &path))
            return Fail(stop, kExitRule, "collateral",
                        "saving the build would also change " + path + ", outside the edits' footprint");
    }

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
    const json dumpC = particle_json::ParticleSystemJson(*C);
    {
        std::string why;
        if (!C->getGraphRepairs().empty())
            why = "the loader repaired the candidate: " + C->getGraphRepairs()[0];
        else if (particle_json::FindInvalidInspectValue(dumpC, &path, &reason))
            why = path + " " + reason;
        else if (dumpC["name"] != systemName)
            why = "the internal name did not persist";
        else if (Names(dumpC) != Names(dumpK))
            why = "the emitters or their names differ from the spec's";
        else if (Tree(dumpC) != Tree(dumpK))
            why = "the emitter tree differs from the spec's";
        else
            for (const json& e : dumpC["emitters"])
                if (e["linkGroup"] != 0) why = "emitter '" + e["name"].get<std::string>() + "' is in a link group";
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
    for (json& emitter : report["emitters"])
        for (json& op : emitter["ops"])
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

int RunBuild(const std::string& spec, const std::wstring& out, const BuildHooks& hooks, json* report)
{
    json& r = *report;
    r       = {{"schema", 1},
               {"verb", "build"},
               {"textureResolution", "unverified"},
               {"warnings", json::array()},
               {"emitters", json::array()}};
    Stop       stop;
    int        failedEmitter = -1;
    const bool ok            = Build(spec, out, hooks, r, &stop, &failedEmitter);
    r["outcome"]             = ok ? "ok" : (stop.code == kExitPostcheck ? "postcondition-failed" : "refused");
    r["exitCode"]            = ok ? kExitOk : stop.code;
    if (!ok)
    {
        r["refusal"]            = {{"category", stop.category}, {"reason", stop.reason}};
        r["refusal"]["emitter"] = failedEmitter >= 0 ? json(failedEmitter) : json(nullptr);
        r["refusal"]["op"]      = stop.op >= 0 ? json(stop.op) : json(nullptr);
    }
    return ok ? kExitOk : stop.code;
}
} // namespace host
