#ifndef GAMEOBJECTCATALOG_H
#define GAMEOBJECTCATALOG_H

// Game-object catalog: enumerates every renderable game/mod object by
// its in-game Name and resolves it to a renderable `.alo` model path, through
// the injected IFileManager (mod -> base -> MEG). Pure data + FileManager --
// no engine / D3D / UI coupling -- so it is unit-testable with a mock FM and
// reused by the engine () to populate the reference-object picker.
//
// Parallels SkydomeEnvironment (the skydome-list reader) but spans the WHOLE
// object graph. "Data\\XML\\GameObjectFiles.xml" lists ~120 object files; each
// lists game objects as DIRECT children of its root element
// (`<GroundVehicle Name="...">`, `<SpaceUnit Name="...">`, ...). An object's
// renderable model is the first present of its *_Model_Name fields; an object
// may instead be a `Variant_Of_Existing_Type` of another (possibly cross-file)
// object and inherit that parent's model -- so resolution is two-phase:
//   (1) parse every listed file into a global Name -> raw-entry map, then
//   (2) resolve Variant_Of inheritance once all entries are known
//       (cross-file + cyclic-safe; a variant's own model overrides its parent).
//
// Skinned-vs-rigid is a property of the `.alo`, not the XML category (e.g. the
// "Scout_Trooper" object rides a rigid speeder bike), and decoding every model
// up front (thousands of files) would freeze startup -- so the catalog build is
// XML-only and ProbeModelSkinned() is a lazy, caller-cached per-model check run
// only when an object is actually selected.

#include <string>
#include <vector>
#include <map>

class IFileManager;

// Coarse grouping for the picker, derived from the object's container tag.
// `Excluded` = a model-bearing object that is NOT a unit/structure (planet,
// marker/spawn zone, dummy, death clone, particle prop) -- never listed in the
// picker. `Other` is now the catch-all for UNRECOGNISED unit/structure tags and IS
// listed (the filter fails toward showing, so a unit type we don't recognise is
// never silently dropped -- see IsPickerListedCategory).
enum class GameObjectCategory
{
    Vehicle, Infantry, Structure, Turret, Hero, Prop, Space, Projectile, Other, Excluded
};

// One hardpoint definition (a <HardPoint> in the hardpoint files). EaW units
// reference these by Name in their <HardPoints> list; the editor mounts the optional
// `modelToAttach` .alo at `attachmentBone` (a bone in the UNIT model) to render the
// unit complete, and hides the meshes on the damage/collision bones (the damaged-state
// geometry the engine shows only when a hardpoint is destroyed). All fields optional
// (may be empty) -- mods frequently omit Model_To_Attach. Bone names match the .alo's
// AloBone names CASE-INSENSITIVELY (XML is all-caps; the .alo is mixed-case).
struct HardPointDef
{
    std::string modelToAttach;       // <Model_To_Attach> -- ".alo" to mount, or empty (nothing to attach)
    std::string attachmentBone;      // <Attachment_Bone> -- bone in the UNIT model where the model mounts
    std::string damageDecalBone;     // <Damage_Decal>     -- unit bone whose mesh is damaged-state (hide intact)
    std::string damageParticlesBone; // <Damage_Particles> -- unit bone whose mesh is damaged-state (hide intact)
    std::string collisionMeshBone;   // <Collision_Mesh>   -- unit bone whose mesh is the collision hull (hide)
};

// One enumerated game object that resolved to a renderable model.
struct GameObjectRef
{
    std::string        name;        // Name= attribute (in-game key; picker label)
    std::string        modelPath;   // resolved bare ".alo" filename (after Variant_Of inheritance)
    GameObjectCategory category = GameObjectCategory::Other;
    std::string        tag;         // raw container tag, e.g. "GroundVehicle" (grouping / diagnostics)
    std::string        sourceFile;  // listed XML it came from (diagnostics)
    // HardPoint Names this object references (its <HardPoints> comma list, with
    // Variant_Of inheritance resolved). Looked up in GameObjectCatalog::hardpoints at
    // select-time to mount attach models + collect the damage bones to hide.
    std::vector<std::string> hardpointNames;
};

struct GameObjectCatalog
{
    std::vector<GameObjectRef> objects;  // every object with a resolvable model, sorted by name
    // HardPoint table for the whole active content, keyed by lower-cased Name
    // (parsed once per build from HardPointDataFiles.xml -> the listed hardpoint files).
    std::map<std::string, HardPointDef> hardpoints;
};

// Build the catalog from "Data\\XML\\GameObjectFiles.xml" via `fm`. Clears `out`
// first. Returns false only if GameObjectFiles.xml itself can't be read/parsed
// (out left empty); true otherwise (out may still be empty). A listed object
// file that can't be read/parsed is skipped (non-fatal). Never throws.
//
// Like the canonical skydome lists, the vanilla GameObjectFiles.xml resolves
// from the base game's config.meg even under a mod, so the picker is populated
// out of the box; a mod's own GameObjectFiles.xml (or its overriding object
// files) take priority through `fm`'s mod -> base ordering.
bool BuildGameObjectCatalog(IFileManager& fm, GameObjectCatalog& out);

// Lazy per-model renderability probe (loads + decodes the `.alo` via AloModel).
// The catalog stores only XML; calls this on-select and caches the result
// (the `.alo` parse is far too costly to run for every object at build time).
//   Renderable          -- has >= 1 rigid, visible sub-mesh the renderer draws
//   SkinnedUnsupported  -- loads, but every visible sub-mesh is skinned (v1 skip)
//   LoadFailed          -- empty path, or present-but-malformed / non-mesh .alo
//   NotFound            -- the file is genuinely absent (getFile miss), distinct
//                          from LoadFailed so the picker can say "model file not
//                          found" rather than the misleading "couldn't load".
//                          E.g. a Name listed in GameObjectFiles.xml whose .alo
//                          isn't shipped in this mod/base (Prop_ElectricBox_00).
// The accept condition is kept in lockstep with ReferenceObjectMesh::Load's draw
// filter (skip 0x402-hidden meshes + the shared AloIsSkinnedVertexFormat /
// AloIsNonVisibleShader predicates) so a "Renderable" verdict means the renderer
// will actually show geometry.
enum class ModelProbeResult { Renderable, SkinnedUnsupported, LoadFailed, NotFound };
ModelProbeResult ProbeModelSkinned(IFileManager& fm, const std::string& modelPath);

// Stable display string for a category (picker headers / dump mode).
const char* GameObjectCategoryName(GameObjectCategory c);

// Categories surfaced in the reference-object picker: UNITS + STRUCTURES.
// EXCLUSION-based (fails toward SHOWING): everything is listed EXCEPT Prop,
// Projectile, and Excluded (explicit model-bearing non-units -- planets / markers /
// dummies / death clones / particles). Crucially `Other` (unrecognised unit/structure
// tags like groundcompany, groundbuildable, flagship*, capturables) IS listed -- an
// earlier keep-only-known-categories filter dropped real units whose tags weren't
// recognised (e.g. Mod's flagshipunit capital ships landed in Other and vanished).
bool IsPickerListedCategory(GameObjectCategory c);

#endif
