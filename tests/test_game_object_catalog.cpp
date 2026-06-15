// Unit tests for the game-object catalog (src/GameObjectCatalog.cpp).
//
// Drives the catalog with a mock IFileManager backed by in-memory XML that
// mirrors the GameObjectFiles.xml -> per-file object layout -- no game assets
// required. Covers enumeration, the model-field fallback + precedence, the
// no-model skip, Variant_Of_Existing_Type resolution (same-file, cross-file,
// deep chain, own-model override, missing parent, cyclic + self-cyclic),
// first-wins on duplicate Names, de-dup of a doubly-listed file, a missing
// listed file (non-fatal), the GameObjectFiles.xml-missing path, category
// mapping, and stable name sorting. Plus the trivial ProbeModelSkinned fail
// branches; the Renderable / SkinnedUnsupported split is validated against real
// `.alo` via the --probe dump mode. Standalone console exe; see
// tests/build_test_game_object_catalog.bat.

#include "GameObjectCatalog.h"
#include "managers.h"
#include "files.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

// Mock FileManager: serves registered paths from in-memory strings.
struct MockFM : IFileManager
{
    std::map<std::string, std::string> files;
    IFile* getFile(const std::string& path) override
    {
        auto it = files.find(path);
        if (it == files.end()) return nullptr;
        MemoryFile* mf = new MemoryFile();
        if (!it->second.empty())
            mf->write(it->second.data(), (unsigned long)it->second.size());
        mf->seek(0);
        return mf;
    }
};

static const char* kFileList =
    "<?xml version=\"1.0\" ?>\n"
    "<Game_Object_Files>\n"
    "  <File>FileA.xml</File>\n"
    "  <!-- comment between entries is tolerated -->\n"
    "  <File>FileB.xml</File>\n"
    "  <File>Missing.xml</File>\n"        // listed but not served -> non-fatal
    "  <File>FileA.xml</File>\n"          // doubly listed -> de-duped
    "</Game_Object_Files>\n";

static const char* kFileA =
    "<?xml version=\"1.0\" ?>\n"
    "<Objects>\n"
    "  <GroundVehicle Name=\"Tank\"><Land_Model_Name>EV_Tank.ALO</Land_Model_Name></GroundVehicle>\n"
    "  <GroundVehicle Name=\"Tank_Variant\"><Variant_Of_Existing_Type>Tank</Variant_Of_Existing_Type></GroundVehicle>\n"
    "  <GroundVehicle Name=\"Tank_Special\">\n"
    "    <Variant_Of_Existing_Type>Tank</Variant_Of_Existing_Type>\n"
    "    <Land_Model_Name>EV_Tank_Special.ALO</Land_Model_Name>\n"   // own model overrides parent
    "  </GroundVehicle>\n"
    "  <SpaceUnit Name=\"Frigate\"><Space_Model_Name>EV_Frigate.ALO</Space_Model_Name></SpaceUnit>\n"
    "  <GroundInfantry Name=\"Trooper\"><Land_Model_Name>EI_Trooper.ALO</Land_Model_Name></GroundInfantry>\n"
    "  <GroundTurret Name=\"LaserTurret\"><Land_Model_Name>EB_LaserTurret.ALO</Land_Model_Name></GroundTurret>\n"
    "  <Projectile Name=\"Bolt\"><Model_Name>P_Bolt.ALO</Model_Name></Projectile>\n"
    "  <Planet Name=\"Tatooine\"><Galactic_Model_Name>W_Tatooine.ALO</Galactic_Model_Name></Planet>\n"
    "  <GroundCompany Name=\"Company_NoModel\"><Some_Field>x</Some_Field></GroundCompany>\n"  // no model, no variant
    "  <GroundVehicle Name=\"DualModel\">\n"
    "    <Land_Model_Name>EV_Land.ALO</Land_Model_Name>\n"
    "    <Space_Model_Name>EV_Space.ALO</Space_Model_Name>\n"          // Land wins
    "  </GroundVehicle>\n"
    "  <GroundVehicle><Land_Model_Name>EV_Anon.ALO</Land_Model_Name></GroundVehicle>\n"  // no Name -> ignored
    "  <GroundVehicle Name=\"CycA\"><Variant_Of_Existing_Type>CycB</Variant_Of_Existing_Type></GroundVehicle>\n"
    "  <GroundVehicle Name=\"CycB\"><Variant_Of_Existing_Type>CycA</Variant_Of_Existing_Type></GroundVehicle>\n"
    "  <GroundVehicle Name=\"SelfCyc\"><Variant_Of_Existing_Type>SelfCyc</Variant_Of_Existing_Type></GroundVehicle>\n"
    "  <GroundVehicle Name=\"Orphan\"><Variant_Of_Existing_Type>NoSuchParent</Variant_Of_Existing_Type></GroundVehicle>\n"
    "  <GroundVehicle Name=\"ChainA\"><Variant_Of_Existing_Type>ChainB</Variant_Of_Existing_Type></GroundVehicle>\n"
    "  <SpaceProp Name=\"Asteroid\"><Space_Model_Name>W_Asteroid.ALO</Space_Model_Name></SpaceProp>\n"
    "  <HeroUnit Name=\"Vader\"><Land_Model_Name>EI_Vader.ALO</Land_Model_Name></HeroUnit>\n"
    "  <GroundStructure Name=\"Barracks\"><Land_Model_Name>EB_Barracks_A.ALO</Land_Model_Name></GroundStructure>\n"  // first-wins
    "  <GroundStructure Name=\"Empire_Anti_Aircraft_Turret\"><Land_Model_Name>EB_AA.ALO</Land_Model_Name></GroundStructure>\n"  // vanilla turret pattern: tag=Structure, name=Turret
    "  <SpaceUnit Name=\"V-wing_Fighter\"><Space_Model_Name>RV_VWing.ALO</Space_Model_Name></SpaceUnit>\n"  // parent; note lowercase 'w' (vanilla casing)
    "  <StarBase Name=\"Home_One_Starbase\"><Space_Model_Name>EV_StarBase.ALO</Space_Model_Name></StarBase>\n"  // tag 'starbase' must beat 'base'
    "  <TransportUnit Name=\"Hauler\"><Land_Model_Name>EV_Hauler.ALO</Land_Model_Name></TransportUnit>\n"  // 'transport' -> Vehicle
    "  <GroundVehicle Name=\"CaseDupA\"><Land_Model_Name>EV_CaseA.ALO</Land_Model_Name></GroundVehicle>\n"  // case-insensitive dedup (FileB lists 'casedupa')
    "  <GroundCompany Name=\"Clone_Company\"><Land_Model_Name>EI_Clone.ALO</Land_Model_Name></GroundCompany>\n"  // unrecognised unit tag -> Other, but LISTED
    "  <Marker Name=\"Spawn_Marker_01\"><Land_Model_Name>w_flag_marker.alo</Land_Model_Name></Marker>\n"  // model-bearing non-unit -> Excluded
    "</Objects>\n";

static const char* kFileB =
    "<?xml version=\"1.0\" ?>\n"
    "<Objects>\n"
    "  <GroundVehicle Name=\"Tank_FromB\"><Variant_Of_Existing_Type>Tank</Variant_Of_Existing_Type></GroundVehicle>\n"  // cross-file
    "  <GroundVehicle Name=\"ChainB\"><Variant_Of_Existing_Type>ChainC</Variant_Of_Existing_Type></GroundVehicle>\n"
    "  <GroundVehicle Name=\"ChainC\"><Land_Model_Name>EV_ChainTail.ALO</Land_Model_Name></GroundVehicle>\n"
    "  <GroundStructure Name=\"Barracks\"><Land_Model_Name>EB_Barracks_B.ALO</Land_Model_Name></GroundStructure>\n"  // dup; FileA wins
    "  <SpaceUnit Name=\"V-wing_Fighter_Red\"><Variant_Of_Existing_Type>V-Wing_Fighter</Variant_Of_Existing_Type></SpaceUnit>\n"  // cross-file + CASE-MISMATCH ref (upper 'W') -> must still resolve
    "  <GroundVehicle Name=\"casedupa\"><Land_Model_Name>EV_CaseB.ALO</Land_Model_Name></GroundVehicle>\n"  // folds to CaseDupA's key; FileA wins
    "</Objects>\n";

static const GameObjectRef* find(const GameObjectCatalog& cat, const std::string& name)
{
    for (const auto& r : cat.objects) if (r.name == name) return &r;
    return nullptr;
}

// --- dump mode -------------------------------------------------------------

// A FileManager backed by a real directory: "Data\XML\<f>" -> <xmlDir>\<f>;
// any "Data\Art\Models\*" -> <forcedAlo> when set (probe mode). Misses -> null.
struct RealDirFM : IFileManager
{
    std::wstring xmlDir;
    std::wstring forcedAlo;
    IFile* getFile(const std::string& path) override
    {
        std::wstring wpath;
        const std::string xmlPfx = "Data\\XML\\";
        const std::string modPfx = "Data\\Art\\Models\\";
        if (!forcedAlo.empty() && path.rfind(modPfx, 0) == 0)
        {
            wpath = forcedAlo;
        }
        else if (!xmlDir.empty() && path.rfind(xmlPfx, 0) == 0)
        {
            std::string leaf = path.substr(xmlPfx.size());
            wpath = xmlDir + L"\\" + std::wstring(leaf.begin(), leaf.end());
        }
        else return nullptr;

        try { return new PhysicalFile(wpath, PhysicalFile::READ); }
        catch (...) { return nullptr; }
    }
};

static int dumpRealCatalog(const char* xmlDir)
{
    RealDirFM fm;
    fm.xmlDir.assign(xmlDir, xmlDir + std::strlen(xmlDir));

    // Time the build (averaged over a few iterations to smooth OS-cache /
    // scheduler noise). Set CATALOG_PARSE_THREADS=1 to force the serial path for an
    // A/B against the parallel default on identical content.
    GameObjectCatalog cat;
    bool ok = false;
    const int kIters = 5;
    double bestMs = 1e30, totalMs = 0.0;
    for (int it = 0; it < kIters; ++it)
    {
        GameObjectCatalog tmp;
        const auto t0 = std::chrono::steady_clock::now();
        ok = BuildGameObjectCatalog(fm, tmp);
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        totalMs += ms;
        if (ms < bestMs) bestMs = ms;
        if (it == kIters - 1) cat = std::move(tmp);
    }
    {
        char tbuf[16] = { 0 };
        const DWORD g = GetEnvironmentVariableA("CATALOG_PARSE_THREADS", tbuf, sizeof(tbuf));
        std::printf("build time: best=%.1f ms  avg=%.1f ms  (%d iters, CATALOG_PARSE_THREADS=%s)\n",
                    bestMs, totalMs / kIters, kIters,
                    (g > 0 && g < sizeof(tbuf)) ? tbuf : "auto");
    }
    std::printf("build=%s  objects=%zu\n", ok ? "true" : "false", cat.objects.size());
    if (!ok) return 2;

    size_t hist[10] = { 0 };
    for (const auto& r : cat.objects) hist[(int)r.category]++;
    static const GameObjectCategory cats[] = {
        GameObjectCategory::Vehicle, GameObjectCategory::Infantry, GameObjectCategory::Structure,
        GameObjectCategory::Turret, GameObjectCategory::Hero, GameObjectCategory::Prop,
        GameObjectCategory::Space, GameObjectCategory::Projectile, GameObjectCategory::Other,
        GameObjectCategory::Excluded
    };
    std::printf("category histogram:\n");
    size_t pickerListed = 0;
    for (GameObjectCategory c : cats)
    {
        std::printf("  %-12s %zu%s\n", GameObjectCategoryName(c), hist[(int)c],
                    IsPickerListedCategory(c) ? "  [picker]" : "");
        if (IsPickerListedCategory(c)) pickerListed += hist[(int)c];
    }
    std::printf("picker-listed (units+structures) total: %zu of %zu\n", pickerListed, cat.objects.size());

    static const char* samples[] = {
        "AT_AT_Walker", "AT_AT_Walker_REB09", "AT_ST_Walker", "Star_Destroyer"
    };
    std::printf("sample lookups:\n");
    for (const char* s : samples)
    {
        const GameObjectRef* r = find(cat, s);
        if (r) std::printf("  %-28s -> %-28s [%s] (%s)\n",
                           r->name.c_str(), r->modelPath.c_str(),
                           GameObjectCategoryName(r->category), r->sourceFile.c_str());
        else   std::printf("  %-28s -> (not found)\n", s);
    }
    // PR2] Hardpoint resolution against the real content.
    std::printf("hardpoint table size: %zu\n", cat.hardpoints.size());
    std::printf("sample unit hardpoints (Name -> Model_To_Attach @ Attachment_Bone; damage/collision bones):\n");
    for (const char* s : samples)
    {
        const GameObjectRef* r = find(cat, s);
        if (!r || r->hardpointNames.empty()) continue;
        std::printf("  %s (%zu hardpoints):\n", r->name.c_str(), r->hardpointNames.size());
        for (const std::string& hpName : r->hardpointNames)
        {
            std::string k = hpName;
            for (char& c : k) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);   // ASCII lower
            auto it = cat.hardpoints.find(k);
            if (it == cat.hardpoints.end()) { std::printf("    %-34s -> (def missing)\n", hpName.c_str()); continue; }
            const HardPointDef& d = it->second;
            std::printf("    %-34s model=%-28s bone=%-16s dmg=%s|%s coll=%s\n",
                        hpName.c_str(),
                        d.modelToAttach.empty()  ? "(none)" : d.modelToAttach.c_str(),
                        d.attachmentBone.empty() ? "(none)" : d.attachmentBone.c_str(),
                        d.damageDecalBone.c_str(), d.damageParticlesBone.c_str(), d.collisionMeshBone.c_str());
        }
    }

    std::printf("first 12 objects:\n");
    for (size_t i = 0; i < cat.objects.size() && i < 12; ++i)
        std::printf("  %-28s -> %-28s [%s]\n",
                    cat.objects[i].name.c_str(), cat.objects[i].modelPath.c_str(),
                    GameObjectCategoryName(cat.objects[i].category));
    return 0;
}

static int dumpProbe(const char* aloPath)
{
    RealDirFM fm;
    fm.forcedAlo.assign(aloPath, aloPath + std::strlen(aloPath));
    ModelProbeResult r = ProbeModelSkinned(fm, "probe.alo");
    const char* s = (r == ModelProbeResult::Renderable) ? "Renderable"
                  : (r == ModelProbeResult::SkinnedUnsupported) ? "SkinnedUnsupported"
                  : (r == ModelProbeResult::NotFound) ? "NotFound"
                  : "LoadFailed";
    std::printf("probe %s -> %s\n", aloPath, s);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 3 && std::strcmp(argv[1], "--probe") == 0) return dumpProbe(argv[2]);
    if (argc >= 2) return dumpRealCatalog(argv[1]);

    MockFM fm;
    fm.files["Data\\XML\\GameObjectFiles.xml"] = kFileList;
    fm.files["Data\\XML\\FileA.xml"]           = kFileA;
    fm.files["Data\\XML\\FileB.xml"]           = kFileB;

    GameObjectCatalog cat;
    bool ok = BuildGameObjectCatalog(fm, cat);

    // ---- enumeration + skips -----------------------------------------------
    std::printf("[enumerate]\n");
    CHECK(ok, "build ok");
    CHECK(cat.objects.size() == 24, "24 resolvable objects (no-model / anon / cyclic / orphan / case-dup skipped; +Clone_Company +Spawn_Marker_01)");
    CHECK(find(cat, "Company_NoModel") == nullptr, "no-model object skipped");
    CHECK(find(cat, "Orphan") == nullptr, "missing-parent variant skipped");
    bool anyEmptyName = false, anyEmptyModel = false;
    for (const auto& r : cat.objects) { if (r.name.empty()) anyEmptyName = true; if (r.modelPath.empty()) anyEmptyModel = true; }
    CHECK(!anyEmptyName, "anonymous (no Name) child skipped");
    CHECK(!anyEmptyModel, "every emitted object has a model");

    // ---- direct model + model-field fallback -------------------------------
    std::printf("[model fields]\n");
    {
        const GameObjectRef* tank = find(cat, "Tank");
        CHECK(tank && tank->modelPath == "EV_Tank.ALO", "Land_Model_Name resolved");
        const GameObjectRef* frig = find(cat, "Frigate");
        CHECK(frig && frig->modelPath == "EV_Frigate.ALO", "Space_Model_Name resolved");
        const GameObjectRef* bolt = find(cat, "Bolt");
        CHECK(bolt && bolt->modelPath == "P_Bolt.ALO", "Model_Name resolved");
        const GameObjectRef* tat = find(cat, "Tatooine");
        CHECK(tat && tat->modelPath == "W_Tatooine.ALO", "Galactic_Model_Name resolved");
        const GameObjectRef* dual = find(cat, "DualModel");
        CHECK(dual && dual->modelPath == "EV_Land.ALO", "Land wins over Space when both present");
    }

    // ---- Variant_Of resolution ---------------------------------------------
    std::printf("[variant_of]\n");
    {
        const GameObjectRef* v = find(cat, "Tank_Variant");
        CHECK(v && v->modelPath == "EV_Tank.ALO", "same-file variant inherits parent model");
        const GameObjectRef* sp = find(cat, "Tank_Special");
        CHECK(sp && sp->modelPath == "EV_Tank_Special.ALO", "variant's OWN model overrides parent");
        const GameObjectRef* xf = find(cat, "Tank_FromB");
        CHECK(xf && xf->modelPath == "EV_Tank.ALO", "cross-file variant inherits parent model");
        const GameObjectRef* ca = find(cat, "ChainA");
        CHECK(ca && ca->modelPath == "EV_ChainTail.ALO", "deep cross-file chain A->B->C resolves to tail model");
        CHECK(find(cat, "CycA") == nullptr && find(cat, "CycB") == nullptr, "cyclic A<->B excluded (no infinite loop)");
        CHECK(find(cat, "SelfCyc") == nullptr, "self-cyclic variant excluded");
        const GameObjectRef* vw = find(cat, "V-wing_Fighter_Red");
        CHECK(vw && vw->modelPath == "RV_VWing.ALO", "case-mismatch Variant_Of ref (V-Wing vs V-wing) resolves (engine-equivalent)");
    }

    // ---- first-wins on duplicate Name + de-dup listed file -----------------
    std::printf("[dedup]\n");
    {
        const GameObjectRef* b = find(cat, "Barracks");
        CHECK(b && b->modelPath == "EB_Barracks_A.ALO", "duplicate Name: first-listed file wins");
        CHECK(b && b->sourceFile == "FileA.xml", "duplicate Name sourceFile is the first one");
        int barracks = 0; for (const auto& r : cat.objects) if (r.name == "Barracks") ++barracks;
        CHECK(barracks == 1, "duplicate Name emitted once");
        const GameObjectRef* cd = find(cat, "CaseDupA");
        CHECK(cd && cd->modelPath == "EV_CaseA.ALO", "case-insensitive first-wins: CaseDupA beats FileB's 'casedupa'");
        CHECK(find(cat, "casedupa") == nullptr, "the lower-cased duplicate is not emitted as a second object");
    }

    // ---- category mapping --------------------------------------------------
    std::printf("[category]\n");
    {
        auto catOf = [&](const char* n) { const GameObjectRef* r = find(cat, n); return r ? r->category : GameObjectCategory::Other; };
        CHECK(catOf("Tank") == GameObjectCategory::Vehicle, "GroundVehicle -> Vehicle");
        CHECK(catOf("Frigate") == GameObjectCategory::Space, "SpaceUnit -> Space");
        CHECK(catOf("Trooper") == GameObjectCategory::Infantry, "GroundInfantry -> Infantry");
        CHECK(catOf("LaserTurret") == GameObjectCategory::Turret, "GroundTurret -> Turret");
        CHECK(catOf("Bolt") == GameObjectCategory::Projectile, "Projectile -> Projectile");
        CHECK(catOf("Asteroid") == GameObjectCategory::Prop, "SpaceProp -> Prop (prop beats space)");
        CHECK(catOf("Vader") == GameObjectCategory::Hero, "HeroUnit -> Hero");
        CHECK(catOf("Barracks") == GameObjectCategory::Structure, "GroundStructure -> Structure");
        CHECK(catOf("Empire_Anti_Aircraft_Turret") == GameObjectCategory::Turret, "GroundStructure named *_Turret -> Turret (name escalation)");
        CHECK(catOf("Home_One_Starbase") == GameObjectCategory::Space, "StarBase -> Space (precedence: starbase beats 'base'->Structure)");
        CHECK(catOf("Hauler") == GameObjectCategory::Vehicle, "TransportUnit -> Vehicle ('transport')");
        CHECK(catOf("Tatooine") == GameObjectCategory::Excluded, "Planet -> Excluded (model-bearing non-unit, S50)");
        CHECK(catOf("Clone_Company") == GameObjectCategory::Other, "GroundCompany -> Other (unrecognised unit tag, shown)");
        CHECK(catOf("Spawn_Marker_01") == GameObjectCategory::Excluded, "Marker -> Excluded (model-bearing non-unit)");
    }

    // ---- picker category filter (exclusion-based: units+structures + the
    //      Other catch-all IN; Prop/Projectile/Excluded OUT) ------------------
    std::printf("[picker filter]\n");
    {
        // The predicate: everything listed EXCEPT Prop / Projectile / Excluded.
        CHECK(IsPickerListedCategory(GameObjectCategory::Vehicle),    "Vehicle is picker-listed");
        CHECK(IsPickerListedCategory(GameObjectCategory::Infantry),   "Infantry is picker-listed");
        CHECK(IsPickerListedCategory(GameObjectCategory::Structure),  "Structure is picker-listed");
        CHECK(IsPickerListedCategory(GameObjectCategory::Turret),     "Turret is picker-listed");
        CHECK(IsPickerListedCategory(GameObjectCategory::Hero),       "Hero is picker-listed");
        CHECK(IsPickerListedCategory(GameObjectCategory::Space),      "Space is picker-listed");
        CHECK(IsPickerListedCategory(GameObjectCategory::Other),       "Other IS picker-listed (unrecognised units shown)");
        CHECK(!IsPickerListedCategory(GameObjectCategory::Prop),       "Prop is NOT picker-listed");
        CHECK(!IsPickerListedCategory(GameObjectCategory::Projectile), "Projectile is NOT picker-listed");
        CHECK(!IsPickerListedCategory(GameObjectCategory::Excluded),   "Excluded is NOT picker-listed");

        // Filtering the built catalog drops Prop/Projectile/Excluded and keeps every
        // unit/structure INCLUDING the unrecognised-tag Other ones (mirrors
        // Engine::EnumerateReferenceObjects).
        std::vector<GameObjectRef> listed;
        for (const auto& r : cat.objects)
            if (IsPickerListedCategory(r.category)) listed.push_back(r);
        auto inListed = [&](const char* n) {
            for (const auto& r : listed) if (r.name == n) return true;
            return false;
        };
        CHECK(!inListed("Bolt"),       "Projectile 'Bolt' filtered out of the picker list");
        CHECK(!inListed("Asteroid"),   "Prop 'Asteroid' filtered out of the picker list");
        CHECK(!inListed("Tatooine"),   "Excluded 'Tatooine' (planet) filtered out");
        CHECK(!inListed("Spawn_Marker_01"), "Excluded 'Spawn_Marker_01' (marker) filtered out");
        CHECK(inListed("Tank"),         "Vehicle 'Tank' kept in the picker list");
        CHECK(inListed("Frigate"),      "Space 'Frigate' kept in the picker list");
        CHECK(inListed("Trooper"),      "Infantry 'Trooper' kept in the picker list");
        CHECK(inListed("LaserTurret"),  "Turret 'LaserTurret' kept in the picker list");
        CHECK(inListed("Vader"),        "Hero 'Vader' kept in the picker list");
        CHECK(inListed("Barracks"),     "Structure 'Barracks' kept in the picker list");
        CHECK(inListed("Clone_Company"),"Other 'Clone_Company' (unrecognised unit tag) KEPT -- the Mod-units-missing fix");
        bool anyExcluded = false;
        for (const auto& r : listed)
            if (!IsPickerListedCategory(r.category)) anyExcluded = true;
        CHECK(!anyExcluded, "filtered list contains ONLY units/structures");
    }

    // ---- sorted by name ----------------------------------------------------
    std::printf("[sorted]\n");
    {
        bool sorted = true;
        for (size_t i = 1; i < cat.objects.size(); ++i)
            if (cat.objects[i - 1].name > cat.objects[i].name) sorted = false;
        CHECK(sorted, "objects sorted by name");
    }

    // ---- hardpoint table + per-unit hardpoint names (PR2) ------------
    std::printf("[hardpoints]\n");
    {
        MockFM hpfm;
        hpfm.files["Data\\XML\\GameObjectFiles.xml"] =
            "<Game_Object_Files><File>Units.xml</File></Game_Object_Files>";
        hpfm.files["Data\\XML\\Units.xml"] =
            "<Objects>"
            "  <GroundVehicle Name=\"HP_Tank\"><Land_Model_Name>EV_HPTank.ALO</Land_Model_Name>"
            "    <HardPoints>HP_Tank_Cannon, HP_Tank_Shield</HardPoints></GroundVehicle>"
            "  <GroundVehicle Name=\"HP_Tank_Inherit\"><Variant_Of_Existing_Type>HP_Tank</Variant_Of_Existing_Type></GroundVehicle>"
            "  <GroundVehicle Name=\"HP_Tank_Override\"><Variant_Of_Existing_Type>HP_Tank</Variant_Of_Existing_Type>"
            "    <HardPoints>HP_Tank_Shield</HardPoints></GroundVehicle>"
            "</Objects>";
        hpfm.files["Data\\XML\\HardPointDataFiles.xml"] =
            "<Hard_Point_Files><File>HardPoints_Test.xml</File><File>Missing.xml</File></Hard_Point_Files>";  // missing file non-fatal
        hpfm.files["Data\\XML\\HardPoints_Test.xml"] =
            "<HardPoints>"
            "  <HardPoint Name=\"HP_Tank_Cannon\"><Type>HARD_POINT_WEAPON_LASER</Type>"
            "    <Model_To_Attach>EV_Tank_Cannon.alo</Model_To_Attach><Attachment_Bone>HP_Cannon_BONE</Attachment_Bone>"
            "    <Damage_Decal>HP_Cannon_BLAST</Damage_Decal><Damage_Particles>HP_Cannon_EMIT</Damage_Particles>"
            "    <Collision_Mesh>HP_Cannon_COLL</Collision_Mesh></HardPoint>"
            "  <HardPoint Name=\"HP_Tank_Shield\"><Type>HARD_POINT_SHIELD_GENERATOR</Type>"
            "    <Attachment_Bone>HP_Shield_BONE</Attachment_Bone></HardPoint>"  // no Model_To_Attach (mod omitted)
            "</HardPoints>";

        GameObjectCatalog hc;
        bool hok = BuildGameObjectCatalog(hpfm, hc);
        CHECK(hok, "hardpoint-test catalog builds");

        // Hardpoint table parsed (lower-cased keys; all five geometry/bone tags; optional model).
        auto hp = [&](const char* k) -> const HardPointDef* {
            auto it = hc.hardpoints.find(k); return it == hc.hardpoints.end() ? nullptr : &it->second;
        };
        const HardPointDef* cannon = hp("hp_tank_cannon");
        CHECK(cannon && cannon->modelToAttach == "EV_Tank_Cannon.alo", "Model_To_Attach parsed");
        CHECK(cannon && cannon->attachmentBone == "HP_Cannon_BONE", "Attachment_Bone parsed");
        CHECK(cannon && cannon->damageDecalBone == "HP_Cannon_BLAST", "Damage_Decal parsed");
        CHECK(cannon && cannon->damageParticlesBone == "HP_Cannon_EMIT", "Damage_Particles parsed");
        CHECK(cannon && cannon->collisionMeshBone == "HP_Cannon_COLL", "Collision_Mesh parsed");
        const HardPointDef* shield = hp("hp_tank_shield");
        CHECK(shield && shield->modelToAttach.empty(), "absent Model_To_Attach -> empty (mod omitted)");
        CHECK(shield && shield->attachmentBone == "HP_Shield_BONE", "shield Attachment_Bone parsed");

        // Per-unit hardpoint name lists, with Variant_Of inherit-vs-override.
        const GameObjectRef* tank = find(hc, "HP_Tank");
        CHECK(tank && tank->hardpointNames.size() == 2 &&
              tank->hardpointNames[0] == "HP_Tank_Cannon" && tank->hardpointNames[1] == "HP_Tank_Shield",
              "unit <HardPoints> comma list parsed + ordered");
        const GameObjectRef* inh = find(hc, "HP_Tank_Inherit");
        CHECK(inh && inh->hardpointNames.size() == 2 && inh->hardpointNames[0] == "HP_Tank_Cannon",
              "Variant_Of with no <HardPoints> INHERITS the parent's list");
        const GameObjectRef* ovr = find(hc, "HP_Tank_Override");
        CHECK(ovr && ovr->hardpointNames.size() == 1 && ovr->hardpointNames[0] == "HP_Tank_Shield",
              "Variant_Of with its own <HardPoints> REPLACES (not merges) the parent's list");
    }

    // ---- missing GameObjectFiles.xml ---------------------------------------
    std::printf("[total miss]\n");
    {
        MockFM empty;
        GameObjectCatalog c2;
        bool ok2 = BuildGameObjectCatalog(empty, c2);
        CHECK(!ok2, "missing GameObjectFiles.xml -> false");
        CHECK(c2.objects.empty(), "missing list -> empty out");
    }

    // ---- ProbeModelSkinned trivial branches (real classify via --probe) ----
    std::printf("[probe]\n");
    {
        CHECK(ProbeModelSkinned(fm, "") == ModelProbeResult::LoadFailed, "empty modelPath -> LoadFailed");
        CHECK(ProbeModelSkinned(fm, "nope.alo") == ModelProbeResult::NotFound, "missing file -> NotFound");
    }

    std::printf("\n=== GameObjectCatalog: %s ===\n", g_failed == 0 ? "ALL PASS" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
