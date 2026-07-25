// Unit tests for the skydome environment reader (src/SkydomeEnvironment.cpp).
//
// Drives the reader with a mock IFileManager backed by in-memory XML that
// mirrors the vanilla FoC layout -- no game assets required. Covers
// enumeration, the no-model skip, defaults for absent fields, case-insensitive
// In_Background, model resolution, the total-miss path, and primary/secondary
// pair resolution (incl. the asymmetric-miss case). Standalone console exe;
// see tests/build_test_skydome_environment.bat.

#include "SkydomeEnvironment.h"
#include "managers.h"
#include "files.h"

#include <array>
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

static const char* kSpacePrimary =
    "<?xml version=\"1.0\" ?>\n"
    "<SpacePrimarySkydomes>\n"
    "  <SpacePrimarySkydome Name=\"Stars_Low\">\n"
    "    <Space_Model_Name>w_stars_low.alo</Space_Model_Name>\n"
    "    <Scale_Factor>1.0</Scale_Factor>\n"
    "    <Sort_Order_Adjust>-1</Sort_Order_Adjust>\n"
    "    <In_Background>yes</In_Background>\n"
    "  </SpacePrimarySkydome>\n"
    "  <SpacePrimarySkydome Name=\"Cin_Space_Green_Screen\">\n"
    "    <Space_Model_Name>w_stars_greenscreen.alo</Space_Model_Name>\n"
    "    <Scale_Factor>20.0</Scale_Factor>\n"
    "    <In_Background>No</In_Background>\n"
    "  </SpacePrimarySkydome>\n"
    "  <!-- comment between entries is tolerated -->\n"
    "  <SpacePrimarySkydome Name=\"NoModel\">\n"
    "    <Space_Model_Name></Space_Model_Name>\n"
    "  </SpacePrimarySkydome>\n"
    "  <SpacePrimarySkydome>\n"
    "    <Space_Model_Name>w_noname.alo</Space_Model_Name>\n"
    "  </SpacePrimarySkydome>\n"
    "  <SpacePrimarySkydome Name=\"GarbageScale\">\n"
    "    <Space_Model_Name>w_garbage.alo</Space_Model_Name>\n"
    "    <Scale_Factor>not_a_number</Scale_Factor>\n"
    "  </SpacePrimarySkydome>\n"
    "</SpacePrimarySkydomes>\n";

static const char* kSpaceSecondary =
    "<?xml version=\"1.0\" ?>\n"
    "<SpaceSecondarySkydomes>\n"
    "  <SpaceSecondarySkydome Name=\"Star_Backdrop_Blue\">\n"
    "    <Space_Model_Name>w_stars_nebula_blue.alo</Space_Model_Name>\n"
    "    <Scale_Factor>25.0</Scale_Factor>\n"
    "    <Sort_Order_Adjust>-1</Sort_Order_Adjust>\n"
    "    <In_Background>yes</In_Background>\n"
    "  </SpaceSecondarySkydome>\n"
    "</SpaceSecondarySkydomes>\n";

static const char* kLandPrimary =
    "<?xml version=\"1.0\" ?>\n"
    "<LandPrimarySkydomes>\n"
    "  <LandPrimarySkydome Name=\"Day_Blue_Sky\">\n"
    "    <Land_Model_Name>w_sky00.alo</Land_Model_Name>\n"
    "    <Scale_Factor>1.0</Scale_Factor>\n"
    "    <In_Background>no</In_Background>\n"
    "  </LandPrimarySkydome>\n"
    "</LandPrimarySkydomes>\n";

// --- GameObjectFiles-driven locator fixtures (mod domes under non-canonical
//     filenames; a mod registers e.g. Props\Skydomes_Space_Secondary.xml) -------
static const char* kGameObjectFiles =
    "<?xml version=\"1.0\" ?>\n"
    "<Game_Object_Files>\n"
    "  <File>GroundUnits.xml</File>\n"                       // non-skydome -> ignored
    "  <File>Props\\Skydomes_Space_Secondary.xml</File>\n"   // mod renamed/relocated
    "  <File>Extra_Space_Secondary.xml</File>\n"             // second listed file (dedup)
    "</Game_Object_Files>\n";

static const char* kGroundUnits =
    "<?xml version=\"1.0\" ?>\n"
    "<Units>\n"
    "  <SpaceUnit Name=\"X_Wing\"><Space_Model_Name>xwing.alo</Space_Model_Name></SpaceUnit>\n"
    "</Units>\n";

static const char* kModSecondary =
    "<?xml version=\"1.0\" ?>\n"
    "<SpaceSecondarySkydomes>\n"
    "  <SpaceSecondarySkydome Name=\"Mod_Nebula\">\n"
    "    <Space_Model_Name>mod_nebula.alo</Space_Model_Name>\n"
    "    <Scale_Factor>25.0</Scale_Factor>\n"
    "  </SpaceSecondarySkydome>\n"
    "  <SpaceSecondarySkydome Name=\"Dup_Dome\">\n"
    "    <Space_Model_Name>dup_first.alo</Space_Model_Name>\n"
    "  </SpaceSecondarySkydome>\n"
    "</SpaceSecondarySkydomes>\n";

static const char* kExtraSecondary =
    "<?xml version=\"1.0\" ?>\n"
    "<SpaceSecondarySkydomes>\n"
    "  <SpaceSecondarySkydome Name=\"Dup_Dome\">\n"             // duplicate Name -> first wins
    "    <Space_Model_Name>dup_second.alo</Space_Model_Name>\n"
    "  </SpaceSecondarySkydome>\n"
    "  <SpaceSecondarySkydome Name=\"Extra_Nebula\">\n"
    "    <Space_Model_Name>extra_nebula.alo</Space_Model_Name>\n"
    "  </SpaceSecondarySkydome>\n"
    "</SpaceSecondarySkydomes>\n";

static const SkydomeRef* find(const std::vector<SkydomeRef>& v, const std::string& name)
{
    for (const auto& r : v) if (r.name == name) return &r;
    return nullptr;
}

// Dump mode (argv[1] = axis 0..3, argv[2] = real *Skydomes.xml path): parse +
// print, no assertions. Validates the reader against real install XML.
static int dumpRealXml(int axisIdx, const char* path)
{
    static const char* canon[] = {
        "LandPrimarySkydomes.xml", "LandSecondarySkydomes.xml",
        "SpacePrimarySkydomes.xml", "SpaceSecondarySkydomes.xml"
    };
    if (axisIdx < 0 || axisIdx > 3) { std::printf("axis must be 0..3\n"); return 2; }
    std::wstring wpath(path, path + std::strlen(path));
    std::string content;
    try {
        IFile* f = new PhysicalFile(wpath, PhysicalFile::READ);
        std::vector<unsigned char> b = ReadAndRelease(f);
        content.assign((const char*)b.data(), b.size());
    } catch (...) { std::printf("cannot read %s\n", path); return 2; }

    MockFM fm;
    fm.files[std::string("Data\\XML\\") + canon[axisIdx]] = content;
    std::vector<SkydomeRef> list;
    bool ok = LoadSkydomeList(fm, (SkydomeAxis)axisIdx, list);
    std::printf("load=%s  refs=%zu\n", ok ? "true" : "false", list.size());
    for (const auto& r : list)
        std::printf("  %-28s model=%-32s scale=%g sort=%d bg=%d\n",
            r.name.c_str(), r.modelPath.c_str(), r.scaleFactor, r.sortOrderAdjust, (int)r.inBackground);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 2) return dumpRealXml(std::atoi(argv[1]), argv[2]);

    MockFM fm;
    fm.files["Data\\XML\\SpacePrimarySkydomes.xml"]   = kSpacePrimary;
    fm.files["Data\\XML\\SpaceSecondarySkydomes.xml"] = kSpaceSecondary;
    fm.files["Data\\XML\\LandPrimarySkydomes.xml"]    = kLandPrimary;
    fm.files["Data\\Art\\Models\\w_stars_low.alo"]    = std::string(64, '\xAB');  // dummy mesh bytes

    // ---- enumeration --------------------------------------------------------
    std::printf("[enumerate]\n");
    {
        std::vector<SkydomeRef> list;
        bool ok = LoadSkydomeList(fm, SkydomeAxis::SpacePrimary, list);
        CHECK(ok, "SpacePrimary load ok");
        CHECK(list.size() == 3, "3 refs (NoModel + unnamed entries skipped)");
        const SkydomeRef* low = find(list, "Stars_Low");
        CHECK(low != nullptr, "Stars_Low present");
        if (low)
        {
            CHECK(low->modelPath == "w_stars_low.alo", "Stars_Low model path");
            CHECK(low->scaleFactor == 1.0f, "Stars_Low scale 1.0");
            CHECK(low->sortOrderAdjust == -1, "Stars_Low sortOrder -1");
            CHECK(low->inBackground == true, "Stars_Low inBackground (yes)");
        }
        const SkydomeRef* grn = find(list, "Cin_Space_Green_Screen");
        CHECK(grn != nullptr, "Green_Screen present");
        if (grn)
        {
            CHECK(grn->scaleFactor == 20.0f, "Green scale 20.0");
            CHECK(grn->sortOrderAdjust == 0, "Green sortOrder defaults to 0 (absent)");
            CHECK(grn->inBackground == false, "Green inBackground false (case-insensitive No)");
        }
        CHECK(find(list, "NoModel") == nullptr, "NoModel skipped (empty model)");
        const SkydomeRef* gs = find(list, "GarbageScale");
        CHECK(gs != nullptr && gs->scaleFactor == 1.0f, "GarbageScale: non-numeric Scale_Factor defaults to 1.0");
        bool anyEmptyName = false;
        for (const auto& r : list) if (r.name.empty()) anyEmptyName = true;
        CHECK(!anyEmptyName, "unnamed entry skipped (no empty-name refs)");
    }

    // ---- land axis uses Land_Model_Name ------------------------------------
    std::printf("[land axis]\n");
    {
        std::vector<SkydomeRef> list;
        CHECK(LoadSkydomeList(fm, SkydomeAxis::LandPrimary, list), "LandPrimary load ok");
        CHECK(list.size() == 1 && list[0].modelPath == "w_sky00.alo", "Land model via Land_Model_Name");
        CHECK(list[0].inBackground == false, "Land inBackground false (no)");
    }

    // ---- total miss ---------------------------------------------------------
    std::printf("[total miss]\n");
    {
        MockFM empty;
        std::vector<SkydomeRef> list;
        bool ok = LoadSkydomeList(empty, SkydomeAxis::SpacePrimary, list);
        CHECK(!ok, "missing list file -> false");
        CHECK(list.empty(), "missing list -> empty out");
    }

    // ---- model resolution ---------------------------------------------------
    std::printf("[resolve model]\n");
    {
        SkydomeRef ref; ref.modelPath = "w_stars_low.alo";
        std::vector<unsigned char> bytes;
        CHECK(ResolveSkydomeModel(fm, ref, bytes) && bytes.size() == 64, "resolve existing model bytes");

        SkydomeRef missing; missing.modelPath = "nope.alo";
        std::vector<unsigned char> b2;
        CHECK(!ResolveSkydomeModel(fm, missing, b2), "missing model -> false");

        SkydomeRef noPath;
        std::vector<unsigned char> b3;
        CHECK(!ResolveSkydomeModel(fm, noPath, b3), "empty modelPath -> false");
    }

    // ---- primary + secondary pair resolution -------------------------------
    std::printf("[map environment]\n");
    {
        MapEnvironment env;
        bool ok = LoadMapEnvironment(fm, SkydomeContext::Space, "Stars_Low", "Star_Backdrop_Blue", env);
        CHECK(ok, "Space env load ok");
        CHECK(env.hasPrimary && env.primary.name == "Stars_Low", "primary resolved");
        CHECK(env.hasSecondary && env.secondary.modelPath == "w_stars_nebula_blue.alo", "secondary resolved");

        // Asymmetric miss: secondary name not in the list.
        MapEnvironment env2;
        bool ok2 = LoadMapEnvironment(fm, SkydomeContext::Space, "Stars_Low", "DoesNotExist", env2);
        CHECK(ok2, "asymmetric: load returns ok (both lists readable)");
        CHECK(env2.hasPrimary && !env2.hasSecondary, "asymmetric: primary resolves, secondary unset");

        // Empty secondary name -> primary only, no secondary lookup.
        MapEnvironment env3;
        bool ok3 = LoadMapEnvironment(fm, SkydomeContext::Space, "Stars_Low", "", env3);
        CHECK(ok3, "empty secondary: load returns ok");
        CHECK(env3.hasPrimary && !env3.hasSecondary, "empty secondary name -> primary only");
    }

    // ---- GameObjectFiles-driven locator (mod domes under non-canonical names) ----
    std::printf("[gameobjectfiles locator]\n");
    {
        MockFM mod;
        mod.files["Data\\XML\\GameObjectFiles.xml"]                 = kGameObjectFiles;
        mod.files["Data\\XML\\GroundUnits.xml"]                     = kGroundUnits;
        mod.files["Data\\XML\\Props\\Skydomes_Space_Secondary.xml"] = kModSecondary;
        mod.files["Data\\XML\\Extra_Space_Secondary.xml"]           = kExtraSecondary;
        // No canonical Data\XML\SpaceSecondarySkydomes.xml -> proves the locator
        // does NOT depend on the hardcoded filename, only on the root element.

        std::vector<SkydomeRef> list;
        bool ok = LoadSkydomeList(mod, SkydomeAxis::SpaceSecondary, list);
        CHECK(ok, "GOF: SpaceSecondary load ok");
        CHECK(find(list, "Mod_Nebula") != nullptr,
              "GOF: renamed/relocated mod dome found (Props\\Skydomes_Space_Secondary.xml)");
        CHECK(find(list, "Extra_Nebula") != nullptr, "GOF: second listed skydome file harvested");
        CHECK(find(list, "X_Wing") == nullptr, "GOF: non-skydome file (root <Units>) ignored");
        const SkydomeRef* dup = find(list, "Dup_Dome");
        CHECK(dup != nullptr, "GOF: Dup_Dome present");
        CHECK(dup != nullptr && dup->modelPath == "dup_first.alo",
              "GOF: duplicate Name across files -> first listed file wins");
        int dupCount = 0; for (const auto& r : list) if (r.name == "Dup_Dome") ++dupCount;
        CHECK(dupCount == 1, "GOF: duplicate Name deduped to a single entry");
    }

    // ---- fallback: no GameObjectFiles.xml -> canonical filename (today's behavior) ----
    std::printf("[locator fallback]\n");
    {
        MockFM fb;
        fb.files["Data\\XML\\SpaceSecondarySkydomes.xml"] = kSpaceSecondary;  // no GameObjectFiles.xml
        std::vector<SkydomeRef> list;
        bool ok = LoadSkydomeList(fb, SkydomeAxis::SpaceSecondary, list);
        CHECK(ok, "fallback: load ok via canonical filename when no GameObjectFiles.xml");
        CHECK(find(list, "Star_Backdrop_Blue") != nullptr,
              "fallback: canonical dome found when no GameObjectFiles.xml");
    }

    // ---- robustness: sniff-miss falls back to a full parse (root past 1KB) ----
    std::printf("[locator robustness]\n");
    {
        // Root element sits past the 1KB sniff window (huge leading comment), so the
        // cheap sniff can't see it -> must fall back to a full parse, not drop the file.
        std::string deep =
            "<?xml version=\"1.0\" ?>\n<!-- " + std::string(1500, 'x') + " -->\n"
            "<SpaceSecondarySkydomes>\n"
            "  <SpaceSecondarySkydome Name=\"Deep_Root_Nebula\">\n"
            "    <Space_Model_Name>deep.alo</Space_Model_Name>\n"
            "  </SpaceSecondarySkydome>\n"
            "</SpaceSecondarySkydomes>\n";
        MockFM big;
        big.files["Data\\XML\\GameObjectFiles.xml"] =
            "<?xml version=\"1.0\" ?>\n<Game_Object_Files>\n  <File>Deep.xml</File>\n</Game_Object_Files>\n";
        big.files["Data\\XML\\Deep.xml"] = deep;
        std::vector<SkydomeRef> list;
        LoadSkydomeList(big, SkydomeAxis::SpaceSecondary, list);
        CHECK(find(list, "Deep_Root_Nebula") != nullptr,
              "sniff-miss (root past 1KB comment) -> full-parse fallback still finds the dome");

        // UTF-8 BOM before the <?xml?> decl: sniff skips the BOM bytes; dome found.
        MockFM bom;
        bom.files["Data\\XML\\GameObjectFiles.xml"] =
            "<?xml version=\"1.0\" ?>\n<Game_Object_Files>\n  <File>Bom.xml</File>\n</Game_Object_Files>\n";
        bom.files["Data\\XML\\Bom.xml"] = std::string("\xEF\xBB\xBF") + kSpaceSecondary;
        std::vector<SkydomeRef> blist;
        LoadSkydomeList(bom, SkydomeAxis::SpaceSecondary, blist);
        CHECK(find(blist, "Star_Backdrop_Blue") != nullptr, "UTF-8 BOM before <?xml?> -> dome still found");

        // GameObjectFiles.xml present but malformed -> fall back to canonical (no
        // silent empty picker that hides the vanilla domes that DO exist).
        MockFM badGof;
        badGof.files["Data\\XML\\GameObjectFiles.xml"]      = "not even xml <<<";
        badGof.files["Data\\XML\\SpaceSecondarySkydomes.xml"] = kSpaceSecondary;
        std::vector<SkydomeRef> flist;
        bool fok = LoadSkydomeList(badGof, SkydomeAxis::SpaceSecondary, flist);
        CHECK(fok, "malformed GameObjectFiles.xml -> ok via canonical fallback");
        CHECK(find(flist, "Star_Backdrop_Blue") != nullptr,
              "malformed GameObjectFiles.xml -> canonical domes still listed (no silent empty)");
    }

    // ---- LoadAllSkydomeLists: one GOF pass == per-axis LoadSkydomeList ----
    std::printf("[load-all one-pass]\n");
    {
        MockFM all_fm;
        all_fm.files["Data\\XML\\GameObjectFiles.xml"] =
            "<?xml version=\"1.0\" ?>\n<Game_Object_Files>\n"
            "  <File>GroundUnits.xml</File>\n"                       // non-skydome -> ignored
            "  <File>SpacePrimarySkydomes.xml</File>\n"
            "  <File>Props\\Skydomes_Space_Secondary.xml</File>\n"   // mod renamed secondary
            "  <File>LandPrimarySkydomes.xml</File>\n"
            "</Game_Object_Files>\n";
        all_fm.files["Data\\XML\\GroundUnits.xml"]                     = kGroundUnits;
        all_fm.files["Data\\XML\\SpacePrimarySkydomes.xml"]            = kSpacePrimary;
        all_fm.files["Data\\XML\\Props\\Skydomes_Space_Secondary.xml"] = kModSecondary;
        all_fm.files["Data\\XML\\LandPrimarySkydomes.xml"]             = kLandPrimary;

        std::array<std::vector<SkydomeRef>, kNumSkydomeAxes> all;
        LoadAllSkydomeLists(all_fm, all);

        // Per-axis equivalence with LoadSkydomeList (same FM).
        const SkydomeAxis axes[4] = { SkydomeAxis::LandPrimary, SkydomeAxis::LandSecondary,
                                      SkydomeAxis::SpacePrimary, SkydomeAxis::SpaceSecondary };
        bool allEqual = true;
        for (int a = 0; a < kNumSkydomeAxes; ++a) {
            std::vector<SkydomeRef> per;
            LoadSkydomeList(all_fm, axes[a], per);
            if (per.size() != all[a].size()) { allEqual = false; break; }
            for (size_t i = 0; i < per.size(); ++i)
                if (per[i].name != all[a][i].name || per[i].modelPath != all[a][i].modelPath) { allEqual = false; break; }
        }
        CHECK(allEqual, "LoadAllSkydomeLists matches LoadSkydomeList for every axis");
        CHECK(find(all[(int)SkydomeAxis::SpaceSecondary], "Mod_Nebula") != nullptr, "all: mod renamed secondary -> SpaceSecondary bucket");
        CHECK(find(all[(int)SkydomeAxis::SpacePrimary], "Stars_Low") != nullptr, "all: SpacePrimary bucketed");
        CHECK(find(all[(int)SkydomeAxis::LandPrimary], "Day_Blue_Sky") != nullptr, "all: LandPrimary bucketed");
        CHECK(find(all[(int)SkydomeAxis::SpacePrimary], "X_Wing") == nullptr, "all: non-skydome (GroundUnits) not bucketed anywhere");

        // No GameObjectFiles.xml -> canonical fallback for every axis.
        MockFM fb_fm;
        fb_fm.files["Data\\XML\\SpaceSecondarySkydomes.xml"] = kSpaceSecondary;
        fb_fm.files["Data\\XML\\LandPrimarySkydomes.xml"]    = kLandPrimary;
        std::array<std::vector<SkydomeRef>, kNumSkydomeAxes> allf;
        LoadAllSkydomeLists(fb_fm, allf);
        CHECK(find(allf[(int)SkydomeAxis::SpaceSecondary], "Star_Backdrop_Blue") != nullptr, "all fallback: SpaceSecondary via canonical");
        CHECK(find(allf[(int)SkydomeAxis::LandPrimary], "Day_Blue_Sky") != nullptr, "all fallback: LandPrimary via canonical");

        // ResolveMapEnvironment from pre-loaded lists (no file I/O).
        MapEnvironment env;
        ResolveMapEnvironment(all, SkydomeContext::Space, "Stars_Low", "Mod_Nebula", env);
        CHECK(env.hasPrimary && env.primary.name == "Stars_Low", "resolve-from-lists: primary resolved");
        CHECK(env.hasSecondary && env.secondary.modelPath == "mod_nebula.alo", "resolve-from-lists: renamed secondary resolved");
        MapEnvironment env2;
        ResolveMapEnvironment(all, SkydomeContext::Space, "Stars_Low", "DoesNotExist", env2);
        CHECK(env2.hasPrimary && !env2.hasSecondary, "resolve-from-lists: missing secondary unset");
    }

    std::printf("\n=== SkydomeEnvironment: %s ===\n", g_failed == 0 ? "ALL PASS" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
