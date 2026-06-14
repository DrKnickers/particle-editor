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

class IFileManager;

// Coarse grouping for the picker, derived from the object's container tag.
enum class GameObjectCategory
{
    Vehicle, Infantry, Structure, Turret, Hero, Prop, Space, Projectile, Other
};

// One enumerated game object that resolved to a renderable model.
struct GameObjectRef
{
    std::string        name;        // Name= attribute (in-game key; picker label)
    std::string        modelPath;   // resolved bare ".alo" filename (after Variant_Of inheritance)
    GameObjectCategory category = GameObjectCategory::Other;
    std::string        tag;         // raw container tag, e.g. "GroundVehicle" (grouping / diagnostics)
    std::string        sourceFile;  // listed XML it came from (diagnostics)
};

struct GameObjectCatalog
{
    std::vector<GameObjectRef> objects;  // every object with a resolvable model, sorted by name
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
//   LoadFailed          -- empty path, missing file, or a malformed / non-mesh .alo
// The accept condition is kept in lockstep with ReferenceObjectMesh::Load's draw
// filter (skip 0x402-hidden meshes + the shared AloIsSkinnedVertexFormat /
// AloIsNonVisibleShader predicates) so a "Renderable" verdict means the renderer
// will actually show geometry.
enum class ModelProbeResult { Renderable, SkinnedUnsupported, LoadFailed };
ModelProbeResult ProbeModelSkinned(IFileManager& fm, const std::string& modelPath);

// Stable display string for a category (picker headers / dump mode).
const char* GameObjectCategoryName(GameObjectCategory c);

#endif
