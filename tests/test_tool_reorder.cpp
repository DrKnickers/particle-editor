// Unit test for `--tool reorder` (src/host/ToolReorder.cpp), run in-process
// through RunReorder.
//
//   - a root moves, with its subtree, to the gap it names; the report's map
//     describes the candidate exactly; the candidate is save-stable and
//     reloads with every link mapped;
//   - each refusal reaches its exit code: a malformed request (2), an invalid
//     source (3), a hash or name mismatch or a missing emitter (4), and a
//     child, linked or unmoved target (5);
//   - nothing reaches --out unless the reorder succeeded.
//
// Runs from the repo root (the native test runner's cwd). Standalone console
// exe; see the test_tool_reorder entry in tests/native-tests.json.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "effect/ParticleSystem.h"
#include "effect/ParticleSystemJson.h"
#include "host/ToolIO.h"
#include "host/ToolReorder.h"
#include "simulation/ParticleSystemInstance.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// Link stub: ~Emitter calls ParticleSystemInstance::RemoveEmitter, whose real
// body is D3D-coupled; no EmitterInstance is ever registered here.
void ParticleSystemInstance::RemoveEmitter(EmitterInstance*) {}

using json = nlohmann::json;
typedef ParticleSystem::Emitter Emitter;

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

static bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// The fixture: roots "Smoke" (with life child "Sparks"), "Fire", "Linked"
// (link group 1) and "Ember", each with its own lifetime so a moved payload
// is recognisable.
static std::string Fixture()
{
    ParticleSystem system;
    system.setName("fixture");
    Emitter* smoke = system.addRootEmitter();
    smoke->name = "Smoke";
    smoke->lifetime = 1.5f;
    Emitter* sparks = system.addLifetimeEmitter(smoke);
    sparks->name = "Sparks";
    sparks->lifetime = 0.5f;
    Emitter* fire = system.addRootEmitter();
    fire->name = "Fire";
    fire->lifetime = 2.0f;
    Emitter* linked = system.addRootEmitter();
    linked->name = "Linked";
    linked->linkGroup = 1;
    Emitter* ember = system.addRootEmitter();
    ember->name = "Ember";
    ember->lifetime = 3.0f;
    return host::SaveToBytes(system);
}

static std::string g_fixture;
static json g_dump;

static size_t PositionOf(const json& dump, const std::string& name)
{
    for (size_t i = 0; i < dump["emitters"].size(); i++)
        if (dump["emitters"][i]["name"] == name) return i;
    return static_cast<size_t>(-1);
}

static json Request(const std::string& name, json toRoot, json extra = json::object())
{
    json r = {{"schema", 1}, {"sourceDocumentHash", host::Sha256Hex(g_fixture)},
              {"move", {{"index", PositionOf(g_dump, name)}, {"name", name}}}, {"toRoot", toRoot}};
    for (auto it = extra.begin(); it != extra.end(); ++it) r[it.key()] = it.value();
    return r;
}

struct Result
{
    int  code;
    json report;
};

static Result Run(const std::string& request, const std::string& source = g_fixture)
{
    DeleteFileW(P(L"out.alo").c_str());
    Result r;
    r.code = host::RunReorder(source, host::Sha256Hex(source), request, P(L"out.alo"), &r.report);
    return r;
}

static json Dump(const std::string& bytes)
{
    std::string error;
    std::unique_ptr<ParticleSystem> system = host::ParseSource(bytes, &error);
    return system ? particle_json::ParticleSystemJson(*system) : json();
}

static std::vector<std::string> RootNames(const json& dump)
{
    std::vector<std::string> names;
    for (const json& e : dump["emitters"])
        if (e["parent"].is_null()) names.push_back(e["name"].get<std::string>());
    return names;
}

static void TestMove()
{
    printf("a root moves to its gap, with its subtree\n");
    check(RootNames(g_dump) == std::vector<std::string>({"Smoke", "Fire", "Linked", "Ember"}), "the fixture's root order");

    Result r = Run(Request("Ember", 0).dump());
    const std::string out = ReadAll(P(L"out.alo"));
    const json c = Dump(out);
    check(r.code == 0 && r.report["outcome"] == "ok", "Ember to gap 0 succeeds");
    check(RootNames(c) == std::vector<std::string>({"Ember", "Smoke", "Fire", "Linked"}), "Ember is drawn first");
    check(r.report["candidateHash"] == host::Sha256Hex(out), "the report names the candidate's hash");
    std::string error;
    std::unique_ptr<ParticleSystem> reloaded = host::ParseSource(out, &error);
    check(reloaded && host::SaveToBytes(*reloaded) == out && reloaded->getGraphRepairs().empty(), "the candidate is save-stable");

    const json& map = r.report["map"];
    bool mapped = map.size() == g_dump["emitters"].size();
    for (size_t b = 0; mapped && b < map.size(); b++)
    {
        const json& s = g_dump["emitters"][b];
        const json& e = c["emitters"][map[b].get<size_t>()];
        mapped = e["name"] == s["name"] && e["lifetime"] == s["lifetime"] && e["index"] == map[b] &&
                 (s["parent"].is_null() ? e["parent"].is_null() : e["parent"] == map[s["parent"].get<size_t>()]) &&
                 (s["spawnDuringLife"].is_null() ? e["spawnDuringLife"].is_null() : e["spawnDuringLife"] == map[s["spawnDuringLife"].get<size_t>()]);
    }
    check(mapped, "the map moves every emitter, its index and its links");

    r = Run(Request("Smoke", 4).dump());
    const json moved = Dump(ReadAll(P(L"out.alo")));
    const size_t sparks = PositionOf(moved, "Sparks");
    check(r.code == 0 && RootNames(moved) == std::vector<std::string>({"Fire", "Linked", "Ember", "Smoke"}), "Smoke to the last gap succeeds");
    check(sparks != static_cast<size_t>(-1) && moved["emitters"][sparks]["parent"] == PositionOf(moved, "Smoke") &&
          moved["emitters"][PositionOf(moved, "Smoke")]["spawnDuringLife"] == sparks, "Sparks stays Smoke's life child");
}

static void Refused(const json& request, int code, const char* category, const std::string& what, const std::string& source = g_fixture)
{
    const Result r = Run(request.is_string() ? request.get<std::string>() : request.dump(), source);
    check(r.code == code && r.report["outcome"] == "refused" && r.report["refusal"]["category"] == category && !Exists(P(L"out.alo")),
          what + " (exit " + std::to_string(r.code) + ", " + (r.report.contains("refusal") ? r.report["refusal"]["category"].dump() : "no refusal") + ")");
}

static void TestRefusals()
{
    printf("refusals\n");
    Refused(json("not json"), 2, "request-malformed", "a request that is not JSON");
    Refused(Request("Fire", 0, {{"extra", 1}}), 2, "request-malformed", "an unknown key");
    Refused(Request("Fire", -1), 2, "request-malformed", "a negative gap");
    json noName = Request("Fire", 0);
    noName["move"].erase("name");
    Refused(noName, 2, "request-malformed", "a move without a name");
    Refused(Request("Fire", 0), 3, "source-invalid", "a source that does not load", std::string("not a particle file"));
    Refused(Request("Fire", 0, {{"sourceDocumentHash", std::string(64, '0')}}), 4, "hash-mismatch", "a request for other bytes");
    json wrong = Request("Fire", 0);
    wrong["move"]["name"] = "Ember";
    Refused(wrong, 4, "name-mismatch", "a position whose name differs");
    json missing = Request("Fire", 0);
    missing["move"]["index"] = 99;
    Refused(missing, 4, "no-target", "a position past the last emitter");
    Refused(Request("Sparks", 0), 5, "not-a-root", "a child");
    Refused(Request("Linked", 0), 5, "linked-target", "a linked root");
    Refused(Request("Fire", 1), 5, "no-move", "a gap that leaves the order unchanged");
    Refused(Request("Fire", 2), 5, "no-move", "the gap just below it");
    Refused(Request("Fire", 5), 5, "no-move", "a gap past the last root");
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // keep every line if a check crashes
    printf("test_tool_reorder\n");
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    g_dir = std::wstring(temp) + L"pe tool reorder " + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(g_dir.c_str(), nullptr);
    g_fixture = Fixture();
    g_dump = Dump(g_fixture);

    TestMove();
    TestRefusals();

    DeleteFileW(P(L"out.alo").c_str());
    RemoveDirectoryW(g_dir.c_str());
    printf("%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) printf("all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
