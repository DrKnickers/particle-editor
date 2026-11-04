#ifndef TOOL_SELECTOR_H
#define TOOL_SELECTOR_H

// `--tool patch` request parsing and selector resolution (Phase 4B plan
// section 3.4). Pure: it reads JSON text and an `inspect` dump, and touches no
// file, window or device.
//
// A selector names one emitter by the path of spawn slots that reaches it,
// bound to the exact source bytes it was written against:
//
//   { "sourceDocumentHash": "<sha256 of the .alo>",
//     "rolePath": [ {"role": "root", "name": "Smoke"}, {"role": "deathChild", "name": "Embers"} ],
//     "expected": { "blendMode": 2, "lifetime": 1.3 } }
//
// Positional handles (`id`, `index`, `stableId`) are refused anywhere in it:
// positions shift under every structural edit, names and slots do not.

#include <cmath>
#include <set>
#include <string>
#include <vector>
#include "host/third_party/nlohmann/json.hpp"

namespace tool_selector
{
using json = nlohmann::json;

// ---- Strict JSON ------------------------------------------------------------

static const size_t kMaxRequestBytes = 16u * 1024u * 1024u;

namespace detail
{
inline bool FindNonFinite(const json& value, const std::string& path, std::string* where)
{
    if (value.is_number_float() && !std::isfinite(value.get<double>()))
    {
        *where = path.empty() ? "(root)" : path;
        return true;
    }
    if (value.is_object())
        for (auto it = value.begin(); it != value.end(); ++it)
            if (FindNonFinite(it.value(), path + "." + it.key(), where)) return true;
    if (value.is_array())
        for (size_t i = 0; i < value.size(); i++)
            if (FindNonFinite(value[i], path + "[" + std::to_string(i) + "]", where)) return true;
    return false;
}
} // namespace detail

// Parse a request file: at most kMaxRequestBytes, UTF-8 (a UTF-8 BOM is
// skipped, a UTF-16 one refused by name), no duplicate object keys (the
// parser would keep only the last), and no number too large for a double.
inline bool ParseStrictJson(const std::string& bytes, json* out, std::string* reason)
{
    if (bytes.size() > kMaxRequestBytes)
    {
        *reason = "the request is larger than 16 MiB";
        return false;
    }
    if (bytes.size() >= 2 && ((unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xFE ||
                              (unsigned char)bytes[0] == 0xFE && (unsigned char)bytes[1] == 0xFF))
    {
        *reason = "the request is UTF-16; save it as UTF-8";
        return false;
    }
    std::vector<std::set<std::string>> open; // keys seen, per open object
    std::string                        duplicate;
    auto                               callback = [&](int, json::parse_event_t event, json& parsed) -> bool
    {
        if (event == json::parse_event_t::object_start)
            open.emplace_back();
        else if (event == json::parse_event_t::object_end)
        {
            if (!open.empty()) open.pop_back();
        }
        else if (event == json::parse_event_t::key && !open.empty() && duplicate.empty())
        {
            const std::string key = parsed.get<std::string>();
            if (!open.back().insert(key).second) duplicate = key;
        }
        return true;
    };
    json parsed = json::parse(bytes.begin(), bytes.end(), callback, false);
    if (parsed.is_discarded())
    {
        *reason = "the request is not valid JSON (or not valid UTF-8)";
        return false;
    }
    if (!duplicate.empty())
    {
        *reason = "the request repeats the key '" + duplicate + "' in one object";
        return false;
    }
    std::string where;
    if (detail::FindNonFinite(parsed, "", &where))
    {
        *reason = "the request holds a number too large for a double at " + where;
        return false;
    }
    *out = parsed;
    return true;
}

// ---- Selector resolution ----------------------------------------------------

// The refusal categories, all exit 4.
enum class Refusal
{
    None,
    Malformed,        // not the documented shape
    BannedKey,        // id, index or stableId anywhere in the selector
    HashMismatch,     // the source is not the document the selector was written for
    UnknownExpected,  // an `expected` key that inspect does not emit
    NoMatch,          // no emitter on the role path
    ExpectedMismatch, // emitters on the path, none with the expected values
    Ambiguous         // more than one emitter left
};

inline const char* RefusalName(Refusal refusal)
{
    switch (refusal)
    {
        case Refusal::None:             return "none";
        case Refusal::Malformed:        return "selector-malformed";
        case Refusal::BannedKey:        return "selector-banned-key";
        case Refusal::HashMismatch:     return "selector-hash-mismatch";
        case Refusal::UnknownExpected:  return "selector-unknown-expected-key";
        case Refusal::NoMatch:          return "selector-no-match";
        case Refusal::ExpectedMismatch: return "selector-expected-mismatch";
        case Refusal::Ambiguous:        return "selector-ambiguous";
    }
    return "unknown";
}

struct Resolution
{
    Refusal     refusal;
    std::string reason;
    size_t      emitter;    // position in the emitter list, when resolved
    json        candidates; // every emitter on the role path: {position, name, mismatched}
};

namespace detail
{
inline Resolution Refuse(Refusal refusal, const std::string& reason, json candidates = json::array())
{
    Resolution r;
    r.refusal    = refusal;
    r.reason     = reason;
    r.emitter    = 0;
    r.candidates = candidates;
    return r;
}

inline bool FindBannedKey(const json& value, std::string* key)
{
    if (value.is_object())
        for (auto it = value.begin(); it != value.end(); ++it)
        {
            if (it.key() == "id" || it.key() == "index" || it.key() == "stableId")
            {
                *key = it.key();
                return true;
            }
            if (FindBannedKey(it.value(), key)) return true;
        }
    if (value.is_array())
        for (const json& item : value)
            if (FindBannedKey(item, key)) return true;
    return false;
}

// `expected` against a dumped value, in inspect's encodings: an integer
// field matches an integral number exactly, a float field matches the
// number narrowed to float32 exactly, strings byte for byte, and arrays and
// objects member by member.
inline bool Matches(const json& expected, const json& actual)
{
    // Dumped integers are positions or 32-bit fields, so doubles hold them exactly.
    if (actual.is_number_integer() || actual.is_number_unsigned())
    {
        if (!expected.is_number()) return false;
        const double v = expected.get<double>();
        return v == std::floor(v) && v == actual.get<double>();
    }
    if (actual.is_number_float())
    {
        if (!expected.is_number()) return false;
        const double v = expected.get<double>();
        if (!(std::fabs(v) <= 3.4028234663852886e38)) return false; // beyond float32
        return static_cast<float>(v) == static_cast<float>(actual.get<double>());
    }
    if (actual.is_array())
    {
        if (!expected.is_array() || expected.size() != actual.size()) return false;
        for (size_t i = 0; i < actual.size(); i++)
            if (!Matches(expected[i], actual[i])) return false;
        return true;
    }
    if (actual.is_object())
    {
        if (!expected.is_object() || expected.size() != actual.size()) return false;
        for (auto it = actual.begin(); it != actual.end(); ++it)
            if (!expected.contains(it.key()) || !Matches(expected[it.key()], it.value())) return false;
        return true;
    }
    return expected == actual; // string, boolean, null
}

inline json Candidate(const json& emitters, size_t position, const std::vector<std::string>& mismatched)
{
    return {{"position", position}, {"name", emitters[position]["name"]}, {"mismatched", mismatched}};
}
} // namespace detail

// Resolve `selector` against `dump` (an `inspect` document of the source,
// with its sourceDocumentHash). Exactly one emitter, or a refusal that says
// why and lists every candidate the role path reached.
inline Resolution ResolveSelector(const json& selector, const json& dump)
{
    using detail::Refuse;
    if (!selector.is_object()) return Refuse(Refusal::Malformed, "the selector must be an object");
    std::string banned;
    if (detail::FindBannedKey(selector, &banned))
        return Refuse(Refusal::BannedKey,
                      "'" + banned + "' is a position, which shifts under edits; select by rolePath and expected");
    for (auto it = selector.begin(); it != selector.end(); ++it)
        if (it.key() != "sourceDocumentHash" && it.key() != "rolePath" && it.key() != "expected")
            return Refuse(Refusal::Malformed, "unknown selector key '" + it.key() + "'");

    const json* hash = selector.contains("sourceDocumentHash") ? &selector["sourceDocumentHash"] : nullptr;
    if (!hash || !hash->is_string())
        return Refuse(Refusal::Malformed, "the selector needs sourceDocumentHash, the SHA-256 that inspect reported");
    const std::string sourceHash = dump.value("sourceDocumentHash", std::string());
    if (sourceHash.empty() || *hash != sourceHash)
        return Refuse(Refusal::HashMismatch, "sourceDocumentHash " + hash->get<std::string>() +
                                                 " does not match the source (" + sourceHash + "); inspect it again");

    const json* path = selector.contains("rolePath") ? &selector["rolePath"] : nullptr;
    if (!path || !path->is_array() || path->empty())
        return Refuse(Refusal::Malformed, "rolePath must be a non-empty array of {role, name} steps");
    for (size_t i = 0; i < path->size(); i++)
    {
        const json&       step = (*path)[i];
        const std::string at   = "rolePath[" + std::to_string(i) + "]";
        if (!step.is_object() || step.size() != 2 || !step.contains("role") || !step.contains("name") ||
            !step["role"].is_string() || !step["name"].is_string())
            return Refuse(Refusal::Malformed, at + " must be exactly {\"role\": <string>, \"name\": <string>}");
        const std::string role = step["role"].get<std::string>();
        if (i == 0 && role != "root") return Refuse(Refusal::Malformed, at + " must have role 'root'");
        if (i > 0 && role != "lifeChild" && role != "deathChild")
            return Refuse(Refusal::Malformed, at + " role must be 'lifeChild' or 'deathChild'");
    }

    const json* expected = selector.contains("expected") ? &selector["expected"] : nullptr;
    if (!expected || !expected->is_object() || expected->empty())
        return Refuse(Refusal::Malformed, "expected must be an object naming at least one field");
    const json& emitters = dump["emitters"];
    if (!emitters.empty())
        for (auto it = expected->begin(); it != expected->end(); ++it)
            if (!emitters[0].contains(it.key()))
                return Refuse(Refusal::UnknownExpected,
                              "expected key '" + it.key() + "' is not an inspect emitter key");

    // Walk the path. Only roots can share a name; each later step follows one slot.
    std::vector<size_t>      reached;
    const std::string        rootName = (*path)[0]["name"].get<std::string>();
    std::vector<std::string> rootNames;
    for (size_t i = 0; i < emitters.size(); i++)
    {
        if (!emitters[i]["parent"].is_null()) continue;
        rootNames.push_back(emitters[i]["name"].get<std::string>());
        if (emitters[i]["name"] == rootName) reached.push_back(i);
    }
    if (reached.empty())
    {
        json names = rootNames;
        return Refuse(Refusal::NoMatch, "no root emitter is named '" + rootName + "'; roots: " + names.dump());
    }
    for (size_t s = 1; s < path->size(); s++)
    {
        const std::string   slot = (*path)[s]["role"] == "lifeChild" ? "spawnDuringLife" : "spawnOnDeath";
        const std::string   name = (*path)[s]["name"].get<std::string>();
        std::vector<size_t> next;
        json                seen = json::array();
        for (size_t at : reached)
        {
            const json& child = emitters[at][slot];
            if (child.is_null() || child.get<uint64_t>() >= emitters.size()) continue;
            const size_t c = static_cast<size_t>(child.get<uint64_t>());
            seen.push_back(emitters[c]["name"]);
            if (emitters[c]["name"] == name) next.push_back(c);
        }
        if (next.empty())
            return Refuse(Refusal::NoMatch, "rolePath[" + std::to_string(s) + "]: no " +
                                                (*path)[s]["role"].get<std::string>() + " named '" + name + "'; the " +
                                                slot + " slots there hold " + seen.dump());
        reached = next;
    }

    // Filter by `expected`.
    json                candidates = json::array();
    std::vector<size_t> kept;
    for (size_t at : reached)
    {
        std::vector<std::string> mismatched;
        for (auto it = expected->begin(); it != expected->end(); ++it)
            if (!detail::Matches(it.value(), emitters[at][it.key()])) mismatched.push_back(it.key());
        candidates.push_back(detail::Candidate(emitters, at, mismatched));
        if (mismatched.empty()) kept.push_back(at);
    }
    if (kept.empty())
        return Refuse(Refusal::ExpectedMismatch,
                      "the role path reaches " + std::to_string(reached.size()) +
                          " emitter(s), none with the expected values",
                      candidates);
    if (kept.size() > 1)
        return Refuse(Refusal::Ambiguous,
                      std::to_string(kept.size()) +
                          " emitters match the role path and expected values; add expected fields that tell them apart",
                      candidates);
    Resolution r;
    r.refusal    = Refusal::None;
    r.emitter    = kept[0];
    r.candidates = candidates;
    return r;
}

} // namespace tool_selector

#endif
