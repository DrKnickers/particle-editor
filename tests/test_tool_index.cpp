// Unit test for `--tool index` (src/host/ToolIndex.cpp), run in-process
// through RunIndex over a scratch content stack: a base root with
// Data\MegaFiles.xml, a MEG and loose files, and one mod layer.
//
//   - every particle system directly in Data\Art\Models is listed once across
//     loose files and the MEG, the loose copy winning; meshes are counted and
//     skipped, a particle system that does not load is a failure;
//   - one record per (effect, emitter, texture slot) with the emitter's blend
//     mode, orientation, textureSize and distinct Index-track values;
//   - each texture resolves through the editor's chain: a loose file, the
//     .DDS retry from the MEG, or unresolved; the mod layer's copy wins, and
//     the same basename under the two stacks is two identities;
//   - the stack hash is ContentStackHash of the layers, as --observe stamps it,
//     and the working directory the chain's first step reads from is recorded;
//   - a root or layer that is not a folder is a usage error (2), a root with no
//     MEG list a source error (3).
//
// Standalone console exe; see the test_tool_index entry in tests/native-tests.json.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "effect/ParticleSystem.h"
#include "gamedata/managers.h"
#include "common/crc32.h"
#include "host/ContentStack.h"
#include "host/ToolIndex.h"
#include "host/ToolIO.h"
#include "simulation/ParticleSystemInstance.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Link stub: ~Emitter calls ParticleSystemInstance::RemoveEmitter, whose real
// body is D3D-coupled; no EmitterInstance is ever registered here.
void ParticleSystemInstance::RemoveEmitter(EmitterInstance*) {}

using json = nlohmann::json;
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

static std::wstring g_root; // scratch folder, slash-terminated

static void WriteBytes(const std::wstring& path, const std::string& bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << bytes;
}

static void MakeFolders(const std::wstring& path)
{
    for (size_t at = path.find(L'\\', g_root.size()); at != std::wstring::npos; at = path.find(L'\\', at + 1))
        CreateDirectoryW(path.substr(0, at).c_str(), NULL);
    CreateDirectoryW(path.c_str(), NULL);
}

static std::string Hash(const std::string& s)
{
    return host::Sha256Hex(s);
}
static std::string Utf8(const std::wstring& w)
{
    return AssetPathUtf8(w);
}

// A MEG v1 archive: counts, the name table, the CRC-sorted index, then the data.
struct MegEntry
{
    std::string name, bytes;
};
static void WriteMeg(const std::wstring& path, const std::vector<MegEntry>& entries)
{
    auto u32 = [](std::string& out, unsigned long v)
    {
        out.append(reinterpret_cast<const char*>(&v), 4);
    };
    std::string names;
    for (const auto& e : entries)
    {
        const unsigned short length = static_cast<unsigned short>(e.name.size());
        names.append(reinterpret_cast<const char*>(&length), 2);
        names += e.name;
    }
    unsigned long start =
        8 + static_cast<unsigned long>(names.size()) + 20 * static_cast<unsigned long>(entries.size());
    std::vector<unsigned long> starts;
    for (const auto& e : entries)
    {
        starts.push_back(start);
        start += static_cast<unsigned long>(e.bytes.size());
    }
    std::vector<size_t> order(entries.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b)
              {
                  return crc32(entries[a].name.c_str(), entries[a].name.size()) <
                         crc32(entries[b].name.c_str(), entries[b].name.size());
              });
    std::string out;
    u32(out, static_cast<unsigned long>(entries.size()));
    u32(out, static_cast<unsigned long>(entries.size()));
    out += names;
    for (size_t index = 0; index < order.size(); ++index)
    {
        const MegEntry& e = entries[order[index]];
        u32(out, crc32(e.name.c_str(), e.name.size()));
        u32(out, static_cast<unsigned long>(index));
        u32(out, static_cast<unsigned long>(e.bytes.size()));
        u32(out, starts[order[index]]);
        u32(out, static_cast<unsigned long>(order[index]));
    }
    for (const auto& e : entries) out += e.bytes;
    WriteBytes(path, out);
}

static void SetIndexKeys(Emitter* emitter, const std::vector<std::pair<float, float>>& keys)
{
    auto& track = emitter->tracks[ParticleSystem::TRACK_INDEX]->keys;
    track.clear();
    for (const auto& key : keys) track.insert(Emitter::Track::Key(key.first, key.second));
}

// P_LOOSE: "Smoke" (smoke.tga, found only as SMOKE.DDS in the MEG; a normal
// map that resolves nowhere) and "Fire" (fire.tga, world oriented).
static std::string LooseEffect()
{
    ParticleSystem system;
    system.setName("p_loose");
    Emitter* smoke       = system.addRootEmitter();
    smoke->name          = "Smoke";
    smoke->colorTexture  = "smoke.tga";
    smoke->normalTexture = "Missing_Normal.tga";
    smoke->blendMode     = ParticleSystem::BLEND_TRANSPARENT;
    smoke->textureSize   = 4;
    SetIndexKeys(smoke, {{0.0f, 0.0f}, {30.0f, 3.0f}, {60.0f, 3.0f}, {100.0f, 5.0f}});
    Emitter* fire         = system.addRootEmitter();
    fire->name            = "Fire";
    fire->colorTexture    = "fire.tga";
    fire->blendMode       = ParticleSystem::BLEND_ADDITIVE;
    fire->isWorldOriented = true;
    Emitter* blank        = system.addRootEmitter();
    blank->name           = "Blank";
    blank->colorTexture   = "";
    blank->normalTexture  = ""; // saved without a normal-map chunk: loads the default name
    Emitter* master       = system.addRootEmitter();
    master->name          = "Master"; // keeps the default colour name, which the file always spells out
    return host::SaveToBytes(system);
}

static std::string OneEmitter(const char* name, const char* emitterName, const char* texture)
{
    ParticleSystem system;
    system.setName(name);
    Emitter* emitter      = system.addRootEmitter();
    emitter->name         = emitterName;
    emitter->colorTexture = texture;
    return host::SaveToBytes(system);
}

static const std::string kMesh   = std::string("\x00\x02\x00\x00\x04\x00\x00\x00mesh", 12);
static const std::string kBroken = std::string("\x00\x09\x00\x00\xff\xff\xff\x7f", 8);

static std::wstring g_base, g_mod;

static void BuildStack()
{
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    g_root = std::wstring(temp) + L"pe-tool-index-" + std::to_wstring(GetCurrentProcessId()) + L"\\";
    CreateDirectoryW(g_root.c_str(), NULL);
    g_base = g_root + L"base\\";
    g_mod  = g_root + L"mod\\";
    MakeFolders(g_base + L"Data\\Art\\Models\\Sub");
    MakeFolders(g_base + L"Data\\Art\\Textures");
    MakeFolders(g_mod + L"Data\\Art\\Models");
    MakeFolders(g_mod + L"Data\\Art\\Textures");
    MakeFolders(g_root + L"nomeg");
    MakeFolders(g_root + L"cwd");

    WriteBytes(g_base + L"Data\\MegaFiles.xml",
               "<?xml version=\"1.0\"?>\n<Mega_Files>\n<File>Data\\Base.meg</File>\n</Mega_Files>\n");
    WriteMeg(g_base + L"Data\\Base.meg",
             {
                 {"DATA\\ART\\MODELS\\P_MEG.ALO", OneEmitter("p_meg", "Glow", "Glow.tga")},
                 {"DATA\\ART\\MODELS\\P_LOOSE.ALO", OneEmitter("p_loose_stale", "Stale", "stale.tga")},
                 {"DATA\\ART\\MODELS\\MESH_IN_MEG.ALO", kMesh},
                 {"DATA\\ART\\MODELS\\SUB\\P_MEGDEEP.ALO", OneEmitter("p_megdeep", "Deep", "deep.tga")},
                 {"DATA\\ART\\TEXTURES\\GLOW.TGA", "glow-meg"},
                 {"DATA\\ART\\TEXTURES\\SMOKE.DDS", "smoke-dds-meg"},
             });
    const std::wstring models = g_base + L"Data\\Art\\Models\\";
    WriteBytes(models + L"P_Loose.alo", LooseEffect());
    WriteBytes(models + L"Mesh.alo", kMesh);
    WriteBytes(models + L"Broken.alo", kBroken);
    WriteBytes(models + L"Notes.txt", "not a model");
    WriteBytes(models + L"Sub\\P_Deep.alo", OneEmitter("p_deep", "Deep", "deep.tga"));
    WriteBytes(g_base + L"Data\\Art\\Textures\\FIRE.TGA", "fire-base");

    WriteBytes(g_mod + L"Data\\Art\\Models\\P_Mod.alo", OneEmitter("p_mod", "Ember", "Fire.TGA"));
    WriteBytes(g_mod + L"Data\\Art\\Textures\\Fire.tga", "fire-mod");

    // The chain's first step reads the name as given, relative to the working directory.
    SetCurrentDirectoryW((g_root + L"cwd").c_str());
}

struct Result
{
    int         code;
    json        doc;
    std::string error;
};

static Result Index(const std::vector<std::wstring>& roots, const std::vector<std::wstring>& layers)
{
    Result           r;
    host::IndexStack stack;
    stack.gameRoots = roots;
    stack.layers    = layers;
    r.code          = host::RunIndex(stack, &r.doc, &r.error);
    return r;
}

static json Find(json doc, const std::string& effect, const std::string& emitter, const std::string& slot)
{
    for (json& record : doc["records"])
        if (record["effect"] == effect && record["emitter"]["name"] == emitter && record["slot"] == slot) return record;
    return json();
}

static std::vector<std::string> Files(json list)
{
    std::vector<std::string> files;
    for (json& entry : list) files.push_back(entry["file"].get<std::string>());
    return files;
}

static const std::string kLoose = "DATA\\ART\\MODELS\\P_LOOSE.ALO";
static const std::string kMeg   = "DATA\\ART\\MODELS\\P_MEG.ALO";
static const std::string kMod   = "DATA\\ART\\MODELS\\P_MOD.ALO";

static void TestUnmodded(json& base)
{
    printf("the base stack\n");
    Result r  = Index({g_base}, {});
    base      = r.doc;
    json& doc = r.doc;
    check(r.code == 0, "the index succeeds");
    check(doc["content"]["layers"] == json::array() && doc["content"]["sha256"] == host::ContentStackHash({}) &&
              doc["content"]["sha256"] == Hash("[]"),
          "the stack hash is --observe's hash of the empty layer list");
    check(doc["content"]["workingDirectory"] == host::ContentPaths({g_root + L"cwd"})[0],
          "the working directory the literal-name step reads from is recorded");
    check(Files(doc["systems"]) == std::vector<std::string>({kLoose, kMeg}),
          "each particle system directly in Data\\Art\\Models is listed once, sorted; a subfolder is not");
    check(doc["summary"]["meshes"] == 2 && doc["summary"]["models"] == 5,
          "the loose and archived meshes are counted and skipped");
    check(Files(doc["failures"]) == std::vector<std::string>({"DATA\\ART\\MODELS\\BROKEN.ALO"}) &&
              !doc["failures"][0]["reason"].get<std::string>().empty(),
          "a particle system that does not load is a failure");

    json& loose = doc["systems"][0];
    check(loose["source"]["kind"] == "loose" &&
              loose["source"]["path"] == Utf8(g_base + L"DATA\\ART\\MODELS\\P_LOOSE.ALO") &&
              loose["sha256"] == Hash(LooseEffect()) && loose["internalName"] == "p_loose" && loose["emitters"] == 4,
          "the loose copy wins over the MEG's, hashed as read");
    check(doc["systems"][1]["source"]["kind"] == "archive" &&
              doc["systems"][1]["source"]["member"] == "DATA\\ART\\MODELS\\P_MEG.ALO",
          "a MEG-only system names its member");

    check(doc["records"].size() == 9 && doc["summary"]["records"] == 9,
          "one record per (effect, emitter, texture slot)");
    json smoke = Find(doc, kLoose, "Smoke", "color");
    check(smoke["blendMode"] == ParticleSystem::BLEND_TRANSPARENT && smoke["textureSize"] == 4 &&
              smoke["isWorldOriented"] == false && smoke["indexValues"] == json({0.0, 3.0, 5.0}) &&
              smoke["emitter"]["index"] == 0,
          "the record carries the emitter's blend, orientation, atlas and Index values");
    check(smoke["texture"]["request"] == "smoke.tga" && smoke["texture"]["canonicalName"] == "SMOKE.TGA" &&
              smoke["texture"]["outcome"] == "resolved" && smoke["texture"]["source"]["kind"] == "archive" &&
              smoke["texture"]["source"]["member"] == "DATA\\ART\\TEXTURES\\SMOKE.DDS" &&
              smoke["texture"]["sha256"] == Hash("smoke-dds-meg"),
          "a name found only as .DDS resolves by the retry, from the MEG");
    json normal = Find(doc, kLoose, "Smoke", "normal");
    check(normal["texture"]["outcome"] == "unresolved" && normal["texture"]["source"].is_null() &&
              normal["texture"]["sha256"].is_null() && normal["texture"]["canonicalName"] == "MISSING_NORMAL.TGA",
          "a texture found nowhere is unresolved, with no substitute");
    json fire = Find(doc, kLoose, "Fire", "color");
    check(fire["isWorldOriented"] == true && fire["texture"]["source"]["kind"] == "loose" &&
              fire["texture"]["source"]["path"] == Utf8(g_base + L"Data\\Art\\Textures\\FIRE.TGA") &&
              fire["texture"]["sha256"] == Hash("fire-base"),
          "a loose base texture resolves to its file");
    check(Find(doc, kLoose, "Blank", "color").is_null(), "an empty texture slot has no record");
    json blank = Find(doc, kLoose, "Blank", "normal");
    check(blank["texture"]["canonicalName"] == "P_PARTICLE_DEPTH_MASTER.TGA" &&
              blank["texture"]["loaderDefault"] == true &&
              Find(doc, kLoose, "Fire", "normal")["texture"]["loaderDefault"] == true,
          "the default normal name, whether the file omits the chunk (Blank) or spells it out (Fire), is recorded and "
          "flagged");
    check(smoke["texture"]["loaderDefault"] == false && normal["texture"]["loaderDefault"] == false,
          "an authored name is not flagged");
    json master = Find(doc, kLoose, "Master", "color");
    check(master["texture"]["canonicalName"] == "P_PARTICLE_MASTER.TGA" && master["texture"]["loaderDefault"] == false,
          "a colour name equal to the default is still the file's own, not flagged");
    check(doc["summary"]["textures"]["lookups"] == 9 && doc["summary"]["textures"]["resolved"] == 3 &&
              doc["summary"]["textures"]["unresolved"] == 6 && doc["summary"]["unverifiable"].is_null(),
          "the summary counts");
}

static void TestModded(json base)
{
    printf("the base stack under a mod layer\n");
    Result r   = Index({g_base}, {g_mod});
    json&  doc = r.doc;
    check(r.code == 0, "the index succeeds");
    check(doc["content"]["sha256"] == host::ContentStackHash({g_mod}) &&
              doc["content"]["sha256"] != base["content"]["sha256"] &&
              doc["content"]["layers"] == host::ContentPaths({g_mod}),
          "the stack hash covers the layer");
    check(Files(doc["systems"]) == std::vector<std::string>({kLoose, kMeg, kMod}), "the layer's own effect is listed");
    json fire  = Find(doc, kLoose, "Fire", "color");
    json ember = Find(doc, kMod, "Ember", "color");
    check(fire["texture"]["source"]["path"] == Utf8(g_mod + L"Data\\Art\\Textures\\FIRE.TGA") &&
              fire["texture"]["sha256"] == Hash("fire-mod"),
          "the layer's copy of a base texture wins");
    check(ember["texture"]["canonicalName"] == "FIRE.TGA" && ember["texture"]["sha256"] == fire["texture"]["sha256"],
          "two requests spelt differently reach the one identity");
    json baseFire = Find(base, kLoose, "Fire", "color");
    check(baseFire["texture"]["canonicalName"] == fire["texture"]["canonicalName"] &&
              baseFire["texture"]["sha256"] != fire["texture"]["sha256"] &&
              base["content"]["sha256"] != doc["content"]["sha256"],
          "the same basename under two stacks is two identities");
    check(Find(doc, kLoose, "Smoke", "color")["texture"] == Find(base, kLoose, "Smoke", "color")["texture"],
          "a texture the layer does not override resolves as before");
}

static void TestRefusals()
{
    printf("refusals\n");
    Result r = Index({g_root + L"absent"}, {});
    check(r.code == 2 && r.error.find("game root") != std::string::npos,
          "a game root that is not a folder is a usage error");
    r = Index({g_base}, {g_root + L"absent"});
    check(r.code == 2 && r.error.find("layer") != std::string::npos, "a layer that is not a folder is a usage error");
    r = Index({g_root + L"nomeg"}, {});
    check(r.code == 3 && r.error.find("MegaFiles.xml") != std::string::npos,
          "a root with no MEG list is a source error");
}

int main()
{
    setvbuf(stdout, NULL, _IONBF, 0); // a crash still shows every check before it
    printf("test_tool_index\n");
    BuildStack();
    try
    {
        json base;
        TestUnmodded(base);
        TestModded(base);
        TestRefusals();
    }
    catch (const std::exception& e)
    {
        check(false, std::string("no exception: ") + e.what());
    }
    SetCurrentDirectoryW((g_root + L"..").c_str()); // leave the folder before removing it
    std::error_code ignored;
    std::filesystem::remove_all(g_root, ignored);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
