#include "SkydomeEnvironment.h"

#include "managers.h"     // IFileManager
#include "xml.h"          // XMLTree / XMLNode
#include "files.h"        // IFile, ReadAndRelease
#include "utils.h"        // WideToAnsi

#include <cwctype>        // towlower
#include <string>

namespace
{
    struct AxisConfig
    {
        const char*    file;        // canonical Data\XML filename
        const wchar_t* entryTag;    // per-entry element name
        const wchar_t* modelTag;    // preferred model-path element
        const wchar_t* altModelTag; // fallback model-path element (other context)
    };

    AxisConfig configFor(SkydomeAxis axis)
    {
        switch (axis)
        {
            case SkydomeAxis::LandPrimary:    return { "LandPrimarySkydomes.xml",    L"LandPrimarySkydome",    L"Land_Model_Name",  L"Space_Model_Name" };
            case SkydomeAxis::LandSecondary:  return { "LandSecondarySkydomes.xml",  L"LandSecondarySkydome",  L"Land_Model_Name",  L"Space_Model_Name" };
            case SkydomeAxis::SpacePrimary:   return { "SpacePrimarySkydomes.xml",   L"SpacePrimarySkydome",   L"Space_Model_Name", L"Land_Model_Name" };
            case SkydomeAxis::SpaceSecondary: return { "SpaceSecondarySkydomes.xml", L"SpaceSecondarySkydome", L"Space_Model_Name", L"Land_Model_Name" };
        }
        return { "", L"", L"", L"" };
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
}

bool LoadSkydomeList(IFileManager& fm, SkydomeAxis axis, std::vector<SkydomeRef>& out)
{
    out.clear();
    const AxisConfig cfg = configFor(axis);
    if (cfg.file[0] == '\0') return false;

    IFile* f = fm.getFile(std::string("Data\\XML\\") + cfg.file);
    if (f == nullptr) return false;

    XMLTree xml;
    try { xml.parse(f); }
    catch (...) { f->Release(); return false; }
    f->Release();

    const XMLNode* root = xml.getRoot();
    if (root == nullptr) return false;

    for (unsigned i = 0; i < root->getNumChildren(); ++i)
    {
        const XMLNode* e = root->getChild(i);
        if (e->getName() != cfg.entryTag) continue;   // tolerate comments / other elements

        std::string name = WideToAnsi(e->getAttribute(L"Name"));
        if (name.empty()) continue;                   // unnamed entry can't be referenced -> skip

        std::wstring model = childData(e, cfg.modelTag);
        if (model.empty()) model = childData(e, cfg.altModelTag);
        if (model.empty()) continue;                  // no renderable model -> skip

        SkydomeRef ref;
        ref.name      = name;
        ref.modelPath = WideToAnsi(model);
        const float scale   = wtofSafe(childData(e, L"Scale_Factor"));
        ref.scaleFactor     = (scale > 0.0f) ? scale : 1.0f;  // absent / junk / <=0 -> 1.0 (avoid invisible dome)
        ref.sortOrderAdjust = wtoiSafe(childData(e, L"Sort_Order_Adjust"));
        ref.layerZAdjust    = wtofSafe(childData(e, L"Layer_Z_Adjust"));
        ref.inBackground    = isYes(childData(e, L"In_Background"));
        out.push_back(ref);
    }
    return true;
}

bool ResolveSkydomeModel(IFileManager& fm, const SkydomeRef& ref, std::vector<unsigned char>& outBytes)
{
    outBytes.clear();
    if (ref.modelPath.empty()) return false;

    IFile* f = fm.getFile(std::string("Data\\Art\\Models\\") + ref.modelPath);
    if (f == nullptr) return false;

    try { outBytes = ReadAndRelease(f); }   // takes ownership + Releases f (even on throw)
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
