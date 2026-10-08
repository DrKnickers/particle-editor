// `--tool inspect` completeness (Phase 4B PR 1; tasks/2026-10-07-phase4b-tool-verbs-plan.md,
// benchmark plan section 3.2). The member enumeration in ParticleSystemJson.h
// is maintained by hand; these checks catch a drift mechanically:
//
//   A. leaveParticles: a file without chunk 0x0002 loads (and re-saves) true;
//      a file carrying false still reads false.
//   B. Chunk coverage: a fixture exercising every optional state is saved and
//      its chunk ids are walked by path; every path must map to the inspect
//      fields that carry it, and every mapped path must occur. Catches a new
//      chunk.
//   C. Member tables: every member persisted-members.json maps appears in the
//      dump, and the sizeof guards. (members.test.mjs checks the same table
//      against the header declarations, which catches a new member.)
//   D. Projection: for every persisted item, a mutation's in-memory dump run
//      through ProjectPersisted equals the dump of the saved-and-reloaded
//      file, compared as text so -0.0 and 0.0 differ. Cases the projection
//      calls unloadable must fail to load.
//   E. Inspect refusals: a non-finite float or a non-UTF-8 string is found
//      and named; valid non-ASCII is kept byte for byte; a raw 0x17 infinity
//      loads clamped and inspects cleanly.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "effect/ParticleSystem.h"
#include "effect/ParticleSystemJson.h"
#include "effect/PersistedProjection.h"
#include "effect/LinkGroup.h"
#include "simulation/ParticleSystemInstance.h"
#include "common/files.h"
#include "common/exceptions.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Link stub -- see test_alo_roundtrip.cpp. ~Emitter calls
// ParticleSystemInstance::RemoveEmitter whose real body is D3D-coupled.
void ParticleSystemInstance::RemoveEmitter(EmitterInstance*) {}

using json = nlohmann::json;
typedef ParticleSystem::Emitter Emitter;
typedef Emitter::Track Track;

static int g_failed = 0;
static int g_checks = 0;

static void check(bool ok, const std::string& what)
{
    g_checks++;
    if (!ok)
    {
        g_failed++;
        printf("  FAIL: %s\n", what.c_str());
    }
}

static std::string SaveBytes(ParticleSystem& system)
{
    MemoryFile file;
    system.write(&file);
    std::string bytes(file.size(), '\0');
    file.seek(0);
    if (!bytes.empty()) file.read(&bytes[0], static_cast<unsigned long>(bytes.size()));
    return bytes;
}

// Null when the bytes do not load.
static std::unique_ptr<ParticleSystem> LoadBytes(const std::string& bytes)
{
    try
    {
        MemoryFile file;
        if (!bytes.empty()) file.write(bytes.data(), static_cast<unsigned long>(bytes.size()));
        file.seek(0);
        return std::make_unique<ParticleSystem>(&file);
    }
    catch (...)
    {
        return nullptr;
    }
}

struct TimeValue { float time, value; };

static void SetKeys(Emitter& e, int track, std::vector<TimeValue> keys)
{
    e.trackContents[track].keys.clear();
    for (const auto& k : keys) e.trackContents[track].keys.insert(Track::Key(k.time, k.value));
    e.tracks[track] = &e.trackContents[track];
}

// The base document: two linked roots with a custom exempt set, a third root
// with a life child and a death child, unlocked and locked colour channels,
// interior keys on every track kind.
static std::unique_ptr<ParticleSystem> BuildBase()
{
    auto system = std::make_unique<ParticleSystem>();
    system->setName("json_base");
    Emitter* a = system->addRootEmitter();
    a->name = "Root A";
    a->colorTexture = "p_smoke.tga";
    a->normalTexture = "p_smoke_n.tga";
    SetKeys(*a, 0, {{0, 1.0f}, {25, 0.5f}, {100, 0.2f}});
    SetKeys(*a, 1, {{0, 0.8f}, {50, 0.4f}, {100, 0.0f}});
    a->tracks[2] = a->tracks[0];
    SetKeys(*a, 3, {{0, 0.0f}, {12.5f, 1.0f}, {87.5f, 1.0f}, {100, 0.0f}});
    SetKeys(*a, 4, {{0, 5.0f}, {62.5f, 40.0f}, {100, 80.0f}});
    SetKeys(*a, 5, {{0, 0.0f}, {25, 1.0f}, {50, 2.0f}, {100, 3.0f}});
    SetKeys(*a, 6, {{0, 0.0f}, {37.5f, 90.0f}, {100, -45.0f}});
    a->lifetime = 1.25f;
    a->randomLifetimePerc = 0.2f;

    Emitter* life = system->addLifetimeEmitter(a);
    life->name = "Life";
    Emitter* death = system->addDeathEmitter(a);
    death->name = "Death";

    Emitter* b = system->addRootEmitter();
    b->name = "Linked B";
    Emitter* c = system->addRootEmitter();
    c->name = "Linked C";
    b->linkGroup = 1;
    c->linkGroup = 1;
    LinkExemptFlags flags = GetDefaultLinkExemptFlags();
    flags.lifetime = true;
    flags.trackScale = true;
    system->setLinkExemptFlags(1, flags);
    return system;
}

static std::string g_baseBytes;

static std::unique_ptr<ParticleSystem> FreshBase() { return LoadBytes(g_baseBytes); }

// ---------------------------------------------------------------------------
// A. leaveParticles default
// ---------------------------------------------------------------------------
static void TestLeaveParticlesDefault()
{
    printf("A. leaveParticles\n");
    ParticleSystem system;
    system.addRootEmitter();
    std::string bytes = SaveBytes(system);
    // The system ends with chunk 0x0002: an 8-byte header and one byte.
    const size_t tail = 9;
    check(bytes.size() > tail && static_cast<unsigned char>(bytes[bytes.size() - tail]) == 0x02,
          "the saved file ends with chunk 0x0002");
    std::string without = bytes.substr(0, bytes.size() - tail);
    uint32_t rootSize = 0;
    memcpy(&rootSize, &without[4], 4);
    rootSize -= tail;
    memcpy(&without[4], &rootSize, 4);
    for (int run = 0; run < 3; run++)
    {
        auto loaded = LoadBytes(without);
        check(loaded != nullptr, "a file without chunk 0x0002 loads");
        if (!loaded) return;
        check(loaded->getLeaveParticles(), "a file without chunk 0x0002 loads leaveParticles = true");
        check(particle_json::ParticleSystemJson(*loaded)["leaveParticles"] == true, "and inspects as true");
        auto resaved = LoadBytes(SaveBytes(*loaded));
        check(resaved && resaved->getLeaveParticles(), "and re-saves as true");
    }
    system.setLeaveParticles(false);
    auto falseLoaded = LoadBytes(SaveBytes(system));
    check(falseLoaded && !falseLoaded->getLeaveParticles(), "a file carrying false still reads false");
}

// ---------------------------------------------------------------------------
// B. Chunk coverage
// ---------------------------------------------------------------------------
static std::string Hex(long type)
{
    char buffer[16];
    sprintf_s(buffer, "%04lx", static_cast<unsigned long>(type));
    return buffer;
}

// Data chunks whose payload is a sequence of mini-chunks.
static bool HoldsMiniChunks(const std::string& path)
{
    static const std::set<std::string> paths = {
        "0900/0800/0700/0002", "0900/0800/0700/0036",
        "0900/0800/0700/0001/0000", "0900/0800/0700/0001/0001",
    };
    return paths.count(path) != 0;
}

static void WalkChunks(ChunkReader& reader, const std::string& prefix, std::set<std::string>* paths)
{
    ChunkType type;
    while ((type = reader.next()) != -1)
    {
        const std::string path = prefix.empty() ? Hex(type) : prefix + "/" + Hex(type);
        paths->insert(path);
        if (reader.size() < 0)
            WalkChunks(reader, path, paths);
        else if (HoldsMiniChunks(path))
        {
            ChunkType mini;
            while ((mini = reader.nextMini()) != -1) paths->insert(path + "/" + Hex(mini).substr(2));
        }
    }
}

static void TestChunkCoverage()
{
    printf("B. chunk coverage\n");
    auto system = FreshBase();
    Emitter& a = system->getEmitter(0);
    // Every optional state: 0x11, randomRotation, an explicit unlock (green
    // unlocked with the same keys as red).
    a.has11 = true;
    a.unknown11 = 0.5f;
    a.randomRotation = true;
    a.randomRotationAverage = 30.0f;
    a.trackContents[1] = *a.tracks[0];
    a.tracks[1] = &a.trackContents[1];
    check(a.explicitUnlockMask() != 0, "the fixture carries an explicit unlock");
    const std::string bytes = SaveBytes(*system);

    // Path -> the inspect fields (emitter keys, or system keys for 0900/*)
    // that carry it.
    const std::string E = "0900/0800/0700";
    const std::string P = E + "/0002/";
    const std::map<std::string, std::vector<std::string>> table = {
        {"0900", {}}, {"0900/0000", {"name"}}, {"0900/0001", {}}, {"0900/0800", {"emitters"}},
        {"0900/0002", {"leaveParticles"}}, {"0900/0003", {"linkExempts"}},
        {E, {}}, {E + "/0002", {}},
        {P + "04", {"blendMode"}}, {P + "05", {"nTriangles"}}, {P + "06", {"unknown06"}},
        {P + "07", {"useBursts"}}, {P + "43", {"parentLinkStrength"}}, {P + "08", {"linkToSystem"}},
        {P + "09", {"inwardSpeed"}}, {P + "0a", {"acceleration"}}, {P + "0c", {"gravity"}},
        {P + "0f", {"lifetime"}}, {P + "11", {"unknown11"}}, {P + "12", {"randomScalePerc"}},
        {P + "13", {"randomLifetimePerc"}}, {P + "49", {"unknown49"}}, {P + "10", {"textureSize"}},
        {P + "14", {"index"}}, {P + "15", {"unknown15"}}, {P + "17", {"randomRotationVariance"}},
        {P + "0b", {"inwardAcceleration"}}, {P + "23", {"randomRotationDirection"}},
        {P + "24", {"initialDelay"}}, {P + "25", {"burstDelay"}}, {P + "26", {"nParticlesPerBurst"}},
        {P + "27", {"nBursts"}}, {P + "28", {"parentLinkStrength"}}, {P + "2a", {"nParticlesPerSecond"}},
        {P + "2b", {"unknown2b"}}, {P + "2c", {"randomColors"}}, {P + "2d", {"doColorAddGrayscale"}},
        {P + "2e", {"isWorldOriented"}}, {P + "2f", {"groundBehavior"}}, {P + "30", {"bounciness"}},
        {P + "31", {"affectedByWind"}}, {P + "32", {"freezeTime"}}, {P + "33", {"skipTime"}},
        {P + "34", {"emitFromMesh"}}, {P + "35", {"objectSpaceAcceleration"}}, {P + "3b", {"isHeatParticle"}},
        {P + "3c", {"emitFromMeshOffset"}}, {P + "3d", {"isWeatherParticle"}}, {P + "3e", {"weatherCubeSize"}},
        {P + "3f", {"unknown3f"}}, {P + "40", {"weatherFadeoutDistance"}}, {P + "41", {"hasTail"}},
        {P + "42", {"tailSize"}}, {P + "44", {"unknown44"}}, {P + "46", {"noDepthTest"}},
        {P + "47", {"weatherCubeDistance"}}, {P + "48", {"randomRotation"}},
        {E + "/0003", {"colorTexture"}}, {E + "/0016", {"name"}},
        {E + "/0029", {"groups"}}, {E + "/0029/1100", {"groups"}}, {E + "/0029/1100/1101", {"groups"}},
        {E + "/0001", {"tracks"}},
        {E + "/0001/0000", {"tracks"}}, {E + "/0001/0000/02", {"tracks"}}, {E + "/0001/0000/03", {"tracks"}},
        {E + "/0001/0000/04", {"tracks"}}, {E + "/0001/0001", {"tracks"}}, {E + "/0001/0001/05", {"tracks"}},
        {E + "/0036", {}}, {E + "/0036/37", {"spawnOnDeath"}}, {E + "/0036/39", {"spawnDuringLife"}},
        {E + "/0045", {"normalTexture"}}, {E + "/0100", {"linkGroup"}}, {E + "/0101", {"tracks"}},
    };
    // randomRotationAverage rides in the rotation track's first endpoint
    // (E/0001/0000/02) under randomRotation; it has no chunk of its own.

    MemoryFile file;
    file.write(bytes.data(), static_cast<unsigned long>(bytes.size()));
    file.seek(0);
    std::set<std::string> paths;
    ChunkReader reader(&file);
    WalkChunks(reader, "", &paths);

    const json dump = particle_json::ParticleSystemJson(*system);
    for (const std::string& path : paths)
    {
        auto it = table.find(path);
        check(it != table.end(), "chunk path " + path + " is mapped to inspect fields");
        if (it == table.end()) continue;
        for (const std::string& field : it->second)
        {
            const bool systemField = path.rfind(E, 0) != 0;
            check(systemField ? dump.contains(field) : dump["emitters"][0].contains(field),
                  "chunk path " + path + " maps to a dumped field " + field);
        }
    }
    for (const auto& entry : table) check(paths.count(entry.first) != 0, "mapped chunk path " + entry.first + " occurs in the fixture");
    printf("  %zu chunk paths walked, %zu mapped\n", paths.size(), table.size());
}

// ---------------------------------------------------------------------------
// C. Member tables
// ---------------------------------------------------------------------------
static std::string RepoRoot()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe);
    for (int up = 0; up < 2; up++) path = path.substr(0, path.find_last_of(L"\\/"));
    char narrow[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr);
    return narrow;
}

static void TestMemberTables()
{
    printf("C. member tables\n");
    check(sizeof(Emitter::Group) == 16 * 4, "Group is sixteen 4-byte members");
    size_t exemptCount = 0;
    particle_json::ExemptMembers(&exemptCount);
    check(sizeof(LinkExemptFlags) == exemptCount, "every LinkExemptFlags byte is a named member in the dump");

    std::ifstream in(RepoRoot() + "/scripts/vfx-benchmark/fixtures/persisted-members.json");
    check(in.good(), "persisted-members.json is readable");
    if (!in.good()) return;
    const json table = json::parse(in);
    auto system = FreshBase();
    const json dump = particle_json::ParticleSystemJson(*system);
    const json& emitter = dump["emitters"][0];
    for (auto it = table["Emitter"]["persisted"].begin(); it != table["Emitter"]["persisted"].end(); ++it)
        check(emitter.contains(it.value().get<std::string>()), "Emitter member " + it.key() + " is dumped");
    for (auto it = table["Group"]["persisted"].begin(); it != table["Group"]["persisted"].end(); ++it)
        check(emitter["groups"][0].contains(it.value().get<std::string>()), "Group member " + it.key() + " is dumped");
    const json& flags = dump["linkExempts"][0]["flags"];
    check(flags.size() == table["LinkExemptFlags"]["persisted"].size(), "the dump names every LinkExemptFlags member");
    for (auto it = table["LinkExemptFlags"]["persisted"].begin(); it != table["LinkExemptFlags"]["persisted"].end(); ++it)
        check(flags.contains(it.value().get<std::string>()), "LinkExemptFlags member " + it.key() + " is dumped");
}

// ---------------------------------------------------------------------------
// D. Projection
// ---------------------------------------------------------------------------
struct Mutation
{
    std::string name;
    std::function<void(ParticleSystem&)> apply;
    bool unloadable;
};

static std::vector<Mutation> Mutations()
{
    auto on = [](int index, std::function<void(Emitter&)> f) {
        return [index, f](ParticleSystem& s) { f(s.getEmitter(index)); };
    };
    std::vector<Mutation> m = {
        {"unchanged", [](ParticleSystem&) {}, false},
        {"system name", [](ParticleSystem& s) { s.setName("renamed_system"); }, false},
        {"leaveParticles false", [](ParticleSystem& s) { s.setLeaveParticles(false); }, false},
        {"exempt set changed", [](ParticleSystem& s) {
            LinkExemptFlags f = GetDefaultLinkExemptFlags(); f.gravity = true; f.unknown49 = true;
            s.setLinkExemptFlags(1, f); }, false},
        {"exempt set for an empty group", [](ParticleSystem& s) {
            LinkExemptFlags f = GetDefaultLinkExemptFlags(); f.tailSize = true; s.setLinkExemptFlags(7, f); }, false},
        {"name non-ASCII", on(0, [](Emitter& e) { e.name = "Fum\xC3\xA9" "e \xE2\x9C\xA8"; }), false},
        {"textures", on(1, [](Emitter& e) { e.colorTexture = "p_spark.tga"; e.normalTexture = "p_spark_n.dds"; }), false},
        {"normal texture cleared", on(0, [](Emitter& e) { e.normalTexture = ""; }), false},
        {"link group", on(4, [](Emitter& e) { e.linkGroup = 9; }), false},
    };
    // Every boolean.
    const std::vector<std::pair<const char*, bool Emitter::*>> bools = {
        {"linkToSystem", &Emitter::linkToSystem}, {"objectSpaceAcceleration", &Emitter::objectSpaceAcceleration},
        {"doColorAddGrayscale", &Emitter::doColorAddGrayscale}, {"affectedByWind", &Emitter::affectedByWind},
        {"isHeatParticle", &Emitter::isHeatParticle}, {"isWeatherParticle", &Emitter::isWeatherParticle},
        {"hasTail", &Emitter::hasTail}, {"noDepthTest", &Emitter::noDepthTest},
        {"randomRotationDirection", &Emitter::randomRotationDirection}, {"isWorldOriented", &Emitter::isWorldOriented},
        {"useBursts", &Emitter::useBursts}, {"unknown15", &Emitter::unknown15}, {"unknown2b", &Emitter::unknown2b},
        {"unknown44", &Emitter::unknown44},
    };
    for (const auto& b : bools)
    {
        auto member = b.second;
        m.push_back({std::string("toggle ") + b.first, on(0, [member](Emitter& e) { e.*member = !(e.*member); }), false});
    }
    // Every float scalar, with an ordinary and a negative-zero value.
    const std::vector<std::pair<const char*, float Emitter::*>> floats = {
        {"gravity", &Emitter::gravity}, {"lifetime", &Emitter::lifetime}, {"initialDelay", &Emitter::initialDelay},
        {"burstDelay", &Emitter::burstDelay}, {"inwardSpeed", &Emitter::inwardSpeed},
        {"inwardAcceleration", &Emitter::inwardAcceleration}, {"randomScalePerc", &Emitter::randomScalePerc},
        {"randomLifetimePerc", &Emitter::randomLifetimePerc}, {"weatherCubeSize", &Emitter::weatherCubeSize},
        {"tailSize", &Emitter::tailSize}, {"parentLinkStrength", &Emitter::parentLinkStrength},
        {"weatherCubeDistance", &Emitter::weatherCubeDistance}, {"randomRotationVariance", &Emitter::randomRotationVariance},
        {"bounciness", &Emitter::bounciness}, {"freezeTime", &Emitter::freezeTime}, {"skipTime", &Emitter::skipTime},
        {"emitFromMeshOffset", &Emitter::emitFromMeshOffset}, {"weatherFadeoutDistance", &Emitter::weatherFadeoutDistance},
        {"unknown3f", &Emitter::unknown3f}, {"randomRotationAverage", &Emitter::randomRotationAverage},
    };
    for (const auto& f : floats)
    {
        auto member = f.second;
        m.push_back({std::string(f.first) + " = 0.7", on(0, [member](Emitter& e) { e.*member = 0.7f; }), false});
        m.push_back({std::string(f.first) + " = -0.0", on(0, [member](Emitter& e) { e.*member = -0.0f; }), false});
        m.push_back({std::string(f.first) + " = 3.4e38", on(0, [member](Emitter& e) { e.*member = 3.4e38f; }), false});
    }
    // Every integer, with an ordinary value, 0 and the all-ones value.
    const std::vector<std::pair<const char*, unsigned long Emitter::*>> ints = {
        {"nBursts", &Emitter::nBursts}, {"blendMode", &Emitter::blendMode}, {"textureSize", &Emitter::textureSize},
        {"nParticlesPerSecond", &Emitter::nParticlesPerSecond}, {"nTriangles", &Emitter::nTriangles},
        {"nParticlesPerBurst", &Emitter::nParticlesPerBurst}, {"groundBehavior", &Emitter::groundBehavior},
        {"unknown06", &Emitter::unknown06}, {"unknown49", &Emitter::unknown49},
    };
    for (const auto& i : ints)
    {
        auto member = i.second;
        for (unsigned long v : {7ul, 17ul, 0ul, 0xFFFFFFFFul})
            m.push_back({std::string(i.first) + " = " + std::to_string(v), on(0, [member, v](Emitter& e) { e.*member = v; }), false});
    }
    m.push_back({"emitFromMesh = 2", on(0, [](Emitter& e) { e.emitFromMesh = 2; }), false});
    m.push_back({"emitFromMesh = -1", on(0, [](Emitter& e) { e.emitFromMesh = -1; }), false});
    m.push_back({"acceleration", on(0, [](Emitter& e) { e.acceleration[0] = 1; e.acceleration[1] = -2.5f; e.acceleration[2] = -0.0f; }), false});
    m.push_back({"randomColors", on(0, [](Emitter& e) { for (int i = 0; i < 4; i++) e.randomColors[i] = 0.1f * (i + 1); }), false});
    m.push_back({"randomRotationVariance = 2.5 clamps", on(0, [](Emitter& e) { e.randomRotationVariance = 2.5f; }), false});
    m.push_back({"randomRotationVariance = -1 clamps", on(0, [](Emitter& e) { e.randomRotationVariance = -1.0f; }), false});
    m.push_back({"has11 with a value", on(0, [](Emitter& e) { e.has11 = true; e.unknown11 = -3.25f; }), false});
    m.push_back({"unknown11 without has11 is not saved", on(0, [](Emitter& e) { e.unknown11 = 9.0f; }), false});
    m.push_back({"index scrambled in memory", on(0, [](Emitter& e) { e.index = 42; }), false});

    // Groups: every member of the speed and position groups, and the
    // lifetime group's derived and preserved members.
    for (int g : {0, 1, 2})
    {
        m.push_back({"group " + std::to_string(g) + " every member", on(0, [g](Emitter& e) {
            Emitter::Group& x = e.groups[g];
            x.type = 3; x.minX = 1; x.minY = 2; x.minZ = 3; x.maxX = 4; x.maxY = 5; x.maxZ = 6;
            x.sideLength = 7; x.sphereRadius = 8; x.sphereEdge = 1; x.cylinderRadius = 9; x.cylinderEdge = 1;
            x.cylinderHeight = 10; x.valX = 11; x.valY = 12; x.valZ = -0.0f; }), false});
    }
    m.push_back({"lifetime group derivation", on(0, [](Emitter& e) { e.lifetime = 2.3f; e.randomLifetimePerc = 0.37f; }), false});
    m.push_back({"group 0 type 5 is unloadable", on(0, [](Emitter& e) { e.groups[0].type = 5; }), true});
    m.push_back({"group 1 type 9 is overwritten", on(0, [](Emitter& e) { e.groups[1].type = 9; }), false});

    // Tracks.
    m.push_back({"colour interior 0.5 quantises", on(0, [](Emitter& e) { SetKeys(e, 0, {{0, 1}, {50, 0.5f}, {100, 0}}); }), false});
    m.push_back({"colour endpoints quantise and pin to 0/100", on(0, [](Emitter& e) { SetKeys(e, 3, {{3, 0.333f}, {50, 0.2f}, {97, 0.777f}}); }), false});
    m.push_back({"colour endpoint 1.5 wraps", on(0, [](Emitter& e) { SetKeys(e, 3, {{0, 1.5f}, {100, 0}}); }), false});
    m.push_back({"colour interior 1.5 is unloadable", on(0, [](Emitter& e) { SetKeys(e, 3, {{0, 0}, {50, 1.5f}, {100, 0}}); }), true});
    m.push_back({"interior time 30 moves", on(0, [](Emitter& e) { SetKeys(e, 4, {{0, 1}, {30, 2}, {100, 3}}); }), false});
    m.push_back({"interior time above 100 is unloadable", on(0, [](Emitter& e) { SetKeys(e, 4, {{0, 1}, {101, 2}, {102, 2}, {103, 3}}); }), true});
    m.push_back({"reversed index values are kept", on(0, [](Emitter& e) { SetKeys(e, 5, {{0, 3}, {25, 2}, {50, 1}, {100, 0}}); }), false});
    m.push_back({"index values survive as floats", on(0, [](Emitter& e) { SetKeys(e, 5, {{0, 0.5f}, {50, 2.75f}, {100, 3.25f}}); }), false});
    m.push_back({"interpolation smooth and step", on(0, [](Emitter& e) {
        e.tracks[4]->interpolation = Track::IT_SMOOTH; e.tracks[6]->interpolation = Track::IT_STEP; }), false});
    m.push_back({"interpolation 7 clamps", on(0, [](Emitter& e) { e.tracks[4]->interpolation = (Track::InterpolationType)7; }), false});
    m.push_back({"interpolation -1 kept", on(0, [](Emitter& e) { e.tracks[5]->interpolation = Track::IT_UNKNOWN; }), false});
    m.push_back({"randomRotation drops rotation keys", on(0, [](Emitter& e) { e.randomRotation = true; e.randomRotationAverage = 12.5f; }), false});
    m.push_back({"randomRotation off resets the average", on(0, [](Emitter& e) { e.randomRotation = false; e.randomRotationAverage = 12.5f; }), false});

    // Aliasing.
    m.push_back({"green locked to red", on(0, [](Emitter& e) { e.tracks[1] = e.tracks[0]; }), false});
    m.push_back({"blue unlocked, different keys", on(0, [](Emitter& e) { SetKeys(e, 2, {{0, 0.1f}, {100, 0.9f}}); }), false});
    m.push_back({"green unlocked with red's keys (explicit unlock)", on(0, [](Emitter& e) {
        e.trackContents[1] = *e.tracks[0]; e.tracks[1] = &e.trackContents[1]; }), false});
    m.push_back({"blue locked to green that equals red", on(0, [](Emitter& e) {
        e.trackContents[1] = *e.tracks[0]; e.tracks[1] = &e.trackContents[1]; e.tracks[2] = e.tracks[1]; }), false});
    m.push_back({"channels equal only after quantisation", on(0, [](Emitter& e) {
        SetKeys(e, 1, {{0, 0.5f}, {100, 0.2f}}); SetKeys(e, 2, {{0, 0.5001f}, {100, 0.2001f}}); }), false});
    m.push_back({"all four channels independent", on(0, [](Emitter& e) {
        for (int i = 0; i < 4; i++) SetKeys(e, i, {{0, 0.1f * i}, {50, 0.2f * i}, {100, 0.3f}}); }), false});
    m.push_back({"default locked channels on a child", on(1, [](Emitter&) {}), false});
    return m;
}

static void TestProjection()
{
    printf("D. projection\n");
    int checked = 0;
    for (const Mutation& mutation : Mutations())
    {
        auto system = FreshBase();
        mutation.apply(*system);
        const json before = particle_json::ParticleSystemJson(*system);
        json projected;
        std::string reason;
        const bool loadable = particle_json::ProjectPersisted(before, &projected, &reason);
        auto reloaded = LoadBytes(SaveBytes(*system));
        if (mutation.unloadable)
        {
            check(!loadable, mutation.name + ": the projection calls it unloadable");
            check(reloaded == nullptr, mutation.name + ": the saved file does not load");
            continue;
        }
        check(loadable, mutation.name + ": the projection loads (" + reason + ")");
        check(reloaded != nullptr, mutation.name + ": the saved file loads");
        if (!loadable || !reloaded) continue;
        const json after = particle_json::ParticleSystemJson(*reloaded);
        const bool same = projected.dump() == after.dump();
        check(same, mutation.name + ": the projection equals the reloaded dump");
        if (!same)
        {
            for (size_t i = 0; i < after["emitters"].size(); i++)
                for (auto it = after["emitters"][i].begin(); it != after["emitters"][i].end(); ++it)
                    if (projected["emitters"][i][it.key()].dump() != it.value().dump())
                        printf("    emitters[%zu].%s projected %s reloaded %s\n", i, it.key().c_str(),
                               projected["emitters"][i][it.key()].dump().c_str(), it.value().dump().c_str());
            for (const char* key : {"name", "leaveParticles", "linkExempts"})
                if (projected[key] != after[key]) printf("    %s differs\n", key);
        }
        // A second save is a fixed point.
        auto twice = LoadBytes(SaveBytes(*reloaded));
        check(twice && particle_json::ParticleSystemJson(*twice).dump() == after.dump(), mutation.name + ": a second save changes nothing");
        checked++;
    }
    printf("  %d loadable mutations projected\n", checked);
}

// Key order is the multiset's, not insertion order.
static void TestKeyOrder()
{
    printf("D2. key order\n");
    auto system = FreshBase();
    Emitter& e = system->getEmitter(0);
    e.trackContents[4].keys.clear();
    for (float t : {100.0f, 50.0f, 0.0f}) e.trackContents[4].keys.insert(Track::Key(t, t / 10));
    const json keys = particle_json::ParticleSystemJson(*system)["emitters"][0]["tracks"][4]["keys"];
    check(keys.dump() == "[[0.0,0.0],[50.0,5.0],[100.0,10.0]]", "keys inserted in reverse are dumped in time order");
}

// ---------------------------------------------------------------------------
// E. Inspect refusals
// ---------------------------------------------------------------------------
static void ExpectInvalid(ParticleSystem& system, const std::string& path, const std::string& reason)
{
    std::string foundPath, foundReason;
    const bool found = particle_json::FindInvalidInspectValue(particle_json::ParticleSystemJson(system), &foundPath, &foundReason);
    check(found && foundPath == path && foundReason.find(reason) != std::string::npos,
          "refusal names " + path + " (" + reason + "), got " + foundPath + " " + foundReason);
}

static void TestRefusals()
{
    printf("E. refusals\n");
    {
        auto s = FreshBase();
        s->getEmitter(0).lifetime = std::numeric_limits<float>::quiet_NaN();
        ExpectInvalid(*s, "emitters[0].lifetime", "finite");
    }
    {
        auto s = FreshBase();
        s->getEmitter(2).unknown3f = std::numeric_limits<float>::infinity();
        ExpectInvalid(*s, "emitters[2].unknown3f", "finite");
    }
    {
        auto s = FreshBase();
        s->getEmitter(0).groups[2].valY = -std::numeric_limits<float>::infinity();
        ExpectInvalid(*s, "emitters[0].groups[2].valY", "finite");
    }
    {
        auto s = FreshBase();
        s->getEmitter(1).name = "bad \xE9 byte";
        ExpectInvalid(*s, "emitters[1].name", "UTF-8");
    }
    {
        auto s = FreshBase();
        s->setName("\xED\xA0\x80");   // an encoded surrogate
        ExpectInvalid(*s, "name", "UTF-8");
    }
    {
        // Valid non-ASCII is kept byte for byte, and a name that a
        // replacement decode would collapse onto it stays distinct.
        auto s = FreshBase();
        s->getEmitter(0).name = "Fum\xC3\xA9" "e";
        s->getEmitter(1).name = "Fum\xEF\xBF\xBD" "e";
        const json dump = particle_json::ParticleSystemJson(*s);
        std::string p, r;
        check(!particle_json::FindInvalidInspectValue(dump, &p, &r), "valid non-ASCII names are accepted");
        check(dump["emitters"][0]["name"].get<std::string>() == "Fum\xC3\xA9" "e", "the name's bytes are kept");
        check(dump["emitters"][0]["name"] != dump["emitters"][1]["name"], "distinct names stay distinct");
    }
    for (float raw : {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()})
    {
        // A raw infinity in 0x17 loads clamped: inspect reports the loaded state.
        auto s = FreshBase();
        s->getEmitter(0).randomRotationVariance = raw;
        auto loaded = LoadBytes(SaveBytes(*s));
        check(loaded != nullptr, "a raw 0x17 infinity loads");
        if (!loaded) continue;
        const json dump = particle_json::ParticleSystemJson(*loaded);
        std::string p, r;
        check(!particle_json::FindInvalidInspectValue(dump, &p, &r), "and inspects cleanly");
        check(dump["emitters"][0]["randomRotationVariance"].get<double>() == (raw > 0 ? 1.0 : 0.0),
              "as the clamped value");
    }
    {
        std::string p, r;
        check(particle_json::IsValidUtf8("") && particle_json::IsValidUtf8("\xF4\x8F\xBF\xBF"), "U+10FFFF is valid");
        check(!particle_json::IsValidUtf8("\xF4\x90\x80\x80"), "above U+10FFFF is invalid");
        check(!particle_json::IsValidUtf8("\xC0\xAF"), "an overlong form is invalid");
        check(!particle_json::IsValidUtf8("\xE2\x82"), "a truncated sequence is invalid");
    }
}

// Optional: `test_particle_system_json --corpus <dir>` checks the projection
// against every particle file under <dir> (load, project, compare with a save
// and reload). Not part of the gate; used to ground the projection on real
// files.
static int RunCorpus(const std::wstring& root)
{
    int files = 0, projected = 0, notParticles = 0;
    std::vector<std::wstring> stack = {root};
    while (!stack.empty())
    {
        const std::wstring dir = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW data;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &data);
        if (h == INVALID_HANDLE_VALUE) continue;
        do
        {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring path = dir + L"\\" + name;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { stack.push_back(path); continue; }
            if (name.size() < 4 || _wcsicmp(name.c_str() + name.size() - 4, L".alo") != 0) continue;
            std::ifstream in(path, std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto loaded = LoadBytes(bytes);
            files++;
            if (!loaded) { notParticles++; continue; }
            const json before = particle_json::ParticleSystemJson(*loaded);
            json expected;
            std::string reason;
            const bool ok = particle_json::ProjectPersisted(before, &expected, &reason);
            auto reloaded = LoadBytes(SaveBytes(*loaded));
            const bool same = ok && reloaded && expected.dump() == particle_json::ParticleSystemJson(*reloaded).dump();
            if (same) projected++;
            else wprintf(L"  MISMATCH %ls %hs\n", path.c_str(), reason.c_str());
        } while (FindNextFileW(h, &data));
        FindClose(h);
    }
    printf("corpus: %d .alo files, %d not particle systems, %d projected exactly, %d mismatched\n",
           files, notParticles, projected, files - notParticles - projected);
    return projected == files - notParticles ? 0 : 1;
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--corpus")
    {
        wchar_t root[MAX_PATH * 4];
        MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, root, MAX_PATH * 4);
        return RunCorpus(root);
    }
    printf("test_particle_system_json\n");
    auto base = BuildBase();
    g_baseBytes = SaveBytes(*base);
    check(FreshBase() != nullptr, "the base fixture loads");

    TestLeaveParticlesDefault();
    TestChunkCoverage();
    TestMemberTables();
    TestProjection();
    TestKeyOrder();
    TestRefusals();

    printf("%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) printf("all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
