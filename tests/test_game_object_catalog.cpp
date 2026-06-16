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
#include "AloModel.h"   // dbg] --dumpalo: decode bones/connections/meshes
#include "xml.h"        // dbg] --xmltest: does a raw XML file parse?

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
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
    std::wstring              xmlDir;    // single-dir convenience (back-compat)
    std::vector<std::wstring> xmlDirs;   // dbg] multi-root: searched in PRECEDENCE order, first hit wins
    std::wstring              forcedAlo;
    IFile* getFile(const std::string& path) override
    {
        const std::string xmlPfx = "Data\\XML\\";
        const std::string modPfx = "Data\\Art\\Models\\";
        if (!forcedAlo.empty() && path.rfind(modPfx, 0) == 0)
        {
            try { return new PhysicalFile(forcedAlo, PhysicalFile::READ); }
            catch (...) { return nullptr; }
        }
        // non-XML "Data\..." paths (the GameObjectList.lua roster lives at
        // Data\Scripts\Library\) resolve against each root's MOD ROOT (the configured dir
        // minus its trailing \Data\XML), first hit wins -- mirrors the editor's roots.
        if (path.rfind("Data\\", 0) == 0 && path.rfind(xmlPfx, 0) != 0)
        {
            std::wstring wp(path.begin(), path.end());
            std::vector<std::wstring> dirs = xmlDirs;
            if (!xmlDir.empty()) dirs.push_back(xmlDir);
            const std::wstring suf = L"\\Data\\XML";
            for (std::wstring modRoot : dirs)
            {
                if (modRoot.size() >= suf.size() &&
                    _wcsicmp(modRoot.c_str() + modRoot.size() - suf.size(), suf.c_str()) == 0)
                    modRoot = modRoot.substr(0, modRoot.size() - suf.size());
                try { return new PhysicalFile(modRoot + L"\\" + wp, PhysicalFile::READ); }
                catch (...) { /* miss -> next root */ }
            }
            return nullptr;
        }
        if (path.rfind(xmlPfx, 0) == 0)
        {
            std::string leaf = path.substr(xmlPfx.size());
            std::wstring wleaf(leaf.begin(), leaf.end());
            // Mirror the editor's content-root precedence (root, submod..., Core):
            // try each Data\XML dir in order, first that has the file wins.
            std::vector<std::wstring> dirs = xmlDirs;
            if (!xmlDir.empty()) dirs.push_back(xmlDir);
            for (const std::wstring& d : dirs)
            {
                try { return new PhysicalFile(d + L"\\" + wleaf, PhysicalFile::READ); }
                catch (...) { /* miss -> try next root */ }
            }
        }
        return nullptr;
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

    size_t roleHist[4] = { 0 };   // indexed by ObjRole (Excluded, Unit, Structure, Hero)
    size_t pickerListed = 0;
    for (const auto& r : cat.objects) { roleHist[(int)r.role]++; if (IsPickerListed(r)) ++pickerListed; }
    std::printf("role histogram: Excluded=%zu Unit=%zu Structure=%zu Hero=%zu\n",
                roleHist[0], roleHist[1], roleHist[2], roleHist[3]);
    std::printf("picker-listed (fieldable units+structures + all heroes) total: %zu of %zu\n",
                pickerListed, cat.objects.size());

    static const char* samples[] = {
        "AT_AT_Walker", "AT_AT_Walker_REB09", "AT_ST_Walker", "Star_Destroyer"
    };
    std::printf("sample lookups:\n");
    for (const char* s : samples)
    {
        const GameObjectRef* r = find(cat, s);
        if (r) std::printf("  %-28s -> %-28s [%s/%s/%s] (%s)\n",
                           r->name.c_str(), r->modelPath.c_str(),
                           ObjDomainName(r->domain), ObjRoleName(r->role), ObjBucketName(r->bucket),
                           r->sourceFile.c_str());
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
                    ObjBucketName(cat.objects[i].bucket));
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

// dbg] Decode one .alo and print its skeleton + per-mesh connection bone +
// hidden flag -- so a hardpoint's Damage_*/Collision_Mesh bone can be cross-checked
// against the model the unit actually renders (the Autem_Venator damage-decal bug:
// hardpoints authored for EV_Venator.alo, unit renders Venator_OFC.alo).
static int dumpAlo(const char* aloPath)
{
    std::wstring wpath(aloPath, aloPath + std::strlen(aloPath));
    IFile* f = nullptr;
    try { f = new PhysicalFile(wpath, PhysicalFile::READ); }
    catch (...) { std::printf("open FAILED: %s\n", aloPath); return 2; }

    AloModel m;
    try { m = LoadAloModel(f); }
    catch (...) { std::printf("decode FAILED: %s\n", aloPath); f->Release(); return 3; }
    f->Release();

    std::printf("=== %s ===\n", aloPath);
    std::printf("bones=%zu  connections=%zu  meshes=%zu\n",
                m.bones.size(), m.connections.size(), m.meshes.size());

    std::printf("-- bones --\n");
    for (size_t i = 0; i < m.bones.size(); ++i)
        std::printf("  [%2zu] %-28s parent=%u\n", i, m.bones[i].name.c_str(), m.bones[i].parentIndex);

    std::printf("-- meshes (idx name hidden coll -> connected bone | shader) --\n");
    for (size_t mi = 0; mi < m.meshes.size(); ++mi)
    {
        const char* boneName = "(unconnected)";
        for (const AloConnection& c : m.connections)
            if (c.objectIndex == mi)
            {
                boneName = (c.boneIndex < m.bones.size()) ? m.bones[c.boneIndex].name.c_str() : "(oob)";
                break;
            }
        const char* shader = m.meshes[mi].subMeshes.empty()
                           ? "(none)" : m.meshes[mi].subMeshes[0].shaderName.c_str();
        std::printf("  [%2zu] %-30s h=%d c=%d -> %-24s | %s\n",
                    mi, m.meshes[mi].name.c_str(),
                    m.meshes[mi].hidden ? 1 : 0, m.meshes[mi].collision ? 1 : 0,
                    boneName, shader);
    }
    return 0;
}

// dbg] Build the catalog over MULTIPLE content roots (root, submod..., Core)
// and print one unit's resolved model + hardpointNames + the hide-bones the engine
// would compute -- the authoritative check for "why are Autem_Venator's damage decals
// still visible" (does the Variant_Of-inherited hardpoint list resolve at all?).
static int dumpUnit(const char* unitName, const std::vector<std::string>& dirs)
{
    RealDirFM fm;
    for (const std::string& d : dirs) fm.xmlDirs.emplace_back(d.begin(), d.end());

    GameObjectCatalog cat;
    bool ok = BuildGameObjectCatalog(fm, cat);
    std::printf("catalog build=%s objects=%zu hardpoint-table=%zu\n",
                ok ? "true" : "false", cat.objects.size(), cat.hardpoints.size());
    if (!ok) return 2;

    // List every catalog object whose name contains `unitName` (case-insensitive) so
    // "which Venators are listed + their hardpoint counts" is answered in one shot.
    {
        std::string needle = unitName; for (char& c : needle) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        std::printf("-- catalog objects matching '%s' --\n", unitName);
        size_t hits = 0;
        for (const GameObjectRef& o : cat.objects)
        {
            std::string lname = o.name; for (char& c : lname) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (lname.find(needle) == std::string::npos) continue;
            ++hits;
            std::printf("  %-44s [%-9s] hp=%-2zu model=%s\n",
                        o.name.c_str(), ObjBucketName(o.bucket),
                        o.hardpointNames.size(), o.modelPath.c_str());
        }
        std::printf("  (%zu matches)\n", hits);
    }

    const GameObjectRef* r = find(cat, unitName);
    if (!r) { std::printf("UNIT '%s' NOT in catalog (filtered / parent not parsed / no model)\n", unitName); return 3; }
    std::printf("unit '%s'  model=%s  class=%s/%s/%s  source=%s\n",
                r->name.c_str(), r->modelPath.c_str(),
                ObjDomainName(r->domain), ObjRoleName(r->role), ObjBucketName(r->bucket),
                r->sourceFile.c_str());

    auto lower = [](std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32); return s; };
    std::printf("hardpointNames: %zu\n", r->hardpointNames.size());
    std::set<std::string> hideBones, attachBones;
    for (const std::string& hp : r->hardpointNames)
    {
        auto it = cat.hardpoints.find(lower(hp));
        if (it == cat.hardpoints.end()) { std::printf("  %-30s -> (def MISSING from hardpoint table)\n", hp.c_str()); continue; }
        const HardPointDef& d = it->second;
        std::printf("  %-30s model=%-20s bone=%-16s dmgD=%-18s dmgP=%-16s coll=%s\n",
                    hp.c_str(),
                    d.modelToAttach.empty()  ? "(none)" : d.modelToAttach.c_str(),
                    d.attachmentBone.empty() ? "(none)" : d.attachmentBone.c_str(),
                    d.damageDecalBone.empty()     ? "-" : d.damageDecalBone.c_str(),
                    d.damageParticlesBone.empty() ? "-" : d.damageParticlesBone.c_str(),
                    d.collisionMeshBone.empty()   ? "-" : d.collisionMeshBone.c_str());
        if (!d.damageDecalBone.empty())     hideBones.insert(lower(d.damageDecalBone));
        if (!d.damageParticlesBone.empty()) hideBones.insert(lower(d.damageParticlesBone));
        if (!d.collisionMeshBone.empty())   hideBones.insert(lower(d.collisionMeshBone));
        if (!d.modelToAttach.empty() && !d.attachmentBone.empty()) attachBones.insert(lower(d.attachmentBone));
    }
    for (const std::string& a : attachBones) hideBones.erase(a);   // mount points never hidden ()
    std::printf("=> resulting hide-bone set (lower-cased, mount bones erased): %zu\n", hideBones.size());
    for (const std::string& b : hideBones) std::printf("     %s\n", b.c_str());
    return 0;
}

// dbg] Parse a raw XML file path directly (no FM) and report success + root
// element + direct-child count -- to classify "object file silently dropped from the
// catalog": is it a parse FAILURE (XMLTree::parse throws) or a content/find issue?
static int dumpXmlTest(const char* path)
{
    std::wstring wpath(path, path + std::strlen(path));
    IFile* f = nullptr;
    try { f = new PhysicalFile(wpath, PhysicalFile::READ); }
    catch (...) { std::printf("OPEN FAILED: %s\n", path); return 2; }

    XMLTree xml;
    try { xml.parse(f); }
    catch (const std::exception& e) { std::printf("PARSE THREW (std::exception: %s): %s\n", e.what(), path); f->Release(); return 3; }
    catch (...) { std::printf("PARSE THREW (unknown): %s\n", path); f->Release(); return 3; }
    f->Release();

    const XMLNode* root = xml.getRoot();
    if (!root) { std::printf("PARSED but NO ROOT: %s\n", path); return 4; }
    unsigned named = 0;
    for (unsigned i = 0; i < root->getNumChildren(); ++i)
        if (!root->getChild(i)->getAttribute(L"Name").empty()) ++named;
    std::printf("PARSE OK: root=<%ls> children=%u named-children=%u  %s\n",
                root->getName().c_str(), root->getNumChildren(), named, path);
    return 0;
}

// Build the catalog over one or more content roots (submod, Core, Mod-base,
// base FoC -- in precedence order) and emit a TSV of every object's profile classification
// + fieldable attribute, plus the verification summary: the leak check (zero picker-listed
// 'loworbit' backdrops) and the SAFETY AUDIT (kept-but-non-fieldable -- exactly what the
// hard gate would HIDE, so a real unit can't silently vanish). This is the headline proof
// that the data-driven classifier behaves on the real mods.
static int dumpClassified(const char* outPath, const std::vector<std::string>& dirs)
{
    RealDirFM fm;
    for (const std::string& d : dirs) fm.xmlDirs.emplace_back(d.begin(), d.end());

    GameObjectCatalog cat;
    if (!BuildGameObjectCatalog(fm, cat)) { std::printf("catalog build FAILED (GameObjectFiles.xml unreadable)\n"); return 2; }

    FILE* out = std::fopen(outPath, "w");
    if (!out) { std::printf("cannot open output: %s\n", outPath); return 2; }
    std::fprintf(out, "name\ttag\tdomain\trole\tbucket\tconflict\tfieldable\tsource\tmodel\n");

    size_t kept = 0, fieldable = 0, listed = 0, leak = 0;
    std::vector<const GameObjectRef*> hidden;   // kept unit/structure but NOT fieldable -> the audit
    std::vector<const GameObjectRef*> exclFieldable;   // Excluded BUT fieldable -> the TRUE silent-drop signal
    for (const GameObjectRef& r : cat.objects)
    {
        char src[8] = { 0 }; size_t si = 0;
        if (r.fieldSource & FS_Buildable)      src[si++] = 'B';
        if (r.fieldSource & FS_StructureBuilt) src[si++] = 'S';
        if (r.fieldSource & FS_Spawned)        src[si++] = 'P';
        if (r.fieldSource & FS_Roster)         src[si++] = 'R';
        if (si == 0) src[si++] = '-';
        std::fprintf(out, "%s\t%s\t%s\t%s\t%s\t%d\t%d\t%s\t%s\n",
                     r.name.c_str(), r.tag.c_str(), ObjDomainName(r.domain), ObjRoleName(r.role),
                     ObjBucketName(r.bucket), r.conflict ? 1 : 0, r.fieldable ? 1 : 0, src, r.modelPath.c_str());

        const bool isKept   = (r.role != ObjRole::Excluded);
        const bool isListed = IsPickerListed(r);   // heroes exempt from the fieldable gate
        if (isKept)              ++kept;
        if (r.fieldable)         ++fieldable;
        if (isListed)            ++listed;
        if (isKept && !isListed) hidden.push_back(&r);   // the real "would be hidden" set (non-hero, non-fieldable)
        if (!isKept && r.fieldable) exclFieldable.push_back(&r);   // a player-fieldable object the classifier DROPPED -> investigate

        std::string ln = r.name; for (char& c : ln) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (IsPickerListed(r) && ln.find("loworbit") != std::string::npos) ++leak;
    }
    std::fclose(out);

    std::printf("objects=%zu  kept(units+structures)=%zu  fieldable=%zu  PICKER-LISTED(IsPickerListed)=%zu\n",
                cat.objects.size(), kept, fieldable, listed);
    std::printf("LEAK CHECK: picker-listed objects with 'loworbit' in name = %zu  (expect 0)\n", leak);
    std::printf("SILENT-DROP CHECK: EXCLUDED-but-FIELDABLE (a fieldable object the classifier dropped -- INVESTIGATE) = %zu\n",
                exclFieldable.size());
    {
        size_t shownE = 0;
        for (const GameObjectRef* e : exclFieldable)
        {
            if (shownE++ >= 30) { std::printf("   ... (%zu more)\n", exclFieldable.size() - 30); break; }
            std::printf("   %-44s <%s>  src=%c%c%c%c\n", e->name.c_str(), e->tag.c_str(),
                        (e->fieldSource & FS_Buildable) ? 'B' : '-', (e->fieldSource & FS_StructureBuilt) ? 'S' : '-',
                        (e->fieldSource & FS_Spawned) ? 'P' : '-', (e->fieldSource & FS_Roster) ? 'R' : '-');
        }
    }
    std::printf("SAFETY AUDIT: kept-but-NON-fieldable (would be HIDDEN by the hard gate) = %zu\n", hidden.size());
    size_t shown = 0;
    for (const GameObjectRef* h : hidden)
    {
        if (shown++ >= 50) { std::printf("   ... (%zu more -- see TSV, filter fieldable==0 && role!=Excluded)\n", hidden.size() - 50); break; }
        std::printf("   %-44s [%s/%s/%s]  <%s>\n", h->name.c_str(),
                    ObjDomainName(h->domain), ObjRoleName(h->role), ObjBucketName(h->bucket), h->tag.c_str());
    }
    std::printf("wrote TSV: %s\n", outPath);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 4 && std::strcmp(argv[1], "--dumpcat") == 0)
    {
        std::vector<std::string> dirs;
        for (int i = 3; i < argc; ++i) dirs.push_back(argv[i]);
        return dumpClassified(argv[2], dirs);
    }
    if (argc >= 3 && std::strcmp(argv[1], "--probe") == 0)   return dumpProbe(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "--xmltest") == 0) return dumpXmlTest(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "--dumpalo") == 0) return dumpAlo(argv[2]);
    if (argc >= 4 && std::strcmp(argv[1], "--unit") == 0)
    {
        std::vector<std::string> dirs;
        for (int i = 3; i < argc; ++i) dirs.push_back(argv[i]);
        return dumpUnit(argv[2], dirs);
    }
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

    // (Legacy [category] + [picker filter] sections retired with the GameObjectCategory
    // enum in fu]; the profile classifier's keep/group is covered by [classify]
    // (role/bucket) + [fieldable] (IsPickerListed) below.)

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

    // ---- BUG-2: the parser tolerates encoding='ASCII' ----------------
    // Many mod XML files declare <?xml ... encoding='ASCII'?>, which expat only knows
    // as "US-ASCII" -> without the unknown-encoding handler the parse threw and the
    // whole file (every object in it) was silently dropped from the catalog.
    std::printf("[encoding ascii]\n");
    {
        MockFM efm;
        efm.files["Data\\XML\\GameObjectFiles.xml"] =
            "<?xml version='1.0' encoding='ASCII'?>\n"
            "<Game_Object_Files><File>Asc.xml</File></Game_Object_Files>";
        efm.files["Data\\XML\\Asc.xml"] =
            "<?xml version='1.0' encoding='ASCII'?>\n"
            "<Objects><SpaceUnit Name=\"Ascii_Unit\"><Space_Model_Name>EV_Ascii.alo</Space_Model_Name></SpaceUnit></Objects>";
        GameObjectCatalog ec;
        bool eok = BuildGameObjectCatalog(efm, ec);
        CHECK(eok, "catalog builds from an encoding='ASCII' GameObjectFiles.xml");
        CHECK(find(ec, "Ascii_Unit") != nullptr, "object in an encoding='ASCII' file is parsed (not silently dropped)");
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

    // ---- profile classifier (pure ClassifyObject) ------------------
    std::printf("[classify]\n");
    {
        auto P = [](const char* tag, ModelFieldKind mf) {
            ObjectProfile p; p.tag = tag; p.modelField = mf; return p;
        };
        // EXCLUDE: renderable non-units.
        {
            ObjectProfile sky = P("SpacePrimarySkydome", ModelFieldKind::Space);
            Classification c = ClassifyObject(sky);
            CHECK(c.role == ObjRole::Excluded, "skydome backdrop -> Excluded");
        }
        CHECK(ClassifyObject(P("LowOrbit", ModelFieldKind::Space)).role == ObjRole::Excluded,
              "LowOrbit backdrop tag -> Excluded");
        CHECK(ClassifyObject(P("Planet", ModelFieldKind::Galactic)).role == ObjRole::Excluded,
              "Planet (galactic model) -> Excluded");
        CHECK(ClassifyObject(P("Props_Buildings_Generic", ModelFieldKind::Land)).role == ObjRole::Excluded,
              "Props_* -> Excluded");
        CHECK(ClassifyObject(P("Projectile", ModelFieldKind::Generic)).role == ObjRole::Excluded,
              "Projectile -> Excluded");
        CHECK(ClassifyObject(P("Squadron", ModelFieldKind::None)).role == ObjRole::Excluded,
              "no model (Squadron wrapper) -> Excluded");
        // Backdrops are excluded by TAG (SpaceProp), not by an Is_Dummy / In_Background flag —
        // EaW puts those flags on real units/structures, so the classifier no longer reads them
        // (the field was retired with fu]); a real Is_Dummy structure staying listed is
        // covered by the --dumpcat audit on the real mods.
        CHECK(ClassifyObject(P("SpaceProp", ModelFieldKind::Space)).role == ObjRole::Excluded,
              "SpaceProp -> Excluded by tag");
        {
            ObjectProfile b = P("SpaceUnit", ModelFieldKind::Space); b.behaviorTokens = {"SKY_DOME"};
            CHECK(ClassifyObject(b).role == ObjRole::Excluded, "behavior SKY_DOME -> Excluded");
        }

        // KEEP: ground units (bucket from mask, else tag fallback).
        {
            ObjectProfile inf = P("GroundInfantry", ModelFieldKind::Land);
            Classification c = ClassifyObject(inf);
            CHECK(c.domain == ObjDomain::Ground && c.role == ObjRole::Unit && c.bucket == ObjBucket::Infantry,
                  "GroundInfantry+Land -> Ground/Unit/Infantry (tag fallback, no mask)");
        }
        {
            ObjectProfile v = P("GroundVehicle", ModelFieldKind::Land); v.maskTokens = {"VEHICLE", "ANTIVEHICLE"};
            Classification c = ClassifyObject(v);
            CHECK(c.domain == ObjDomain::Ground && c.bucket == ObjBucket::Vehicle,
                  "GroundVehicle+mask VEHICLE -> Ground/Vehicle (Anti* ignored)");
        }
        // KEEP: space units, fine bucket from mask.
        {
            ObjectProfile cap = P("SpaceUnit", ModelFieldKind::Space); cap.maskTokens = {"CAPITAL", "ANTICAPITAL"};
            Classification c = ClassifyObject(cap);
            CHECK(c.domain == ObjDomain::Space && c.role == ObjRole::Unit && c.bucket == ObjBucket::Capital,
                  "SpaceUnit+mask CAPITAL -> Space/Unit/Capital");
        }
        {
            ObjectProfile fig = P("SpaceUnit", ModelFieldKind::Space); fig.maskTokens = {"FIGHTER", "ANTIFIGHTER"};
            CHECK(ClassifyObject(fig).bucket == ObjBucket::Fighter, "SpaceUnit+mask FIGHTER -> Fighter");
        }
        // KEEP: structures (ground + space) by mask or no-MovementClass + structure tag.
        {
            ObjectProfile s = P("SpecialStructure", ModelFieldKind::Land);   // no MovementClass
            Classification c = ClassifyObject(s);
            CHECK(c.domain == ObjDomain::Ground && c.role == ObjRole::Structure,
                  "SpecialStructure+Land+no-MovementClass -> Ground/Structure");
        }
        {
            ObjectProfile sb = P("StarBase", ModelFieldKind::Space); sb.maskTokens = {"SPACESTRUCTURE"};
            Classification c = ClassifyObject(sb);
            CHECK(c.domain == ObjDomain::Space && c.role == ObjRole::Structure,
                  "StarBase+SpaceStructure mask -> Space/Structure");
        }
        // KEEP: heroes (own top-level role).
        {
            ObjectProfile h = P("HeroUnit", ModelFieldKind::Land); h.maskTokens = {"INFANTRYHERO"};
            CHECK(ClassifyObject(h).role == ObjRole::Hero, "HeroUnit+InfantryHero mask -> Hero");
        }
        CHECK(ClassifyObject(P("HeroUnit", ModelFieldKind::Space)).role == ObjRole::Hero,
              "HeroUnit tag (no mask) -> Hero");
        // TRANSPORT is domain-ambiguous: bucket follows the (model-derived) domain, no conflict.
        {
            ObjectProfile spt = P("SpaceUnit", ModelFieldKind::Space); spt.maskTokens = { "TRANSPORT" };
            CHECK(ClassifyObject(spt).bucket == ObjBucket::Transport, "Space + mask TRANSPORT -> Transport bucket");
            ObjectProfile gpt = P("GroundVehicle", ModelFieldKind::Land); gpt.maskTokens = { "TRANSPORT" };
            Classification gc = ClassifyObject(gpt);
            CHECK(gc.domain == ObjDomain::Ground && gc.bucket == ObjBucket::Vehicle && !gc.conflict,
                  "Ground + mask TRANSPORT -> Vehicle bucket, NO conflict (TRANSPORT domain-ambiguous)");
        }
        // A structure-ish tag that is MOBILE (has MovementClass) classifies as Unit, not Structure.
        {
            ObjectProfile ms = P("GroundBase", ModelFieldKind::Land); ms.hasMovementClass = true;
            CHECK(ClassifyObject(ms).role == ObjRole::Unit, "structure-ish tag WITH MovementClass -> Unit, not Structure");
        }
        // Name-pattern junk (carries a unit tag but is not a selectable unit).
        {
            ObjectProfile dc; dc.name = "AT_AT_Walker_Death_Clone"; dc.tag = "GroundVehicle"; dc.modelField = ModelFieldKind::Land;
            CHECK(ClassifyObject(dc).role == ObjRole::Excluded, "name '*_Death_Clone' -> Excluded despite unit tag");
        }
        {
            ObjectProfile ct; ct.name = "Republic_Clone_Trooper"; ct.tag = "GroundInfantry"; ct.modelField = ModelFieldKind::Land;
            Classification c = ClassifyObject(ct);
            CHECK(c.role == ObjRole::Unit && c.bucket == ObjBucket::Infantry,
                  "'Clone_Trooper' NOT excluded (name 'clone' must not over-match death_clone)");
        }
        // Conflict surfacing: dual-env + mask-unresolved.
        {
            ObjectProfile dual = P("TransportUnit", ModelFieldKind::Land); dual.dualEnv = true;
            CHECK(ClassifyObject(dual).conflict, "dual-env (Land+Space) object -> conflict flagged");
        }
        {
            ObjectProfile uk = P("SpaceUnit", ModelFieldKind::Space);   // no mask, generic space tag
            Classification c = ClassifyObject(uk);
            CHECK(c.role == ObjRole::Unit && c.bucket == ObjBucket::Other && c.conflict,
                  "mask-unresolved space unit -> Unit/Other + conflict (never dropped)");
        }
    }

    // ---- fieldable reference-graph + lua roster --------------------
    std::printf("[fieldable]\n");
    {
        MockFM ffm;
        ffm.files["Data\\XML\\GameObjectFiles.xml"] =
            "<Game_Object_Files><File>Fld.xml</File></Game_Object_Files>";
        ffm.files["Data\\XML\\Fld.xml"] =
            "<Objects>"
            "  <SpaceUnit Name=\"Carrier\"><Space_Model_Name>EV_Carrier.ALO</Space_Model_Name>"
            "    <Build_Cost_Credits>5000</Build_Cost_Credits>"
            "    <Reserve_Spawned_Units_Tech_0>Tie_Fighter_Squadron, 4</Reserve_Spawned_Units_Tech_0></SpaceUnit>"
            "  <Squadron Name=\"TIE_Fighter_Squadron\"><Squadron_Units>TIE_Fighter, TIE_Fighter</Squadron_Units></Squadron>"
            "  <SpaceUnit Name=\"TIE_Fighter\"><Space_Model_Name>EV_TIE.ALO</Space_Model_Name></SpaceUnit>"   // spawned-only, no cost
            "  <SpaceUnit Name=\"Template_Frigate\"><Space_Model_Name>EV_Frig.ALO</Space_Model_Name>"
            "    <Build_Cost_Credits>3000</Build_Cost_Credits></SpaceUnit>"
            "  <SpaceUnit Name=\"Frigate_Variant\"><Variant_Of_Existing_Type>Template_Frigate</Variant_Of_Existing_Type></SpaceUnit>"  // cost inherited
            "  <SpaceUnit Name=\"Orphan_Ship\"><Space_Model_Name>EV_Orphan.ALO</Space_Model_Name></SpaceUnit>"  // no build/spawn/roster ref
            "  <GroundInfantry Name=\"Roster_Trooper\"><Land_Model_Name>EI_RT.ALO</Land_Model_Name></GroundInfantry>"  // roster-only
            "  <HeroUnit Name=\"Lua_Hero\"><Land_Model_Name>EI_Hero.ALO</Land_Model_Name></HeroUnit>"  // hero, no fieldable signal (lua-granted)
            "  <HeroUnit Name=\"Lua_Hero_Death_Clone\"><Land_Model_Name>EI_HeroDC.ALO</Land_Model_Name></HeroUnit>"  // hero death-clone
            // Profile-signal inheritance through Variant_Of (resolveRaw wiring for mask + behavior):
            "  <SpaceUnit Name=\"Sky_Template\"><Space_Model_Name>EV_Sky.ALO</Space_Model_Name><Behavior>SKY_DOME</Behavior></SpaceUnit>"
            "  <SpaceUnit Name=\"Sky_Variant\"><Variant_Of_Existing_Type>Sky_Template</Variant_Of_Existing_Type></SpaceUnit>"  // inherits SKY_DOME -> Excluded
            "  <SpaceUnit Name=\"Cap_Template\"><Space_Model_Name>EV_Cap.ALO</Space_Model_Name><CategoryMask>CAPITAL | ANTICAPITAL</CategoryMask><Build_Cost_Credits>9000</Build_Cost_Credits></SpaceUnit>"
            "  <SpaceUnit Name=\"Cap_Variant\"><Variant_Of_Existing_Type>Cap_Template</Variant_Of_Existing_Type></SpaceUnit>"  // inherits CAPITAL mask + cost
            // Ground Company_Units member expansion (the ground twin of Squadron_Units):
            "  <GroundCompany Name=\"Inf_Company\"><Build_Cost_Credits>500</Build_Cost_Credits><Company_Units>Inf_Member, Inf_Member</Company_Units></GroundCompany>"
            "  <GroundInfantry Name=\"Inf_Member\"><Land_Model_Name>EI_M.ALO</Land_Model_Name></GroundInfantry>"  // fieldable via Company_Units
            "</Objects>";
        ffm.files["Data\\Scripts\\Library\\GameObjectList.lua"] =
            "return {\n[\"ROSTER_TROOPER\"] = true,\n}\n";

        GameObjectCatalog fc;
        bool fok = BuildGameObjectCatalog(ffm, fc);
        CHECK(fok, "fieldable-test catalog builds");
        auto fld = [&](const char* n){ const GameObjectRef* r = find(fc, n); return r && r->fieldable; };
        auto src = [&](const char* n){ const GameObjectRef* r = find(fc, n); return r ? r->fieldSource : 0u; };

        CHECK(fld("Carrier"), "buildable carrier (Build_Cost_Credits) -> fieldable");
        CHECK((src("Carrier") & FS_Buildable) != 0, "carrier fieldSource = Buildable");
        CHECK(fld("TIE_Fighter"), "carrier-spawned member (Tie_*->TIE_* case-mismatch via Squadron_Units) -> fieldable");
        CHECK((src("TIE_Fighter") & FS_Spawned) != 0, "TIE_Fighter fieldSource = Spawned");
        CHECK(fld("Frigate_Variant"), "variant inheriting Build_Cost from template -> fieldable");
        CHECK(fld("Template_Frigate"), "template with Build_Cost -> fieldable");
        CHECK(fld("Roster_Trooper"), "GameObjectList.lua roster entry -> fieldable");
        CHECK((src("Roster_Trooper") & FS_Roster) != 0, "Roster_Trooper fieldSource = Roster");
        CHECK(!fld("Orphan_Ship"), "model-bearing object with no build/spawn/roster ref -> NOT fieldable");
        CHECK(find(fc, "TIE_Fighter_Squadron") == nullptr, "Squadron wrapper (no model) not in catalog");

        // The hard picker keep-gate for UNITS: IsPickerListed = role != Excluded && fieldable.
        const GameObjectRef* tie  = find(fc, "TIE_Fighter");
        const GameObjectRef* orph = find(fc, "Orphan_Ship");
        CHECK(tie && IsPickerListed(*tie),   "fieldable unit is picker-listed");
        CHECK(orph && !IsPickerListed(*orph), "non-fieldable unit is NOT picker-listed (hard gate)");

        // HEROES are exempt from the fieldable gate (Mod grants many via lua) -- but a hero
        // death-clone is still excluded by name, so the exemption never surfaces junk.
        const GameObjectRef* hero = find(fc, "Lua_Hero");
        CHECK(hero && hero->role == ObjRole::Hero && !hero->fieldable, "lua-only hero: role Hero, NOT fieldable");
        CHECK(hero && IsPickerListed(*hero), "non-fieldable HERO IS picker-listed (heroes exempt)");
        const GameObjectRef* hdc = find(fc, "Lua_Hero_Death_Clone");
        CHECK(hdc && hdc->role == ObjRole::Excluded, "hero death-clone -> Excluded (name junk keeps the exemption safe)");
        CHECK(hdc && !IsPickerListed(*hdc), "hero death-clone NOT picker-listed");

        // Profile-signal inheritance through Variant_Of (the resolveRaw wiring for ALL six signals,
        // not just costRaw): a variant inherits its template's behavior + mask.
        const GameObjectRef* skv = find(fc, "Sky_Variant");
        CHECK(skv && skv->role == ObjRole::Excluded, "variant inherits parent's SKY_DOME behavior -> Excluded");
        const GameObjectRef* cpv = find(fc, "Cap_Variant");
        CHECK(cpv && cpv->bucket == ObjBucket::Capital, "variant inherits parent's CAPITAL mask -> Capital bucket");
        CHECK(cpv && cpv->fieldable, "variant inherits parent's Build_Cost -> fieldable");

        // Company_Units member expansion (ground twin of Squadron_Units).
        const GameObjectRef* im = find(fc, "Inf_Member");
        CHECK(im && im->fieldable && IsPickerListed(*im), "Company_Units member of a buildable company -> fieldable + listed");
    }

    std::printf("\n=== GameObjectCatalog: %s ===\n", g_failed == 0 ? "ALL PASS" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
