// `--tool reorder`. See ToolReorder.h for the request and its checks.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <memory>
#include <string>
#include <vector>
#include "ToolReorder.h"
#include "ToolIO.h"
#include "ToolSelector.h"
#include "effect/ParticleAuthoring.h"
#include "effect/ParticleSystem.h"
#include "effect/ParticleSystemJson.h"

using nlohmann::json;
typedef ParticleSystem::Emitter Emitter;

namespace host
{
namespace
{
struct Stop
{
    int         code = kExitOk;
    std::string category;
    std::string reason;
};

bool Fail(Stop* stop, int code, const std::string& category, const std::string& reason)
{
    stop->code     = code;
    stop->category = category;
    stop->reason   = reason;
    return false;
}

bool IsIndex(const json& v)
{
    return v.is_number_unsigned() || (v.is_number_integer() && v.get<long long>() >= 0);
}

bool CheckRequest(const json& request, Stop* stop)
{
    if (!request.is_object()) return Fail(stop, kExitUsage, "request-malformed", "the request must be a JSON object");
    for (auto it = request.begin(); it != request.end(); ++it)
        if (it.key() != "schema" && it.key() != "sourceDocumentHash" && it.key() != "move" && it.key() != "toRoot")
            return Fail(stop, kExitUsage, "request-malformed", "unknown request key '" + it.key() + "'");
    if (!request.contains("schema") || request["schema"] != 1)
        return Fail(stop, kExitUsage, "request-malformed", "the request needs \"schema\": 1");
    if (!request.contains("sourceDocumentHash") || !request["sourceDocumentHash"].is_string())
        return Fail(stop, kExitUsage, "request-malformed", "the request needs a sourceDocumentHash string");
    const json* move = request.contains("move") ? &request["move"] : nullptr;
    if (!move || !move->is_object() || move->size() != 2 || !move->contains("index") || !IsIndex((*move)["index"]) ||
        !move->contains("name") || !(*move)["name"].is_string())
        return Fail(stop, kExitUsage, "request-malformed", "move must be {\"index\": <position>, \"name\": <string>}");
    if (!request.contains("toRoot") || !IsIndex(request["toRoot"]))
        return Fail(stop, kExitUsage, "request-malformed", "toRoot must be a root gap (a non-negative integer)");
    return true;
}

// An emitter's dump without the fields a reorder rewrites.
json Payload(json emitter)
{
    for (const char* key : {"index", "parent", "spawnOnDeath", "spawnDuringLife"}) emitter.erase(key);
    return emitter;
}

json Mapped(const json& position, const std::vector<size_t>& map)
{
    return position.is_null() ? position : json(map[position.get<size_t>()]);
}

// The candidate is the source with every emitter moved by `map`: payloads
// equal, links and index mapped, the system fields untouched.
bool IsPermutation(const json& source, const json& candidate, const std::vector<size_t>& map, std::string* why)
{
    const json& from = source["emitters"];
    const json& to   = candidate["emitters"];
    if (from.size() != to.size())
    {
        *why = "the emitter count changed";
        return false;
    }
    for (size_t b = 0; b < from.size(); b++)
    {
        const json&       s  = from[b];
        const json&       c  = to[map[b]];
        const std::string at = "emitter " + std::to_string(b) + " -> " + std::to_string(map[b]);
        if (Payload(s) != Payload(c))
        {
            *why = at + ": its contents changed";
            return false;
        }
        if (c["index"] != map[b])
        {
            *why = at + ": its index is not its new position";
            return false;
        }
        for (const char* key : {"parent", "spawnOnDeath", "spawnDuringLife"})
            if (c[key] != Mapped(s[key], map))
            {
                *why = at + ": " + key + " is not the mapped " + key;
                return false;
            }
    }
    json a = source, b = candidate;
    a.erase("emitters");
    b.erase("emitters");
    a.erase("sourceDocumentHash");
    b.erase("sourceDocumentHash");
    if (a != b)
    {
        *why = "a system field changed";
        return false;
    }
    return true;
}

bool Reorder(const std::string&  source,
             const std::string&  sourceHash,
             const std::string&  requestBytes,
             const std::wstring& out,
             json&               report,
             Stop*               stop)
{
    json        request;
    std::string reason;
    if (!tool_selector::ParseStrictJson(requestBytes, &request, &reason))
        return Fail(stop, kExitUsage, "request-malformed", reason);
    if (!CheckRequest(request, stop)) return false;

    // Parse twice: W is reordered; B, the source as a save reproduces it, is
    // what the candidate is compared with.
    std::unique_ptr<ParticleSystem> S = ParseSource(source, &reason);
    if (!S) return Fail(stop, kExitSource, "source-invalid", "the source did not load: " + reason);
    if (!S->getGraphRepairs().empty())
        return Fail(stop, kExitSource, "source-invalid", "the loader repaired the source: " + S->getGraphRepairs()[0]);
    const std::string               baseline = SaveToBytes(*S);
    std::unique_ptr<ParticleSystem> B        = ParseSource(baseline, &reason);
    if (!B) return Fail(stop, kExitInternal, "internal", "the unedited source does not reload: " + reason);
    std::unique_ptr<ParticleSystem> W = ParseSource(source, &reason);
    if (!W) return Fail(stop, kExitInternal, "internal", "the second parse failed: " + reason);

    if (request["sourceDocumentHash"] != sourceHash)
        return Fail(stop, kExitSelector, "hash-mismatch", "the request was written against other source bytes");

    const size_t           pos  = request["move"]["index"].get<size_t>();
    std::vector<Emitter*>& list = W->getEmitters();
    if (pos >= list.size()) return Fail(stop, kExitSelector, "no-target", "there is no emitter " + std::to_string(pos));
    Emitter* target = list[pos];
    if (target->name != request["move"]["name"].get<std::string>())
        return Fail(stop, kExitSelector, "name-mismatch",
                    "emitter " + std::to_string(pos) + " is '" + target->name + "'");
    if (target->parent != nullptr)
        return Fail(stop, kExitRule, "not-a-root", "emitter " + std::to_string(pos) + " is a child");
    if (HasLinkGroupMembership(*target))
        return Fail(stop, kExitRule, "linked-target",
                    "the target is in link group " + std::to_string(target->linkGroup));

    const std::vector<Emitter*> before = list;
    if (!W->moveEmitterToRootIndex(target, request["toRoot"].get<size_t>()))
        return Fail(stop, kExitRule, "no-move",
                    "moving the root to gap " + request["toRoot"].dump() +
                        " leaves the order unchanged or is out of range");
    std::vector<size_t> map(before.size());
    for (size_t b = 0; b < before.size(); b++)
        for (size_t c = 0; c < list.size(); c++)
            if (list[c] == before[b]) map[b] = c;
    report["map"] = map;

    // The candidate, checked before anything is written.
    const std::string               bytesW = SaveToBytes(*W);
    std::unique_ptr<ParticleSystem> C      = ParseSource(bytesW, &reason);
    if (!C) return Fail(stop, kExitPostcheck, "postcondition", "the candidate does not load: " + reason);
    if (!C->getGraphRepairs().empty())
        return Fail(stop, kExitPostcheck, "postcondition",
                    "the loader repaired the candidate: " + C->getGraphRepairs()[0]);
    if (SaveToBytes(*C) != bytesW)
        return Fail(stop, kExitPostcheck, "postcondition", "a save of the reloaded candidate differs from it");
    std::string why;
    if (!IsPermutation(particle_json::ParticleSystemJson(*B), particle_json::ParticleSystemJson(*C), map, &why))
        return Fail(stop, kExitPostcheck, "postcondition", why);

    if (!WriteFileAtomically(out, bytesW))
        return Fail(stop, kExitIo, "io", "the candidate could not be written to --out");
    std::string written;
    if (!ReadFileBytes(out, &written, &reason))
        return Fail(stop, kExitIo, "io", "--out could not be read back: " + reason);
    report["candidateHash"] = Sha256Hex(written);
    return true;
}
} // namespace

int RunReorder(const std::string&  source,
               const std::string&  sourceHash,
               const std::string&  request,
               const std::wstring& out,
               json*               report)
{
    json& r = *report;
    r       = {{"schema", 1}, {"verb", "reorder"}, {"sourceDocumentHash", sourceHash}};
    Stop       stop;
    const bool ok = Reorder(source, sourceHash, request, out, r, &stop);
    r["outcome"]  = ok ? "ok" : (stop.code == kExitPostcheck ? "postcondition-failed" : "refused");
    r["exitCode"] = ok ? kExitOk : stop.code;
    if (!ok) r["refusal"] = {{"category", stop.category}, {"reason", stop.reason}};
    return ok ? kExitOk : stop.code;
}
} // namespace host
