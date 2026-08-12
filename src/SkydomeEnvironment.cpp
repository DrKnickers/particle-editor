#include "SkydomeEnvironment.h"

#include "managers.h"     // IFileManager
#include "xml.h"          // XMLTree / XMLNode
#include "files.h"        // IFile, ReadAndReleaseCapped
#include "ResourceLimits.h" // kMaxAloModelBytes, kMaxCatalogXmlFileCount
#include "utils.h"        // WideToAnsi
#include "AssetPathSafety.h"

#include <cwctype>        // towlower
#include <cstring>        // memchr (root-element sniff)
#include <string>
#include <set>            // dedup of dome Names across listed files
#include <vector>

namespace
{
    struct AxisConfig
    {
        const char*    file;        // canonical Data\XML filename (fallback)
        const char*    rootTag;     // container element name (GameObjectFiles match, ASCII)
        const wchar_t* entryTag;    // per-entry element name
        const wchar_t* modelTag;    // preferred model-path element
        const wchar_t* altModelTag; // fallback model-path element (other context)
    };

    AxisConfig configFor(SkydomeAxis axis)
    {
        switch (axis)
        {
            case SkydomeAxis::LandPrimary:    return { "LandPrimarySkydomes.xml",    "LandPrimarySkydomes",    L"LandPrimarySkydome",    L"Land_Model_Name",  L"Space_Model_Name" };
            case SkydomeAxis::LandSecondary:  return { "LandSecondarySkydomes.xml",  "LandSecondarySkydomes",  L"LandSecondarySkydome",  L"Land_Model_Name",  L"Space_Model_Name" };
            case SkydomeAxis::SpacePrimary:   return { "SpacePrimarySkydomes.xml",   "SpacePrimarySkydomes",   L"SpacePrimarySkydome",   L"Space_Model_Name", L"Land_Model_Name" };
            case SkydomeAxis::SpaceSecondary: return { "SpaceSecondarySkydomes.xml", "SpaceSecondarySkydomes", L"SpaceSecondarySkydome", L"Space_Model_Name", L"Land_Model_Name" };
        }
        return { "", "", L"", L"", L"" };
    }

    // First child element named `tag`, or empty. (XMLNode has no by-name lookup.)
    std::wstring childData(const XMLNode* node, const wchar_t* tag)
    {
        for (unsigned i = 0; i < node->getNumChildren(); ++i)
        {
            const XMLNode* c = node->getChild(i);
            if (c->getName() == tag) return c->getData();
        }
        return std::wstring();
    }

    float wtofSafe(const std::wstring& s)
    {
        try { return s.empty() ? 0.0f : std::stof(s); } catch (...) { return 0.0f; }
    }
    int wtoiSafe(const std::wstring& s)
    {
        try { return s.empty() ? 0 : std::stoi(s); } catch (...) { return 0; }
    }
    bool isYes(std::wstring s)
    {
        for (wchar_t& ch : s) ch = (wchar_t)towlower(ch);
        return s == L"yes" || s == L"true" || s == L"1";
    }

    // Trim ASCII whitespace from both ends (GameObjectFiles <File> text).
    std::string trim(const std::string& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && (unsigned char)s[a]     <= ' ') ++a;
        while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
        return s.substr(a, b - a);
    }

    // Cheaply read just the ROOT element name from a list file (bounded prefix
    // read, no XML parse) so the GameObjectFiles locator can skip the dozens of
    // large unit/prop XMLs it references without fully parsing them. Tolerates a
    // leading <?xml?> decl and <!-- comment -->/<!DOCTYPE>. Returns "" on miss.
    std::string sniffRootElementName(IFileManager& fm, const std::string& path)
    {
        IFile* f = fm.getFile(path);
        if (f == nullptr) return std::string();
        char buf[1024];
        unsigned long n = 0;
        // PhysicalFile::read throws ReadException on a real I/O error; one bad file
        // referenced by GameObjectFiles.xml must not leak `f` or abort the whole
        // picker enumeration -- swallow + release, the caller's full-parse fallback
        // (and the empty-root result) handle it.
        try { n = f->read(buf, (unsigned long)sizeof(buf)); }
        catch (...) { f->Release(); return std::string(); }
        f->Release();
        const char* p   = buf;
        const char* end = buf + (n < sizeof(buf) ? n : sizeof(buf));
        while (p < end)
        {
            if (*p != '<') { ++p; continue; }
            if (p + 1 < end && (p[1] == '?' || p[1] == '!'))   // <?xml?> / <!-- --> / <!DOCTYPE>
            {
                const char* gt = static_cast<const char*>(memchr(p, '>', end - p));
                if (gt == nullptr) break;
                p = gt + 1;
                continue;
            }
            ++p;                                                // first real element: read its name
            const char* nameStart = p;
            while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n'
                   && *p != '>' && *p != '/')
                ++p;
            return std::string(nameStart, p);
        }
        return std::string();
    }

    // Robustness fallback for sniffRootElementName: a full XML parse + root-name
    // check, used only when the cheap sniff can't determine a root (a UTF-16 list
    // file, or a leading comment longer than the sniff window). Rare path -- the
    // sniff resolves the vast majority. Returns true iff the root element is `rootTag`.
    bool fullParseRootMatches(IFileManager& fm, const std::string& path, const char* rootTag)
    {
        IFile* f = fm.getFile(path);
        if (f == nullptr) return false;
        XMLTree xml;
        try { xml.parse(f); }
        catch (...) { f->Release(); return false; }
        const XMLNode* r = xml.getRoot();
        const bool ok = (r != nullptr && WideToAnsi(r->getName()) == rootTag);
        f->Release();
        return ok;
    }

    // Discover the Data\XML-relative list files for `rootTag` via GameObjectFiles.xml
    // (mod-resolved). Each <File> whose target's ROOT element is `rootTag` is kept,
    // in GameObjectFiles order (= mod precedence; getFile already replaced the file by
    // mod->base precedence). Returns false if GameObjectFiles.xml can't be read OR is
    // structurally broken (no root) -- both mean "couldn't read the registry", so the
    // caller falls back to the canonical filename rather than show an empty picker.
    // Returns true with a possibly-empty list when the GOF is readable but registers
    // no dome for this axis.
    bool gatherSkydomeFiles(IFileManager& fm, const char* rootTag, std::vector<std::string>& outFiles)
    {
        outFiles.clear();
        IFile* gof = fm.getFile("Data\\XML\\GameObjectFiles.xml");
        if (gof == nullptr) return false;
        XMLTree xml;
        try { xml.parse(gof); }
        catch (...) { gof->Release(); return false; }
        gof->Release();

        const XMLNode* root = xml.getRoot();
        if (root == nullptr) return false;   // broken GOF -> treat as unreadable, fall back

        // Manifest file-count cap + dedup, matching GameObjectCatalog's reader of
        // this same file (2026-07 audit). This is the SECOND uncapped reader
        // in this translation unit — LoadAllSkydomeLists below was the other, and
        // both had to be fixed: a cap on one reader of a shared manifest is not a
        // cap on the manifest.
        std::set<std::string> seenRel;
        unsigned long kept = 0;
        for (unsigned i = 0; i < root->getNumChildren(); ++i)
        {
            const XMLNode* c = root->getChild(i);
            if (c->getName() != L"File") continue;
            const std::string rel = trim(WideToAnsi(c->getData()));
            if (rel.empty()) continue;
            if (!IsSafeRelativeAssetName(rel)) continue;
            if (!seenRel.insert(rel).second) continue;
            if (kept >= kMaxCatalogXmlFileCount) break;
            ++kept;
            const std::string full = std::string("Data\\XML\\") + rel;
            // Fast path: sniff the root from the first bytes. Only when the sniff is
            // inconclusive (empty) do we pay a full parse -- a sniff that returns a
            // non-matching root is a definitive skip (no full parse needed).
            const std::string sniffed = sniffRootElementName(fm, full);
            const bool match = sniffed.empty() ? fullParseRootMatches(fm, full, rootTag)
                                               : (sniffed == rootTag);
            if (match)
                outFiles.push_back(full);
        }
        return true;
    }

    // Harvest the <entryTag> entries of one parsed list `root` into `out`, deduped
    // by Name against `seen` (first-seen wins; a model-less entry un-claims its Name
    // so a later-listed file can still supply a real one). Shared by LoadSkydomeList
    // (single axis) and LoadAllSkydomeLists (one-pass, all axes).
    void harvestSkydomeEntries(const XMLNode* root, const AxisConfig& cfg,
                               std::set<std::string>& seen, std::vector<SkydomeRef>& out)
    {
        for (unsigned i = 0; i < root->getNumChildren(); ++i)
        {
            const XMLNode* e = root->getChild(i);
            if (e->getName() != cfg.entryTag) continue;   // tolerate comments / non-dome elements

            std::string name = WideToAnsi(e->getAttribute(L"Name"));
            if (name.empty()) continue;                   // unnamed entry can't be referenced -> skip
            if (!seen.insert(name).second) continue;      // already supplied by an earlier file -> skip

            std::wstring model = childData(e, cfg.modelTag);
            if (model.empty()) model = childData(e, cfg.altModelTag);
            // No renderable model -> not a usable dome. Un-claim the Name (erase) so a
            // later-listed file can still supply a real one (a real model in an earlier
            // file already wins via the dedup `continue` above).
            if (model.empty()) { seen.erase(name); continue; }

            // Bound the accepted prefix across the whole axis/load, not one XML
            // file at a time. `out` is shared across routed files and grows only
            // after the existing tag/name/dedup/model filters, so duplicates and
            // model-less entries consume no slots. Preserve the current
            // truncation contract: stop harvesting without failing the load.
            if (out.size() >= kMaxSkydomeEntriesPerAxis) return;

            SkydomeRef ref;
            ref.name      = name;
            ref.modelPath = WideToAnsi(model);
            if (!IsSafeRelativeAssetName(ref.modelPath)) ref.modelPath.clear();
            const float scale   = wtofSafe(childData(e, L"Scale_Factor"));
            ref.scaleFactor     = (scale > 0.0f) ? scale : 1.0f;  // absent / junk / <=0 -> 1.0 (avoid invisible dome)
            ref.sortOrderAdjust = wtoiSafe(childData(e, L"Sort_Order_Adjust"));
            ref.layerZAdjust    = wtofSafe(childData(e, L"Layer_Z_Adjust"));
            ref.inBackground    = isYes(childData(e, L"In_Background"));
            out.push_back(ref);
        }
    }
}

bool LoadSkydomeList(IFileManager& fm, SkydomeAxis axis, std::vector<SkydomeRef>& out)
{
    out.clear();
    const AxisConfig cfg = configFor(axis);
    if (cfg.file[0] == '\0') return false;

    // Discover the list file(s) via GameObjectFiles.xml (mod-resolved) so a mod's
    // domes are found regardless of the file's name/path -- e.g. a mod registers
    // Data\XML\Props\Skydomes_Space_Secondary.xml. Fall back to the canonical
    // vanilla filename when there is no GameObjectFiles.xml (bare install / unit
    // test / odd mod), which preserves the original out-of-the-box behavior.
    std::vector<std::string> files;
    const bool gofPresent = gatherSkydomeFiles(fm, cfg.rootTag, files);
    if (!gofPresent)
        files.push_back(std::string("Data\\XML\\") + cfg.file);

    std::set<std::string> seen;   // first listed file wins on a duplicate Name
    bool anyRead = false;
    for (const std::string& path : files)
    {
        IFile* f = fm.getFile(path);
        if (f == nullptr) continue;
        XMLTree xml;
        bool parsed = false;
        try { xml.parse(f); parsed = true; }
        catch (...) { parsed = false; }
        f->Release();
        if (!parsed) continue;
        const XMLNode* root = xml.getRoot();
        if (root == nullptr) continue;
        anyRead = true;
        harvestSkydomeEntries(root, cfg, seen, out);
    }
    // GameObjectFiles readable -> success even if no dome matched (a valid empty
    // picker); otherwise success iff the canonical fallback file was read.
    return gofPresent ? true : anyRead;
}

bool ResolveSkydomeModel(IFileManager& fm, const SkydomeRef& ref, std::vector<unsigned char>& outBytes)
{
    outBytes.clear();
    if (ref.modelPath.empty()) return false;
    if (!IsSafeRelativeAssetName(ref.modelPath)) return false;

    IFile* f = fm.getFile(std::string("Data\\Art\\Models\\") + ref.modelPath);
    if (f == nullptr) return false;

    try { outBytes = ReadAndReleaseCapped(f, kMaxAloModelBytes); }   // takes ownership + Releases f (even on throw / oversize)
    catch (...) { return false; }
    return !outBytes.empty();
}

bool LoadMapEnvironment(IFileManager& fm, SkydomeContext ctx,
                        const std::string& primaryName, const std::string& secondaryName,
                        MapEnvironment& out)
{
    out = MapEnvironment();
    const SkydomeAxis primAxis = (ctx == SkydomeContext::Land) ? SkydomeAxis::LandPrimary   : SkydomeAxis::SpacePrimary;
    const SkydomeAxis secAxis  = (ctx == SkydomeContext::Land) ? SkydomeAxis::LandSecondary : SkydomeAxis::SpaceSecondary;

    bool ok = true;
    std::vector<SkydomeRef> list;

    if (!primaryName.empty())
    {
        if (LoadSkydomeList(fm, primAxis, list))
        {
            for (const auto& r : list)
                if (r.name == primaryName) { out.primary = r; out.hasPrimary = true; break; }
        }
        else ok = false;
    }
    if (!secondaryName.empty())
    {
        if (LoadSkydomeList(fm, secAxis, list))
        {
            for (const auto& r : list)
                if (r.name == secondaryName) { out.secondary = r; out.hasSecondary = true; break; }
        }
        else ok = false;
    }
    return ok;
}

void LoadAllSkydomeLists(IFileManager& fm, std::array<std::vector<SkydomeRef>, kNumSkydomeAxes>& out)
{
    for (auto& v : out) v.clear();

    const AxisConfig cfg[kNumSkydomeAxes] = {
        configFor(SkydomeAxis::LandPrimary),  configFor(SkydomeAxis::LandSecondary),
        configFor(SkydomeAxis::SpacePrimary), configFor(SkydomeAxis::SpaceSecondary),
    };
    std::set<std::string> seen[kNumSkydomeAxes];   // per-axis Name dedup

    // Gather the candidate <File> paths from GameObjectFiles.xml in ONE parse.
    std::vector<std::string> files;
    bool gofPresent = false;
    {
        IFile* gof = fm.getFile("Data\\XML\\GameObjectFiles.xml");
        if (gof != nullptr)
        {
            XMLTree xml;
            bool parsed = false;
            try { xml.parse(gof); parsed = true; } catch (...) {}
            gof->Release();
            const XMLNode* root = parsed ? xml.getRoot() : nullptr;
            if (root != nullptr)   // readable + has a root -> drive enumeration from it
            {
                gofPresent = true;
                // Same file-count cap the catalog's reader applies to this very
                // manifest (GameObjectCatalog.cpp readFileList). GameObjectFiles.xml
                // has TWO readers and only one of them was bounded, so a crafted
                // manifest was capped for the catalog and unbounded here — each
                // entry costing a root sniff and, when that is inconclusive, a full
                // parse (2026-07 audit). De-dup as we collect so the cap bounds
                // DISTINCT names: a flood of repeats must not push a later
                // legitimate entry past it.
                std::set<std::string> seenRel;
                for (unsigned i = 0; i < root->getNumChildren(); ++i)
                {
                    const XMLNode* c = root->getChild(i);
                    if (c->getName() != L"File") continue;
                    const std::string rel = trim(WideToAnsi(c->getData()));
                    if (!IsSafeRelativeAssetName(rel)) continue;
                    if (rel.empty()) continue;
                    if (!seenRel.insert(rel).second) continue;
                    if (files.size() >= kMaxCatalogXmlFileCount) break;
                    files.push_back(std::string("Data\\XML\\") + rel);
                }
            }
        }
    }

    if (!gofPresent)
    {
        // No (readable) GameObjectFiles.xml -> each axis falls back to its canonical
        // filename (mirrors LoadSkydomeList's fallback), harvested directly per axis.
        for (int a = 0; a < kNumSkydomeAxes; ++a)
        {
            IFile* f = fm.getFile(std::string("Data\\XML\\") + cfg[a].file);
            if (f == nullptr) continue;
            XMLTree xml; bool parsed = false;
            try { xml.parse(f); parsed = true; } catch (...) {}
            f->Release();
            const XMLNode* root = parsed ? xml.getRoot() : nullptr;
            if (root != nullptr) harvestSkydomeEntries(root, cfg[a], seen[a], out[a]);
        }
        return;
    }

    // GOF present: route each listed file to the axis whose rootTag it matches,
    // sniffing the root cheaply first so the dozens of large unit/prop XMLs the GOF
    // also references are never fully parsed -- only the handful of skydome files are.
    for (const std::string& path : files)
    {
        int axisIdx = -1;
        const std::string sniffed = sniffRootElementName(fm, path);
        if (!sniffed.empty())
        {
            for (int a = 0; a < kNumSkydomeAxes; ++a)
                if (sniffed == cfg[a].rootTag) { axisIdx = a; break; }
            if (axisIdx < 0) continue;   // a definite non-skydome root -> skip, no full parse
        }
        // Open + parse once (needed to harvest; also resolves an inconclusive sniff).
        IFile* f = fm.getFile(path);
        if (f == nullptr) continue;
        XMLTree xml; bool parsed = false;
        try { xml.parse(f); parsed = true; } catch (...) {}
        f->Release();
        const XMLNode* root = parsed ? xml.getRoot() : nullptr;
        if (root == nullptr) continue;
        if (axisIdx < 0)   // sniff was inconclusive (UTF-16 / long header) -> classify from the parse
        {
            const std::string rn = WideToAnsi(root->getName());
            for (int a = 0; a < kNumSkydomeAxes; ++a)
                if (rn == cfg[a].rootTag) { axisIdx = a; break; }
            if (axisIdx < 0) continue;
        }
        harvestSkydomeEntries(root, cfg[axisIdx], seen[axisIdx], out[axisIdx]);
    }
}

void ResolveMapEnvironment(const std::array<std::vector<SkydomeRef>, kNumSkydomeAxes>& lists,
                           SkydomeContext ctx,
                           const std::string& primaryName, const std::string& secondaryName,
                           MapEnvironment& out)
{
    out = MapEnvironment();
    const int primAxis = (ctx == SkydomeContext::Land) ? (int)SkydomeAxis::LandPrimary   : (int)SkydomeAxis::SpacePrimary;
    const int secAxis  = (ctx == SkydomeContext::Land) ? (int)SkydomeAxis::LandSecondary : (int)SkydomeAxis::SpaceSecondary;

    if (!primaryName.empty())
        for (const SkydomeRef& r : lists[primAxis])
            if (r.name == primaryName) { out.primary = r; out.hasPrimary = true; break; }
    if (!secondaryName.empty())
        for (const SkydomeRef& r : lists[secAxis])
            if (r.name == secondaryName) { out.secondary = r; out.hasSecondary = true; break; }
}
