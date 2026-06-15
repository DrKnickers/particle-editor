#include "GameObjectCatalog.h"

#include "managers.h"     // IFileManager
#include "xml.h"          // XMLTree / XMLNode
#include "files.h"        // IFile
#include "utils.h"        // WideToAnsi
#include "AloModel.h"     // LoadAloModel + AloIsSkinnedVertexFormat / AloIsNonVisibleShader
#include "exceptions.h"   // wexception (LoadAloModel boundary)

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace
{
    // First child element named `tag`, or empty. (XMLNode has no by-name lookup.)
    // Mirrors SkydomeEnvironment::childData.
    std::wstring childData(const XMLNode* node, const wchar_t* tag)
    {
        for (unsigned i = 0; i < node->getNumChildren(); ++i)
        {
            const XMLNode* c = node->getChild(i);
            if (c->getName() == tag) return c->getData();
        }
        return std::wstring();
    }

    std::string trim(const std::string& s)
    {
        const char* ws = " \t\r\n";
        const size_t a = s.find_first_not_of(ws);
        if (a == std::string::npos) return std::string();
        const size_t b = s.find_last_not_of(ws);
        return s.substr(a, b - a + 1);
    }

    // ASCII lower-case. Used to fold object Names into case-insensitive lookup
    // keys -- the Alamo engine resolves GameObject Names + Variant_Of references
    // case-INSENSITIVELY (the codebase already uses _stricmp for Alamo tokens in
    // Effect.cpp / engine.cpp), so vanilla ships variants like
    // `Variant_Of>V-Wing_Fighter<` against a parent declared `Name="V-wing_Fighter"`.
    // Matching by exact case would silently drop those (and their descendants).
    std::string asciiLower(std::string s)
    {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    // The model field varies by object type. Land first (most objects, and the
    // form the user sizes effects against on the ground), then Space, then the
    // strategic-layer Galactic, then the generic Model_Name. First present wins;
    // the rare object carrying more than one only changes which mesh we show.
    std::string firstModel(const XMLNode* e)
    {
        static const wchar_t* kModelTags[] = {
            L"Land_Model_Name", L"Space_Model_Name", L"Galactic_Model_Name", L"Model_Name"
        };
        for (const wchar_t* tag : kModelTags)
        {
            std::string m = trim(WideToAnsi(childData(e, tag)));
            if (!m.empty()) return m;
        }
        return std::string();
    }

    // Category is primarily the container tag. The one exception is turrets:
    // vanilla declares them as <GroundStructure Name="..._Turret">, so the
    // "turret-ness" lives in the Name -- we escalate to Turret when either the
    // name or the tag says turret, then fall back to tag-based buckets. The
    // tag-based rules are ORDER-SENSITIVE substring tests (e.g. "Starbase" hits
    // the Space rule before the "base"->Structure rule); the leaf test pins the
    // collisions present in the real corpus so a reorder fails CI.
    GameObjectCategory categorize(const std::string& name, const std::string& tag)
    {
        const std::string n = asciiLower(name);
        const std::string t = asciiLower(tag);
        auto nameOrTag = [&](const char* s) { return n.find(s) != std::string::npos || t.find(s) != std::string::npos; };
        auto tagHas    = [&](const char* s) { return t.find(s) != std::string::npos; };

        // EXCLUSION FIRST: model-bearing objects that are NOT units/structures.
        // Checked BEFORE the keyword rules so a "...marker"/"...dummy" tag that also
        // contains "structure" (e.g. multiplayerstructuremarker, w_dummystructure)
        // doesn't slip into Structure. The model-LESS noise (events, abilities, trade
        // routes, campaigns, ...) is already dropped by the no-model skip in the build,
        // so only the model-bearing non-units need naming here: planets, markers/spawn
        // zones, dummy structures, death clones, and particle props. (Planets + death
        // clones are user-chosen exclusions; capturable structures are deliberately NOT
        // here -- they fall through to Other and are shown.)
        if (tagHas("planet") || tagHas("marker") || tagHas("dummy") ||
            tagHas("death_clone") || tagHas("particle"))
            return GameObjectCategory::Excluded;

        // Units + structures. The keyword set is generous on purpose -- the filter
        // fails toward SHOWING, so an unrecognised unit tag falls through to Other
        // (which IS listed) rather than vanishing.
        if (nameOrTag("turret"))                                         return GameObjectCategory::Turret;   // name or tag
        if (tagHas("infantry") || tagHas("trooper"))                     return GameObjectCategory::Infantry;
        if (tagHas("prop"))                                              return GameObjectCategory::Prop;     // SpaceProp / Props_* / Prop_* -> hidden
        if (tagHas("projectile"))                                        return GameObjectCategory::Projectile; // hidden
        if (tagHas("hero") || tagHas("unique") || tagHas("commander"))   return GameObjectCategory::Hero;     // herounit / uniqueunit / genericcommander
        if (tagHas("vehicle") || tagHas("transport"))                    return GameObjectCategory::Vehicle;
        if (tagHas("flagship") || tagHas("starbase") ||
            tagHas("squadron") || tagHas("space"))                       return GameObjectCategory::Space;    // flagship* / factional_starbase / spaceunit / squadron
        if (tagHas("structure") || tagHas("building") || tagHas("base")) return GameObjectCategory::Structure; // groundstructure / specialstructure / groundbase
        return GameObjectCategory::Other;   // unrecognised unit/structure tags (groundcompany, groundbuildable, groundwar*, indigenous_unit, capturables) -> SHOWN
    }

    // One parsed object before Variant_Of resolution. Keyed in `byName` by the
    // lower-cased Name; `name` keeps the original casing for the picker label.
    struct RawEntry
    {
        std::string name;        // original-cased Name= (display label)
        std::string variantOf;   // Variant_Of_Existing_Type (trimmed), or empty
        std::string ownModel;    // this entry's own model, or empty (then inherit via variantOf)
        std::string tag;         // container element name
        std::string sourceFile;  // listed XML it came from
    };

    // Read GameObjectFiles.xml -> the ordered list of object-file names.
    bool readFileList(IFileManager& fm, std::vector<std::string>& files)
    {
        IFile* f = fm.getFile("Data\\XML\\GameObjectFiles.xml");
        if (f == nullptr) return false;

        XMLTree xml;
        try { xml.parse(f); }
        catch (...) { f->Release(); return false; }
        f->Release();

        const XMLNode* root = xml.getRoot();
        if (root == nullptr) return false;

        for (unsigned i = 0; i < root->getNumChildren(); ++i)
        {
            const XMLNode* c = root->getChild(i);
            if (c->getName() != L"File") continue;          // tolerate comments / other elements
            std::string fn = trim(WideToAnsi(c->getData()));
            if (!fn.empty()) files.push_back(fn);
        }
        return true;
    }

    // Phase-1 READ (SERIAL): slurp one listed file's raw bytes via the
    // FileManager into `bytes`. This and readFileList are the ONLY catalog-build
    // accesses to `fm` -- the parallel parse below never touches it, so the MEG
    // handle's shared seek pointer is never raced (files.cpp:89). Missing file ->
    // empty (skipped downstream). A MEG-packed file's SubFile clamps the read to
    // its own extent, so reading size() bytes from offset 0 yields exactly it.
    void readObjectFileBytes(IFileManager& fm, const std::string& fileName,
                             std::vector<char>& bytes)
    {
        bytes.clear();
        IFile* f = fm.getFile(std::string("Data\\XML\\") + fileName);
        if (f == nullptr) return;
        const unsigned long sz = f->size();
        if (sz > 0)
        {
            bytes.resize(sz);
            f->seek(0);
            const unsigned long got = f->read(bytes.data(), sz);
            if (got != sz) bytes.resize(got);   // short read (shouldn't happen) -> trim
        }
        f->Release();
    }

    // Phase-2 PARSE (PARALLEL-SAFE): parse one file's already-read bytes
    // into `entries` in document order. Uses a private MemoryFile + a private
    // XMLTree (XML_ParserCreate per call, no global state -- xml.cpp:125), so
    // concurrent calls on distinct buffers share NO mutable state. Crucially it
    // does NOT dedup: first-wins is applied once, serially, by the ordered merge
    // (so within-file first-wins is preserved by document order + insert-if-absent).
    //
    // STRICT never-throw contract: this runs on a parse worker, where an escaping
    // exception std::terminates (unlike the old sequential build whose throws
    // unwound to StartCatalogBuildIfNeeded's catch(...) -> empty catalog). So ANY
    // failure (malformed XML, or a bad_alloc from new / MemoryFile::write /
    // WideToAnsi / push_back) degrades to "this file contributes nothing" -- the
    // same observable result as the old malformed-file catch-and-skip, never a crash.
    //
    // Footgun (xmlparse.c:694): expat seeds the CRT PRNG via srand()/rand() on the
    // FIRST XML_Parse per thread. MSVC's rand/srand are per-thread, so dedicated +
    // joined parse workers don't race or perturb the engine's rand stream. Do NOT
    // move parsing onto a reused/engine thread (it would disturb that thread's rand).
    void parseObjectBytes(const std::vector<char>& bytes, const std::string& fileName,
                          std::vector<RawEntry>& entries)
    {
        if (bytes.empty()) return;

        MemoryFile* mem = nullptr;
        try
        {
            // RefCounted has a protected dtor + starts at refcount 1 -> heap-allocate
            // and Release() (delete at 0), mirroring MockFM::getFile. XMLTree copies
            // the data into its own nodes, so the MemoryFile can go right after parse.
            mem = new MemoryFile();
            mem->write(bytes.data(), (unsigned long)bytes.size());
            mem->seek(0);
            XMLTree xml;
            xml.parse(mem);          // throws on malformed XML
            mem->Release();
            mem = nullptr;

            const XMLNode* root = xml.getRoot();
            if (root == nullptr) return;

            for (unsigned i = 0; i < root->getNumChildren(); ++i)
            {
                const XMLNode* e = root->getChild(i);
                std::string name = WideToAnsi(e->getAttribute(L"Name"));
                if (name.empty()) continue;             // comment / anonymous / <File> include -> not an object

                RawEntry re;
                re.name       = name;
                re.tag        = WideToAnsi(e->getName());
                re.sourceFile = fileName;
                re.variantOf  = trim(WideToAnsi(childData(e, L"Variant_Of_Existing_Type")));
                re.ownModel   = firstModel(e);
                entries.push_back(re);
            }
        }
        catch (...)
        {
            if (mem) mem->Release();   // throw before the explicit Release -> no leak
            entries.clear();           // partial list from a mid-loop throw -> file contributes nothing
        }
    }

    // Worker count for the parallel parse. Auto = clamp(hardware_concurrency,
    // 1, 8); the CATALOG_PARSE_THREADS env var overrides (1 = force the serial path,
    // used for A/B timing; N = cap the pool). Never exceeds the file count (no idle
    // threads) and never returns 0. GetEnvironmentVariableA (not getenv) so it builds
    // clean under the engine's /WX (getenv -> C4996). <2 files -> serial (no spawn).
    unsigned parseThreadCount(size_t fileCount)
    {
        if (fileCount < 2) return 1;

        unsigned cap;
        char envbuf[16] = { 0 };
        const DWORD got = GetEnvironmentVariableA("CATALOG_PARSE_THREADS", envbuf, sizeof(envbuf));
        if (got > 0 && got < sizeof(envbuf))
        {
            long v = std::atol(envbuf);
            if (v < 1)  v = 1;
            if (v > 64) v = 64;
            cap = (unsigned)v;
        }
        else
        {
            unsigned hw = std::thread::hardware_concurrency();
            if (hw < 1) hw = 1;
            cap = (hw < 8) ? hw : 8;
        }
        if ((size_t)cap > fileCount) cap = (unsigned)fileCount;
        return cap;
    }

    // Phase 2: resolve one object's model. Own model wins; otherwise walk the
    // Variant_Of chain to the first ancestor with a model. Returns empty if the
    // chain dead-ends (no model), references a missing parent, or is cyclic.
    // Keys are folded to lower case so a variant that references its parent with
    // different casing (common in vanilla) still resolves -- matching the engine.
    std::string resolveModel(const std::string& startKey,
                             const std::map<std::string, RawEntry>& byName)
    {
        std::set<std::string> visited;
        std::string cur = startKey;                    // already a folded key
        while (!cur.empty())
        {
            auto it = byName.find(cur);
            if (it == byName.end())            return std::string();  // missing parent
            if (!it->second.ownModel.empty())  return it->second.ownModel;
            if (it->second.variantOf.empty())  return std::string();  // no model, not a variant
            if (!visited.insert(cur).second)   return std::string();  // cycle
            cur = asciiLower(it->second.variantOf);    // next key, folded
        }
        return std::string();
    }
}

// Parses the object XMLs across threads (each file is independent), keeping
// READ serial (only one thread touches the FileManager / MEG handle, files.cpp:89)
// and the first-wins MERGE + Variant_Of RESOLUTION serial. The four phases:
//   1. read  (serial)   -- slurp every listed file's bytes via fm
//   2. parse (parallel) -- each blob -> its own RawEntry list (document order)
//   3. merge (serial)   -- ordered first-wins into byName (file order, then doc order)
//   4. resolve (serial) -- Variant_Of chain + categorize + sort
// The output is independent of the parse thread schedule, so it is byte-identical
// to the old sequential build (the unit test's first-wins/dedup asserts pin this).
bool BuildGameObjectCatalog(IFileManager& fm, GameObjectCatalog& out)
{
    out.objects.clear();

    std::vector<std::string> files;
    if (!readFileList(fm, files)) return false;          // GameObjectFiles.xml unreadable

    // De-dup the listed files, preserving first-listed order (order drives cross-file
    // first-wins; a doubly-listed file is read+parsed once).
    std::vector<std::string> uniqueFiles;
    {
        std::set<std::string> seenFiles;
        for (const std::string& fn : files)
            if (seenFiles.insert(fn).second) uniqueFiles.push_back(fn);
    }
    const size_t n = uniqueFiles.size();

    //-timing] Diagnostic per-phase timing to stderr, gated by the
    // CATALOG_TIMING env var (off in normal runs). Tag: [catalog-timing].
    char tmbuf[8] = { 0 };
    const bool timing = GetEnvironmentVariableA("CATALOG_TIMING", tmbuf, sizeof(tmbuf)) > 0;
    using clk = std::chrono::steady_clock;
    auto ms = [](clk::time_point a, clk::time_point b)
    { return std::chrono::duration<double, std::milli>(b - a).count(); };
    const auto tReadStart = clk::now();

    // Phase 1 -- READ (serial): the only fm access in the parse stage.
    std::vector<std::vector<char>> blobs(n);
    for (size_t i = 0; i < n; ++i)
        readObjectFileBytes(fm, uniqueFiles[i], blobs[i]);
    const auto tParseStart = clk::now();

    // Phase 2 -- PARSE (parallel): perFile[i] written by exactly one worker
    // (disjoint indices) -> no lock needed on the result vectors.
    std::vector<std::vector<RawEntry>> perFile(n);
    const unsigned threads = parseThreadCount(n);
    if (threads <= 1)
    {
        for (size_t i = 0; i < n; ++i)
            parseObjectBytes(blobs[i], uniqueFiles[i], perFile[i]);
    }
    else
    {
        std::atomic<size_t> next{ 0 };
        // The worker body is wrapped so NO exception crosses the thread boundary
        // (a throw out of a std::thread entry -> std::terminate). parseObjectBytes
        // is already internally never-throw; this is the definitive boundary guard.
        auto worker = [&]()
        {
            try
            {
                for (size_t i = next.fetch_add(1); i < n; i = next.fetch_add(1))
                    parseObjectBytes(blobs[i], uniqueFiles[i], perFile[i]);
            }
            catch (...) {}
        };
        std::vector<std::thread> pool;
        pool.reserve(threads);                            // no vector realloc mid-spawn
        // If the OS refuses a thread mid-spawn, the std::thread ctor throws. Catch it:
        // any worker that DID start drains every index via the shared atomic, so we
        // just join what launched. If NONE launched, fall back to a serial parse.
        try
        {
            for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker);
        }
        catch (...) {}
        for (std::thread& th : pool)
            if (th.joinable()) th.join();                 // all joined before phase 3
        if (pool.empty())
            for (size_t i = 0; i < n; ++i)
                parseObjectBytes(blobs[i], uniqueFiles[i], perFile[i]);
    }
    const auto tMergeStart = clk::now();

    // Phase 3 -- MERGE (serial, ordered): first-wins across files in list order,
    // entries in document order. emplace is insert-if-absent -> reproduces the old
    // build's exact first-wins (cross-file AND within-file) regardless of schedule.
    std::map<std::string, RawEntry> byName;
    for (size_t i = 0; i < n; ++i)
        for (const RawEntry& re : perFile[i])
            byName.emplace(asciiLower(re.name), re);

    // Phase 4 -- RESOLVE (serial, unchanged): Variant_Of chain + categorize + sort.
    for (const auto& kv : byName)
    {
        std::string model = resolveModel(kv.first, byName);
        if (model.empty()) continue;                      // no renderable model -> not pickable

        GameObjectRef ref;
        ref.name       = kv.second.name;               // original casing for the picker label
        ref.modelPath  = model;
        ref.tag        = kv.second.tag;
        ref.sourceFile = kv.second.sourceFile;
        ref.category   = categorize(kv.second.name, kv.second.tag);
        out.objects.push_back(ref);
    }

    std::sort(out.objects.begin(), out.objects.end(),
              [](const GameObjectRef& a, const GameObjectRef& b) { return a.name < b.name; });

    if (timing)
    {
        const auto tEnd = clk::now();
        std::fprintf(stderr,
            "[catalog-timing] files=%zu threads=%u  read=%.1f  parse=%.1f  merge+resolve=%.1f  total=%.1f ms\n",
            n, threads, ms(tReadStart, tParseStart), ms(tParseStart, tMergeStart),
            ms(tMergeStart, tEnd), ms(tReadStart, tEnd));
        std::fflush(stderr);
    }
    return true;
}

ModelProbeResult ProbeModelSkinned(IFileManager& fm, const std::string& modelPath)
{
    if (modelPath.empty()) return ModelProbeResult::LoadFailed;

    IFile* f = fm.getFile(std::string("Data\\Art\\Models\\") + modelPath);
    if (f == nullptr) return ModelProbeResult::NotFound;   // file genuinely absent

    AloModel model;
    bool ok = false;
    try { model = LoadAloModel(f); ok = true; }   // AddRef/Releases internally
    catch (const wexception&) { ok = false; }     // malformed / truncated / non-mesh
    f->Release();                                  // release our getFile ref
    if (!ok) return ModelProbeResult::LoadFailed;

    // Renderable iff at least one sub-mesh would actually be drawn -- kept in
    // lockstep with ReferenceObjectMesh::Load: skip 0x402-hidden meshes, then
    // shadow/occluded/heat shaders + skinned formats; an opaque (incl. collision)
    // or transparent sub-mesh means the renderer will show geometry.
    for (const AloMesh& mesh : model.meshes)
    {
        if (mesh.hidden) continue;                                    // 0x402-hidden mesh -> not drawn
        for (const AloSubMesh& sm : mesh.subMeshes)
        {
            if (sm.rawVertexBytes.empty() || sm.vertexCount == 0 || sm.primitiveCount == 0) continue;
            if (AloIsNonVisibleShader(sm.shaderName))         continue;   // shadow / occluded / heat
            // RSkin (1-bone) now renders in bind pose -> counts as renderable;
            // only B4I4 (4-bone) skinning is still deferred.
            if (AloIsSkinnedVertexFormat(sm.vertexFormatName) &&
                sm.vertexFormatName.rfind("alD3dVertRSkin", 0) != 0) continue;
            return ModelProbeResult::Renderable;
        }
    }
    return ModelProbeResult::SkinnedUnsupported;
}

const char* GameObjectCategoryName(GameObjectCategory c)
{
    switch (c)
    {
        case GameObjectCategory::Vehicle:    return "Vehicle";
        case GameObjectCategory::Infantry:   return "Infantry";
        case GameObjectCategory::Structure:  return "Structure";
        case GameObjectCategory::Turret:     return "Turret";
        case GameObjectCategory::Hero:       return "Hero";
        case GameObjectCategory::Prop:       return "Prop";
        case GameObjectCategory::Space:      return "Space";
        case GameObjectCategory::Projectile: return "Projectile";
        case GameObjectCategory::Excluded:   return "Excluded";
        default:                             return "Other";
    }
}

// Exclusion-based: list everything EXCEPT Prop, Projectile, and Excluded
// (model-bearing non-units). `Other` is LISTED -- it holds unrecognised unit/structure
// tags, which must not vanish (fail toward showing).
bool IsPickerListedCategory(GameObjectCategory c)
{
    switch (c)
    {
        case GameObjectCategory::Vehicle:
        case GameObjectCategory::Infantry:
        case GameObjectCategory::Structure:
        case GameObjectCategory::Turret:
        case GameObjectCategory::Hero:
        case GameObjectCategory::Space:
        case GameObjectCategory::Other:       // unrecognised units -> shown
            return true;
        case GameObjectCategory::Prop:
        case GameObjectCategory::Projectile:
        case GameObjectCategory::Excluded:
        default:
            return false;
    }
}
