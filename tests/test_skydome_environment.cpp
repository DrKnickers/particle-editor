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
#include "ResourceLimits.h"   // kMaxCatalogXmlFileCount (manifest cap case)

#include <array>
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
#define CHECK_COUNT(actual, expected, msg) do {                         \
    const size_t got_ = (actual);                                      \
    const size_t expected_ = (expected);                               \
    if (got_ == expected_) { std::printf("  ok: %s\n", msg); }         \
    else { ++g_failed; std::printf("  FAIL: %s (got %zu, expected %zu)\n", \
                                   msg, got_, expected_); }             \
} while (0)

// Mock FileManager: serves registered paths from in-memory strings.
struct MockFM : IFileManager
{
    std::map<std::string, std::string> files;
    // Every path the reader asked for. The manifest-cap case below has no other
    // oracle: the listed files don't exist, so the only observable is how many
    // the enumeration TRIED to open.
    std::vector<std::string> requested;
    IFile* getFile(const std::string& path) override
    {
        requested.push_back(path);
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

static std::string numberedDomeName(unsigned index)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "Dome_%04u", index);
    return buf;
}

static void appendNumberedDome(std::string& xml, unsigned index)
{
    const std::string name = numberedDomeName(index);
    xml += "  <SpacePrimarySkydome Name=\"" + name + "\">\n";
    xml += "    <Space_Model_Name>" + name + ".alo</Space_Model_Name>\n";
    xml += "  </SpacePrimarySkydome>\n";
}

// One routed file of a multi-file entry-cap fixture. A model-less entry and a
// repeated model-bearing Name precede the unique range: neither may consume an
// accepted slot. Repeated file paths are added separately in the GOF fixture.
static std::string makeNumberedDomeFile(unsigned first, unsigned count,
                                        unsigned duplicateIndex,
                                        const char* modelLessName)
{
    std::string xml = "<?xml version=\"1.0\" ?>\n<SpacePrimarySkydomes>\n";
    xml += "  <SpacePrimarySkydome Name=\"";
    xml += modelLessName;
    xml += "\"><Scale_Factor>1</Scale_Factor></SpacePrimarySkydome>\n";
    appendNumberedDome(xml, duplicateIndex);
    for (unsigned i = 0; i < count; ++i)
        appendNumberedDome(xml, first + i);
    xml += "</SpacePrimarySkydomes>\n";
    return xml;
}

static std::string makeEntryCapManifest(bool includeLand)
{
    std::string xml =
        "<?xml version=\"1.0\" ?>\n<Game_Object_Files>\n"
        "  <File>Cap_A.xml</File>\n"
        "  <File>Cap_A.xml</File>\n"
        "  <File>Cap_B.xml</File>\n"
        "  <File>Cap_B.xml</File>\n";
    if (includeLand) xml += "  <File>LandPrimarySkydomes.xml</File>\n";
    xml += "</Game_Object_Files>\n";
    return xml;
}

static bool isExactNumberedPrefix(const std::vector<SkydomeRef>& list,
                                  unsigned count)
{
    if (list.size() < count) return false;
    for (unsigned i = 0; i < count; ++i)
        if (list[i].name != numberedDomeName(i)) return false;
    return true;
}

// Dump mode (argv[1] = axis 0..3, argv[2] = real *Skydomes.xml path): parse +
// print for manual inspection, with no assertions.
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

    // ---- accepted entry cap per axis/load (2026-07 audit, B-UX-6) -----------
    std::printf("[accepted entry cap]\n");
    {
        // Exact literal boundary through LoadSkydomeList. The two files carry
        // 600 + 424 unique model-bearing entries; their repeated Names,
        // model-less entries, and repeated GOF paths must consume no slots.
        MockFM exact;
        exact.files["Data\\XML\\GameObjectFiles.xml"] = makeEntryCapManifest(false);
        exact.files["Data\\XML\\Cap_A.xml"] =
            makeNumberedDomeFile(0u, 600u, 0u, "NoModel_A");
        exact.files["Data\\XML\\Cap_B.xml"] =
            makeNumberedDomeFile(600u, 424u, 599u, "NoModel_B");

        std::vector<SkydomeRef> exactList;
        const bool exactOk =
            LoadSkydomeList(exact, SkydomeAxis::SpacePrimary, exactList);
        CHECK(exactOk, "1024-entry axis load remains successful");
        CHECK_COUNT(exactList.size(), 1024u,
                    "exactly 1024 accepted unique model-bearing entries are retained");
        CHECK(isExactNumberedPrefix(exactList, 1024u),
              "exact-cap load retains Dome_0000 through Dome_1023 in order");

        // Cap+1 through the production cache loader. The output must be exactly
        // the original first 1024 -- not an arbitrary 1024-entry subset -- and
        // a saturated SpacePrimary axis must not suppress a later LandPrimary
        // file. Values are literal so a wrong production constant cannot move
        // the oracle with it.
        MockFM over;
        over.files["Data\\XML\\GameObjectFiles.xml"] = makeEntryCapManifest(true);
        over.files["Data\\XML\\Cap_A.xml"] =
            makeNumberedDomeFile(0u, 600u, 0u, "NoModel_A");
        over.files["Data\\XML\\Cap_B.xml"] =
            makeNumberedDomeFile(600u, 425u, 599u, "NoModel_B");
        over.files["Data\\XML\\LandPrimarySkydomes.xml"] = kLandPrimary;

        std::array<std::vector<SkydomeRef>, kNumSkydomeAxes> capped;
        LoadAllSkydomeLists(over, capped);
        const std::vector<SkydomeRef>& space =
            capped[(int)SkydomeAxis::SpacePrimary];
        CHECK_COUNT(space.size(), 1024u,
                    "1025 accepted candidates truncate to exactly 1024");
        CHECK(isExactNumberedPrefix(space, 1024u),
              "cap+1 load retains the exact first 1024 names in order");
        CHECK(find(space, "Dome_1024") == nullptr,
              "the literal 1025th unique model-bearing entry is omitted");
        CHECK(find(capped[(int)SkydomeAxis::LandPrimary], "Day_Blue_Sky") != nullptr,
              "a capped axis does not suppress a later routed axis");
    }

    // ---- manifest file-count cap (2026-07 audit, B-6) -----------------------
    //
    // GameObjectFiles.xml has TWO readers. GameObjectCatalog's readFileList caps
    // the list at kMaxCatalogXmlFileCount; LoadAllSkydomeLists did not, so a
    // crafted manifest was bounded for one consumer and unbounded for the other,
    // with every extra entry costing a getFile + root sniff here.
    //
    // Oracle: the listed files do not exist, so `requested` counts exactly what
    // the enumeration tried to open.
    std::printf("[manifest cap]\n");
    {
        // DISTINCT paths, not raw probes: a listed file is opened twice (cheap
        // root sniff, then the real parse when the sniff is inconclusive — which
        // it always is for a file that doesn't exist).
        auto countProbes = [](const MockFM& m) {
            std::set<std::string> distinct;
            for (const std::string& p : m.requested)
                if (p.rfind("Data\\XML\\flood", 0) == 0) distinct.insert(p);
            return distinct.size();
        };
        auto buildManifest = [](unsigned n) {
            std::string x = "<?xml version=\"1.0\" ?>\n<GameObjectFiles>\n";
            char buf[64];
            for (unsigned i = 0; i < n; ++i)
            {
                std::snprintf(buf, sizeof(buf), "  <File>flood%06u.xml</File>\n", i);
                x += buf;
            }
            return x + "</GameObjectFiles>\n";
        };

        // Over the cap: clamped, and clamped to EXACTLY the cap.
        {
            MockFM big;
            big.files["Data\\XML\\GameObjectFiles.xml"] = buildManifest(kMaxCatalogXmlFileCount + 904u);
            std::array<std::vector<SkydomeRef>, kNumSkydomeAxes> lists;
            LoadAllSkydomeLists(big, lists);
            const size_t probed = countProbes(big);
            CHECK(probed == (size_t)kMaxCatalogXmlFileCount,
                  "5000-entry manifest probes exactly kMaxCatalogXmlFileCount files");
        }

        // Exactly AT the cap: every entry still consulted. Without this, a guard
        // that fired one entry early would pass the assertion above while
        // silently dropping a legitimate file (handoff: pin a boundary from BOTH
        // sides — asserting the clamp alone cannot tell safety from data loss).
        {
            MockFM legal;
            legal.files["Data\\XML\\GameObjectFiles.xml"] = buildManifest(kMaxCatalogXmlFileCount);
            std::array<std::vector<SkydomeRef>, kNumSkydomeAxes> lists;
            LoadAllSkydomeLists(legal, lists);
            CHECK(countProbes(legal) == (size_t)kMaxCatalogXmlFileCount,
                  "a manifest exactly AT the cap loses nothing");
        }

        // Duplicates must not consume the budget: a flood of repeats must not
        // push a later legitimate entry past the cap (same rule readFileList
        // already applies).
        {
            MockFM dup;
            std::string x = "<?xml version=\"1.0\" ?>\n<GameObjectFiles>\n";
            for (unsigned i = 0; i < kMaxCatalogXmlFileCount + 500u; ++i)
                x += "  <File>flood000000.xml</File>\n";
            x += "  <File>flood999999.xml</File>\n</GameObjectFiles>\n";
            dup.files["Data\\XML\\GameObjectFiles.xml"] = x;
            std::array<std::vector<SkydomeRef>, kNumSkydomeAxes> lists;
            LoadAllSkydomeLists(dup, lists);
            bool sawLate = false;
            size_t rawRepeat = 0;
            for (const std::string& p : dup.requested)
            {
                if (p == "Data\\XML\\flood999999.xml") sawLate = true;
                if (p == "Data\\XML\\flood000000.xml") ++rawRepeat;
            }
            // Without dedup, 4596 repeats fill the 4096 budget and the late
            // distinct entry never gets read.
            CHECK(sawLate, "duplicates don't consume the budget (late distinct entry still read)");
            // RAW probes, not distinct: a distinct-count assertion here is a
            // tautology (the oracle collapses duplicates whether or not the code
            // does). Each kept file is opened twice — sniff, then full parse when
            // the sniff is inconclusive.
            CHECK(rawRepeat == 2, "a repeated entry is opened once (2 raw probes), not 4596 times");
        }

        // The SECOND reader of the same manifest: LoadSkydomeList goes through
        // gatherSkydomeFiles, a separate loop that was independently uncapped.
        // Capping one reader of a shared manifest is not capping the manifest.
        {
            MockFM big2;
            big2.files["Data\\XML\\GameObjectFiles.xml"] = buildManifest(kMaxCatalogXmlFileCount + 904u);
            std::vector<SkydomeRef> list;
            LoadSkydomeList(big2, SkydomeAxis::SpacePrimary, list);
            CHECK(countProbes(big2) == (size_t)kMaxCatalogXmlFileCount,
                  "LoadSkydomeList (gatherSkydomeFiles) applies the same manifest cap");
        }
    }

    std::printf("\n=== SkydomeEnvironment: %s ===\n", g_failed == 0 ? "ALL PASS" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
