#include "GameObjectCatalog.h"

#include "managers.h"     // IFileManager
#include "xml.h"          // XMLTree / XMLNode
#include "files.h"        // IFile
#include "utils.h"        // WideToAnsi
#include "AloModel.h"     // LoadAloModel + AloIsSkinnedVertexFormat / AloIsNonVisibleShader
#include "exceptions.h"   // wexception (LoadAloModel boundary)
#include "AssetPathSafety.h"
#include "ResourceLimits.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>     // also: #ifndef NDEBUG diagnostics for silently-dropped (unparseable) files
#include <cstdlib>
#include <cmath>
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

    // All child elements named `tag`, data joined by single spaces (an object
    // may carry several <Behavior> children; we want every token). ANSI, empty if none.
    std::string childDataAll(const XMLNode* node, const wchar_t* tag)
    {
        std::wstring acc;
        for (unsigned i = 0; i < node->getNumChildren(); ++i)
        {
            const XMLNode* c = node->getChild(i);
            if (c->getName() == tag) { if (!acc.empty()) acc += L' '; acc += c->getData(); }
        }
        return WideToAnsi(acc);
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

    // Split on commas / whitespace / '|' and UPPER-case each token. CategoryMask
    // uses '|' separators, behavior lists use commas or separate elements; the classifier
    // matches upper-cased type tokens. Empty input -> empty.
    std::vector<std::string> tokenizeUpper(const std::string& s)
    {
        std::vector<std::string> out;
        std::string cur;
        for (char ch : s)
        {
            if (ch == ',' || ch == '|' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
            { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
            else cur += (char)std::toupper((unsigned char)ch);
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

    // The model field varies by object type. Land first (most objects, and the
    // form the user sizes effects against on the ground), then Space, then the
    // strategic-layer Galactic, then the generic Model_Name. First present wins.
    // also reports WHICH field won (the domain signal) and whether BOTH a
    // Land and a Space model are present (dual-env -> the classifier flags a conflict).
    std::string firstModel(const XMLNode* e, ModelFieldKind& kind, bool& dualEnv)
    {
        const std::string land  = trim(WideToAnsi(childData(e, L"Land_Model_Name")));
        const std::string space = trim(WideToAnsi(childData(e, L"Space_Model_Name")));
        dualEnv = !land.empty() && !space.empty();
        if (!land.empty())  { kind = ModelFieldKind::Land;  return land;  }
        if (!space.empty()) { kind = ModelFieldKind::Space; return space; }
        const std::string gal = trim(WideToAnsi(childData(e, L"Galactic_Model_Name")));
        if (!gal.empty())   { kind = ModelFieldKind::Galactic; return gal; }
        const std::string gen = trim(WideToAnsi(childData(e, L"Model_Name")));
        if (!gen.empty())   { kind = ModelFieldKind::Generic; return gen; }
        kind = ModelFieldKind::None;
        return std::string();
    }

    // One parsed object before Variant_Of resolution. Keyed in `byName` by the
    // lower-cased Name; `name` keeps the original casing for the picker label.
    struct RawEntry
    {
        std::string name;            // original-cased Name= (display label)
        std::string variantOf;       // Variant_Of_Existing_Type (trimmed), or empty
        std::string ownModel;        // this entry's own model, or empty (then inherit via variantOf)
        std::string tag;             // container element name
        std::string sourceFile;      // listed XML it came from
        std::string hardpointsList;  // raw <HardPoints> comma list (empty = inherit via variantOf)
        // profile signals (own value; resolved own-else-parent up the Variant_Of chain).
        ModelFieldKind ownModelField = ModelFieldKind::None;  // which field ownModel came from
        bool           ownDualEnv    = false;                 // both Land & Space model on THIS entry
        std::string    maskRaw;        // <CategoryMask>
        std::string    behaviorRaw;    // <Behavior> + <LandBehavior> + <SpaceBehavior> (joined)
        std::string    movementRaw;    // <MovementClass>
        std::string    affiliationRaw; // <Affiliation>
        std::string    scaleFactorRaw; // <Scale_Factor> raw string (parsed+guarded in phase 4)
        // fieldable reference-graph SOURCE lists (this object references others).
        std::string    costRaw;          // <Build_Cost_Credits> (fallback <Required_Star_Base_Level>) -> buildable
        std::string    buildMenuRaw;     // <Tactical_Buildable_Objects_Campaign/_Multiplayer> -> names it builds
        std::string    spawnRaw;         // <{Starting,Reserve}_Spawned_Units_Tech_0..5> -> squadrons it spawns
        std::string    squadronUnitsRaw; // <Squadron_Units> + <Company_Units> -> member units (model-bearing)
    };

    // Split a comma-separated list into trimmed, non-empty tokens (the <HardPoints>
    // value, e.g. "HP_A, HP_B , HP_C").
    std::vector<std::string> splitCsv(const std::string& s)
    {
        std::vector<std::string> out;
        size_t start = 0;
        for (;;)
        {
            const size_t comma = s.find(',', start);
            const size_t end   = (comma == std::string::npos) ? s.size() : comma;
            std::string token = trim(s.substr(start, end - start));
            if (!token.empty()) out.push_back(token);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return out;
    }

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

        // De-dup as we collect so the file-count cap bounds DISTINCT names: a
        // flood of duplicate <File> entries must not push a later legitimate file
        // past kMaxCatalogXmlFileCount (release-audit follow-up). Downstream dedup
        // stays as a belt-and-suspenders no-op on this already-unique list.
        std::set<std::string> seen;
        for (unsigned i = 0; i < root->getNumChildren(); ++i)
        {
            const XMLNode* c = root->getChild(i);
            if (c->getName() != L"File") continue;          // tolerate comments / other elements
            std::string fn = trim(WideToAnsi(c->getData()));
            if (!IsSafeRelativeAssetName(fn)) continue;
            if (fn.empty()) continue;
            if (!seen.insert(fn).second) continue;          // skip duplicates
            if (files.size() >= kMaxCatalogXmlFileCount) break;
            files.push_back(fn);
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
                             std::vector<char>& bytes,
                             unsigned long long& catalogXmlBytes)
    {
        bytes.clear();
        if (!IsSafeRelativeAssetName(fileName)) return;
        IFile* f = fm.getFile(std::string("Data\\XML\\") + fileName);
        if (f == nullptr) return;
        const unsigned long sz = f->size();
        if (sz > kMaxXmlFileBytes)
        {
            f->Release();
            return;
        }
        if ((unsigned long long)sz > (unsigned long long)kMaxCatalogXmlTotalBytes - catalogXmlBytes)
        {
            f->Release();
            return;
        }
        catalogXmlBytes += sz;
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
                re.name          = name;
                re.tag           = WideToAnsi(e->getName());
                re.sourceFile    = fileName;
                re.variantOf     = trim(WideToAnsi(childData(e, L"Variant_Of_Existing_Type")));
                re.ownModel      = firstModel(e, re.ownModelField, re.ownDualEnv);
                re.hardpointsList = trim(WideToAnsi(childData(e, L"HardPoints")));   // resolved in phase 4
                // profile signals (resolved through Variant_Of in phase 4).
                re.maskRaw        = trim(WideToAnsi(childData(e, L"CategoryMask")));
                re.behaviorRaw    = trim(childDataAll(e, L"Behavior") + " " +
                                         childDataAll(e, L"LandBehavior") + " " +
                                         childDataAll(e, L"SpaceBehavior"));
                re.movementRaw    = trim(WideToAnsi(childData(e, L"MovementClass")));
                re.affiliationRaw = trim(WideToAnsi(childData(e, L"Affiliation")));
                re.scaleFactorRaw = trim(WideToAnsi(childData(e, L"Scale_Factor")));
                // fieldable source lists.
                re.costRaw        = trim(WideToAnsi(childData(e, L"Build_Cost_Credits")));
                if (re.costRaw.empty())
                    re.costRaw    = trim(WideToAnsi(childData(e, L"Required_Star_Base_Level")));
                re.buildMenuRaw   = trim(WideToAnsi(childData(e, L"Tactical_Buildable_Objects_Campaign")) + " " +
                                         WideToAnsi(childData(e, L"Tactical_Buildable_Objects_Multiplayer")));
                {
                    std::string spawn;
                    for (int tier = 0; tier <= 5; ++tier)
                    {
                        const std::wstring sa = L"Starting_Spawned_Units_Tech_" + std::to_wstring(tier);
                        const std::wstring sb = L"Reserve_Spawned_Units_Tech_"  + std::to_wstring(tier);
                        const std::string  a  = WideToAnsi(childData(e, sa.c_str()));
                        const std::string  b  = WideToAnsi(childData(e, sb.c_str()));
                        if (!a.empty()) { spawn += " "; spawn += a; }
                        if (!b.empty()) { spawn += " "; spawn += b; }
                    }
                    re.spawnRaw = trim(spawn);
                }
                re.squadronUnitsRaw = trim(childDataAll(e, L"Squadron_Units") + " " +
                                           childDataAll(e, L"Company_Units"));
                entries.push_back(re);
            }
        }
        catch (...)
        {
            if (mem) mem->Release();   // throw before the explicit Release -> no leak
            entries.clear();           // partial list from a mid-loop throw -> file contributes nothing
            // Surface the otherwise-SILENT drop -- this is what hid the encoding='ASCII'
            // bug (1/3 of one mod's core files threw -> their units + Variant_Of parents vanished with
            // no signal). Unconditional: a Release user's incomplete picker was
            // undiagnosable with this behind NDEBUG (2026-07 audit). A brief stderr
            // interleave from parallel workers is harmless.
            fprintf(stderr, "[Catalog] object file dropped (XML parse failed): %s\n", fileName.c_str());
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
    // also carries the supplying entry's model field-kind + dual-env so the
    // classifier's domain follows the model that actually renders.
    struct ResolvedModel { std::string path; ModelFieldKind kind = ModelFieldKind::None; bool dualEnv = false; };
    ResolvedModel resolveModel(const std::string& startKey,
                               const std::map<std::string, RawEntry>& byName)
    {
        std::set<std::string> visited;
        std::string cur = startKey;                    // already a folded key
        while (!cur.empty())
        {
            auto it = byName.find(cur);
            if (it == byName.end())            return {};  // missing parent
            if (!it->second.ownModel.empty())  return { it->second.ownModel, it->second.ownModelField, it->second.ownDualEnv };
            if (it->second.variantOf.empty())  return {};  // no model, not a variant
            if (!visited.insert(cur).second)   return {};  // cycle
            cur = asciiLower(it->second.variantOf);    // next key, folded
        }
        return {};
    }

    // Resolve a raw string field own-else-parent up the Variant_Of chain (first
    // non-empty wins; cyclic-safe, case-folded), mirroring resolveModel. For CategoryMask /
    // behavior / MovementClass / Is_Dummy / etc. that sit on a base template and are
    // inherited by its variants.
    std::string resolveRaw(const std::string& startKey,
                           const std::map<std::string, RawEntry>& byName,
                           std::string RawEntry::* field)
    {
        std::set<std::string> visited;
        std::string cur = startKey;
        while (!cur.empty())
        {
            auto it = byName.find(cur);
            if (it == byName.end())            return std::string();
            const std::string& v = it->second.*field;
            if (!v.empty())                    return v;
            if (it->second.variantOf.empty())  return std::string();
            if (!visited.insert(cur).second)   return std::string();
            cur = asciiLower(it->second.variantOf);
        }
        return std::string();
    }

    // Consume one fieldable-token work operation BEFORE its lookup/push/map
    // side effect. The comparison precedes the increment, so the exact boundary
    // is accepted and the counter itself can never overflow.
    bool consumeFieldableTokenWork(unsigned long& used)
    {
        if (used >= kMaxCatalogFieldableTokenWork) return false;
        ++used;
        return true;
    }

    // Split a list on commas / whitespace / '|' and LOWER-case each token (object
    // Names are matched case-INSENSITIVELY -- mods reference e.g. `Tie_*` against
    // `TIE_*`). Every emitted token is temporary allocation/work even if a later
    // map operation deduplicates it, so charge before pushing it into `out`.
    bool tokenizeCommaLower(const std::string& s,
                            unsigned long& fieldableTokenWork,
                            std::vector<std::string>& out)
    {
        out.clear();
        std::string cur;
        for (char ch : s)
        {
            if (ch == ',' || ch == '|' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
            {
                if (!cur.empty())
                {
                    if (!consumeFieldableTokenWork(fieldableTokenWork)) return false;
                    out.push_back(cur);
                    cur.clear();
                }
            }
            else cur += (char)std::tolower((unsigned char)ch);
        }
        if (!cur.empty())
        {
            if (!consumeFieldableTokenWork(fieldableTokenWork)) return false;
            out.push_back(cur);
        }
        return true;
    }
    // A spawn-list token like "...Squadron, 1" -- the count is numeric, the name is not.
    bool isNumericToken(const std::string& t)
    {
        if (t.empty()) return false;
        for (char c : t) if (!(c >= '0' && c <= '9') && c != '-' && c != '+') return false;
        return true;
    }

    // Read a mod's GameObjectList.lua roster allow-list (some submods ship one, with
    // a few hundred to ~900 entries) -- `["NAME"] = true,` lines. Lower-cased names into
    // `out`. Missing file -> empty (base FoC and many mods have none; the fieldable union then
    // falls back to the XML graph). Not lua-evaluated: a literal `["` ... `"]` scan.
    void readRosterLua(IFileManager& fm, std::set<std::string>& out)
    {
        IFile* f = fm.getFile("Data\\Scripts\\Library\\GameObjectList.lua");
        if (!f) return;
        std::string text;
        const unsigned long sz = f->size();
        if (sz > kMaxXmlFileBytes)
        {
            f->Release();
            return;
        }
        if (sz > 0)
        {
            text.resize(sz); f->seek(0);
            const unsigned long got = f->read(&text[0], sz);
            text.resize(got);
#ifndef NDEBUG
            if (got < sz)   // short read -> the roster scan sees a truncated allow-list (under-marks fieldable)
                fprintf(stderr, "[Catalog] GameObjectList.lua short read: %lu of %lu bytes\n", got, sz);
#endif
        }
        f->Release();
        size_t i = 0;
        while ((i = text.find("[\"", i)) != std::string::npos)
        {
            // Count cap, not just the byte cap above (2026-07 audit, an-audit-finding). The
            // file-size check bounds the INPUT; it does not bound how many names
            // that input can produce, and at ~6 bytes per `["x"]` a legal-sized
            // file still yields millions of set inserts. Stop scanning rather
            // than merely stop inserting — past the cap the remaining bytes
            // cannot change the answer, and the scan itself is the other half of
            // the cost.
            if (out.size() >= kMaxRosterEntries) break;
            const size_t s = i + 2;
            const size_t e = text.find("\"]", s);
            if (e == std::string::npos) break;
            out.insert(asciiLower(text.substr(s, e - s)));
            i = e + 2;
        }
    }

    // Mark each catalog object `fieldable` = a player can build or spawn it. The
    // UNION of: the GameObjectList.lua roster; objects with a build cost (resolved through
    // Variant_Of -- cost sits on templates); names listed in any structure's build menu;
    // and names in any carrier/structure spawn list -> whose Squadron_Units/Company_Units
    // members (the model-bearing fighters) are then ALSO marked. Case-insensitive
    // throughout (the catalog keys are already folded). A HARD picker keep-gate
    // (IsPickerListed); the --dump-catalog audit lists kept-but-non-fieldable so a real
    // unit can never silently vanish.
    bool markFieldable(IFileManager& fm,
                       const std::map<std::string, RawEntry>& byName,
                       std::vector<GameObjectRef>& objects)
    {
        unsigned long fieldableTokenWork = 0;
        std::map<std::string, unsigned> bits;   // folded name -> FieldSource bits

        std::set<std::string> roster;
        readRosterLua(fm, roster);
        for (const std::string& n : roster)
        {
            if (!consumeFieldableTokenWork(fieldableTokenWork)) return false;
            bits[n] |= FS_Roster;
        }

        std::vector<std::string> tokens;
        for (const auto& kv : byName)
        {
            const std::string& key = kv.first;   // already folded
            if (!resolveRaw(key, byName, &RawEntry::costRaw).empty())
                bits[key] |= FS_Buildable;

            if (!tokenizeCommaLower(resolveRaw(key, byName, &RawEntry::buildMenuRaw),
                                    fieldableTokenWork, tokens))
                return false;
            for (const std::string& t : tokens)
            {
                if (!consumeFieldableTokenWork(fieldableTokenWork)) return false;
                bits[t] |= FS_StructureBuilt;
            }

            if (!tokenizeCommaLower(resolveRaw(key, byName, &RawEntry::spawnRaw),
                                    fieldableTokenWork, tokens))
                return false;
            for (const std::string& t : tokens)
            {
                if (isNumericToken(t)) continue;
                if (!consumeFieldableTokenWork(fieldableTokenWork)) return false;
                bits[t] |= FS_Spawned;
            }
        }

        // Expand wrappers: a fieldable Squadron/Company's members become fieldable too
        // (collect first -- don't mutate `bits` mid-iteration).
        std::vector<std::pair<std::string, unsigned>> expand;
        for (const auto& kv : byName)
        {
            auto it = bits.find(kv.first);
            if (it == bits.end()) continue;
            if (!tokenizeCommaLower(resolveRaw(kv.first, byName, &RawEntry::squadronUnitsRaw),
                                    fieldableTokenWork, tokens))
                return false;
            for (const std::string& m : tokens)
            {
                if (!consumeFieldableTokenWork(fieldableTokenWork)) return false;
                expand.emplace_back(m, it->second);
            }
        }
        for (const auto& e : expand)
        {
            if (!consumeFieldableTokenWork(fieldableTokenWork)) return false;
            bits[e.first] |= e.second;
        }

        for (GameObjectRef& r : objects)
        {
            auto it = bits.find(asciiLower(r.name));
            if (it != bits.end()) { r.fieldable = true; r.fieldSource = it->second; }
        }
        return true;
    }

    // Phase 2 for HardPoints: an object's own <HardPoints> list wins; otherwise
    // INHERIT the first ancestor's list up the Variant_Of chain (replace-or-inherit,
    // NOT merge -- a variant that declares <HardPoints> overrides its parent's entirely;
    // one that declares none uses the parent's). Mirrors resolveModel (cross-file +
    // cyclic-safe, case-folded keys). Empty if the chain dead-ends with no list.
    std::vector<std::string> resolveHardpoints(const std::string& startKey,
                                               const std::map<std::string, RawEntry>& byName)
    {
        std::set<std::string> visited;
        std::string cur = startKey;
        while (!cur.empty())
        {
            auto it = byName.find(cur);
            if (it == byName.end())                  return {};   // missing parent
            if (!it->second.hardpointsList.empty())  return splitCsv(it->second.hardpointsList);
            if (it->second.variantOf.empty())        return {};   // no list, not a variant
            if (!visited.insert(cur).second)         return {};   // cycle
            cur = asciiLower(it->second.variantOf);
        }
        return {};
    }

    // Parse one hardpoint file's <HardPoint Name="..."> children into `out`
    // (first-wins on a duplicate Name, like objects -- so a higher-precedence root's
    // override shadows a lower one through `fm`). Missing / malformed -> no-op.
    void parseHardpointFile(IFileManager& fm, const std::string& fileName,
                            std::map<std::string, HardPointDef>& out)
    {
        if (!IsSafeRelativeAssetName(fileName)) return;
        IFile* f = fm.getFile(std::string("Data\\XML\\") + fileName);
        if (f == nullptr) return;

        XMLTree xml;
        try { xml.parse(f); }
        catch (...)
        {
#ifndef NDEBUG
            fprintf(stderr, "[Catalog] hardpoint file dropped (XML parse failed): %s\n", fileName.c_str());  // see parseObjectFile
#endif
            f->Release(); return;
        }
        f->Release();

        const XMLNode* root = xml.getRoot();
        if (root == nullptr) return;

        for (unsigned i = 0; i < root->getNumChildren(); ++i)
        {
            const XMLNode* e = root->getChild(i);
            if (e->getName() != L"HardPoint") continue;          // tolerate comments / other
            std::string name = trim(WideToAnsi(e->getAttribute(L"Name")));
            if (name.empty()) continue;
            std::string key = asciiLower(name);
            if (out.find(key) != out.end()) continue;            // first-wins (mod precedence via getFile)

            HardPointDef def;
            def.modelToAttach       = trim(WideToAnsi(childData(e, L"Model_To_Attach")));
            if (!IsSafeRelativeAssetName(def.modelToAttach)) def.modelToAttach.clear();
            def.attachmentBone      = trim(WideToAnsi(childData(e, L"Attachment_Bone")));
            def.damageDecalBone     = trim(WideToAnsi(childData(e, L"Damage_Decal")));
            def.damageParticlesBone = trim(WideToAnsi(childData(e, L"Damage_Particles")));
            def.collisionMeshBone   = trim(WideToAnsi(childData(e, L"Collision_Mesh")));
            out[key] = def;
        }
    }

    // Read HardPointDataFiles.xml (<Hard_Point_Files> -> <File> list; entries may
    // carry a subdir prefix like "HardPoints\\Foo.xml") and parse each listed file into
    // the table. getFile resolves the manifest + each file through `fm`'s mod->base order
    // (so a submod's hardpoint files take precedence). Missing manifest -> empty table
    // (no attachments; graceful).
    void readHardpointTable(IFileManager& fm, std::map<std::string, HardPointDef>& out)
    {
        // The game REPLACES per file by precedence (never merges), so the single
        // highest-precedence HardPointDataFiles.xml wins -- exactly what getFile
        // returns. (This is correct only because the content-root order is faithful:
        // the active submod's index outranks the mod root's stale one -- see
        // FileManager::BuildModContentRoots. With the old mod-root-first order this
        // resolved to a stale index and the table came out empty.)
        IFile* f = fm.getFile("Data\\XML\\HardPointDataFiles.xml");
        if (f == nullptr) return;

        XMLTree xml;
        try { xml.parse(f); }
        catch (...) { f->Release(); return; }
        f->Release();

        const XMLNode* root = xml.getRoot();
        if (root == nullptr) return;

        std::vector<std::string> files;
        std::set<std::string> seen;
        for (unsigned i = 0; i < root->getNumChildren(); ++i)
        {
            const XMLNode* c = root->getChild(i);
            if (c->getName() != L"File") continue;
            std::string fn = trim(WideToAnsi(c->getData()));
            if (!IsSafeRelativeAssetName(fn)) continue;
            if (!fn.empty() && seen.insert(fn).second) files.push_back(fn);
        }
        for (const std::string& fn : files)
            parseHardpointFile(fm, fn, out);
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
    out.hardpoints.clear();

    std::vector<std::string> files;
    if (!readFileList(fm, files)) return false;          // GameObjectFiles.xml unreadable

    // De-dup the listed files, preserving first-listed order (order drives cross-file
    // first-wins; a doubly-listed file is read+parsed once).
    std::vector<std::string> uniqueFiles;
    {
        std::set<std::string> seenFiles;
        for (const std::string& fn : files)
        {
            if (seenFiles.insert(fn).second)
            {
                if (uniqueFiles.size() >= kMaxCatalogXmlFileCount) break;
                uniqueFiles.push_back(fn);
            }
        }
    }
    const size_t n = uniqueFiles.size();

    // Diagnostic per-phase timing to stderr, gated by the
    // CATALOG_TIMING env var (off in normal runs). Tag: [catalog-timing].
    char tmbuf[8] = { 0 };
    const bool timing = GetEnvironmentVariableA("CATALOG_TIMING", tmbuf, sizeof(tmbuf)) > 0;
    using clk = std::chrono::steady_clock;
    auto ms = [](clk::time_point a, clk::time_point b)
    { return std::chrono::duration<double, std::milli>(b - a).count(); };
    const auto tReadStart = clk::now();

    // Phase 1 -- READ (serial): the only fm access in the parse stage.
    std::vector<std::vector<char>> blobs(n);
    unsigned long long catalogXmlBytes = 0;
    for (size_t i = 0; i < n; ++i)
        readObjectFileBytes(fm, uniqueFiles[i], blobs[i], catalogXmlBytes);
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

    // Parse the hardpoint table once (serial -- it touches fm, which the parallel
    // parse must NOT; the per-unit name lists below index into it at select time).
    readHardpointTable(fm, out.hardpoints);

    // Phase 4 -- RESOLVE (serial, unchanged): Variant_Of chain + categorize + sort.
    for (const auto& kv : byName)
    {
        ResolvedModel rm = resolveModel(kv.first, byName);
        if (rm.path.empty()) continue;                    // no renderable model -> not pickable
        if (!IsSafeRelativeAssetName(rm.path)) continue;  // unsafe model path -> missing

        GameObjectRef ref;
        ref.name           = kv.second.name;           // original casing for the picker label
        ref.modelPath      = rm.path;
        ref.tag            = kv.second.tag;
        ref.sourceFile     = kv.second.sourceFile;
        ref.hardpointNames = resolveHardpoints(kv.first, byName);   // Variant_Of-resolved

        // assemble the profile (signals resolved through Variant_Of) + classify.
        ObjectProfile prof;
        prof.name             = kv.second.name;
        prof.tag              = kv.second.tag;
        prof.modelField       = rm.kind;
        prof.dualEnv          = rm.dualEnv;
        prof.maskTokens       = tokenizeUpper(resolveRaw(kv.first, byName, &RawEntry::maskRaw));
        prof.behaviorTokens   = tokenizeUpper(resolveRaw(kv.first, byName, &RawEntry::behaviorRaw));
        prof.hasMovementClass = !resolveRaw(kv.first, byName, &RawEntry::movementRaw).empty();
        prof.affiliation      = resolveRaw(kv.first, byName, &RawEntry::affiliationRaw);
        const Classification cl = ClassifyObject(prof);
        ref.domain      = cl.domain;
        ref.role        = cl.role;
        ref.bucket      = cl.bucket;
        ref.conflict    = cl.conflict;
        ref.affiliation = prof.affiliation;   // surfaced as the picker's faction filter
        // <Scale_Factor> render multiplier: inherit up the Variant_Of chain
        // (first-non-empty), then guard-parse -- reject empty / non-finite / non-positive
        // (a 0 or negative scale yields a singular world matrix -> un-pickable object;
        // mirrors the skydome guard SkydomeEnvironment.cpp). A malformed child stops the
        // inheritance walk at itself (first-non-empty) and falls back to 1.0 (does NOT
        // inherit the parent) -- intended.
        {
            const std::string sfRaw = resolveRaw(kv.first, byName, &RawEntry::scaleFactorRaw);
            float sf = 1.0f;
            if (!sfRaw.empty())
            {
                const float v = std::strtof(sfRaw.c_str(), nullptr);
                if (std::isfinite(v) && v > 0.0f) sf = v;
            }
            ref.scaleFactor = sf;
        }
        // ref.fieldable / fieldSource filled by the fieldable pass below.
        out.objects.push_back(ref);
    }

    std::sort(out.objects.begin(), out.objects.end(),
              [](const GameObjectRef& a, const GameObjectRef& b) { return a.name < b.name; });

    // Fieldable reference-graph + lua roster pass (after objects exist; reads `fm`
    // for GameObjectList.lua). Marks the player-fieldable units/structures for the picker.
    if (!markFieldable(fm, byName, out.objects))
    {
        // The cap is a refusal contract, not a truncation contract: no partially
        // classified objects or hardpoint table may escape as a usable catalog.
        out.objects.clear();
        out.hardpoints.clear();
        return false;
    }

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
    if (!IsSafeRelativeAssetName(modelPath)) return ModelProbeResult::LoadFailed;

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


// ===================== profile classifier =========================
// Pure cascade over an ObjectProfile: EXCLUDE (renderable non-units) -> DOMAIN (from the
// model field-name, corroborated by CategoryMask) -> ROLE + BUCKET. No I/O, so it is
// unit-tested directly (tests/test_game_object_catalog.cpp [classify]). The signal
// vocabulary was established by a 5-mod first-party sweep (base FoC + two major mods);
// CategoryMask is a REFINEMENT only (absent on every unit in base FoC and in one of those
// mods), never a gate -- domain/keep come from the model field + tag + behavior, which
// exist everywhere.
namespace
{
    // A renderable object that is NOT a selectable unit/structure -> Excluded. Tag-DENY
    // (never tag-allow: an unrecognised real unit must fall through to kept, not vanish)
    // + a few behavior tokens. Substring match on the lower-cased tag.
    bool isExcludedTag(const std::string& tl)
    {
        static const char* kDeny[] = {
            "skydome", "loworbit", "planet", "particle", "projectile", "marker",
            "death_clone", "container", "upgrade", "ability", "template", "defunct",
            "mission_object", "indigenous_passive", "squadron", "company", "formation",
            "dummy", "prop", "obstacle", "cinematic", "mov_", "cin_"
        };
        for (const char* d : kDeny) if (tl.find(d) != std::string::npos) return true;
        return false;
    }
    // Some junk carries a unit/structure TAG but is recognizable only by NAME: destruction
    // clones, captured AI variants, cinematic dialogue actors, debug + abstract templates.
    // Conservative substrings (avoid "clone" -- it would hit Clone_Trooper; "mission" -- it
    // would hit Mission_Vao). Crucial because heroes are exempt from the fieldable gate, so a
    // hero death-clone must be Excluded here or it would surface.
    bool isExcludedName(const std::string& nl)
    {
        static const char* kDeny[] = { "death_clone", "_captured", "dialogue_", "debug", "abstract_" };
        for (const char* d : kDeny) if (nl.find(d) != std::string::npos) return true;
        return false;
    }
    // Behavior tokens that mark a non-unit. Deliberately NARROW -- two classes of behavior
    // that LOOK like non-unit markers are NOT here because real units/structures carry them:
    //   - `DUMMY_<type>` is a normal engine AI-hook (DUMMY_STARSHIP on every Star Destroyer,
    //     DUMMY_GROUND_STRUCTURE on every barracks) -- excluding it dropped ~296 base-FoC units.
    //   - `SPACE_OBSTACLE`/`LAND_OBSTACLE` is collision geometry that real structures + asteroids
    //     carry -- excluding it dropped ~160 base-FoC structures (E_Ground_Barracks, StarBases).
    // The genuine non-units those would have caught are already covered by tag (Upgrade/Company/
    // Squadron/Prop/Obstacle) + the no-model skip. SKY_DOME/PLANET/PARTICLE/PROJECTILE/MARKER are
    // safe (mostly redundant with the tag, but catch a backdrop whose tag isn't obvious).
    bool hasExcludeBehavior(const std::vector<std::string>& b)
    {
        for (const std::string& t : b)
            if (t == "SKY_DOME" || t == "PLANET" || t == "PARTICLE" || t == "PROJECTILE" || t == "MARKER")
                return true;
        return false;
    }

    // Domain implied by the CategoryMask type tokens (Anti* are targeting roles -> skip).
    // TRANSPORT is deliberately ABSENT -- it is domain-ambiguous (ground troop transports
    // and space transports both carry it), so reading it as Space mis-flagged ~344 ground
    // vehicles as conflicts; the model field-name decides their domain instead.
    ObjDomain maskDomain(const std::vector<std::string>& m)
    {
        for (const std::string& t : m)
        {
            if (t.rfind("ANTI", 0) == 0) continue;
            if (t == "FIGHTER" || t == "BOMBER" || t == "GUNSHIP" || t == "CORVETTE" ||
                t == "FRIGATE" || t == "CAPITAL" || t == "SUPERCAPITAL" || t == "SUPER" ||
                t == "SPACESTRUCTURE" || t == "SPACEHERO")
                return ObjDomain::Space;
            if (t == "INFANTRY" || t == "VEHICLE" || t == "AIR" || t == "AIRSPEEDER" ||
                t == "AIRGUNSHIP" || t == "STRUCTURE" || t == "WALL" || t == "LANDHERO" ||
                t == "INFANTRYHERO" || t == "VEHICLEHERO")
                return ObjDomain::Ground;
        }
        return ObjDomain::Unknown;
    }
    // Domain implied by the container TAG -- the fallback when the model field is Galactic or
    // generic (real units, esp. base-FoC capitals, resolve their mesh via Galactic_Model_Name,
    // so the field-name gives no ground/space hint). Conservative substrings.
    ObjDomain domainFromTag(const std::string& tl)
    {
        if (tl.find("ground") != std::string::npos || tl.find("infantry") != std::string::npos ||
            tl.find("vehicle") != std::string::npos)
            return ObjDomain::Ground;
        if (tl.find("space")    != std::string::npos || tl.find("squadron") != std::string::npos ||
            tl.find("starbase") != std::string::npos || tl.find("shipyard") != std::string::npos ||
            tl.find("flagship") != std::string::npos)
            return ObjDomain::Space;
        return ObjDomain::Unknown;
    }
    bool maskHasHero(const std::vector<std::string>& m)
    {
        for (const std::string& t : m)
            if (t == "SPACEHERO" || t == "LANDHERO" || t == "INFANTRYHERO" ||
                t == "VEHICLEHERO" || t == "NONCOMBATHERO") return true;
        return false;
    }
    bool maskHasStructure(const std::vector<std::string>& m)
    {
        for (const std::string& t : m)
            if (t == "STRUCTURE" || t == "WALL" || t == "SPACESTRUCTURE") return true;
        return false;
    }
    bool tagIsHero(const std::string& tl)       // HeroUnit / GenericHeroUnit / GenericCommander
    { return tl.find("hero") != std::string::npos || tl.find("commander") != std::string::npos; }
    bool tagIsStructure(const std::string& tl)
    {
        return tl.find("structure") != std::string::npos || tl.find("starbase") != std::string::npos
            || tl.find("shipyard")   != std::string::npos || tl.find("buildable") != std::string::npos
            || tl.find("base")       != std::string::npos || tl.find("wall")      != std::string::npos
            || tl.find("turret")     != std::string::npos;
    }
    // Fine bucket from a CategoryMask token (domain disambiguates Transport), else None.
    ObjBucket bucketFromMask(const std::vector<std::string>& m, ObjDomain dom)
    {
        for (const std::string& t : m)
        {
            if (t.rfind("ANTI", 0) == 0) continue;
            if (t == "FIGHTER")                                   return ObjBucket::Fighter;
            if (t == "BOMBER" || t == "GUNSHIP")                  return ObjBucket::Bomber;
            if (t == "CORVETTE")                                  return ObjBucket::Corvette;
            if (t == "FRIGATE")                                   return ObjBucket::Frigate;
            if (t == "CAPITAL" || t == "SUPERCAPITAL" || t == "SUPER") return ObjBucket::Capital;
            if (t == "TRANSPORT") return (dom == ObjDomain::Space) ? ObjBucket::Transport : ObjBucket::Vehicle;
            if (t == "INFANTRY")                                  return ObjBucket::Infantry;
            if (t == "VEHICLE")                                   return ObjBucket::Vehicle;
            if (t == "AIR" || t == "AIRSPEEDER" || t == "AIRGUNSHIP") return ObjBucket::Air;
        }
        return ObjBucket::None;
    }
    // Fallback bucket from the container tag when no usable mask (ground tags carry a hint;
    // generic space tags like SpaceUnit/UniqueUnit do not -> None -> "Other").
    ObjBucket bucketFromTag(const std::string& tl)
    {
        if (tl.find("infantry") != std::string::npos || tl.find("trooper") != std::string::npos) return ObjBucket::Infantry;
        if (tl.find("vehicle")  != std::string::npos)                                            return ObjBucket::Vehicle;
        return ObjBucket::None;
    }
}

Classification ClassifyObject(const ObjectProfile& p)
{
    Classification c;
    const std::string tl = asciiLower(p.tag);
    const std::string nl = asciiLower(p.name);

    // 1. EXCLUDE -- no renderable model, or a model-bearing non-unit, recognised by TAG / NAME /
    //    a narrow behavior set. Deliberately does NOT gate on the model field-kind, Is_Dummy, or
    //    In_Background: each LOOKS like a non-unit signal but real units carry it in EaW --
    //    capitals resolve their mesh via Galactic_Model_Name (Star_Destroyer); real buildable
    //    structures set Is_Dummy=Yes (E_Gravity_Well_Station, mining facilities); a story unit can
    //    set In_Background=Yes (Eclipse SSD). Backdrops are caught by their Planet/Skydome/Prop TAG
    //    (fail-toward-showing), and the fieldable gate hides any non-fieldable straggler.
    //    (isDummy/inBackground stay captured on the profile for diagnostics; they do not gate here.)
    if (p.modelField == ModelFieldKind::None ||
        isExcludedTag(tl) || isExcludedName(nl) || hasExcludeBehavior(p.behaviorTokens))
    {
        c.role = ObjRole::Excluded;
        return c;
    }

    // 2. DOMAIN -- model field-name decides for Land/Space; Galactic/Generic fall back to the
    //    tag then the mask. Mask corroborates / flags conflict.
    switch (p.modelField)
    {
        case ModelFieldKind::Land:  c.domain = ObjDomain::Ground; break;
        case ModelFieldKind::Space: c.domain = ObjDomain::Space;  break;
        default:                    c.domain = domainFromTag(tl); break;   // Galactic / Generic
    }
    const ObjDomain md = maskDomain(p.maskTokens);
    if (p.dualEnv) c.conflict = true;
    if (md != ObjDomain::Unknown && c.domain != ObjDomain::Unknown && md != c.domain) c.conflict = true;
    if (c.domain == ObjDomain::Unknown) c.domain = (md != ObjDomain::Unknown) ? md : ObjDomain::Ground;

    // 3. ROLE + BUCKET.
    if (maskHasHero(p.maskTokens) || tagIsHero(tl))
    {
        c.role = ObjRole::Hero; c.bucket = ObjBucket::Hero; return c;
    }
    if (maskHasStructure(p.maskTokens) || (!p.hasMovementClass && tagIsStructure(tl)))
    {
        c.role = ObjRole::Structure; c.bucket = ObjBucket::Structure; return c;
    }
    c.role = ObjRole::Unit;
    ObjBucket b = bucketFromMask(p.maskTokens, c.domain);
    if (b == ObjBucket::None) b = bucketFromTag(tl);
    if (b == ObjBucket::None) { b = ObjBucket::Other; c.conflict = true; }   // unresolved -> shown as Other, never dropped
    c.bucket = b;
    return c;
}

const char* ObjDomainName(ObjDomain d)
{
    switch (d) { case ObjDomain::Ground: return "Ground"; case ObjDomain::Space: return "Space"; default: return "Unknown"; }
}
const char* ObjRoleName(ObjRole r)
{
    switch (r) { case ObjRole::Unit: return "Unit"; case ObjRole::Structure: return "Structure";
                 case ObjRole::Hero: return "Hero"; default: return "Excluded"; }
}
const char* ObjBucketName(ObjBucket b)
{
    switch (b)
    {
        case ObjBucket::Infantry:  return "Infantry";  case ObjBucket::Vehicle:   return "Vehicle";
        case ObjBucket::Air:       return "Air";       case ObjBucket::Fighter:   return "Fighter";
        case ObjBucket::Bomber:    return "Bomber";    case ObjBucket::Corvette:  return "Corvette";
        case ObjBucket::Frigate:   return "Frigate";   case ObjBucket::Capital:   return "Capital";
        case ObjBucket::Transport: return "Transport"; case ObjBucket::Structure: return "Structure";
        case ObjBucket::Hero:      return "Hero";      case ObjBucket::Other:     return "Other";
        default:                   return "None";
    }
}
