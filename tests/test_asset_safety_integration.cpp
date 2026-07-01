// Device-free integration tests for fail-closed mod asset path gating.
//
// Drives the catalog and skydome XML loaders through a recording IFileManager.
// Unsafe mod-authored <File> / model values must be treated as missing and must
// never reach getFile after being concatenated under Data\XML or Data\Art\Models.

#include "GameObjectCatalog.h"
#include "SkydomeEnvironment.h"
#include "files.h"
#include "managers.h"
#include "ResourceLimits.h"

#include <array>
#include <cstdio>
#include <map>
#include <sstream>
#include <set>
#include <string>
#include <vector>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

struct HugeFile : IFile
{
    unsigned long pos = 0;
    bool* readAttempted;
    explicit HugeFile(bool* readFlag) : readAttempted(readFlag) {}
    bool eof() override { return pos >= size(); }
    unsigned long size() override { return 64u * 1024u * 1024u + 1u; }
    void seek(unsigned long offset) override { pos = offset; }
    unsigned long tell() override { return pos; }
    unsigned long read(void*, unsigned long n) override { *readAttempted = true; pos += n; return 0; }
    unsigned long write(const void*, unsigned long) override { return 0; }
};

struct ClaimedSizeFile : IFile
{
    unsigned long claimedSize;
    unsigned long pos = 0;
    bool* readAttempted;
    ClaimedSizeFile(unsigned long size, bool* readFlag)
        : claimedSize(size), readAttempted(readFlag) {}
    bool eof() override { return pos >= size(); }
    unsigned long size() override { return claimedSize; }
    void seek(unsigned long offset) override { pos = offset < claimedSize ? offset : claimedSize; }
    unsigned long tell() override { return pos; }
    unsigned long read(void*, unsigned long n) override { *readAttempted = true; pos += n; return 0; }
    unsigned long write(const void*, unsigned long) override { return 0; }
};

struct MockFM : IFileManager
{
    std::map<std::string, std::string> files;
    std::set<std::string> hugeFiles;
    std::map<std::string, unsigned long> claimedSizeFiles;
    std::vector<std::string> calls;
    bool hugeReadAttempted = false;
    bool claimedReadAttempted = false;

    IFile* getFile(const std::string& path) override
    {
        calls.push_back(path);
        if (hugeFiles.count(path)) return new HugeFile(&hugeReadAttempted);
        auto claimed = claimedSizeFiles.find(path);
        if (claimed != claimedSizeFiles.end())
            return new ClaimedSizeFile(claimed->second, &claimedReadAttempted);
        auto it = files.find(path);
        if (it == files.end()) return nullptr;
        MemoryFile* mf = new MemoryFile();
        if (!it->second.empty())
            mf->write(it->second.data(), (unsigned long)it->second.size());
        mf->seek(0);
        return mf;
    }
};

static bool containsRiskyPath(const std::string& p)
{
    if (p.find(':') != std::string::npos) return true;
    if (p.find("..\\") != std::string::npos || p.find("../") != std::string::npos) return true;
    if (p.find("\\\\") != std::string::npos || p.find("//") != std::string::npos) return true;
    if (p.find("\\/") != std::string::npos || p.find("/\\") != std::string::npos) return true;
    return false;
}

static void expectNoRiskyGetFile(const MockFM& fm, size_t start, const char* label)
{
    bool bad = false;
    for (size_t i = start; i < fm.calls.size(); ++i)
        if (containsRiskyPath(fm.calls[i]))
            bad = true;
    CHECK(!bad, label);
}

static const GameObjectRef* findObject(const GameObjectCatalog& cat, const std::string& name)
{
    for (const GameObjectRef& r : cat.objects)
        if (r.name == name) return &r;
    return nullptr;
}

static void testCatalogPathGates()
{
    std::printf("[catalog path gates]\n");
    MockFM fm;
    fm.files["Data\\XML\\GameObjectFiles.xml"] =
        "<Game_Object_Files>"
        "  <File>..\\Escape.xml</File>"
        "  <File>C:\\Escape.xml</File>"
        "  <File>\\\\unc\\Escape.xml</File>"
        "  <File>Safe.xml</File>"
        "</Game_Object_Files>";
    fm.files["Data\\XML\\Safe.xml"] =
        "<Objects>"
        "  <GroundVehicle Name=\"Safe\"><Land_Model_Name>EV_OK.ALO</Land_Model_Name></GroundVehicle>"
        "  <GroundVehicle Name=\"UnsafeModel\"><Land_Model_Name>..\\EV_BAD.ALO</Land_Model_Name></GroundVehicle>"
        "</Objects>";
    fm.files["Data\\XML\\HardPointDataFiles.xml"] =
        "<Hard_Point_Files>"
        "  <File>..\\BadHardpoints.xml</File>"
        "  <File>Hardpoints.xml</File>"
        "</Hard_Point_Files>";
    fm.files["Data\\XML\\Hardpoints.xml"] =
        "<HardPoints>"
        "  <HardPoint Name=\"HP_SAFE\"><Model_To_Attach>EV_SAFE_ATTACH.ALO</Model_To_Attach></HardPoint>"
        "  <HardPoint Name=\"HP_UNSAFE\"><Model_To_Attach>C:\\EV_BAD_ATTACH.ALO</Model_To_Attach></HardPoint>"
        "</HardPoints>";

    GameObjectCatalog cat;
    const bool ok = BuildGameObjectCatalog(fm, cat);
    CHECK(ok, "catalog builds with mixed safe/unsafe XML");
    expectNoRiskyGetFile(fm, 0, "unsafe catalog/hardpoint <File> paths never reach getFile");
    CHECK(findObject(cat, "Safe") != nullptr, "safe model remains cataloged");
    CHECK(findObject(cat, "UnsafeModel") == nullptr, "unsafe primary model is treated as missing");
    auto hp = cat.hardpoints.find("hp_unsafe");
    CHECK(hp != cat.hardpoints.end() && hp->second.modelToAttach.empty(),
          "unsafe Model_To_Attach is not stored for later engine load");

    const size_t beforeProbe = fm.calls.size();
    CHECK(ProbeModelSkinned(fm, "..\\EV_BAD.ALO") == ModelProbeResult::LoadFailed,
          "ProbeModelSkinned treats unsafe model path as missing");
    expectNoRiskyGetFile(fm, beforeProbe, "unsafe ProbeModelSkinned model path never reaches getFile");
}

static void testCatalogSizeCap()
{
    std::printf("[catalog size cap]\n");
    MockFM fm;
    fm.files["Data\\XML\\GameObjectFiles.xml"] =
        "<Game_Object_Files><File>Huge.xml</File></Game_Object_Files>";
    fm.hugeFiles.insert("Data\\XML\\Huge.xml");
    fm.hugeFiles.insert("Data\\Scripts\\Library\\GameObjectList.lua");

    GameObjectCatalog cat;
    const bool ok = BuildGameObjectCatalog(fm, cat);
    CHECK(ok, "catalog build skips an oversized listed XML instead of throwing");
    CHECK(cat.objects.empty(), "oversized listed XML contributes no objects");
    CHECK(!fm.hugeReadAttempted, "oversized listed XML and roster Lua are not read before skip");

    MockFM rosterFm;
    rosterFm.files["Data\\XML\\GameObjectFiles.xml"] =
        "<Game_Object_Files><File>One.xml</File></Game_Object_Files>";
    rosterFm.files["Data\\XML\\One.xml"] =
        "<Objects><GroundVehicle Name=\"One\"><Land_Model_Name>EV_ONE.ALO</Land_Model_Name></GroundVehicle></Objects>";
    rosterFm.hugeFiles.insert("Data\\Scripts\\Library\\GameObjectList.lua");

    GameObjectCatalog rosterCat;
    const bool rosterOk = BuildGameObjectCatalog(rosterFm, rosterCat);
    CHECK(rosterOk, "catalog build skips an oversized roster Lua instead of throwing");
    CHECK(findObject(rosterCat, "One") != nullptr, "oversized roster Lua does not drop normal catalog objects");
    CHECK(!rosterFm.hugeReadAttempted, "oversized roster Lua is not read before skip");
}

static void testCatalogAggregateCap()
{
    std::printf("[catalog aggregate cap]\n");
    MockFM fm;
    fm.files["Data\\XML\\GameObjectFiles.xml"] =
        "<Game_Object_Files>"
        "  <File>Small.xml</File>"
        "  <File>WouldOverflow.xml</File>"
        "</Game_Object_Files>";
    fm.files["Data\\XML\\Small.xml"] = "<Objects></Objects>";
    fm.claimedSizeFiles["Data\\XML\\WouldOverflow.xml"] = kMaxXmlFileBytes;

    GameObjectCatalog cat;
    const bool ok = BuildGameObjectCatalog(fm, cat);
    CHECK(ok, "catalog build tolerates aggregate XML byte budget overflow");
    CHECK(!fm.claimedReadAttempted, "aggregate-overflow XML file is not read before skip");
}

static void testCatalogFileCountCap()
{
    std::printf("[catalog file count cap]\n");
    MockFM fm;
    std::ostringstream gof;
    gof << "<Game_Object_Files>";
    for (unsigned long i = 0; i < kMaxCatalogXmlFileCount; ++i)
        gof << "<File>Missing" << i << ".xml</File>";
    gof << "<File>BeyondCap.xml</File>";
    gof << "</Game_Object_Files>";
    fm.files["Data\\XML\\GameObjectFiles.xml"] = gof.str();
    fm.files["Data\\XML\\BeyondCap.xml"] = "<Objects></Objects>";

    GameObjectCatalog cat;
    const bool ok = BuildGameObjectCatalog(fm, cat);
    CHECK(ok, "catalog build tolerates a file list beyond the count cap");

    bool readBeyondCap = false;
    for (const std::string& call : fm.calls)
        if (call == "Data\\XML\\BeyondCap.xml")
            readBeyondCap = true;
    CHECK(!readBeyondCap, "catalog does not read files beyond the file-count cap");
}

static void testSkydomePathGates()
{
    std::printf("[skydome path gates]\n");
    MockFM fm;
    fm.files["Data\\XML\\GameObjectFiles.xml"] =
        "<Game_Object_Files>"
        "  <File>..\\EscapeSky.xml</File>"
        "  <File>Sky.xml</File>"
        "</Game_Object_Files>";
    fm.files["Data\\XML\\Sky.xml"] =
        "<LandPrimarySkydomes>"
        "  <LandPrimarySkydome Name=\"SafeSky\"><Land_Model_Name>SKY_SAFE.ALO</Land_Model_Name></LandPrimarySkydome>"
        "  <LandPrimarySkydome Name=\"UnsafeSky\"><Land_Model_Name>\\\\unc\\SKY_BAD.ALO</Land_Model_Name></LandPrimarySkydome>"
        "</LandPrimarySkydomes>";

    std::vector<SkydomeRef> refs;
    const bool ok = LoadSkydomeList(fm, SkydomeAxis::LandPrimary, refs);
    CHECK(ok, "skydome list loads with mixed safe/unsafe XML");
    expectNoRiskyGetFile(fm, 0, "unsafe skydome GOF <File> path never reaches getFile");
    bool sawSafe = false, sawUnsafe = false;
    for (const SkydomeRef& r : refs)
    {
        if (r.name == "SafeSky" && r.modelPath == "SKY_SAFE.ALO") sawSafe = true;
        if (r.name == "UnsafeSky" && !r.modelPath.empty()) sawUnsafe = true;
    }
    CHECK(sawSafe, "safe skydome model remains listed");
    CHECK(!sawUnsafe, "unsafe skydome model path is not stored as loadable");

    const size_t beforeResolve = fm.calls.size();
    SkydomeRef bad;
    bad.name = "Bad";
    bad.modelPath = "C:\\SKY_BAD.ALO";
    std::vector<unsigned char> bytes;
    CHECK(!ResolveSkydomeModel(fm, bad, bytes), "ResolveSkydomeModel treats unsafe model path as missing");
    expectNoRiskyGetFile(fm, beforeResolve, "unsafe ResolveSkydomeModel path never reaches getFile");

    const size_t beforeAll = fm.calls.size();
    std::array<std::vector<SkydomeRef>, kNumSkydomeAxes> all;
    LoadAllSkydomeLists(fm, all);
    expectNoRiskyGetFile(fm, beforeAll, "unsafe LoadAllSkydomeLists GOF <File> path never reaches getFile");
}

int main()
{
    std::printf("test_asset_safety_integration\n");
    testCatalogPathGates();
    testCatalogSizeCap();
    testCatalogAggregateCap();
    testCatalogFileCountCap();
    testSkydomePathGates();
    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
