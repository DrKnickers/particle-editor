#ifndef SKYDOMEENVIRONMENT_H
#define SKYDOMEENVIRONMENT_H

// Map-environment reader: enumerates the game/mod's skydome
// GameObjects and resolves a chosen primary+secondary pair through the injected
// IFileManager (mod -> base -> MEG). Pure data + FileManager -- no engine /
// D3D / UI coupling -- so it is unit-testable with a mock FM and reused by,
// which extends MapEnvironment with per-map colour-grade params (the reuse
// contract) rather than adding a second locator.
//
// Confirmed data model (vanilla FoC source; scope doc section 0): a skydome is
// an `.alo` GameObject listed in {Land,Space}{Primary,Secondary}Skydomes.xml.
// The XML carries only a Name + a model path + scale / sort hints; textures and
// cloud-scroll params live inside the `.alo` material (decoded by AloModel).

#include <array>
#include <string>
#include <vector>

class IFileManager;

// The four independent skydome lists (Land/Space x Primary/Secondary).
enum class SkydomeAxis { LandPrimary, LandSecondary, SpacePrimary, SpaceSecondary };

// Battle context for resolving a primary+secondary pair.
enum class SkydomeContext { Land, Space };

// One <...Skydome> GameObject. Textures / cloud-scroll / SH lighting are NOT
// here -- they come from the `.alo` material (see AloModel).
struct SkydomeRef
{
    std::string name;             // Name= attribute (.ted key; picker label)
    std::string modelPath;        // chosen *_Model_Name, bare ".alo" filename
    float       scaleFactor = 1.0f;
    int         sortOrderAdjust = 0;
    float       layerZAdjust = 0.0f;
    bool        inBackground = false;
};

// A resolved environment for one map/context. adds colour-grade fields here.
struct MapEnvironment
{
    SkydomeRef primary, secondary;
    bool hasPrimary = false;
    bool hasSecondary = false;
};

// Number of skydome axes (index space for the `LoadAllSkydomeLists` arrays).
constexpr int kNumSkydomeAxes = 4;   // == enum SkydomeAxis size

// Enumerate every skydome GameObject for `axis`, deduped by Name. Clears `out`
// first. Returns false if no list file could be read (out left empty); true
// otherwise (out may still be empty -- which drives the picker's empty state).
// Never throws.
//
// [#224] Discovers the list files via `Data\\XML\\GameObjectFiles.xml`
// (mod-resolved), classifying each referenced file by its ROOT element -- so a
// mod's domes registered under non-canonical names/paths (e.g. Mod's
// `Props\\Skydomes_Space_Secondary.xml`) are found, not just the canonical
// vanilla filenames. Falls back to the canonical filename when there is no
// GameObjectFiles.xml.
bool LoadSkydomeList(IFileManager& fm, SkydomeAxis axis, std::vector<SkydomeRef>& out);

// [#NN] Build ALL FOUR axis lists in a single GameObjectFiles pass -- parse
// GameObjectFiles.xml once, sniff each referenced file's root once, and bucket
// its entries into the matching axis (`out[(int)axis]`). Equivalent per-axis
// output to calling `LoadSkydomeList` four times, but ~4x cheaper (no repeated
// GOF parse + per-axis re-sniff of every file). Falls back to the four canonical
// filenames when there is no GameObjectFiles.xml. The Engine caches the result
// and rebuilds it only on a mod/submod switch. Never throws.
void LoadAllSkydomeLists(IFileManager& fm,
                         std::array<std::vector<SkydomeRef>, kNumSkydomeAxes>& out);

// Resolve a primary+secondary pair (by Name, first match wins) for `ctx` from
// PRE-LOADED axis lists (e.g. the Engine's cached `LoadAllSkydomeLists` output) --
// no file I/O. An empty name leaves that slot unset. The file-reading sibling is
// `LoadMapEnvironment`.
void ResolveMapEnvironment(const std::array<std::vector<SkydomeRef>, kNumSkydomeAxes>& lists,
                           SkydomeContext ctx,
                           const std::string& primaryName, const std::string& secondaryName,
                           MapEnvironment& out);

// Slurp a dome's model bytes from "Data\\Art\\Models\\<modelPath>" via `fm`.
// Returns false on empty modelPath, a miss, or an empty file. Separated from
// enumeration so the picker can list names cheaply without loading meshes.
bool ResolveSkydomeModel(IFileManager& fm, const SkydomeRef& ref,
                         std::vector<unsigned char>& outBytes);

// Resolve a chosen primary+secondary pair (by Name, first match wins) for the
// given battle context: primary from {ctx}Primary, secondary from
// {ctx}Secondary. An empty name leaves that slot unset. Returns false only if a
// requested slot's list could not be read; true otherwise. Never throws.
bool LoadMapEnvironment(IFileManager& fm, SkydomeContext ctx,
                        const std::string& primaryName, const std::string& secondaryName,
                        MapEnvironment& out);

#endif
