// Unit test for `--tool build` (src/host/ToolBuild.cpp), run in-process
// through RunBuild so the staging seam can inject a failure.
//
//   - a spec of root emitters and lifetime children builds, in order, named,
//     each a new emitter apart from its edits; every op kind lands, and the
//     report says what persisted;
//   - the internal name is the --out file's;
//   - each refusal reaches its exit code: a malformed spec (2), a rule
//     refusal naming its emitter and op, a value lost to the save, collateral
//     re-locking and a non-finite result (5), and a postcondition failure (6);
//   - nothing reaches --out unless every check passed.
//
// Standalone console exe; see the test_tool_build entry in
// tests/native-tests.json.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "effect/ParticleSystem.h"
#include "host/ToolBuild.h"
#include "host/ToolIO.h"
#include "simulation/ParticleSystemInstance.h"
#include "common/files.h"

#include <cstdio>
#include <fstream>
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

struct Result
{
    int  code;
    json report;
};

static Result Run(const std::string& spec, const std::wstring& out = P(L"Out.alo"), const host::BuildHooks& hooks = host::BuildHooks())
{
    Result r;
    r.code = host::RunBuild(spec, out, hooks, &r.report);
    return r;
}

static Result Run(const json& spec)
{
    DeleteFileW(P(L"Out.alo").c_str());
    return Run(spec.dump());
}

static std::unique_ptr<ParticleSystem> LoadOut(const std::wstring& out = P(L"Out.alo"))
{
    std::string error;
    return host::ParseSource(ReadAll(out), &error);
}

static std::string Category(const Result& r)
{
    return r.report.contains("refusal") ? r.report["refusal"]["category"].get<std::string>() : std::string("(none)");
}

// check(), with the exit code and refusal printed when it fails.
static void checkRun(bool ok, const std::string& what, const Result& r)
{
    check(ok, what);
    if (!ok) printf("    exit %d %s\n", r.code, r.report.contains("refusal") ? r.report["refusal"].dump().c_str() : "");
}

static void Refused(const json& spec, int code, const char* category, const std::string& what)
{
    const Result r = Run(spec);
    const bool ok = r.code == code && Category(r) == category && GetFileAttributesW(P(L"Out.alo").c_str()) == INVALID_FILE_ATTRIBUTES;
    check(ok, what);
    if (!ok) printf("    got exit %d, %s: %s\n", r.code, Category(r).c_str(),
                    r.report.contains("refusal") ? r.report["refusal"]["reason"].get<std::string>().c_str() : "");
}

static json Op(const char* op, json fields)
{
    fields["op"] = op;
    return fields;
}

// An emitter entry. `ops` is an initializer list: nlohmann reads `{oneObject}`
// as a copy of the object rather than a one-element array.
static json Entry(const char* name, std::initializer_list<json> ops)
{
    json mutations = json::array();
    for (const json& op : ops) mutations.push_back(op);
    return {{"name", name}, {"mutations", mutations}};
}

static json Spec(std::initializer_list<json> entries)
{
    json list = json::array();
    for (const json& e : entries) list.push_back(e);
    return {{"schema", 1}, {"emitters", list}};
}

static void TestBuild()
{
    printf("build\n");
    Result r = Run(Spec({Entry("Smoke", {})}));
    std::unique_ptr<ParticleSystem> out = LoadOut();
    checkRun(r.code == 0 && out && out->getEmitters().size() == 1 && out->getEmitter(0).name == "Smoke" &&
             r.report["checks"] == json({{"1", true}, {"2", true}, {"3", true}, {"4", true}, {"5", true}}),
             "one root with no edits builds, and checks 1-5 pass", r);
    check(out && out->getName() == "out" && r.report["name"] == "out", "the internal name is the --out file's, lowercased");
    check(r.report["candidateHash"] == host::Sha256Hex(ReadAll(P(L"Out.alo"))), "candidateHash is the hash of --out");
    {
        ParticleSystem fresh;
        fresh.setName("out");
        fresh.addRootEmitter()->name = "Smoke";
        check(ReadAll(P(L"Out.alo")) == host::SaveToBytes(fresh), "with no edits, the file is the editor's new emitter, named");
    }

    // Every op kind but rename, a lifetime child, and a second root.
    json smoke = Entry("Smoke", {
        Op("setProperty", {{"field", "lifetime"}, {"value", 2.5}}),
        Op("setBlendMode", {{"value", 2}}),
        Op("setColorTexture", {{"name", "pe_gen_smoke.dds"}}),
        Op("setNormalTexture", {{"name", "p_particle_depth_master.tga"}}),
        Op("setTrackLock", {{"track", 3}}),
        Op("replaceTrack", {{"track", 3}, {"keys", {{0, 0}, {10, 1}, {100, 0}}}}),
        Op("replaceTrack", {{"track", 0}, {"keys", {{0, 1}, {100, 0.2}}}, {"dependents", {1, 2}}}),
        Op("setInterpolation", {{"track", 4}, {"interpolation", "smooth"}}),
        Op("replaceTrack", {{"track", 5}, {"keys", {{0, 0}, {50, 32}, {100, 63.5}}}}),
        Op("setTrackKey", {{"track", 4}, {"time", 100}, {"value", 40}}),
        Op("addTrackKey", {{"track", 4}, {"time", 25}, {"value", 30}}),
        Op("rescale", {{"timeScale", 1}, {"sizeScale", 2}})});
    smoke["lifeChild"] = Entry("Trail", {Op("setProperty", {{"field", "lifetime"}, {"value", 0.5}})});
    r = Run(Spec({smoke, Entry("Fire", {Op("setBlendMode", {{"value", 1}})})}));
    out = LoadOut();
    const Emitter* s = out && out->getEmitters().size() == 3 ? &out->getEmitter(0) : nullptr;
    const Emitter* t = s ? &out->getEmitter(1) : nullptr;
    const Emitter* f = s ? &out->getEmitter(2) : nullptr;
    checkRun(r.code == 0 && s && s->name == "Smoke" && t->name == "Trail" && f->name == "Fire" && t->parent == s && !f->parent &&
             s->spawnDuringLife == 1, "the tree: each root in order, its lifetime child right after it", r);
    if (!s) return;
    std::vector<Track::Key> scale(s->tracks[4]->keys.begin(), s->tracks[4]->keys.end());
    std::vector<Track::Key> alpha(s->tracks[3]->keys.begin(), s->tracks[3]->keys.end());
    check(s->lifetime == 2.5f && s->blendMode == 2 && s->colorTexture == "pe_gen_smoke.dds" &&
          t->lifetime == 0.5f && f->blendMode == 1, "properties, blend modes and textures land");
    check(s->tracks[3] == &s->trackContents[3] && alpha.size() == 3 && alpha[1].time == 10 && alpha[1].value == 1 &&
          s->tracks[1] == &s->trackContents[0] && s->tracks[0]->keys.rbegin()->value == 51 / 255.0f,
          "alpha unlocked and keyed alone; green and blue still follow red");
    check(s->tracks[4]->interpolation == Track::IT_SMOOTH && scale.size() == 3 && scale[1].time == 25 && scale[1].value == 60 &&
          scale[2].value == 80, "scale: smooth, a set and an added key, then doubled by rescale");
    check(s->tracks[5]->keys.size() == 3 && s->tracks[5]->keys.rbegin()->value == 63.5f, "the index track lands");
    const json& report = r.report["emitters"];
    check(report.size() == 3 && report[1]["parent"] == 0 && report[0]["ops"].size() == 12 &&
          report[0]["ops"][0]["fields"][0] == json({{"path", "emitters[0].lifetime"}, {"before", 1}, {"requested", 2.5}, {"persisted", 2.5}}),
          "the report lists each emitter's ops, with before, requested and persisted");
    bool sawAtlas = false;
    for (const json& w : r.report["warnings"]) sawAtlas = sawAtlas || w["code"] == "atlas-out-of-grid";
    check(!sawAtlas, "index 63.5 sits inside a new emitter's 8x8 atlas: no warning");

    r = Run(Spec({Entry("Smoke", {Op("replaceTrack", {{"track", 5}, {"keys", {{0, 0}, {100, 64}}}})})}));
    sawAtlas = false;
    for (const json& w : r.report["warnings"]) sawAtlas = sawAtlas || w["code"] == "atlas-out-of-grid";
    check(r.code == 0 && sawAtlas, "an index of 64, past the 8x8 grid, is a warning, not a refusal");
}

static void TestLocks()
{
    printf("locks\n");
    Result r = Run(Spec({Entry("Fire", {Op("setTrackLock", {{"track", 1}}), Op("setTrackLock", {{"track", 2}}),
                                        Op("replaceTrack", {{"track", 1}, {"keys", {{0, 1}, {100, 0.4}}}}),
                                        Op("setTrackLock", {{"track", 2}, {"lockTo", 1}})})}));
    std::unique_ptr<ParticleSystem> out = LoadOut();
    const Emitter* e = out ? &out->getEmitter(0) : nullptr;
    checkRun(r.code == 0 && e && e->tracks[1] == &e->trackContents[1] && e->tracks[2] == &e->trackContents[1] &&
             e->tracks[3] == &e->trackContents[0], "unlock, key, and lock to an earlier channel all persist", r);
    Refused(Spec({Entry("Fire", {Op("replaceTrack", {{"track", 0}, {"keys", {{0, 1}, {100, 0}}}})})}), 5, "rule",
            "a new emitter's red drives green, blue and alpha: they must be named");
    Refused(Spec({Entry("Fire", {Op("setTrackLock", {{"track", 1}, {"lockTo", 2}})})}), 5, "rule", "a lock to a later channel");
    Refused(Spec({Entry("Fire", {Op("setTrackLock", {{"track", 4}}) })}), 5, "rule", "a lock on a track that is not a colour");
    // Unlocked with the same keys as red, the reader would lock it back unless chunk 0x0101 marks it.
    r = Run(Spec({Entry("Fire", {Op("setTrackLock", {{"track", 3}})})}));
    out = LoadOut();
    e = out ? &out->getEmitter(0) : nullptr;
    checkRun(r.code == 0 && e && e->tracks[3] == &e->trackContents[3], "an unlocked channel equal to red stays unlocked", r);
}

static void TestRefusals()
{
    printf("refusals\n");
    Result r = Run(Spec({Entry("Smoke", {}), Entry("Fire", {Op("setProperty", {{"field", "lifetime"}, {"value", 2}}),
                                                            Op("setProperty", {{"field", "nTriangles"}, {"value", 0}})})}));
    check(r.code == 5 && Category(r) == "rule" && r.report["refusal"]["emitter"] == 1 && r.report["refusal"]["op"] == 1,
          "a refused op names its emitter and op");
    Refused(Spec({Entry("Smoke", {Op("setProperty", {{"field", "blendMode"}, {"value", 14}})})}), 5, "rule", "an out-of-range value");
    Refused(Spec({Entry("Smoke", {Op("setProperty", {{"field", "lifetime"}, {"value", 3e38}}),
                                  Op("rescale", {{"timeScale", 10}, {"sizeScale", 1}})})}), 5, "non-finite-result",
            "edits that overflow to infinity");
    // Saved only under randomRotation: off, it reloads as a new emitter's 0.
    Refused(Spec({Entry("Smoke", {Op("setProperty", {{"field", "randomRotationAverage"}, {"value", 2}})})}), 5, "value-lost",
            "a rotation average that a save drops while randomRotation is off");

    // The spec's shape: exit 2.
    auto malformed = [](const std::string& text, const std::string& what) {
        DeleteFileW(P(L"Out.alo").c_str());
        const Result m = Run(text);
        const bool ok = m.code == 2 && Category(m) == "spec-malformed" && GetFileAttributesW(P(L"Out.alo").c_str()) == INVALID_FILE_ATTRIBUTES;
        check(ok, what);
        if (!ok) printf("    exit %d %s\n", m.code, m.report.contains("refusal") ? m.report["refusal"].dump().c_str() : "");
    };
    const json good = Spec({Entry("Smoke", {Op("setProperty", {{"field", "lifetime"}, {"value", 2}})})});
    malformed("not json", "a spec that is not JSON");
    malformed("{\"schema\": 1, \"schema\": 1}", "a duplicate key");
    json bad = good; bad["schema"] = 2;
    malformed(bad.dump(), "schema 2");
    bad = good; bad["extra"] = 1;
    malformed(bad.dump(), "an unknown spec key");
    bad = good; bad["emitters"] = json::array();
    malformed(bad.dump(), "no emitters");
    bad = good; bad["emitters"] = json::array();
    for (int i = 0; i < 65; i++) bad["emitters"].push_back(Entry(("E" + std::to_string(i)).c_str(), {}));
    malformed(bad.dump(), "more than 64 entries");
    bad = good; bad["emitters"][0].erase("name");
    malformed(bad.dump(), "an emitter with no name");
    bad = good; bad["emitters"][0]["name"] = "";
    malformed(bad.dump(), "an empty name");
    malformed("{\"schema\": 1, \"emitters\": [{\"name\": \"a\xff\", \"mutations\": []}]}", "a name that is not UTF-8");
    bad = good; bad["emitters"][0]["lifeChild"] = Entry("Smoke", {});
    malformed(bad.dump(), "a child named like its root");
    bad = good; bad["emitters"][0]["lifeChild"] = Entry("Trail", {});
    bad["emitters"][0]["lifeChild"]["lifeChild"] = Entry("Deeper", {});
    malformed(bad.dump(), "a lifetime child of a lifetime child");
    bad = good; bad["emitters"][0]["colour"] = 1;
    malformed(bad.dump(), "an unknown emitter key");
    bad = good; bad["emitters"][0].erase("mutations");
    malformed(bad.dump(), "an emitter with no mutations array");
    bad = good; bad["emitters"][0]["mutations"][0] = Op("rename", {{"name", "Other"}});
    malformed(bad.dump(), "a rename: an emitter takes its entry's name");
    bad = good; bad["emitters"][0]["mutations"][0]["op"] = "explode";
    malformed(bad.dump(), "an unknown op");
    {
        const Result m = Run(bad.dump());
        check(m.report["refusal"]["emitter"] == 0 && m.report["refusal"]["op"] == 0, "a malformed op names its emitter and op");
    }
    bad = good; bad["emitters"][0]["mutations"][0]["value"] = "2";
    malformed(bad.dump(), "a wrongly typed op key");
    bad = good; bad["emitters"][0]["mutations"] = json::array();
    for (int i = 0; i < 257; i++) bad["emitters"][0]["mutations"].push_back(good["emitters"][0]["mutations"][0]);
    malformed(bad.dump(), "more than 256 mutations");
}

static void TestAllOrNothing()
{
    printf("all or nothing\n");
    WriteAll(P(L"prior.alo"), "previous candidate");
    const std::set<std::wstring> before = Listing();
    const json spec = Spec({Entry("Smoke", {Op("setProperty", {{"field", "lifetime"}, {"value", 2}})})});
    Result r = Run(Spec({Entry("Smoke", {Op("setProperty", {{"field", "nTriangles"}, {"value", 0}})})}).dump(), P(L"prior.alo"));
    check(r.code == 5 && ReadAll(P(L"prior.alo")) == "previous candidate", "a refused build leaves --out as it was");

    host::BuildHooks hooks;
    hooks.afterStage = [](const std::wstring& staged) {
        std::string bytes = ReadAll(staged);
        bytes[bytes.size() / 2] ^= 0x5A;
        WriteAll(staged, bytes);
    };
    r = Run(spec.dump(), P(L"prior.alo"), hooks);
    check(r.code == 6 && r.report["outcome"] == "postcondition-failed" && r.report["checks"]["4"] == false &&
          ReadAll(P(L"prior.alo")) == "previous candidate",
          "an injected staging fault fails check 4 with exit 6 and leaves --out as it was");
    check(Listing() == before, "no staged file is left behind");
    r = Run(spec.dump(), P(L"no such dir\\out.alo"));
    check(r.code == 13 && Category(r) == "io", "an unstageable --out exits 13");
    r = Run(spec.dump(), P(L"prior.alo"));
    check(r.code == 0 && r.report["candidateHash"] == host::Sha256Hex(ReadAll(P(L"prior.alo"))),
          "a build that passes replaces an existing --out");
    DeleteFileW(P(L"prior.alo").c_str());
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // keep every line if a check crashes
    printf("test_tool_build\n");
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    g_dir = std::wstring(temp) + L"pe tool build " + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(g_dir.c_str(), nullptr);

    TestBuild();
    TestLocks();
    TestRefusals();
    TestAllOrNothing();

    for (const std::wstring& name : Listing()) DeleteFileW(P(name.c_str()).c_str());
    RemoveDirectoryW(g_dir.c_str());
    printf("%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) printf("all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
