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

// Enumerate every skydome GameObject for `axis` from "Data\\XML\\<file>.xml"
// (resolved mod -> base -> MEG via `fm`). Clears `out` first. Returns false if
// the list file can't be read or parsed (out left empty); true otherwise (out
// may still be empty -- which drives the picker's empty state). Never throws.
//
// This resolves the canonical vanilla filenames, which resolve from the base
// game's config.meg even under a mod, so the picker is populated out of the
// box. Mods that register skydome XML under non-canonical names/paths via
// GameObjectFiles.xml are a follow-up (a GameObjectFiles-driven locator); their
// custom domes won't appear until then, but the vanilla domes do.
bool LoadSkydomeList(IFileManager& fm, SkydomeAxis axis, std::vector<SkydomeRef>& out);

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
