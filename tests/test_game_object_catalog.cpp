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

    GameObjectCatalog cat;
    bool ok = BuildGameObjectCatalog(fm, cat);
    std::printf("build=%s  objects=%zu\n", ok ? "true" : "false", cat.objects.size());
    if (!ok) return 2;

    size_t hist[9] = { 0 };
    for (const auto& r : cat.objects) hist[(int)r.category]++;
    static const GameObjectCategory cats[] = {
        GameObjectCategory::Vehicle, GameObjectCategory::Infantry, GameObjectCategory::Structure,
        GameObjectCategory::Turret, GameObjectCategory::Hero, GameObjectCategory::Prop,
        GameObjectCategory::Space, GameObjectCategory::Projectile, GameObjectCategory::Other
    };
    std::printf("category histogram:\n");
    for (GameObjectCategory c : cats)
        std::printf("  %-12s %zu\n", GameObjectCategoryName(c), hist[(int)c]);

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
    CHECK(cat.objects.size() == 22, "22 resolvable objects (no-model / anon / cyclic / orphan / case-dup skipped)");
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
        CHECK(catOf("Tatooine") == GameObjectCategory::Other, "Planet -> Other (fallthrough)");
    }

    // ---- sorted by name ----------------------------------------------------
    std::printf("[sorted]\n");
    {
        bool sorted = true;
        for (size_t i = 1; i < cat.objects.size(); ++i)
            if (cat.objects[i - 1].name > cat.objects[i].name) sorted = false;
        CHECK(sorted, "objects sorted by name");
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
