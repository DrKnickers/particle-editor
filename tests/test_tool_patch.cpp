// Unit test for `--tool patch` (src/host/ToolPatch.cpp, Phase 4B PR 3),
// run in-process through RunPatch so the staging seam can inject a failure.
//
//   - every op succeeds on a fixture, and the candidate holds what the report
//     says persisted;
//   - each refusal category reaches its exit code: a malformed request (2),
//     an invalid or repaired source (3), a selector refusal (4), a rule
//     refusal (5, including owner decision 7's adjusted outcomes, a linked
//     target, a value lost to the save, collateral re-locking and a
//     non-finite result) and a postcondition failure (6);
//   - nothing reaches --out unless every check passed: a bad last op and an
//     injected staging fault leave the previous --out and no temp files;
//   - a zero-op patch of each shipped fixture writes write(load(src)), which
//     is the source byte for byte for the four canonical ones.
//
// Runs from the repo root (the native test runner's cwd). Standalone console
// exe; see the test_tool_patch entry in tests/native-tests.json.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "effect/ParticleSystem.h"
#include "effect/ParticleSystemJson.h"
#include "host/ToolIO.h"
#include "host/ToolPatch.h"
#include "simulation/ParticleSystemInstance.h"
#include "common/files.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Link stub: ~Emitter calls ParticleSystemInstance::RemoveEmitter, whose real
// body is D3D-coupled; no EmitterInstance is ever registered here.
void ParticleSystemInstance::RemoveEmitter(EmitterInstance*) {}

using json = nlohmann::json;
typedef ParticleSystem::Emitter Emitter;
typedef Emitter::Track Track;

static int g_failed = 0;
static int g_checks = 0;
static std::wstring g_dir;

static void check(bool ok, const std::string& what)
{
    g_checks++;
    if (ok) printf("  ok: %s\n", what.c_str());
    else { g_failed++; printf("  FAIL: %s\n", what.c_str()); }
}

static std::wstring P(const wchar_t* name) { return g_dir + L"\\" + name; }

static std::string ReadAll(const std::wstring& path)
{
    std::string bytes, error;
    host::ReadFileBytes(path, &bytes, &error);
    return bytes;
}

static void WriteAll(const std::wstring& path, const std::string& bytes)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

static std::set<std::wstring> Listing()
{
    std::set<std::wstring> names;
    WIN32_FIND_DATAW data;
    HANDLE h = FindFirstFileW(P(L"*").c_str(), &data);
    if (h == INVALID_HANDLE_VALUE) return names;
    do
    {
        const std::wstring name = data.cFileName;
        if (name != L"." && name != L"..") names.insert(name);
    } while (FindNextFileW(h, &data));
    FindClose(h);
    return names;
}

static void SetKeys(Track& t, std::initializer_list<Track::Key> keys)
{
    t.keys.clear();
    for (const Track::Key& k : keys) t.keys.insert(k);
}

// The fixture: root "Smoke" (unlocked channels, a 2x2 atlas, red endpoint at
// 127/255), its life child "Sparks" (G/B/A locked to R, the default), a
// second root "Linked A" in link group 1 with "Linked B", and "Spinner"
// with randomRotation on.
static std::string Fixture()
{
    ParticleSystem system;
    system.setName("fixture");
    Emitter* smoke = system.addRootEmitter();
    smoke->name = "Smoke";
    for (int t = 0; t < ParticleSystem::NUM_TRACKS; t++) smoke->tracks[t] = &smoke->trackContents[t];
    SetKeys(smoke->trackContents[0], {Track::Key(0, 127 / 255.0f), Track::Key(50, 0.6f), Track::Key(100, 1)});
    SetKeys(smoke->trackContents[1], {Track::Key(0, 0.2f), Track::Key(25, 0.4f), Track::Key(100, 1)});
    SetKeys(smoke->trackContents[2], {Track::Key(0, 1), Track::Key(100, 0)});
    SetKeys(smoke->trackContents[3], {Track::Key(0, 0), Track::Key(75, 1), Track::Key(100, 0)});
    SetKeys(smoke->trackContents[4], {Track::Key(0, 4), Track::Key(50, 8), Track::Key(100, 2)});
    SetKeys(smoke->trackContents[5], {Track::Key(0, 0), Track::Key(50, 2), Track::Key(100, 3)});
    smoke->textureSize = 4;
    smoke->lifetime = 1.5f;
    smoke->colorTexture = "p_smoke.tga";
    smoke->normalTexture = "p_smoke_n.tga";
    Emitter* sparks = system.addLifetimeEmitter(smoke);
    sparks->name = "Sparks";
    Emitter* a = system.addRootEmitter();
    a->name = "Linked A";
    Emitter* b = system.addRootEmitter();
    b->name = "Linked B";
    a->linkGroup = 1;
    b->linkGroup = 1;
    Emitter* spinner = system.addRootEmitter();
    spinner->name = "Spinner";
    spinner->randomRotation = true;
    spinner->randomRotationAverage = 2;
    return host::SaveToBytes(system);
}

static std::string g_fixture;
static std::string g_fixtureHash;

struct Result
{
    int  code;
    json report;
};

// `mutations` is an initializer list, not a json: nlohmann reads `{oneObject}`
// as a copy of the object rather than a one-element array.
static json Request(const char* root, std::initializer_list<json> list, const char* child = nullptr, const char* childRole = nullptr,
                    const std::string& hash = g_fixtureHash)
{
    json mutations = json::array();
    for (const json& op : list) mutations.push_back(op);
    json path = json::array();
    path.push_back({{"role", "root"}, {"name", root}});
    if (child) path.push_back({{"role", childRole}, {"name", child}});
    return {{"schema", 1},
            {"selector", {{"sourceDocumentHash", hash}, {"rolePath", path}, {"expected", {{"textureSize", child ? 64 : (std::string(root) == "Smoke" ? 4 : 64)}}}}},
            {"mutations", mutations}};
}

static Result Run(const std::string& request, const std::wstring& out, const std::string& source = g_fixture,
                  const host::PatchHooks& hooks = host::PatchHooks(), const std::wstring& input = L"fixture.alo")
{
    Result r;
    r.code = host::RunPatch(source, host::Sha256Hex(source), request, input, out, hooks, &r.report);
    return r;
}

static Result Run(const json& request, const std::wstring& out = P(L"out.alo"))
{
    DeleteFileW(out.c_str());
    return Run(request.dump(), out);
}

static std::unique_ptr<ParticleSystem> LoadOut(const std::wstring& out = P(L"out.alo"))
{
    std::string error;
    return host::ParseSource(ReadAll(out), &error);
}

static const Emitter* Named(const ParticleSystem* system, const std::string& name)
{
    if (!system) return nullptr;
    for (const Emitter* e : system->getEmitters())
        if (e->name == name) return e;
    return nullptr;
}

static std::string Category(const Result& r)
{
    return r.report.contains("refusal") ? r.report["refusal"]["category"].get<std::string>() : std::string("(none)");
}

static void Refused(const json& request, int code, const char* category, const std::string& what)
{
    const Result r = Run(request);
    const bool ok = r.code == code && Category(r) == category && GetFileAttributesW(P(L"out.alo").c_str()) == INVALID_FILE_ATTRIBUTES;
    check(ok, what);
    if (!ok) printf("    got exit %d, %s: %s\n", r.code, Category(r).c_str(),
                    r.report.contains("refusal") ? r.report["refusal"]["reason"].get<std::string>().c_str() : "");
}

// check(), with the exit code and refusal printed when it fails.
static void checkRun(bool ok, const std::string& what, const Result& r)
{
    check(ok, what);
    if (!ok) printf("    exit %d %s\n", r.code, r.report.contains("refusal") ? r.report["refusal"].dump().c_str() : "");
}

static json Op(const char* op, json fields)
{
    fields["op"] = op;
    return fields;
}

static void TestOps()
{
    printf("ops\n");
    Result r = Run(Request("Smoke", {Op("setProperty", {{"field", "lifetime"}, {"value", 2.5}})}));
    std::unique_ptr<ParticleSystem> out = LoadOut();
    check(r.code == 0 && r.report["outcome"] == "ok" && Named(out.get(), "Smoke") && Named(out.get(), "Smoke")->lifetime == 2.5f,
          "setProperty: exit 0 and the candidate holds the value");
    if (r.code != 0) { printf("    report: %s\n", r.report.dump().c_str()); return; }
    const json& field = r.report["ops"][0]["fields"][0];
    check(field["path"] == "emitters[0].lifetime" && field["before"] == 1.5 && field["requested"] == 2.5 && field["persisted"] == 2.5 &&
          r.report["ops"][0]["effect"] == "changed", "the report gives the path, before, requested and persisted");
    check(r.report["ops"][0]["footprint"] == json::array({"emitters[0].lifetime", "emitters[0].groups[1]"}),
          "the footprint adds the derived lifetime group");
    check(r.report["checks"] == json({{"1", true}, {"2", true}, {"3", true}, {"4", true}, {"5", true}}) &&
          r.report["candidateHash"] == host::Sha256Hex(ReadAll(P(L"out.alo"))) && r.report["selector"]["emitter"] == 0,
          "checks 1-5 pass and candidateHash is the hash of --out");
    check(r.report["textureResolution"] == "unverified", "texture resolution is unverified");

    r = Run(Request("Smoke", {Op("setProperty", {{"field", "lifetime"}, {"value", 1.5}})}));
    check(r.code == 0 && r.report["ops"][0]["effect"] == "unchanged", "setting the current value is unchanged, not refused");

    r = Run(Request("Smoke", {Op("setBlendMode", {{"value", 2}}), Op("setColorTexture", {{"name", "p_fire.tga"}}),
                              Op("setNormalTexture", {{"name", "p_particle_depth_master.tga"}}), Op("setProperty", {{"field", "groups[2].maxY"}, {"value", 3}}),
                              Op("setProperty", {{"field", "hasTail"}, {"value", true}})}));
    out = LoadOut();
    const Emitter* smoke = Named(out.get(), "Smoke");
    checkRun(r.code == 0 && smoke && smoke->blendMode == 2 && smoke->colorTexture == "p_fire.tga" &&
          smoke->normalTexture == "p_particle_depth_master.tga" && smoke->groups[2].maxY == 3 && smoke->hasTail,
          "blend mode, both textures, a group member and a boolean in one patch", r);
    Refused(Request("Smoke", {Op("setNormalTexture", {{"name", ""}})}), 5, "rule",
            "an empty normal texture (it saves no chunk and reloads as the default name; check 3 caught it)");

    r = Run(Request("Smoke", {Op("rename", {{"name", "Smoke Fast"}}), Op("setProperty", {{"field", "gravity"}, {"value", -2}})}));
    out = LoadOut();
    check(r.code == 0 && Named(out.get(), "Smoke Fast") && Named(out.get(), "Smoke Fast")->gravity == -2.0f && !Named(out.get(), "Smoke"),
          "rename, then a later op still edits the renamed emitter");

    r = Run(Request("Smoke", {Op("rescale", {{"timeScale", 2}, {"sizeScale", 0.5}})}));
    out = LoadOut();
    smoke = Named(out.get(), "Smoke");
    check(r.code == 0 && smoke && smoke->lifetime == 3.0f && smoke->tracks[4]->keys.begin()->value == 2.0f,
          "rescale doubles the lifetime and halves the scale track");

    r = Run(Request("Smoke", {Op("setInterpolation", {{"track", 4}, {"interpolation", "step"}}),
                              Op("replaceTrack", {{"track", 5}, {"keys", {{0, 0}, {25, 1}, {50, 2}, {75, 3}, {100, 3}}}}),
                              Op("setTrackKey", {{"track", 4}, {"time", 50}, {"newTime", 62.5}, {"value", 9}}),
                              Op("setTrackKey", {{"track", 4}, {"time", 100}, {"value", 1}}),
                              Op("addTrackKey", {{"track", 3}, {"time", 12.5}, {"value", 1}})}));
    out = LoadOut();
    smoke = Named(out.get(), "Smoke");
    std::vector<Track::Key> scale, alpha;
    if (smoke) scale.assign(smoke->tracks[4]->keys.begin(), smoke->tracks[4]->keys.end());
    if (smoke) alpha.assign(smoke->tracks[3]->keys.begin(), smoke->tracks[3]->keys.end());
    checkRun(r.code == 0 && smoke && smoke->tracks[4]->interpolation == Track::IT_STEP && smoke->tracks[5]->keys.size() == 5 &&
          scale.size() == 3 && scale[1].time == 62.5f && scale[1].value == 9 && scale[2].time == 100 && scale[2].value == 1 &&
          alpha.size() == 4 && alpha[1].time == 12.5f && alpha[1].value == 1,
          "interpolation, a full track, an interior move, a border value and an added key", r);
}

static void TestLocks()
{
    printf("locks\n");
    const json set = {{"track", 0}, {"time", 0}, {"value", 0}};   // bytes: 0 and 1 save exactly
    Refused(Request("Smoke", {Op("setTrackKey", set)}, "Sparks", "lifeChild"), 5, "rule",
            "editing a channel others are locked to needs its dependents named");
    json wrong = set;
    wrong["dependents"] = {1, 2};
    Refused(Request("Smoke", {Op("setTrackKey", wrong)}, "Sparks", "lifeChild"), 5, "rule", "dependents must be exact");
    json locked = set;
    locked["track"] = 2;
    Refused(Request("Smoke", {Op("setTrackKey", locked)}, "Sparks", "lifeChild"), 5, "rule", "a locked channel is refused");
    json right = set;
    right["dependents"] = {3, 1, 2};
    const Result r = Run(Request("Smoke", {Op("setTrackKey", right)}, "Sparks", "lifeChild"));
    std::unique_ptr<ParticleSystem> out = LoadOut();
    const Emitter* sparks = Named(out.get(), "Sparks");
    checkRun(r.code == 0 && sparks && sparks->tracks[1] == &sparks->trackContents[0] && sparks->tracks[0]->keys.begin()->value == 0 &&
          r.report["ops"][0]["footprint"].size() == 4,
          "with its dependents named, the source edit lands and the locks persist; all four tracks are in the footprint", r);
}

static void TestDecision7()
{
    printf("adjusted outcomes (decision 7)\n");
    Refused(Request("Smoke", {Op("setTrackKey", {{"track", 4}, {"time", 33}, {"value", 1}})}), 5, "rule", "a key that is not found");
    Refused(Request("Smoke", {Op("setTrackKey", {{"track", 4}, {"time", 0}, {"newTime", 10}, {"value", 1}})}), 5, "rule",
            "a border key whose time would move");
    Refused(Request("Smoke", {Op("addTrackKey", {{"track", 4}, {"time", 50}, {"value", 1}})}), 5, "rule",
            "an add the dedup would nudge off its time");
    Refused(Request("Smoke", {Op("addTrackKey", {{"track", 4}, {"time", 100}, {"value", 1}})}), 5, "rule",
            "an add at an occupied 100 (the dedup would land it at 99.999)");
    Refused(Request("Smoke", {Op("setTrackKey", {{"track", 0}, {"time", 0}, {"value", 0.4985}})}), 5, "value-lost",
            "a colour value that saves as the byte already stored");
    const Result r = Run(Request("Smoke", {Op("setTrackKey", {{"track", 0}, {"time", 0}, {"value", 0.75}})}));
    // The field is the whole track; key 0 asked for 0.75 and saves as 191/255.
    const json& field = r.report["ops"][0]["fields"][0];
    const double asked = field["requested"]["keys"][0][1].get<double>(), kept = field["persisted"]["keys"][0][1].get<double>();
    checkRun(r.code == 0 && r.report["ops"][0]["effect"] == "changed" && asked == 0.75 &&
             static_cast<float>(kept) == 191.0f / 255.0f,
             "a colour value that quantises but still changes is accepted, its persisted value reported", r);
}

static void TestRefusals()
{
    printf("refusals\n");
    Refused(Request("Smoke", {Op("setProperty", {{"field", "index"}, {"value", 3}})}), 5, "rule", "an unsettable field");
    Refused(Request("Smoke", {Op("setProperty", {{"field", "blendMode"}, {"value", 14}})}), 5, "rule", "an out-of-range value");
    Refused(Request("Smoke", {Op("rename", {{"name", "Sparks"}})}), 5, "rule", "a rename onto another emitter's name");
    Refused(Request("Smoke", {Op("setColorTexture", {{"name", ""}})}), 5, "rule", "an empty colour texture");
    Refused(Request("Smoke", {Op("rescale", {{"timeScale", 0}, {"sizeScale", 1}})}), 5, "rule", "a zero rescale");
    Refused(Request("Spinner", {Op("setTrackKey", {{"track", 6}, {"time", 0}, {"value", 1}})}), 5, "rule",
            "rotation keys while randomRotation is on");
    Refused(Request("Smoke", {Op("setTrackKey", {{"track", 7}, {"time", 0}, {"value", 1}})}), 5, "rule", "a track past the last");
    Refused(Request("Smoke", {Op("setInterpolation", {{"track", -1}, {"interpolation", "step"}})}), 5, "rule", "a negative track");
    Refused(Request("Linked A", {Op("setProperty", {{"field", "lifetime"}, {"value", 2}})}), 5, "linked-target", "a linked target");
    Refused(Request("Smoke", {Op("setProperty", {{"field", "lifetime"}, {"value", 3e38}}), Op("rescale", {{"timeScale", 10}, {"sizeScale", 1}})}),
            5, "non-finite-result", "edits that overflow to infinity");
    // The reader re-locks a channel whose keys equal an earlier unlocked one's.
    Refused(Request("Smoke", {Op("replaceTrack", {{"track", 0}, {"keys", {{0, 0.2}, {25, 0.4}, {100, 1}}}})}), 5, "collateral",
            "an edit that would make the reader re-lock another channel");
    Refused(Request("Smoke", {Op("setProperty", {{"field", "lifetime"}, {"value", 2}})}, nullptr, nullptr,
                    std::string(64, 'a')), 4, "selector-hash-mismatch", "a changed source hash");
    Refused(Request("Nope", {}), 4, "selector-no-match", "no match");

    // Request shape: exit 2.
    auto malformed = [](const std::string& text, const std::string& what) {
        DeleteFileW(P(L"out.alo").c_str());
        const Result r = Run(text, P(L"out.alo"));
        check(r.code == 2 && Category(r) == "request-malformed", what);
    };
    const json good = Request("Smoke", {Op("setProperty", {{"field", "lifetime"}, {"value", 2}})});
    malformed("not json", "a request that is not JSON");
    malformed("{\"schema\": 1, \"schema\": 1}", "a duplicate key");
    json bad = good; bad["schema"] = 2;
    malformed(bad.dump(), "schema 2");
    bad = good; bad["extra"] = 1;
    malformed(bad.dump(), "an unknown request key");
    bad = good; bad["mutations"][0]["op"] = "explode";
    malformed(bad.dump(), "an unknown op");
    bad = good; bad["mutations"][0]["colour"] = 1;
    malformed(bad.dump(), "an unknown op key");
    bad = good; bad["mutations"][0].erase("value");
    malformed(bad.dump(), "a missing op key");
    bad = good; bad["mutations"][0]["value"] = "2";
    malformed(bad.dump(), "a wrongly typed op key");
    bad = Request("Smoke", {Op("setTrackKey", {{"track", 4.5}, {"time", 0}, {"value", 1}})});
    malformed(bad.dump(), "a fractional track number");
    bad = good;
    bad["mutations"] = json::array();
    for (int i = 0; i < 257; i++) bad["mutations"].push_back(good["mutations"][0]);
    malformed(bad.dump(), "more than 256 mutations");

    // The source: exit 3.
    DeleteFileW(P(L"out.alo").c_str());
    Result r = Run(good.dump(), P(L"out.alo"), std::string("not a particle file"));
    check(r.code == 3 && Category(r) == "source-invalid", "a source that does not load");
    {
        ParticleSystem system;
        system.setName("loop");
        Emitter* e = system.addRootEmitter();
        e->name = "Loop";
        e->spawnOnDeath = 0;
        r = Run(good.dump(), P(L"out.alo"), host::SaveToBytes(system));
        check(r.code == 3 && Category(r) == "source-invalid" && r.report["refusal"]["reason"].get<std::string>().find("repaired") != std::string::npos,
              "a source the loader repaired");
    }
}

static void TestAllOrNothing()
{
    printf("all or nothing\n");
    WriteAll(P(L"prior.alo"), "previous candidate");
    const std::set<std::wstring> before = Listing();
    const json lastBad = Request("Smoke", {Op("setProperty", {{"field", "lifetime"}, {"value", 2}}),
                                           Op("setProperty", {{"field", "nTriangles"}, {"value", 0}})});
    Result r = Run(lastBad.dump(), P(L"prior.alo"));
    check(r.code == 5 && r.report["refusal"]["op"] == 1 && ReadAll(P(L"prior.alo")) == "previous candidate",
          "a bad last op refuses the whole patch and leaves --out as it was");

    host::PatchHooks hooks;
    hooks.afterStage = [](const std::wstring& staged) {
        std::string bytes = ReadAll(staged);
        bytes[bytes.size() / 2] ^= 0x5A;
        WriteAll(staged, bytes);
    };
    r = Run(Request("Smoke", {Op("setProperty", {{"field", "lifetime"}, {"value", 2}})}).dump(), P(L"prior.alo"), g_fixture, hooks);
    check(r.code == 6 && r.report["outcome"] == "postcondition-failed" && r.report["checks"]["4"] == false &&
          ReadAll(P(L"prior.alo")) == "previous candidate",
          "an injected staging fault fails check 4 with exit 6 and leaves --out as it was");
    check(Listing() == before, "no staged file is left behind");
    r = Run(Request("Smoke", {Op("setProperty", {{"field", "lifetime"}, {"value", 2}})}).dump(), P(L"no such dir\\out.alo"));
    check(r.code == 13 && Category(r) == "io", "an unstageable --out exits 13");
    r = Run(Request("Smoke", {Op("setProperty", {{"field", "lifetime"}, {"value", 2}})}).dump(), P(L"prior.alo"));
    check(r.code == 0 && r.report["candidateHash"] == host::Sha256Hex(ReadAll(P(L"prior.alo"))),
          "a patch that passes replaces an existing --out");
    DeleteFileW(P(L"prior.alo").c_str());
}

static void TestWarnings()
{
    printf("warnings\n");
    Result r = Run(Request("Smoke", {Op("setTrackKey", {{"track", 5}, {"time", 50}, {"value", 6}})}));
    bool atlas = false, name = false;
    for (const json& w : r.report["warnings"]) { atlas = atlas || w["code"] == "atlas-out-of-grid"; name = name || w["code"] == "internal-name"; }
    check(r.code == 0 && atlas, "a key moved out of the 2x2 grid is a warning, not a refusal");
    check(!name, "no name warning when the internal name matches the input file name");
    r = Run(Request("Smoke", {Op("setProperty", {{"field", "textureSize"}, {"value", 1}})}));
    atlas = false;
    for (const json& w : r.report["warnings"]) atlas = atlas || w["code"] == "atlas-out-of-grid";
    check(r.code == 0 && atlas, "shrinking textureSize re-checks the existing keys");
    DeleteFileW(P(L"out.alo").c_str());
    r = Run(Request("Smoke", {Op("setProperty", {{"field", "gravity"}, {"value", 1}})}).dump(), P(L"out.alo"), g_fixture,
            host::PatchHooks(), L"other.alo");
    name = false;
    for (const json& w : r.report["warnings"]) name = name || w["code"] == "internal-name";
    check(r.code == 0 && name, "an internal name that differs from the input file name is a warning");
}

static void TestZeroOp()
{
    printf("zero-op patches of the shipped fixtures\n");
    const wchar_t* shipped[] = {
        L"site\\guide\\downloads\\P_EXPLOSION_EXAMPLE.ALO", L"site\\guide\\downloads\\P_EXPLOSION_EXAMPLE_FLIPBOOK.ALO",
        L"web\\apps\\editor\\tests\\fixtures\\a11y-base-state.alo", L"web\\apps\\editor\\tests\\fixtures\\bump-asteroids.alo",
        L"web\\apps\\editor\\tests\\fixtures\\replay-guard.alo", L"web\\apps\\editor\\tests\\fixtures\\replay-weather.alo",
        L"web\\apps\\editor\\tests\\fixtures\\singleton-emitter.alo"};
    const bool canonical[] = {true, true, false, false, true, true, false};
    int identical = 0, resaved = 0;
    for (size_t i = 0; i < 7; i++)
    {
        const std::string source = ReadAll(shipped[i]);
        std::string error;
        std::unique_ptr<ParticleSystem> loaded = host::ParseSource(source, &error);
        if (!loaded) { wprintf(L"    cannot load %ls\n", shipped[i]); continue; }
        // The first unlinked root; name and linkGroup tell same-named roots apart.
        const json dump = particle_json::ParticleSystemJson(*loaded);
        json root;
        for (const json& e : dump["emitters"])
            if (e["parent"].is_null() && e["linkGroup"] == 0) { root = e["name"]; break; }
        json step = {{"role", "root"}, {"name", root}};
        json path = json::array();
        path.push_back(step);
        const json request = {{"schema", 1},
                              {"selector", {{"sourceDocumentHash", host::Sha256Hex(source)}, {"rolePath", path},
                                            {"expected", {{"name", root}, {"linkGroup", 0}}}}},
                              {"mutations", json::array()}};
        DeleteFileW(P(L"zero.alo").c_str());
        const Result r = Run(request.dump(), P(L"zero.alo"), source, host::PatchHooks(), shipped[i]);
        const std::string candidate = ReadAll(P(L"zero.alo"));
        if (r.code == 0 && candidate == source) identical++;
        if (r.code == 0 && candidate == host::SaveToBytes(*loaded)) resaved++;
        if (r.code != 0 || (candidate == source) != canonical[i])
            wprintf(L"    %ls: exit %d, identical %d\n", shipped[i], r.code, candidate == source ? 1 : 0);
    }
    DeleteFileW(P(L"zero.alo").c_str());
    check(identical == 4, "the four canonical fixtures come back byte for byte");
    check(resaved == 7, "every fixture comes back as write(load(src))");
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // keep every line if a check crashes
    printf("test_tool_patch\n");
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    g_dir = std::wstring(temp) + L"pe tool patch " + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(g_dir.c_str(), nullptr);
    g_fixture = Fixture();
    g_fixtureHash = host::Sha256Hex(g_fixture);

    TestOps();
    TestLocks();
    TestDecision7();
    TestRefusals();
    TestAllOrNothing();
    TestWarnings();
    TestZeroOp();

    for (const std::wstring& name : Listing()) DeleteFileW(P(name.c_str()).c_str());
    RemoveDirectoryW(g_dir.c_str());
    printf("%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) printf("all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
