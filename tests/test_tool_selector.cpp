// Unit test for `--tool patch` request parsing and selector resolution
// (src/host/ToolSelector.h, Phase 4B plan section 3.4).
//
// Selectors resolve against the real `inspect` dump of a built particle
// system, so the parent and spawn-slot encodings are the ones the tool reads.
// Every refusal row of the plan's section 5 has a case: a changed hash, no
// match, more than one match, a banned positional key, an expected mismatch,
// an unknown or empty `expected`; plus the duplicate-root tree that only
// `expected` can resolve.
//
// Standalone console exe; see the test_tool_selector entry in
// tests/native-tests.json.

#include "effect/ParticleSystem.h"
#include "effect/ParticleSystemJson.h"
#include "host/ToolSelector.h"
#include "simulation/ParticleSystemInstance.h"

#include <cstdio>
#include <memory>
#include <string>

// Link stub: ~Emitter calls ParticleSystemInstance::RemoveEmitter, whose real
// body is D3D-coupled; no EmitterInstance is ever registered here.
void ParticleSystemInstance::RemoveEmitter(EmitterInstance*) {}

using json = nlohmann::json;
using tool_selector::Refusal;
typedef ParticleSystem::Emitter Emitter;

static int g_failed = 0;
static int g_checks = 0;

static void check(bool ok, const std::string& what)
{
    g_checks++;
    if (ok)
        printf("  ok: %s\n", what.c_str());
    else
    {
        g_failed++;
        printf("  FAIL: %s\n", what.c_str());
    }
}

static const char* kHash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

// Two roots both named "Smoke" (blend modes 2 and 1); the first has a life
// child "Sparks" with a death child "Embers", and a death child "Debris".
// A third root, "Flash", has no children.
static json Dump()
{
    ParticleSystem system;
    system.setName("selector_fixture");
    Emitter* smokeA            = system.addRootEmitter();
    smokeA->name               = "Smoke";
    smokeA->blendMode          = 2;
    smokeA->colorTexture       = "p_smoke.tga";
    smokeA->lifetime           = 1.3f;
    Emitter* sparks            = system.addLifetimeEmitter(smokeA);
    sparks->name               = "Sparks";
    Emitter* embers            = system.addDeathEmitter(sparks);
    embers->name               = "Embers";
    Emitter* debris            = system.addDeathEmitter(smokeA);
    debris->name               = "Debris";
    Emitter* smokeB            = system.addRootEmitter();
    smokeB->name               = "Smoke";
    smokeB->blendMode          = 1;
    smokeB->colorTexture       = "p_smoke.tga";
    smokeB->lifetime           = 1.3f;
    Emitter* flash             = system.addRootEmitter();
    flash->name                = "Flash";
    json dump                  = particle_json::ParticleSystemJson(system);
    dump["sourceDocumentHash"] = kHash;
    return dump;
}

static size_t PositionOf(const json& dump, const std::string& name, uint64_t blendMode)
{
    for (size_t i = 0; i < dump["emitters"].size(); i++)
        if (dump["emitters"][i]["name"] == name && dump["emitters"][i]["blendMode"] == blendMode) return i;
    return (size_t)-1;
}

static json Selector(json rolePath, json expected)
{
    return {{"sourceDocumentHash", kHash}, {"rolePath", rolePath}, {"expected", expected}};
}

static json Step(const char* role, const char* name)
{
    return {{"role", role}, {"name", name}};
}

// An explicit array: nlohmann reads `json x = {oneObject}` as a copy of the
// object, not a one-element array.
static json Path(std::initializer_list<json> steps)
{
    json path = json::array();
    for (const json& step : steps) path.push_back(step);
    return path;
}

static tool_selector::Resolution Resolve(const json& selector, const json& dump)
{
    return tool_selector::ResolveSelector(selector, dump);
}

static void TestResolution(const json& dump)
{
    printf("resolution\n");
    const json smokeA = Path({Step("root", "Smoke")});

    auto r = Resolve(Selector(smokeA, {{"blendMode", 2}}), dump);
    check(r.refusal == Refusal::None && r.emitter == PositionOf(dump, "Smoke", 2),
          "duplicate root names: expected {blendMode: 2} picks the first Smoke");
    check(r.candidates.size() == 2, "the resolution still lists both role-path candidates");
    r = Resolve(Selector(smokeA, {{"blendMode", 1}}), dump);
    check(r.refusal == Refusal::None && r.emitter == PositionOf(dump, "Smoke", 1),
          "expected {blendMode: 1} picks the second Smoke");

    r = Resolve(Selector(Path({Step("root", "Smoke"), Step("lifeChild", "Sparks"), Step("deathChild", "Embers")}),
                         {{"colorTexture", "p_particle_master.tga"}}),
                dump);
    check(r.refusal == Refusal::None && dump["emitters"][r.emitter]["name"] == "Embers",
          "a three-step path reaches the grandchild through the life then death slots");
    r = Resolve(Selector(Path({Step("root", "Smoke"), Step("deathChild", "Debris")}), {{"lifetime", 1}}), dump);
    check(r.refusal == Refusal::None && dump["emitters"][r.emitter]["name"] == "Debris", "a death-slot child resolves");

    r = Resolve(Selector(smokeA, {{"lifetime", 1.3}}), dump);
    check(r.refusal == Refusal::Ambiguous && r.candidates.size() == 2,
          "both Smokes match lifetime 1.3 (narrowed to float32): ambiguous, both listed");
    check(r.candidates[0]["mismatched"].empty() && r.candidates[1]["mismatched"].empty() && !r.reason.empty(),
          "an ambiguous refusal says why and lists each candidate");
    r = Resolve(Selector(smokeA, {{"lifetime", 1.30000001}}), dump);
    check(r.refusal == Refusal::Ambiguous, "a double that narrows to the same float32 matches");
    r = Resolve(Selector(smokeA, {{"lifetime", 1.3000001}}), dump);
    check(r.refusal == Refusal::ExpectedMismatch, "a double one float32 step away does not");
    r = Resolve(Selector(smokeA, {{"lifetime", 1.31}}), dump);
    check(r.refusal == Refusal::ExpectedMismatch && r.candidates.size() == 2 &&
              r.candidates[0]["mismatched"] == json::array({"lifetime"}),
          "expected mismatch: no candidate holds the value, each candidate names its mismatched keys");
    r = Resolve(Selector(smokeA, {{"blendMode", 2.5}}), dump);
    check(r.refusal == Refusal::ExpectedMismatch, "a fractional value never matches an integer field");
    r = Resolve(Selector(smokeA, {{"blendMode", "2"}}), dump);
    check(r.refusal == Refusal::ExpectedMismatch, "a string never matches a number field (inspect's encodings)");
    r = Resolve(Selector(smokeA, {{"colorTexture", "P_SMOKE.TGA"}, {"blendMode", 2}}), dump);
    check(r.refusal == Refusal::ExpectedMismatch, "strings compare byte for byte (no case folding)");
    r = Resolve(Selector(smokeA, {{"acceleration", {0, 0, 0}}, {"blendMode", 1}}), dump);
    check(r.refusal == Refusal::None && r.emitter == PositionOf(dump, "Smoke", 1), "arrays compare member by member");
    r = Resolve(Selector(smokeA, {{"parent", nullptr}, {"blendMode", 2}}), dump);
    check(r.refusal == Refusal::None, "null compares as null");

    r = Resolve(Selector(Path({Step("root", "Nope")}), {{"blendMode", 2}}), dump);
    check(r.refusal == Refusal::NoMatch && r.reason.find("Flash") != std::string::npos,
          "no matching root: refused, and the reason lists the roots");
    r = Resolve(Selector(Path({Step("root", "Smoke"), Step("deathChild", "Sparks")}), {{"blendMode", 2}}), dump);
    check(r.refusal == Refusal::NoMatch && r.reason.find("Debris") != std::string::npos,
          "a name in the wrong slot does not match; the reason names what the slots hold");
    r = Resolve(Selector(Path({Step("root", "Flash"), Step("lifeChild", "Sparks")}), {{"blendMode", 1}}), dump);
    check(r.refusal == Refusal::NoMatch, "an empty slot does not match");
    r = Resolve(Selector(Path({Step("root", "Sparks")}), {{"blendMode", 1}}), dump);
    check(r.refusal == Refusal::NoMatch, "a child is not a root");
    r = Resolve(Selector(Path({Step("root", "Smok")}), {{"blendMode", 2}}), dump);
    check(r.refusal == Refusal::NoMatch, "names match in full, not by prefix");
}

static void TestRefusals(const json& dump)
{
    printf("refusals\n");
    const json path = Path({Step("root", "Smoke")});
    const json good = Selector(path, {{"blendMode", 2}});

    json changed                  = good;
    changed["sourceDocumentHash"] = std::string(64, 'f');
    check(Resolve(changed, dump).refusal == Refusal::HashMismatch, "a changed hash is refused");
    json noHash = good;
    noHash.erase("sourceDocumentHash");
    check(Resolve(noHash, dump).refusal == Refusal::Malformed, "a missing hash is refused");

    const char* bannedKeys[] = {"id", "index", "stableId"};
    for (const char* key : bannedKeys)
    {
        json top        = good;
        top[key]        = 0;
        json inExpected = Selector(path, {{"blendMode", 2}, {key, 0}});
        json inStep     = Selector(Path({json{{"role", "root"}, {"name", "Smoke"}, {key, 0}}}), {{"blendMode", 2}});
        check(Resolve(top, dump).refusal == Refusal::BannedKey &&
                  Resolve(inExpected, dump).refusal == Refusal::BannedKey &&
                  Resolve(inStep, dump).refusal == Refusal::BannedKey,
              std::string("'") + key + "' is refused at the top, in expected and in a step");
    }

    check(Resolve(Selector(path, {{"noSuchField", 1}}), dump).refusal == Refusal::UnknownExpected,
          "an unknown expected key is refused");
    check(Resolve(Selector(path, json::object()), dump).refusal == Refusal::Malformed, "an empty expected is refused");
    json noExpected = good;
    noExpected.erase("expected");
    check(Resolve(noExpected, dump).refusal == Refusal::Malformed, "a missing expected is refused");

    json extra    = good;
    extra["note"] = "x";
    check(Resolve(extra, dump).refusal == Refusal::Malformed, "an unknown selector key is refused");
    check(Resolve(Selector(json::array(), {{"blendMode", 2}}), dump).refusal == Refusal::Malformed,
          "an empty rolePath is refused");
    check(Resolve(Selector(Path({Step("lifeChild", "Smoke")}), {{"blendMode", 2}}), dump).refusal == Refusal::Malformed,
          "a path that does not start at a root is refused");
    check(Resolve(Selector(Path({Step("root", "Smoke"), Step("root", "Sparks")}), {{"blendMode", 2}}), dump).refusal ==
              Refusal::Malformed,
          "root after the first step is refused");
    check(Resolve(Selector(Path({Step("root", "Smoke"), Step("child", "Sparks")}), {{"blendMode", 2}}), dump).refusal ==
              Refusal::Malformed,
          "an unknown role is refused");
    check(
        Resolve(Selector(Path({json{{"role", "root"}}}), {{"blendMode", 2}}), dump).refusal == Refusal::Malformed &&
            Resolve(Selector(Path({json{{"role", "root"}, {"name", 3}}}), {{"blendMode", 2}}), dump).refusal ==
                Refusal::Malformed &&
            Resolve(Selector(Path({json{{"role", "root"}, {"name", "Smoke"}, {"slot", 1}}}), {{"blendMode", 2}}), dump)
                    .refusal == Refusal::Malformed,
        "a step without a name, with a non-string name, or with an extra key is refused");
    check(Resolve(json::array(), dump).refusal == Refusal::Malformed, "a non-object selector is refused");

    bool          named = true;
    const Refusal all[] = {Refusal::Malformed,       Refusal::BannedKey, Refusal::HashMismatch,
                           Refusal::UnknownExpected, Refusal::NoMatch,   Refusal::ExpectedMismatch,
                           Refusal::Ambiguous};
    for (Refusal refusal : all)
        named = named && std::string(tool_selector::RefusalName(refusal)).compare(0, 9, "selector-") == 0;
    check(named, "every refusal has a report category");
}

static void TestStrictJson()
{
    printf("strict request JSON\n");
    json        out;
    std::string reason;
    check(tool_selector::ParseStrictJson("{\"a\": 1, \"b\": {\"c\": [1, 2]}}", &out, &reason) && out["b"]["c"][1] == 2,
          "a plain request parses");
    check(tool_selector::ParseStrictJson("\xEF\xBB\xBF{\"a\": 1}", &out, &reason), "a UTF-8 BOM is skipped");
    check(!tool_selector::ParseStrictJson(std::string("\xFF\xFE{\0\"\0", 6), &out, &reason) &&
              reason.find("UTF-16") != std::string::npos,
          "a UTF-16 BOM gets a clear error");
    check(!tool_selector::ParseStrictJson("{\"a\": 1, \"a\": 2}", &out, &reason) &&
              reason.find("'a'") != std::string::npos,
          "a duplicate key is refused (the parser alone would keep the last)");
    check(!tool_selector::ParseStrictJson("{\"x\": {\"a\": 1}, \"y\": [{\"b\": 1, \"b\": 1}]}", &out, &reason),
          "a duplicate key inside a nested object is refused");
    check(tool_selector::ParseStrictJson("{\"x\": {\"a\": 1}, \"y\": {\"a\": 1}}", &out, &reason),
          "the same key in sibling objects is fine");
    check(!tool_selector::ParseStrictJson("{\"a\": 1e400}", &out, &reason), "a number beyond a double is refused");
    check(!tool_selector::ParseStrictJson("{\"a\": NaN}", &out, &reason) &&
              !tool_selector::ParseStrictJson("{\"a\": Infinity}", &out, &reason),
          "NaN and Infinity literals are refused");
    check(!tool_selector::ParseStrictJson("{\"a\": \"\xE9\"}", &out, &reason), "invalid UTF-8 in a string is refused");
    check(!tool_selector::ParseStrictJson("{\"a\": 1} trailing", &out, &reason), "trailing text is refused");
    check(!tool_selector::ParseStrictJson(std::string(tool_selector::kMaxRequestBytes + 1, ' '), &out, &reason) &&
              reason.find("16 MiB") != std::string::npos,
          "a request over 16 MiB is refused before parsing");
}

int main()
{
    printf("test_tool_selector\n");
    const json dump = Dump();
    TestResolution(dump);
    TestRefusals(dump);
    TestStrictJson();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) printf("all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
