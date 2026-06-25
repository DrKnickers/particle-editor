#ifndef GAMEOBJECTCATALOG_H
#define GAMEOBJECTCATALOG_H

// Game-object catalog: enumerates every renderable game/mod object by
// its in-game Name and resolves it to a renderable `.alo` model path, through
// the injected IFileManager (mod -> base -> MEG). Pure data + FileManager --
// no engine / D3D / UI coupling -- so it is unit-testable with a mock FM and
// reused by the engine to populate the reference-object picker.
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

// The profile-based classifier (replaced the old tag-keyword `categorize()`
// heuristic for the picker keep/group decision). Three orthogonal axes derived from a
// multi-signal ObjectProfile via the pure, unit-tested ClassifyObject():
//   domain  -- ground vs space, from the resolved MODEL FIELD-NAME (Land vs Space),
//              the one signal present across every mod (CategoryMask is absent in base
//              FoC + Mod). Corroborated by CategoryMask/behavior; disagreement or a
//              dual-env (Land+Space) object sets `conflict`.
//   role    -- Excluded / Unit / Structure / Hero. Excluded = a renderable object that
//              is NOT a unit/structure (skydome/planet backdrop, prop, marker, particle,
//              projectile, death-clone, dummy, template, formation wrapper). The picker
//              keeps role != Excluded.
//   bucket  -- the fine sub-group for the future collapsible sections.
enum class ModelFieldKind { None, Land, Space, Galactic, Generic };
enum class ObjDomain      { Unknown, Ground, Space };
enum class ObjRole        { Excluded, Unit, Structure, Hero };
enum class ObjBucket {
    None,
    Infantry, Vehicle, Air,                                   // ground units
    Fighter, Bomber, Corvette, Frigate, Capital, Transport,   // space units
    Structure, Hero, Other                                    // shared
};

// Why an object counts as "fieldable" (a player can actually field it). Bit flags so a
// single object can be reached more than one way. NotFieldable = 0.
enum FieldSource : unsigned
{
    FS_None           = 0,
    FS_Buildable      = 1u << 0,   // has Build_Cost_Credits (fallback Required_Star_Base_Level)
    FS_StructureBuilt = 1u << 1,   // named in a structure's Tactical_Buildable_Objects_* list
    FS_Spawned        = 1u << 2,   // named in a carrier/structure spawn list (Reserve/Starting_Spawned_Units_Tech_*)
    FS_Roster         = 1u << 3,   // present in a mod's GameObjectList.lua roster allow-list
};

// The signals gathered per object (own value merged with the Variant_Of parent's), fed
// to ClassifyObject. Pure data so the classifier is unit-testable in isolation.
struct ObjectProfile
{
    std::string              name;            // object Name= (some junk is only recognizable by name)
    std::string              tag;             // container element name, exact as-read (classifier lower-cases internally)
    ModelFieldKind           modelField = ModelFieldKind::None;  // which model field won
    bool                     dualEnv    = false;                 // both Land AND Space model present
    std::vector<std::string> maskTokens;      // <CategoryMask> tokens, UPPER-cased (Anti*/All/None kept; classifier ignores)
    std::vector<std::string> behaviorTokens;  // Behavior + Land/SpaceBehavior tokens, UPPER-cased
    bool                     hasMovementClass = false;
    std::string              affiliation;      // <Affiliation> -- surfaced as the picker's faction filter
};

struct Classification
{
    ObjDomain domain   = ObjDomain::Unknown;
    ObjRole   role     = ObjRole::Excluded;
    ObjBucket bucket   = ObjBucket::None;
    bool      conflict = false;   // model/mask domain disagreement or dual-env: surfaced, not hidden
};

// Pure classifier: profile -> {domain, role, bucket, conflict}. No FileManager / I/O.
Classification ClassifyObject(const ObjectProfile& p);

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
    std::string        tag;         // raw container tag, e.g. "GroundVehicle" (grouping / diagnostics)
    std::string        sourceFile;  // listed XML it came from (diagnostics)
    // HardPoint Names this object references (its <HardPoints> comma list, with
    // Variant_Of inheritance resolved). Looked up in GameObjectCatalog::hardpoints at
    // select-time to mount attach models + collect the damage bones to hide.
    std::vector<std::string> hardpointNames;

    // Profile classification (from ClassifyObject) + the fieldable attribute.
    ObjDomain domain      = ObjDomain::Unknown;
    ObjRole   role        = ObjRole::Excluded;
    ObjBucket bucket      = ObjBucket::None;
    bool      conflict    = false;
    bool      fieldable   = false;        // player can build/spawn it -> hard picker keep-gate
    unsigned  fieldSource = FS_None;      // FieldSource bits explaining why (diagnostics)
    std::string affiliation;              // <Affiliation> (raw, may be a comma list) -> picker faction filter

    // <Scale_Factor> uniform render multiplier (Variant_Of-inherited, resolved
    // in BuildGameObjectCatalog phase 4); 1.0 when absent / non-positive / non-finite.
    // The game draws the model at scaleFactor x its native .alo size; the reference-
    // object renderer applies it so an imported unit matches its in-game size.
    float     scaleFactor = 1.0f;
};

// Picker keep-gate. Units/structures must be fieldable (a player can build/spawn
// them) -- the hard gate that trims the roster. HEROES are EXEMPT: Mod grants many heroes
// via galactic/lua scripts invisible to the static fieldable graph (a hard gate hid 197 of
// Mod's 320 heroes -- Ahsoka, Anakin, ...), so the bounded, curated hero set is shown in
// full once the name/tag junk (death-clones etc.) is excluded.
inline bool IsPickerListed(const GameObjectRef& r)
{
    if (r.role == ObjRole::Excluded) return false;
    if (r.role == ObjRole::Hero)     return true;     // heroes exempt from the fieldable gate
    return r.fieldable;
}

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
// The catalog stores only XML; the engine calls this on-select and caches the result
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

// Stable display strings for the classification axes (dump / picker).
const char* ObjDomainName(ObjDomain d);
const char* ObjRoleName(ObjRole r);
const char* ObjBucketName(ObjBucket b);

#endif
