#include "GameObjectCatalog.h"

#include "managers.h"     // IFileManager
#include "xml.h"          // XMLTree / XMLNode
#include "files.h"        // IFile
#include "utils.h"        // WideToAnsi
#include "AloModel.h"     // LoadAloModel + AloIsSkinnedVertexFormat / AloIsNonVisibleShader
#include "exceptions.h"   // wexception (LoadAloModel boundary)

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
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

        if (nameOrTag("turret"))                                         return GameObjectCategory::Turret;   // name or tag
        if (tagHas("infantry") || tagHas("trooper"))                     return GameObjectCategory::Infantry;
        if (tagHas("prop"))                                              return GameObjectCategory::Prop;     // SpaceProp / Props_* / Prop_*
        if (tagHas("hero"))                                              return GameObjectCategory::Hero;
        if (tagHas("projectile"))                                        return GameObjectCategory::Projectile;
        if (tagHas("vehicle") || tagHas("transport"))                    return GameObjectCategory::Vehicle;
        if (tagHas("starbase") || tagHas("squadron") || tagHas("space")) return GameObjectCategory::Space;
        if (tagHas("structure") || tagHas("building") || tagHas("base")) return GameObjectCategory::Structure;
        return GameObjectCategory::Other;
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

    // Phase 1: parse one object file's direct children into `byName` (first-wins
    // on a duplicate Name). Missing / malformed file -> no-op (non-fatal). We do
    // NOT follow nested <File> includes some mods use -- v1 trusts the flat
    // GameObjectFiles.xml list (a follow-up if a mod needs includes).
    void parseObjectFile(IFileManager& fm, const std::string& fileName,
                         std::map<std::string, RawEntry>& byName)
    {
        IFile* f = fm.getFile(std::string("Data\\XML\\") + fileName);
        if (f == nullptr) return;

        XMLTree xml;
        try { xml.parse(f); }
        catch (...) { f->Release(); return; }
        f->Release();

        const XMLNode* root = xml.getRoot();
        if (root == nullptr) return;

        for (unsigned i = 0; i < root->getNumChildren(); ++i)
        {
            const XMLNode* e = root->getChild(i);
            std::string name = WideToAnsi(e->getAttribute(L"Name"));
            if (name.empty()) continue;                 // comment / anonymous / <File> include -> not an object
            std::string key = asciiLower(name);
            if (byName.find(key) != byName.end()) continue;  // first-wins (case-insensitive, like the engine)

            RawEntry re;
            re.name       = name;
            re.tag        = WideToAnsi(e->getName());
            re.sourceFile = fileName;
            re.variantOf  = trim(WideToAnsi(childData(e, L"Variant_Of_Existing_Type")));
            re.ownModel   = firstModel(e);
            byName[key]   = re;
        }
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

bool BuildGameObjectCatalog(IFileManager& fm, GameObjectCatalog& out)
{
    out.objects.clear();

    std::vector<std::string> files;
    if (!readFileList(fm, files)) return false;          // GameObjectFiles.xml unreadable

    std::map<std::string, RawEntry> byName;
    std::set<std::string> seenFiles;
    for (const std::string& fn : files)
    {
        if (!seenFiles.insert(fn).second) continue;       // de-dup a doubly-listed file
        parseObjectFile(fm, fn, byName);
    }

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
    return true;
}

ModelProbeResult ProbeModelSkinned(IFileManager& fm, const std::string& modelPath)
{
    if (modelPath.empty()) return ModelProbeResult::LoadFailed;

    IFile* f = fm.getFile(std::string("Data\\Art\\Models\\") + modelPath);
    if (f == nullptr) return ModelProbeResult::LoadFailed;

    AloModel model;
    bool ok = false;
    try { model = LoadAloModel(f); ok = true; }   // AddRef/Releases internally
    catch (const wexception&) { ok = false; }     // malformed / truncated / non-mesh
    f->Release();                                  // release our getFile ref
    if (!ok) return ModelProbeResult::LoadFailed;

    // Renderable iff at least one sub-mesh would actually be drawn -- the exact
    // condition ReferenceObjectMesh uses to accept a sub-mesh.
    for (const AloMesh& mesh : model.meshes)
        for (const AloSubMesh& sm : mesh.subMeshes)
        {
            if (sm.rawVertexBytes.empty() || sm.vertexCount == 0 || sm.primitiveCount == 0) continue;
            if (AloIsNonVisibleShader(sm.shaderName))         continue;   // collision / shadow hull
            if (AloIsSkinnedVertexFormat(sm.vertexFormatName)) continue;  // v1 defers skinning
            return ModelProbeResult::Renderable;
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
        default:                             return "Other";
    }
}
