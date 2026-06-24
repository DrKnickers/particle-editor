// Bridge schema — single source of truth for the JSON contract between
// the React UI and the C++ host. Imported by both web/apps/editor/'s
// MockBridge + NativeBridge, and (eventually) consumed by the C++
// dispatcher via a JSON-schema codegen step.

export type RequestId = string;  // UUID v4

// ============================================================================
// Primitive types
// ============================================================================

/** Three-component vector serialised as a JSON array `[x, y, z]`. */
export type Vec3 = readonly [number, number, number];

/** Four-component vector serialised as a JSON array `[x, y, z, w]`.
 *  When used for a colour, the channel order is `[r, g, b, a]`
 *  with each component in `[0, 1]` floating point. */
export type Vec4 = readonly [number, number, number, number];

/** COLORREF — a 32-bit integer with bytes laid out as `0x00BBGGRR`
 *  (Windows convention; low byte is the *red* channel). Used for
 *  background and ground-solid-color where the engine reads/writes
 *  Win32 COLORREFs directly. Distinct from `Vec4`-as-colour which is
 *  the floating-point linear-space form used for engine lights and
 *  ambient/shadow tints. */
export type Color = number;

// ============================================================================
// Engine state DTO
// ============================================================================

export type CameraDto = {
  position: Vec3;
  target: Vec3;
  up: Vec3;
};

export type LightDto = {
  diffuse: Vec4;
  specular: Vec4;
  position: Vec4;
  direction: Vec4;
};

/** Identifier for the three engine lights. Mirrors `Engine::LightType`
 *  (`LT_SUN | LT_FILL1 | LT_FILL2`) via a string literal on the wire
 *  so JSON readers don't have to memorise an enum. */
export type LightWhich = "sun" | "fill1" | "fill2";

/** One light's RAW persisted settings, as the registry stores them —
 *  intensity and base colour are kept SEPARATE here (the engine only
 *  stores the post-multiplied `intensity × colour` Vec4, which is lossy).
 *  `az`/`alt` are the z-angle / tilt in degrees (stored directly, so no
 *  direction-vector inversion is needed). `diffuse`/`specular` are packed
 *  COLORREFs (`Color`). Fills carry `specular` for shape-compat but it's
 *  unused (fills are diffuse-only). Used by `settings/lighting` so the
 *  LightingPanel can display the user's true intensity/colour split rather
 *  than the folded value recovered from the engine snapshot. */
export type LightSettingsDto = {
  intensity: number;
  az: number;
  alt: number;
  diffuse: Color;
  specular: Color;
};

/** The full raw lighting settings read from the registry — the source the
 *  LightingPanel seeds its displayed controls from. Mirrors legacy
 *  `PushLightingToEngine`'s registry reads (legacy Win32 UI). `forceAlign`
 *  is the `LightingForceFillAlignment` flag. The host returns canonical
 *  defaults (not the live registry) under `--test-host` so the
 *  dialog-lighting a11y golden stays deterministic. */
export type LightingSettingsDto = {
  sun: LightSettingsDto;
  fill1: LightSettingsDto;
  fill2: LightSettingsDto;
  ambient: Color;
  shadow: Color;
  forceAlign: boolean;
};

// ─── Spawner DTO (Phase 3 Screen 8 Batch 4) ──────────────────────────
//
// Mirrors `SpawnerConfig` at [src/SpawnerDriver.h:18]. Defaults match
// `SpawnerConfig()` (Auto mode, disabled, burst 1, no spacing, 10 s
// interval, origin, 5 s lifetime, no jitter). `enabled` is meaningful
// only in Auto mode (toggle the recurring schedule); Manual mode bursts
// only on explicit `spawner/trigger`.

export type SpawnerMode = "manual" | "auto";

export type SpawnerParamsDto = {
  mode: SpawnerMode;
  enabled: boolean;
  /** 1..MAX_BURST_SIZE (=10) instances per burst. */
  burstSize: number;
  /** 0..MAX_SPACING_SEC (=10) seconds between instances inside a burst. */
  spacingSec: number;
  /** Auto-mode only: 0..MAX_INTERVAL_SEC (=60) seconds between burst
   *  starts. Ignored in Manual mode but always carried through the DTO
   *  so the panel keeps its setting when the user toggles modes. */
  intervalSec: number;
  /** World-space spawn point. */
  position: Vec3;
  /** Initial velocity, units/sec. */
  velocity: Vec3;
  /** Hard cap on each spawned instance's lifetime, 0..MAX_LIFETIME_SEC
   *  (=600). 0 means "no cap — instance lives until particles die
   *  naturally". */
  maxLifetimeSec: number;
  /** Per-axis ± jitter on the spawn position, world units. */
  jitterPosition: Vec3;
  /** Deterministic arc: constant acceleration applied over each
   *  instance's lifetime, units/sec². () */
  acceleration: Vec3;
  /** Smooth squiggle: per-axis peak lateral displacement, world units.
   *  Layered on top of the arc with a per-instance random phase. () */
  squiggleAmplitude: Vec3;
  /** Squiggle frequency in Hz (oscillations/sec), 0..SQUIGGLE_FREQ_MAX
   *  (=20), shared across axes. () */
  squiggleFrequency: number;
};

// ─── Emitter tree DTO (Phase 3 Screen 8 Batch 4 + Screen 4 Batch A) ──
//
// Shape used by both `emitters/list` (live tree) and
// `emitters/preview-from-file` (import preview). Screen 4 Batch A
// extends with role + link-group + visibility so the sidebar tree can
// render glyphs and link-group dots without an additional round-trip.
//
// `role` identifies the slot the emitter occupies in its parent:
//   - "root"     : top-level (no parent). The synthetic id=-1 wrapper
//                  also uses this role.
//   - "lifetime" : parent->spawnDuringLife points at this emitter
//                  (continuous spawning during parent's lifetime).
//   - "death"    : parent->spawnOnDeath points at this emitter (spawned
//                  once when the parent particle dies).
//
// `linkGroup` mirrors `ParticleSystem::Emitter::linkGroup` (). 0
// means unlinked; non-zero IDs are stable within a single system.
//
// `visible` mirrors `ParticleSystem::Emitter::visible` — an editor-only
// per-emitter visibility toggle.

export type EmitterRole = "root" | "lifetime" | "death";

// Spawn-quantity params mirrored onto every tree node (chain-load
// warning). Field names match EmitterPropertiesDto exactly so the host and
// the mock copy them verbatim; the consumer is estimateChainLoad() in
// web/apps/editor/src/lib/chain-load.ts.
export type SpawnParamsDto = Pick<
  EmitterPropertiesDto,
  | "lifetime"
  | "useBursts"
  | "nBursts"
  | "burstDelay"
  | "nParticlesPerSecond"
  | "nParticlesPerBurst"
>;

// All-zero spawn for synthetic roots and test fixtures that don't care
// about chain-load (estimate = 0 → never warns).
export const ZERO_SPAWN: Readonly<SpawnParamsDto> = Object.freeze({
  lifetime: 0,
  useBursts: false,
  nBursts: 0,
  burstDelay: 0,
  nParticlesPerSecond: 0,
  nParticlesPerBurst: 0,
});

export type EmitterTreeNode = {
  id: number;
  // Stable per-emitter identity (reorder glide). `id` is a POSITIONAL index
  // that reshuffles on every structural change; `stableId` is a monotonic
  // handle assigned at emitter creation, stable across reorder/reparent —
  // the React tree keys rows by it so an order change MOVES (FLIP-animates)
  // rows instead of remounting them. Runtime-only (never persisted to .alo);
  // undo/redo rebuilds emitters and therefore issues fresh stableIds.
  stableId: number;
  name: string;
  role: EmitterRole;
  linkGroup: number;
  visible: boolean;
  spawn: SpawnParamsDto;
  children: EmitterTreeNode[];
};

// Imported reference object (a real game/mod object placed in the preview
// as a scale reference). Category mirrors the host's GameObjectCategory
// (GameObjectCatalog.h); the picker groups the Name list by it. Status is the
// lazy .alo probe verdict for the CURRENTLY-selected object — "skinned" and
// "load-failed" mean the object can't render (skinned units are a v1 deferral).
// The profile-classifier axes that drive the picker's collapsible tree.
// `domain` = which top-level section (Heroes go under their own section regardless of
// domain; everything else by Ground/Space). `bucket` = the sub-group within a domain.
export type ReferenceObjectDomain = "Ground" | "Space" | "Unknown";
export type ReferenceObjectRole = "Unit" | "Structure" | "Hero" | "Excluded";
export type ReferenceObjectBucket =
  | "Infantry" | "Vehicle" | "Air"                                           // ground units
  | "Fighter" | "Bomber" | "Corvette" | "Frigate" | "Capital" | "Transport"  // space units
  | "Structure" | "Hero" | "Other" | "None";                                 // shared
export type ReferenceObjectEntry = {
  name: string;
  domain: ReferenceObjectDomain;
  role: ReferenceObjectRole;
  bucket: ReferenceObjectBucket;
  // Raw <Affiliation> (may be a comma list, e.g. "Rebel, Trade_Federation", or empty).
  // Drives the picker's faction filter chips; the picker tokenises it on commas.
  affiliation: string;
};
// "model-missing" = the model file is genuinely absent from the current
// mod/base (distinct from "load-failed" = present but undecodable), so the
// picker can say "model file not found" rather than "couldn't load".
export type ReferenceObjectStatus = "none" | "ok" | "skinned" | "load-failed" | "model-missing";
// Per-slot game-skydome load outcome. "load-failed" = a Name is chosen but
// its .alo wouldn't load (the renderer falls back to solid colour); the picker
// surfaces it instead of silently showing the dome as selected.
export type SkydomeSlotStatus = "none" | "ok" | "load-failed";

export type EngineStateDto = {
  // ─── File / editor-level state ─────────────────────────────────────
  // Phase 3 Screen 8 Batch 3: dirty + current file path live at the top
  // of the DTO so they group with editor-level state (not engine
  // parameters). `currentFilePath` is null when the in-memory particle
  // system has never been saved (untitled). `dirty` is true if any
  // engine mutation has occurred since the last file/new/open/save
  // success — drives the window title's `*` indicator and the
  // SaveChangesPrompt on destructive ops (New / Open / Recent).
  currentFilePath: string | null;
  dirty: boolean;

  // Ground plane
  ground: boolean;                  // GetGround()
  groundZ: number;                  // GetGroundZ()
  groundTexture: number;            // GetGroundTexture() — slot index 0..kGroundTextureCount-1
  groundSolidColor: Color;          // GetGroundSolidColor() — slot kGroundSolidColorSlot colour
  groundColor: Color;               // GetGroundColor() — effective floor colour (solid colour, or the loaded texture's average)
  groundSlotCustomPaths: string[];  // GetGroundSlotCustomPath() across all slots

  // Skydome — legacy bundled/custom texture slot (simple-background fallback)
  skydomeSlot: number;              // GetSkydomeSlot() — 0=Off, 1-8=bundled, 9-11=custom
  skydomeCustomPaths: string[];     // GetSkydomeCustomPath() for slots 9..11

  // Game-faithful skydome environment (real .alo domes by GameObject Name).
  // Empty primary+secondary names => no game dome (the slot above renders instead).
  skydomeContext: "land" | "space";   // GetSkydomeContext()
  skydomePrimaryName: string;         // GetSkydomePrimaryName() — "" = none
  skydomeSecondaryName: string;       // GetSkydomeSecondaryName() — "" = none
  // Per-slot load outcome. "load-failed" => the chosen dome's .alo
  // wouldn't load and the renderer fell back to the solid background.
  skydomePrimaryStatus: SkydomeSlotStatus;    // GetSkydomePrimaryStatus()
  skydomeSecondaryStatus: SkydomeSlotStatus;  // GetSkydomeSecondaryStatus()

  // Imported reference object (scale reference) + unit grid.
  referenceObjectName: string;             // GetReferenceObjectName() — "" = none
  referenceObjectVisible: boolean;         // GetReferenceObjectVisible()
  referenceObjectLocked: boolean;          // IsReferenceLocked() — placement frozen
  referenceObjectPosition: Vec3;           // GetReferencePosition() — world units
  // degrees [yaw,pitch,roll]; the engine is Z-up, so yaw = heading about world
  // up (Z), pitch = tilt about X, roll = bank about Y (yaw applied last).
  referenceObjectRotation: Vec3;           // GetReferenceRotation()
  referenceObjectStatus: ReferenceObjectStatus;  // lazy probe of the selected .alo
  // true while the units/structures catalog is (re)building off the UI thread;
  // the picker shows "Loading objects…" and re-queries its list on the true->false edge.
  referenceCatalogBuilding: boolean;       // IsReferenceCatalogBuilding()
  gridVisible: boolean;                    // GetGridVisible()
  gridSpacing: number;                     // GetGridSpacing() — world units between lines
  snapEnabled: boolean;                    // GetSnapEnabled() — gizmo grid/angle snap

  // Background (solid colour when skydome slot 0)
  background: Color;                // GetBackground()

  // Lights / ambient / shadow
  lights: { sun: LightDto; fill1: LightDto; fill2: LightDto };
  ambient: Vec4;                    // GetAmbient()
  shadow: Vec4;                     // GetShadow()

  // Bloom
  bloom: boolean;                   // GetBloom()
  bloomAvailable: boolean;          // IsBloomAvailable()
  bloomStrength: number;            // GetBloomStrength()
  bloomCutoff: number;              // GetBloomCutoff()
  bloomSize: number;                // GetBloomSize()

  // Leave particles after instance death (Task 2.7). Defaults true —
  // matches ParticleSystem's constructor seed at [ParticleSystem.cpp:956].
  // Read via ParticleSystem::getLeaveParticles(); persisted with the
  // particle system (chunk-serialised at [ParticleSystem.cpp:948]).
  leaveParticles: boolean;

  // Debug
  heatDebug: boolean;               // GetHeatDebug()

  // View state (preview clock)
  paused: boolean;                  // IsPreviewPaused()

  // Camera
  camera: CameraDto;                // GetCamera()

  // Wind / gravity — exposed on Engine but not currently driven by any UI.
  // Included in the DTO so future panels can read them without a schema
  // bump; setters live on Engine but are intentionally not wired to a
  // bridge command yet.
  wind: Vec3;                       // GetWind()
  gravity: Vec3;                    // GetGravity()

  // Spawner (Phase 3 Screen 8 Batch 4). Defaults mirror SpawnerConfig()'s
  // initialiser at [src/SpawnerDriver.h:18] — Auto mode + disabled +
  // burst 1 + 0 s spacing + 10 s interval + origin + 5 s lifetime + no
  // jitter. The native host owns this config (m_spawnerConfig on
  // BridgeDispatcher) for snapshot parity; mutations route through
  // spawner/start.
  spawner: SpawnerParamsDto;

  // Currently-selected emitter id, or null when nothing is selected
  // (Screen 4 Batch A). The full tree itself is fetched via
  // `emitters/list` because trees can be large (hundreds of nodes for
  // complex systems) and shouldn't ride every snapshot; only the
  // (cheap) scalar selection rides here so React derives the selected
  // row styling from the snapshot without an extra round-trip.
  selectedEmitterId: number | null;

  // D6 — currently-active mod's full path on disk, or null when
  // Unmodded. The full mod *list* is fetched separately via
  // `mods/list` because it changes rarely (refresh / disk scan)
  // compared to engine state. The path alone rides every snapshot so
  // the Mods menu's check-mark stays in sync with FileManager's
  // current mod basepath without a second round-trip after a select.
  activeModPath: string | null;

  // Undo / redo availability — drives the Edit menu's Undo/Redo
  // enable-state. Mirrors UndoStack::CanUndo / CanRedo on the host
  // side, plus the head-of-history auto-cap logic in undo/perform
  // (canUndo is true whenever the user has done at least one
  // captureUndo-bearing mutation since the last undo-stack Clear,
  // OR the cursor is mid-redo-branch).
  canUndo: boolean;
  canRedo: boolean;
};

// ─── Mods (D6) ──────────────────────────────────────────────────
//
// One discovered mod under <gameRoot>/Mods/. Shape matches the legacy
// C++ `ModEntry` struct (src/ModManager.h) minus `wstring → string`
// conversion at the bridge boundary.
//
// `nickname` is the user-set display name from the registry under
// HKCU\Software\AloParticleEditor\ModNicknames — empty string if no
// nickname is set; the React menu falls back to `folderName` in that
// case.
//
// `isFoC` discriminates Forces of Corruption (under corruption/Mods)
// from Base Game (under GameData/Mods). The React menu groups by this
// flag (FoC first, then Base Game) matching the legacy popup layout.
export type ModDescriptor = {
  path: string;
  folderName: string;
  nickname: string;
  isFoC: boolean;
  rootHasArt?: boolean;   // mod root itself carries Data\Art (optional; default false)
};

// A stackable content layer (a top-level mod with Data\Art, or a nested folder).
export type LayerRef = {
  path: string;            // absolute, slash-free
  label: string;
  parentLabel?: string;    // present for kind === "nested"
  parentPath?: string;     // owning mod's absolute slash-free path; present for kind === "nested"
  isFoC: boolean;
  kind: "mod" | "nested";
};

// ─── Viewport input (Phase 2) ────────────────────────────────
//
// Discriminated union carried by the `viewport/input` Request kind.
// The host's InputDispatcher switches on `type` and PostMessages the
// corresponding Win32 message to the (hidden) viewport popup HWND.
//
// Coordinates (`x`, `y`) are popup-client physical pixels, which equals
// main-client CSS coords × `devicePixelRatio` at event time. The
// popup spans the full main client per T4c.4 so client-of-popup ≡
// client-of-main; the renderer doesn't need to know the popup's screen
// position.
//
// `buttons` is a Win32 MK_* bitmask reassembled from the DOM event:
//   MK_LBUTTON = 0x0001, MK_RBUTTON = 0x0002, MK_SHIFT = 0x0004,
//   MK_CONTROL = 0x0008, MK_MBUTTON = 0x0010
// The renderer reads `event.buttons` (DOM 1/2/4 bits for L/R/M) plus
// `event.shiftKey/ctrlKey` and reassembles MK_*. The engine's WNDPROC
// reads modifiers exclusively from these bits — never from
// GetAsyncKeyState — so the bridge-payload bitmask is the source of
// truth even though the HWND is hidden.
//
// `deltaY` is WHEEL_DELTA units (120 per notch, positive = away-from-
// user / "zoom in" per the existing handler at HostWindow.cpp:1350).
// The renderer normalises DOM `WheelEvent.deltaY` to this convention
// (DOM is opposite-sign; multiply by -1 and quantise to ±120 per
// notch).
//
// `vk` is a Win32 virtual-key code (e.g. VK_SHIFT=16). Renderer uses
// `event.keyCode` which is legacy but still populated for every key
// the engine could care about. `repeat` is `event.repeat` — the host
// stamps lParam bit-30 from this so the engine's WM_KEYDOWN repeat
// filter at HostWindow.cpp:1296 works unchanged.
//
// `blur` corresponds to `window.blur` — the host posts WM_KILLFOCUS
// to the popup so the defensive cursor-bound-spawn cleanup at
// HostWindow.cpp:1325 runs when the user Alt-Tabs away mid-Shift-hold.
export type ViewportInputEvent =
  | { type: "mousemove"; x: number; y: number; buttons: number }
  | { type: "mousedown" | "mouseup"; button: "left" | "right" | "middle";
      x: number; y: number; buttons: number }
  | { type: "wheel"; x: number; y: number; deltaY: number; buttons: number }
  | { type: "keydown" | "keyup"; vk: number; repeat: boolean }
  | { type: "blur" };

// ─── Track DTO (Phase 3 Screen 6 Batch A) ────────────────────────────
//
// Per-emitter animation curves. The native `Emitter::tracks[7]` slot
// (one Track* per channel) maps to a fixed-order JSON array on the
// wire: Red, Green, Blue, Alpha, Scale, Index, RotationSpeed. The
// names are intrinsic to the array position — `TRACK_NAMES[i]` is
// authoritative for both sides of the bridge.
//
// `keys` are sorted ascending by time. Time is in 0..100 (matches
// legacy `CurveEditor_SetHorzRange(hEditor, 0.0f, 100.0f, true)` at
// [src/UI/TrackEditor.cpp:58]). Value range is per-track and computed
// React-side, not on the wire — the schema carries raw values.
//
// `interpolation` maps the native enum:
//   IT_LINEAR (0) → "linear"
//   IT_SMOOTH (1) → "smooth"
//   IT_STEP   (2) → "step"
// IT_UNKNOWN (-1) is never serialised — the host coerces it to
// "linear" before sending.
export type InterpolationType = "linear" | "smooth" | "step";

export type TrackKey = {
  time: number;
  value: number;
};

export type TrackName =
  | "red"
  | "green"
  | "blue"
  | "alpha"
  | "scale"
  | "index"
  | "rotationSpeed";

export type TrackDto = {
  name: TrackName;
  keys: TrackKey[];
  interpolation: InterpolationType;
  /** Pointer-equality lock target. When this channel's
   *  `emit->tracks[i]` aliases another channel's `trackContents[j]`,
   *  the two share key data — editing the locked channel is
   *  disabled and changes to the target channel propagate
   *  automatically. `null` (the common case) means the channel
   *  owns its own keys. Only the first four channels
   *  (red/green/blue/alpha) participate in locking; the engine
   *  forbids locking for scale/index/rotationSpeed regardless of
   *  what's sent. Per the legacy semantic at
   *  [TrackEditor.cpp:90-110](src/UI/TrackEditor.cpp:90) and the
   *  pointer-identity model in
   *  [ParticleSystem.cpp:428](src/ParticleSystem.cpp:428). */
  lockedTo: TrackName | null;
};

/** Fixed-order names for the 7 tracks. Index matches the native
 *  `Emitter::tracks[i]` slot, which is also the order the
 *  `emitters/get-tracks` wire response uses. Single source of truth
 *  for both the host serialiser and React-side label/picker lookups. */
export const TRACK_NAMES: readonly TrackName[] = Object.freeze([
  "red",
  "green",
  "blue",
  "alpha",
  "scale",
  "index",
  "rotationSpeed",
]);

// ─── Emitter properties DTO (Phase 4.1 Fix dispatch 1) ──────────────
//
// Mirrors every editable field on `ParticleSystem::Emitter`
// ([src/ParticleSystem.h:71-204]). Grouped by the UI tab that surfaces
// the field (Basic / Appearance / Physics) so a reviewer can see at a
// glance what's wired where. Fix dispatch 1 wires the Basic group to
// `EmitterPropertyTabs`; Appearance + Physics ride this DTO so dispatches
// 2 and 3 add only UI, not schema.
//
// `groups: GroupDto[]` mirrors the C array `Group groups[NUM_GROUPS]`
// (NUM_GROUPS = 3). Per-Group fields match the `#pragma pack(1)` struct
// in `ParticleSystem::Emitter::Group`. The position-vec triples (`minX/
// minY/minZ`, etc.) collapse into `Vec3` on the wire so the JS side
// doesn't have to spell out each axis. The `type` field is the engine
// enum index (`GT_EXACT` / `GT_BOX` / `GT_CUBE` / `GT_SPHERE` /
// `GT_CYLINDER`); kept as a number rather than a string union because
// the UI surfaces it as a Radix Select with numeric values matching the
// engine enum.

export type GroupDto = {
  type: number;                  // ParticleSystem::GT_* (0..4)
  min: Vec3;                     // (minX, minY, minZ)
  max: Vec3;                     // (maxX, maxY, maxZ)
  sideLength: number;
  sphereRadius: number;
  sphereEdge: number;
  cylinderRadius: number;
  cylinderEdge: number;
  cylinderHeight: number;
  val: Vec3;                     // (valX, valY, valZ)
};

export type EmitterPropertiesDto = {
  // ── Basic ── ([ParticleSystem.h:140] name, :154 linkToSystem,
  // :162 randomRotation, :165 useBursts, :168 lifetime, :169 initialDelay,
  // :170 burstDelay, :175 randomLifetimePerc, :174 randomScalePerc,
  // :178 parentLinkStrength, :180-181 randomRotationAverage/Variance,
  // :184-185 freezeTime/skipTime, :188 nBursts, :189 index,
  // :192 nParticlesPerSecond, :194 nParticlesPerBurst).
  name: string;
  lifetime: number;
  initialDelay: number;
  useBursts: boolean;
  nBursts: number;
  burstDelay: number;
  nParticlesPerBurst: number;
  nParticlesPerSecond: number;
  randomLifetimePerc: number;
  randomScalePerc: number;
  randomRotation: boolean;
  randomRotationDirection: boolean;
  randomRotationAverage: number;
  randomRotationVariance: number;
  freezeTime: number;
  skipTime: number;
  linkToSystem: boolean;
  parentLinkStrength: number;
  index: number;

  // ── Appearance ── ([ParticleSystem.h:141-142 colorTexture/
  // normalTexture, :156 doColorAddGrayscale, :157 affectedByWind,
  // :158 isHeatParticle, :160 hasTail, :161 noDepthTest,
  // :164 isWorldOriented, :177 tailSize, :182 randomColors,
  // :190 blendMode, :191 textureSize, :193 nTriangles).
  colorTexture: string;
  normalTexture: string;
  blendMode: number;
  textureSize: number;
  nTriangles: number;
  doColorAddGrayscale: boolean;
  randomColors: Vec4;
  hasTail: boolean;
  tailSize: number;
  isHeatParticle: boolean;
  isWorldOriented: boolean;
  noDepthTest: boolean;
  affectedByWind: boolean;

  // ── Physics ── ([ParticleSystem.h:155 objectSpaceAcceleration,
  // :159 isWeatherParticle, :166 emitFromMesh, :167 gravity,
  // :171 inwardSpeed, :172 inwardAcceleration, :173 acceleration,
  // :176 weatherCubeSize, :179 weatherCubeDistance, :183 bounciness,
  // :186 emitFromMeshOffset, :187 weatherFadeoutDistance,
  // :195 groundBehavior). `groups` is :145.
  acceleration: Vec3;
  gravity: number;
  inwardSpeed: number;
  inwardAcceleration: number;
  objectSpaceAcceleration: boolean;
  bounciness: number;
  groundBehavior: number;
  emitFromMesh: number;
  emitFromMeshOffset: number;
  isWeatherParticle: boolean;
  weatherCubeSize: number;
  weatherCubeDistance: number;
  weatherFadeoutDistance: number;

  groups: GroupDto[];
};

// ============================================================================
// Other DTOs (expanded in later tasks)
// ============================================================================

export type EmitterPatchDto = Record<string, unknown>;  // expanded later

// `EmitterTreeDto` is the wire shape returned by `emitters/list`. Until
// Screen 4 fleshes out the per-emitter field set, it's a thin wrapper
// over the minimal `EmitterTreeNode` (one root node + descendants).
export type EmitterTreeDto = { root: EmitterTreeNode };

// ============================================================================
// Requests: JS → host
// ============================================================================

// One entry in the frequently-used texture palette. `slotMask` is the
// raw C++ bit-flag (SLOT_COLOR=1, SLOT_BUMP=2) recording which slots the
// texture has been used as. sub-feature B]
export type PaletteEntry = { filename: string; pinned: boolean; slotMask: number };

// Autosave crash-recovery candidate (). Returned by
// `autosave/check-recovery` when a crashed prior session left an orphaned
// autosave under %TEMP%\AloParticleEditor\. At least one tier is present.
// Mtimes are epoch-ms so React can render "N minutes ago" without a wire
// format dependency.
export type AutosaveOrphan = {
  originalFilename: string;        // "" = untitled ("Unsaved new file")
  recentMtimeMs: number | null;    // recent-tier file mtime, null if no recent file
  stableMtimeMs: number | null;    // stable-tier file mtime, null if no stable file
};

export type Request =
  // File / recents
  | { kind: "file/new";                   params: Record<string, never> }
  | { kind: "file/open";                  params: { path?: string; filter?: "alo" | "skydome" | "ground" } }   // path undef = native picker; filter selects lpstrFilter (default "alo")
  | { kind: "file/save";                  params: { path?: string } }   // path undef = native picker
  | { kind: "file/save-as";               params: Record<string, never> } // always opens native picker
  | { kind: "file/recent/list";           params: Record<string, never> }

  // Texture browse — host-side native file dialog for an emitter's
  // color/bump texture. Opens in the active mod's texture folder,
  // filtered to *.tga;*.dds; returns the chosen file's basename (or ""
  // if cancelled). `slot` drives only the dialog title (and future
  // palette usage-tracking). feature-parity A]
  | { kind: "textures/browse";            params: { slot: "color" | "bump" } }

  // Frequently-used texture palette — per-mod pinned + recent textures
  // backed by the C++ TexturePalette::Store. `list` applies the
  // slot-aware filter default (and persists it); `thumbnail` decodes a
  // texture to a base64 PNG data URI (null = missing/broken);
  // `toggle-pin` flips pinned state (rejects when pins are full);
  // `touch-recent` records a texture as used in the given slot.
  // No active mod ⇒ list is empty and mutations are no-ops. sub-feature B]
  | { kind: "textures/palette/list";         params: { slot: "color" | "bump" } }
  | { kind: "textures/palette/thumbnail";    params: { filename: string } }
  | {
      kind: "textures/get-preview";
      // bare basename as stored in EmitterDto.colorTexture; host prepends
      // Data\Art\Textures\ and forces a .DDS swap (no .tga fallback — spec §7).
      // flattenAlpha (default true on the host) forces every pixel fully opaque
      // (alpha=255) so ADDITIVE frames (RGB content, alpha ~0) are visible; pass
      // false to honor the texture's real alpha.
      params: { filename: string; flattenAlpha?: boolean };
    }
  | { kind: "textures/palette/toggle-pin";   params: { filename: string } }
  | { kind: "textures/palette/touch-recent"; params: { filename: string; slot: "color" | "bump" } }

  // Engine state — full snapshot
  | { kind: "engine/state/snapshot";      params: Record<string, never> }

  // Engine setters — ground
  | { kind: "engine/set/ground";              params: { enabled: boolean } }
  | { kind: "engine/set/ground-z";            params: { z: number } }
  | { kind: "engine/set/ground-texture";      params: { slot: number } }
  | { kind: "engine/set/ground-solid-color";  params: { rgb: Color } }
  | { kind: "engine/set/ground-slot-custom-path"; params: { slot: number; path: string } }

  // Engine setters — skydome / background
  | { kind: "engine/set/skydome-slot";        params: { slot: number } }
  | { kind: "engine/set/skydome-custom-path"; params: { slot: number; path: string } }
  // Select real game/mod domes by GameObject Name for a battle context.
  // Empty names clear the corresponding slot.
  | { kind: "engine/set/skydome-environment"; params: { context: "land" | "space"; primaryName: string; secondaryName: string } }
  | { kind: "engine/set/background";          params: { rgb: Color } }

  // Imported reference object + unit grid.
  // `reference-object` selects by in-game Name ("" clears it). `-transform`
  // carries world position + rotation (degrees [yaw,pitch,roll]; Z-up engine, so
  // yaw = heading about world Z). Grid spacing is world units between lines (host
  // clamps to > 0).
  | { kind: "engine/set/reference-object";           params: { name: string } }
  | { kind: "engine/set/reference-object-visible";   params: { visible: boolean } }
  | { kind: "engine/set/reference-object-lock";      params: { locked: boolean } }
  | { kind: "engine/set/reference-object-transform"; params: { position: Vec3; rotation: Vec3 } }
  | { kind: "engine/set/grid-visible";               params: { visible: boolean } }
  | { kind: "engine/set/grid-spacing";               params: { spacing: number } }
  | { kind: "engine/set/snap-enabled";               params: { enabled: boolean } }

  // Engine setters — bloom
  | { kind: "engine/set/bloom";               params: { enabled: boolean } }
  | { kind: "engine/set/bloom-strength";      params: { v: number } }
  | { kind: "engine/set/bloom-cutoff";        params: { v: number } }
  | { kind: "engine/set/bloom-size";          params: { v: number } }

  // Engine setters — leave particles after instance death (Task 2.7).
  // Mirrors ParticleSystem::setLeaveParticles / getLeaveParticles
  // ([src/ParticleSystem.h:343,347]). When true (default), particles
  // continue to live after their owning instance is killed (the
  // instance just stops spawning); when false the engine destroys
  // the instance + its remaining particles immediately. Honored by
  // Engine::KillParticleSystem at [src/engine.cpp:197].
  | { kind: "engine/set/leave-particles";     params: { enabled: boolean } }

  // Engine setters — debug / camera / lighting
  | { kind: "engine/set/heat-debug";          params: { enabled: boolean } }
  | { kind: "engine/set/camera";              params: CameraDto }
  | { kind: "engine/set/light";               params: { which: LightWhich } & LightDto }
  | { kind: "engine/set/ambient";             params: { color: Vec4 } }
  | { kind: "engine/set/shadow";              params: { color: Vec4 } }

  // Engine setters — view state (preview clock)
  | { kind: "engine/set/paused";              params: { paused: boolean } }
  | { kind: "engine/set/overload-guard";      params: { enabled: boolean; maxParticles: number } }
  | { kind: "engine/set/msaa-level";          params: { level: number } }
  // [model-shadows] "Model shadows" view preference — casts the game's stencil
  // shadow for the reference model onto the ground (and self-shadows it).
  // View-only; persisted web-side, re-applied at startup.
  | { kind: "engine/set/model-shadows";       params: { enabled: boolean } }
  // [soft-shadows] "Soft shadows" view preference — blurs the stencil shadow
  // edges to match the game's soft-shadow rendering.
  // View-only; persisted web-side, re-applied at startup.
  | { kind: "engine/set/soft-shadows";        params: { enabled: boolean } }
  // [hard-guard] Estimated alive particles per placed instance, pushed by
  // the web (chain-load.ts). The engine multiplies by its live placed-
  // instance count for the preemptive overload gate.
  | { kind: "engine/set/estimated-load";      params: { perInstance: number } }

  // T9] Test-only knob: suppress the 4 Hz stats/tick emission
  // AND tell React's StatusBar to clear its local state via
  // stats/frozen-changed. Used by a11y spec beforeEach for deterministic
  // UIA goldens. `frozen` defaults to true on the host if omitted.
  | { kind: "stats/set-frozen";               params: { frozen: boolean } }

  // Engine actions
  | { kind: "engine/action/clear";            params: Record<string, never> }
  | { kind: "engine/action/reload-shaders";   params: Record<string, never> }
  | { kind: "engine/action/reload-textures";  params: Record<string, never> }
  | { kind: "engine/action/on-particle-system-changed"; params: { track: number } }
  | { kind: "engine/action/step-frames";      params: { frames: number } }
  | { kind: "engine/action/rescale-system";   params: { durationScalePercent: number; sizeScalePercent: number } }

  // Engine queries
  | { kind: "engine/query/ground-slot-empty";  params: { slot: number } }
  | { kind: "engine/query/skydome-slot-empty"; params: { slot: number } }
  // Enumerate selectable game-dome Names for a battle context (both the
  // primary and secondary lists), read from the game/mod's *Skydomes.xml.
  | { kind: "engine/query/skydome-list";       params: { context: "land" | "space" } }
  // Enumerate selectable game objects (Name + category) for the reference-
  // object picker, read from the active mod/base via GameObjectFiles.xml.
  | { kind: "engine/query/reference-object-list"; params: Record<string, never> }
  | { kind: "engine/query/bloom-available";    params: Record<string, never> }
  | { kind: "engine/query/msaa-levels";        params: Record<string, never> }

  // Settings (cross-mode registry persistence). Legacy persists lighting
  // under HKCU\Software\AloParticleEditor; the new UI reads it back so
  // settings round-trip between the two UIs.
  //   - `settings/lighting` (get) returns the full raw lighting split
  //     (intensity/colour kept separate, angles in degrees) the panel
  //     seeds its displayed controls from — incl. the Force Align flag.
  //   - `settings/lighting-force-align/set` writes just the
  //     `LightingForceFillAlignment` REG_DWORD on toggle.
  //   - `settings/lighting/set` writes the FULL raw split back to the
  //     registry (all 16 light values + the flag) so the new UI's edits
  //     and its Reset persist — otherwise a value the legacy dialog wrote
  //     (e.g. a stale ambient) reappears on every reopen/restart and the
  //     new UI can never overwrite it. Same key names as the GET above.
  // Host returns canonical defaults (get) / no-ops the writes (set) under
  // `--test-host` so the dialog-lighting a11y golden stays deterministic —
  // unless ALO_SETTINGS_LIVE is set (the test seam that drives the real
  // registry over CDP without disturbing the a11y harness).
  | { kind: "settings/lighting";                 params: Record<string, never> }
  | { kind: "settings/lighting-force-align/set"; params: { enabled: boolean } }
  | { kind: "settings/lighting/set";             params: LightingSettingsDto }

  // Emitters (Phase 3+)
  | { kind: "emitters/list";              params: Record<string, never> }
  | { kind: "emitters/select";            params: { id: number | null } }
  | { kind: "emitters/update";            params: { id: number; patch: EmitterPatchDto } }
  | { kind: "emitters/import-from-file";  params: { path: string; selected: number[] } }
  | { kind: "emitters/preview-from-file"; params: { path: string } }

  // Track read (Phase 3 Screen 6 Batch A). Read-only this batch; key
  // mutations land with Screen 5 / Screen 6 Batch B.
  | { kind: "emitters/get-tracks";        params: { id: number } }

  // Emitter property read/write (Phase 4.1 Fix dispatch 1). Single
  // round-trip read returns the full property DTO for `id`. Writes use
  // a JSON patch object — only the keys actually present in `patch` are
  // applied to the engine, so the React form can fire one field at a
  // time without sending the whole DTO each commit.
  | { kind: "emitters/get-properties";    params: { id: number } }
  | { kind: "emitters/set-properties";
      params: { id: number; patch: Partial<EmitterPropertiesDto> } }

  // Track mutations (Phase 3 Screen 5 / Screen 6 Batch B-α). Border
  // keys (first + last in time order) are silently skipped server-side
  // by `delete-track-keys` — they define the track's time range and
  // are not deletable per legacy semantics. The React UI filters them
  // out before calling for cleanliness; the host enforces.
  | { kind: "emitters/delete-track-keys";
      params: { id: number; track: TrackName; times: number[] } }
  | { kind: "emitters/set-track-interpolation";
      params: { id: number; track: TrackName; interpolation: InterpolationType } }

  // Per-channel track lock — point `emit->tracks[channel]` at another
  // channel's `trackContents`, making the locked channel a read-only
  // alias whose keys auto-track the target's. `lockTo: null` restores
  // the channel to its own trackContents (unlock). The engine only
  // honours locking for red/green/blue/alpha and only "later-channel
  // locks to earlier-channel" pairs (mirrors legacy
  // [TrackEditor.cpp:90-110](src/UI/TrackEditor.cpp:90)); other
  // combinations are silently rejected with an ok response.
  | { kind: "emitters/set-track-lock";
      params: { id: number; channel: TrackName; lockTo: TrackName | null } }

  // Track key mutations (Phase 3 Screen 6 Batch B-β).
  //
  // `set-track-key` moves an existing key. The host erases the key at
  // `oldTime` and inserts a new key at `(newTime, newValue)`. Border
  // keys (first + last by time) silently override `newTime = oldTime`
  // so only the value moves — matches the drag-time-fixed rule.
  //
  // `add-track-key` inserts a new key. If a key already exists at the
  // exact `time`, the host bumps `time` slightly so the multiset
  // doesn't accumulate dupes. Returns the *actual* inserted time so
  // the React side can auto-select the new key without a re-fetch.
  | { kind: "emitters/set-track-key";
      params: { id: number; track: TrackName; oldTime: number; newTime: number; newValue: number } }
  | { kind: "emitters/add-track-key";
      params: { id: number; track: TrackName; time: number; value: number } }

  // Emitter mutations (Phase 3 Screen 4 Batch B1)
  | { kind: "emitters/duplicate";                       params: { id: number } }
  | { kind: "emitters/duplicate-many";                  params: { ids: number[] } }   // batch: duplicate each; response.newIds are the copies, aligned to input order
  | { kind: "emitters/delete";                          params: { id: number } }
  | { kind: "emitters/rename";                          params: { id: number; name: string } }
  | { kind: "emitters/duplicate-with-index-increment";  params: { id: number; delta: number } }

  // Emitter mutations (Phase 3 Screen 4 Batch B2)
  //
  // add-lifetime-child / add-death-child wrap
  // `ParticleSystem::addLifetimeEmitter` / `::addDeathEmitter`. The new
  // emitter inherits the parent's spawn slot (lifetime or death). When
  // the slot is already filled the host responds with `newId: -1`; the
  // React side disables the menu item before reaching that path so the
  // negative-id branch is defensive coverage.
  //
  // move reorders the emitter among its siblings. Practically a root-
  // only operation per legacy semantics (children of the same role can't
  // be swapped — there's at most one of each). The host refuses bad
  // moves silently; the React side disables the menu item at the edges.
  //
  // linkGroups/set-membership operates on a batch of ids:
  //   - `groupId === null` or `0`: leave / clear membership (set to 0).
  //   - `groupId > 0`: join the named group.
  //   - `groupId === -1`: sentinel for "create a new group" — host picks
  //     the smallest unused positive uint32_t.
  | { kind: "emitters/add-lifetime-child";  params: { parentId: number } }
  | { kind: "emitters/add-death-child";     params: { parentId: number } }
  // Phase 4.1 Fix dispatch 5: legacy "New Root Emitter" menu item.
  // Wraps `ParticleSystem::addRootEmitter()` (parameter-less; the
  // optional template overload isn't exposed across the wire). Always
  // succeeds at the engine level — returns `newId: -1` only when the
  // particle-system pointer is unavailable (shouldn't happen in
  // practice).
  | { kind: "emitters/add-root";            params: Record<string, never> }
  | { kind: "emitters/move";                params: { id: number; direction: "up" | "down" } }
  | { kind: "emitters/move-many";           params: { ids: number[]; direction: "up" | "down" } }   // batch: move the selected roots as a block; response.newIds follow them
  | { kind: "emitters/reorder-many";        params: { ids: number[]; rootIndex: number } }   // batch drag-reorder: move selected roots to land contiguous at gap rootIndex; response.newIds follow them (input order)
  // (Group A polish): visibility ops for the EmitterTree panel
  // toolbar. `set-visible` flips a single emitter's `visible` flag
  // without touching its children (matches legacy
  // `EmitterList_ToggleEmitterVisibility`). `set-all-visible` walks
  // the whole tree (matches legacy `EmitterList_SetAllEmitterVisibility`).
  // Both emit `emitters/tree/changed` + `engine/state/changed`.
  | { kind: "emitters/set-visible";         params: { id: number; visible: boolean } }
  | { kind: "emitters/set-all-visible";     params: { visible: boolean } }
  | { kind: "linkGroups/set-membership";    params: { ids: number[]; groupId: number | null } }

  // Emitter drag/drop (Phase 3 Screen 4 Batch B3)
  //
  // Tagged-union to keep the two semantics cleanly separated. React side
  // computes `slot` and `rootIndex` before calling — the bridge never
  // carries "auto" or "any". Refusal paths (cycle, slot-full, source not
  // a root for reorder) return `{ ok: false; error: string }`.
  //
  // `rootIndex` follows `ParticleSystem::moveEmitterToRootIndex`'s gap
  // semantics: gap 0 = before first root, gap K = between roots K-1 and
  // K, gap N = after the last root. The engine refuses no-op gaps
  // (sourceRootIdx and sourceRootIdx+1) silently as `ok: false`.
  | { kind: "emitters/drop";
      params:
        | { mode: "reorder";  id: number; rootIndex: number }
        | { mode: "reparent"; id: number; targetId: number; slot: "lifetime" | "death" }
    }

  // Emitter clipboard (Phase 3 Screen 4 Batch C)
  //
  // Process-local clipboard. The host serialises selected emitters
  // (with their subtrees) into an in-memory byte buffer using the
  // existing `MemoryFile` + `Emitter::write(writer, copy=true)` +
  // `Emitter(ChunkReader&)` pattern (same as import / Batch B1
  // duplicate). The buffer survives across copy → paste calls on the
  // same process — no cross-instance sharing (matches legacy).
  //
  //   - `emitters/copy { ids }`  serialises each named emitter (plus
  //     its subtree) and stashes the result. No tree mutation, no
  //     dirty flag.
  //   - `emitters/cut { ids }`   copy semantics, then deletes each
  //     emitter atomically (single undo capture + single tree-changed
  //     event). Descending-id delete order keeps prior indices valid
  //     during the loop.
  //   - `emitters/paste { afterId? }` deserialises the clipboard
  //     buffer as new root emitters. `afterId` (optional) names the
  //     root to insert after; omitted/null = append at the end of
  //     roots. Returns the new ids in insertion order.
  | { kind: "emitters/copy";   params: { ids: number[] } }
  | { kind: "emitters/cut";    params: { ids: number[] } }
  | { kind: "emitters/paste";  params: { afterId?: number } }

  //   - `emitters/paste-as-child { parentId, slot }` deserialises the
  //     FIRST clipboard buffer and attaches it into the parent's
  //     lifetime/death child slot (legacy Paste As ▸). Refused
  //     (newId -1) when the slot is filled or the clipboard is empty.
  //     One emitter per slot — a multi-buffer clipboard pastes only
  //     buffer[0].
  | { kind: "emitters/paste-as-child"; params: { parentId: number; slot: "lifetime" | "death" } }

  // Per-emitter rescale (Phase 3 Screen 4 Batch B1 — Screen-8 sub-dialog)
  | { kind: "engine/action/rescale-emitter";  params: { id: number; durationScalePercent: number; sizeScalePercent: number } }

  // Link-group exempt-set CRUD (Phase 3 Screen 4 Batch B1 — surface)
  | { kind: "linkGroups/list-exempt-fields";   params: { groupId: number } }
  | { kind: "linkGroups/set-exempt-fields";    params: { groupId: number; fields: string[] } }
  | { kind: "linkGroups/reset-exempt-fields";  params: { groupId: number } }

  // join-conflict preview (read-only). Given the SAME params
  // `set-membership` would receive, return — per joining emitter — the
  // list of non-exempt fields that disagree with the value a join would
  // overwrite them to, so the UI can warn before clobbering. Mirrors the
  // host exactly: for `groupId > 0` the canonical is the group's first
  // member and the exempt set is the group's; for `groupId === -1` (new
  // group) the canonical is the first id and the exempt set is defaults;
  // for `0`/`null` (leave) there's nothing to overwrite → no conflicts.
  // Pure read — no mutation, no undo capture, no events.
  | { kind: "linkGroups/diff-membership";      params: { ids: number[]; groupId: number | null } }

  // Read-only preview for the settings dialog: given a PROPOSED exempt set
  // for an existing group, which members would be overwritten to the
  // canonical (first-in-tree-order) value when a now-exempt field becomes
  // shared. Drives the inline disagreement warning before set-exempt-fields
  // resolves it. Pure read — no mutation, no undo, no events.
  | { kind: "linkGroups/diff-exempt-change";   params: { groupId: number; exempt: string[] } }

  // Undo / spawner / layout / accelerators
  | { kind: "undo/perform";               params: { direction: "undo" | "redo" } }
  | { kind: "layout/viewport-rect";       params: { x: number; y: number; w: number; h: number } }
  // B1.4 T4c: under the popup-spans-window architecture
  // (popup HWND occupies the WebView's main-row area at all times),
  // splitter drag no longer resizes the popup. Instead, the centre-
  // quadrant rect — the visible "scene rect" inside the popup — is
  // updated via this message. AlphaCompositor stamps `alpha=0` for
  // the four bands outside this rect, so panels behind the popup's
  // alpha-zero regions show through and receive their own mouse
  // events (WS_EX_LAYERED + UpdateLayeredWindow(ULW_ALPHA) makes
  // alpha-zero areas transparent for both rendering AND hit-testing).
  // Camera frustum is unchanged — D3D9 still renders at full popup
  // backbuffer size with popup aspect; the scene rect is purely a
  // compositor-level mask. Engine::Reset is NOT triggered by this
  // message (that path stays bound to layout/viewport-rect, which
  // now updates only on window resize, not splitter drag).
  // Coords in MAIN-HWND-CLIENT space (DPR-multiplied), same as
  // layout/viewport-rect.
  | { kind: "layout/scene-rect";          params: { x: number; y: number; w: number; h: number } }
  // Item 3 (dock-slide viewport stutter): ONE-SHOT dock-slide animation.
  // The per-frame layout/scene-rect stream is clumpy/gappy on the emit side,
  // and the uncapped host render loop samples it at irregular Δt → the
  // viewport's cropped edge juddered against the browser-smooth panel. Instead
  // the web sends ONE of these at toggle time and the host re-renders the
  // engine at a wall-clock-lerped rect every frame, synced to the CSS
  // flex-grow tween (matched cubic-bezier + back-dated QPC clock).
  // `from`/`to` are scene rects in device px (same space as layout/scene-rect);
  // `msElapsedAtSend` = ms since the rAF commit where the panel flex actually
  // changed, so the host can pin its curve to the CSS origin across the IPC
  // hop. No-op when the host has no DComp compositor attached (the
  // scene-rect animation runs on the DComp path).
  | {
      kind: "animate-scene-rect";
      params: {
        from: { x: number; y: number; w: number; h: number };
        to: { x: number; y: number; w: number; h: number };
        durationMs: number;
        easing: string;
        msElapsedAtSend: number;
      };
    }
  // Push the current theme background colour to the host's composition
  // backing. In the DComp tree is [backing, engine, webview]; the
  // engine visual is clipped to the scene rect, so every transparent DOM
  // region OUTSIDE that rect (panel gaps, splitter seams, rounded-corner
  // wedges) composites over the rearmost backing visual. Painting that
  // backing the app-shell `--bg` makes those regions blend into the shell
  // instead of showing the black host backing. `color` is a CSS colour
  // string as resolved by getComputedStyle — `#rrggbb` or `rgb(r,g,b)`;
  // the host parses both and ignores anything it can't parse. Sent on
  // first paint and on every theme change. No-op in browser (MockBridge)
  // mode.
  | { kind: "host/backing-color";         params: { color: string } }
  // B1.3.1.1: capture the current engine viewport as a base64-encoded
  // PNG. React's Modal calls this on open to grab a frozen snapshot
  // of the engine output, renders the PNG as an <img> portaled into
  // the viewport DOM, and full-occludes the engine popup — so
  // Dialog.Overlay's `backdrop-blur-sm` can blur engine + panels
  // uniformly without any popup-boundary seam. The compositor reads
  // its cached pre-stamp DIB, encodes via GDI+, and returns base64;
  // the host's "no frame yet" path returns an empty string + zero
  // dims so the caller can short-circuit.
  | { kind: "viewport/capture-snapshot";  params: Record<string, never> }
  // Phase 2: renderer-side DOM events on the in-DOM <canvas>
  // are forwarded to the host, which synthesizes the corresponding
  // Win32 message and PostMessages it to the (hidden) viewport popup
  // HWND. The engine's existing viewport WNDPROC consumes the
  // synthetic messages unchanged. Coords are popup-client physical
  // pixels (= main-client CSS × devicePixelRatio at event time).
  // `buttons` is a Win32 MK_* bitmask rebuilt from the DOM event
  // (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_SHIFT | MK_CONTROL).
  // `deltaY` is in WHEEL_DELTA units (120 per notch, +up). `vk` is a
  // Win32 virtual-key code (e.g. VK_SHIFT=16). `blur` corresponds to
  // window.blur — the host posts WM_KILLFOCUS so the engine's
  // defensive cursor-bound-spawn cleanup runs on Alt-Tab.
  | { kind: "viewport/input";             params: ViewportInputEvent }
  | { kind: "spawner/start";              params: SpawnerParamsDto }
  | { kind: "spawner/trigger";            params: Record<string, never> }
  | { kind: "spawner/stop";               params: Record<string, never> }
  // (Group D): host quit. Posts WM_CLOSE to the main HostWindow
  // which falls through DefWindowProc → DestroyWindow → the existing
  // WM_DESTROY cleanup chain. React's File → Exit menu item is the
  // sole caller and gates on the dirty-prompt before dispatching.
  | { kind: "app/quit";                   params: Record<string, never> }
  // (Group D): cascade reset for the View → Reset View Settings
  // menu. Pushes engine defaults for background, ground, bloom,
  // skydome, and lighting in one host-side action (one emit of
  // engine/state/changed at the end). Mirrors the legacy main.cpp
  // which prompts Yes/No and then resets the same surface. The
  // confirmation dialog lives React-side via Radix AlertDialog;
  // the dispatcher's job is purely to apply the defaults.
  | { kind: "engine/action/reset-view-settings"; params: Record<string, never> }
  // D6 / — Mods menu surface. `mods/list` returns the discovered
  // mod catalog (each mod + its nested layers as a flat `layers` list), the
  // ordered `stack`, and `activePath` (= primary layer); the React menu calls
  // it once at mount and again after `mods/refresh`. `mods/refresh` re-scans
  // the on-disk Mods\ directories — returns the same shape as `mods/list` so
  // the caller can replace its local cache atomically. `mods/set-layers`
  // replaces the whole content-layer stack (see below).
  | { kind: "mods/list";                  params: Record<string, never> }
  | { kind: "mods/refresh";               params: Record<string, never> }
  // Set the ordered content-layer stack (absolute paths, front = highest;
  // [] = Unmodded). Replaces mods/select + mods/set-submods. Emits engine/state/changed
  // and persists to HKCU\Software\AloParticleEditor\LastLayers.
  | { kind: "mods/set-layers";            params: { paths: string[] } }

  // Autosave crash-recovery (). React-initiated on mount (no host→React
  // startup event, so there's no fire-before-subscribe race): `check-recovery`
  // scans for an orphaned autosave from a crashed prior session and returns it
  // (or null — also null under --test-host / when a CLI file was given);
  // `recover` applies the user's choice (restore a tier, or discard) and
  // consumes the orphan either way.
  | { kind: "autosave/check-recovery";    params: Record<string, never> }
  | { kind: "autosave/recover";           params: { choice: "recent" | "stable" | "discard" } }

  | { kind: "register-accelerators";      params: { combos: string[] } };

// One response shape per Request kind.
// Split into two halves to avoid TS2321 "Excessive stack depth comparing
// types" — TypeScript's conditional-type depth checker trips on chains
// longer than ~100 branches. Both halves share the same `never` tail so
// the composed type is `ResponseForA<R> extends never ? ResponseForB<R> : ResponseForA<R>`.
type ResponseForA<R extends Request> =
  // File
  R extends { kind: "file/new" }                  ? Record<string, never> :
  R extends { kind: "file/open" }                 ? { ok: true; path?: string } | { ok: false; error: string } :
  R extends { kind: "file/save" }                 ? { ok: true; path?: string } | { ok: false; error: string } :
  R extends { kind: "file/save-as" }              ? { ok: true; path?: string } | { ok: false; error: string } :
  R extends { kind: "file/recent/list" }          ? { paths: string[] } :
  R extends { kind: "textures/browse" }           ? { filename: string } :

  // Texture palette (sub-feature B)
  R extends { kind: "textures/palette/list" }
    ? { hasMod: boolean; filter: "color" | "bump"; pins: PaletteEntry[]; recents: PaletteEntry[] } :
  R extends { kind: "textures/palette/thumbnail" }
    ? { dataUri: string | null; status: "ok" | "missing" | "broken" } :
  R extends { kind: "textures/get-preview" }
    ? // discriminated on status so illegal states are unrepresentable
      | { status: "ok"; dataUri: string; srcW: number; srcH: number } // srcW/H = ORIGINAL DDS dims (pre-downsample)
      | { status: "missing" }
      | { status: "broken" } :
  R extends { kind: "textures/palette/toggle-pin" }
    ? { ok: true; pinned: boolean } | { ok: false; reason: "pins-full" } :
  R extends { kind: "textures/palette/touch-recent" }
    ? { ok: true } :

  // Mods (D6 /)
  R extends { kind: "mods/list" }    ? { mods: ModDescriptor[]; layers: LayerRef[]; stack: string[]; activePath: string | null } :
  R extends { kind: "mods/refresh" } ? { mods: ModDescriptor[]; layers: LayerRef[]; stack: string[]; activePath: string | null } :
  R extends { kind: "mods/set-layers" } ? { ok: boolean; stack: string[] } | { ok: false; error: string } :

  // Autosave crash-recovery ()
  R extends { kind: "autosave/check-recovery" }   ? { orphan: AutosaveOrphan | null } :
  R extends { kind: "autosave/recover" }          ? Record<string, never> :

  // Engine snapshot
  R extends { kind: "engine/state/snapshot" }     ? EngineStateDto :

  // Engine setters — all return empty object
  R extends { kind: "engine/set/ground" }                  ? Record<string, never> :
  R extends { kind: "engine/set/ground-z" }                ? Record<string, never> :
  R extends { kind: "engine/set/ground-texture" }          ? Record<string, never> :
  R extends { kind: "engine/set/ground-solid-color" }      ? Record<string, never> :
  R extends { kind: "engine/set/ground-slot-custom-path" } ? Record<string, never> :
  R extends { kind: "engine/set/skydome-slot" }            ? Record<string, never> :
  R extends { kind: "engine/set/skydome-custom-path" }     ? Record<string, never> :
  R extends { kind: "engine/set/skydome-environment" }     ? Record<string, never> :
  R extends { kind: "engine/set/reference-object" }           ? Record<string, never> :
  R extends { kind: "engine/set/reference-object-visible" }   ? Record<string, never> :
  R extends { kind: "engine/set/reference-object-lock" }      ? Record<string, never> :
  R extends { kind: "engine/set/reference-object-transform" } ? Record<string, never> :
  R extends { kind: "engine/set/grid-visible" }               ? Record<string, never> :
  R extends { kind: "engine/set/grid-spacing" }               ? Record<string, never> :
  R extends { kind: "engine/set/snap-enabled" }               ? Record<string, never> :
  R extends { kind: "engine/set/background" }              ? Record<string, never> :
  R extends { kind: "engine/set/bloom" }                   ? Record<string, never> :
  R extends { kind: "engine/set/bloom-strength" }          ? Record<string, never> :
  R extends { kind: "engine/set/bloom-cutoff" }            ? Record<string, never> :
  R extends { kind: "engine/set/bloom-size" }              ? Record<string, never> :
  R extends { kind: "engine/set/leave-particles" }         ? Record<string, never> :
  R extends { kind: "engine/set/heat-debug" }              ? Record<string, never> :
  R extends { kind: "engine/set/camera" }                  ? Record<string, never> :
  R extends { kind: "engine/set/light" }                   ? Record<string, never> :
  R extends { kind: "engine/set/ambient" }                 ? Record<string, never> :
  R extends { kind: "engine/set/shadow" }                  ? Record<string, never> :
  R extends { kind: "engine/set/paused" }                  ? Record<string, never> :
  R extends { kind: "engine/set/overload-guard" }          ? Record<string, never> :
  R extends { kind: "engine/set/msaa-level" }              ? Record<string, never> :
  R extends { kind: "engine/set/model-shadows" }           ? Record<string, never> :
  R extends { kind: "engine/set/soft-shadows" }            ? Record<string, never> :
  R extends { kind: "engine/set/estimated-load" }          ? Record<string, never> :
  R extends { kind: "stats/set-frozen" }                   ? Record<string, never> :

  // Engine actions — empty body
  R extends { kind: "engine/action/clear" }                       ? Record<string, never> :
  R extends { kind: "engine/action/reload-shaders" }              ? Record<string, never> :
  R extends { kind: "engine/action/reload-textures" }             ? Record<string, never> :
  R extends { kind: "engine/action/on-particle-system-changed" }  ? Record<string, never> :
  R extends { kind: "engine/action/step-frames" }                 ? Record<string, never> :
  R extends { kind: "engine/action/rescale-system" }              ? Record<string, never> :

  // Engine queries
  R extends { kind: "engine/query/ground-slot-empty" }   ? boolean :
  R extends { kind: "engine/query/skydome-slot-empty" }  ? boolean :
  R extends { kind: "engine/query/skydome-list" }        ? { primary: string[]; secondary: string[] } :
  R extends { kind: "engine/query/reference-object-list" } ? { objects: ReferenceObjectEntry[]; building?: boolean } :
  R extends { kind: "engine/query/bloom-available" }     ? boolean :
  R extends { kind: "engine/query/msaa-levels" }         ? { levels: number[]; current: number } :

  // Settings (cross-mode registry persistence)
  R extends { kind: "settings/lighting" }                 ? LightingSettingsDto :
  R extends { kind: "settings/lighting-force-align/set" } ? Record<string, never> :
  R extends { kind: "settings/lighting/set" }             ? Record<string, never> :

  never;

type ResponseForB<R extends Request> =
  // Emitters
  R extends { kind: "emitters/list" }             ? EmitterTreeDto :
  R extends { kind: "emitters/select" }           ? Record<string, never> :
  R extends { kind: "emitters/update" }           ? Record<string, never> :
  R extends { kind: "emitters/import-from-file" } ? { imported: number } :
  R extends { kind: "emitters/preview-from-file" } ?
    | { ok: true; tree: EmitterTreeNode }
    | { ok: false; error: string } :

  // Track read (Phase 3 Screen 6 Batch A). Always returns 7 tracks in
  // TRACK_NAMES order; an unknown id yields 7 empty tracks rather than
  // an error so the panel can render a "no data yet" stub without
  // special-casing the failure.
  R extends { kind: "emitters/get-tracks" } ? { tracks: TrackDto[] } :

  // Emitter properties (Phase 4.1 Fix dispatch 1). Read returns the
  // full DTO; write returns an empty object after the patch is
  // applied. Unknown id: read returns default-shaped properties
  // (zeros + empty strings) so the form can render a disabled
  // placeholder instead of an error; write is a silent no-op.
  R extends { kind: "emitters/get-properties" } ? { properties: EmitterPropertiesDto } :
  R extends { kind: "emitters/set-properties" } ? Record<string, never> :

  // Track mutations (Phase 3 Screen 5 / Screen 6 Batch B-α)
  R extends { kind: "emitters/delete-track-keys" }       ? Record<string, never> :
  R extends { kind: "emitters/set-track-interpolation" } ? Record<string, never> :
  R extends { kind: "emitters/set-track-lock" } ? Record<string, never> :

  // Track key mutations (Phase 3 Screen 6 Batch B-β).
  // set-track-key returns empty; add-track-key returns the actual
  // inserted (time, value) which may differ from the requested time
  // when a same-time collision triggered a dedupe-bump.
  R extends { kind: "emitters/set-track-key" } ? Record<string, never> :
  R extends { kind: "emitters/add-track-key" } ? { time: number; value: number } :

  // Emitter mutations (Phase 3 Screen 4 Batch B1)
  R extends { kind: "emitters/duplicate" } ?
    | { ok: true; newId: number }
    | { ok: false; error: string } :
  R extends { kind: "emitters/duplicate-many" } ?
    | { ok: true; newIds: number[] }
    | { ok: false; error: string } :
  R extends { kind: "emitters/delete" }                         ? Record<string, never> :
  R extends { kind: "emitters/rename" }                         ? Record<string, never> :
  R extends { kind: "emitters/duplicate-with-index-increment" } ? { newId: number } :

  // Emitter mutations (Phase 3 Screen 4 Batch B2)
  R extends { kind: "emitters/add-lifetime-child" } ? { newId: number } :
  R extends { kind: "emitters/add-death-child" }    ? { newId: number } :
  R extends { kind: "emitters/add-root" }           ? { newId: number } :
  R extends { kind: "emitters/move" }               ? Record<string, never> :
  R extends { kind: "emitters/move-many" }          ? { newIds: number[] } :
  R extends { kind: "emitters/reorder-many" } ?
    | { ok: true; newIds: number[] }
    | { ok: false; error: string } :
  R extends { kind: "emitters/set-visible" }        ? Record<string, never> :
  R extends { kind: "emitters/set-all-visible" }    ? Record<string, never> :
  R extends { kind: "linkGroups/set-membership" }   ? Record<string, never> :

  // Emitter drag/drop (Phase 3 Screen 4 Batch B3)
  R extends { kind: "emitters/drop" } ?
    | { ok: true }
    | { ok: false; error: string } :

  // Emitter clipboard (Phase 3 Screen 4 Batch C)
  R extends { kind: "emitters/copy" }  ? Record<string, never> :
  R extends { kind: "emitters/cut" }   ? Record<string, never> :
  R extends { kind: "emitters/paste" } ? { newIds: number[] } :
  R extends { kind: "emitters/paste-as-child" } ? { newId: number } :

  // Per-emitter rescale (Phase 3 Screen 4 Batch B1)
  R extends { kind: "engine/action/rescale-emitter" } ? Record<string, never> :

  // Link-group exempt-set CRUD (Phase 3 Screen 4 Batch B1)
  R extends { kind: "linkGroups/list-exempt-fields" }  ? { fields: string[] } :
  R extends { kind: "linkGroups/set-exempt-fields" }   ? Record<string, never> :
  R extends { kind: "linkGroups/reset-exempt-fields" } ? Record<string, never> :
  R extends { kind: "linkGroups/diff-membership" }     ? { conflicts: { id: number; fields: string[] }[] } :
  R extends { kind: "linkGroups/diff-exempt-change" }  ? { conflicts: { id: number; fields: string[] }[] } :

  // Undo / spawner / layout / accelerators
  R extends { kind: "undo/perform" }              ? { applied: boolean; label?: string } :
  R extends { kind: "layout/viewport-rect" }      ? Record<string, never> :
  R extends { kind: "layout/scene-rect" }         ? Record<string, never> :
  R extends { kind: "animate-scene-rect" }        ? Record<string, never> :
  R extends { kind: "host/backing-color" }        ? Record<string, never> :
  R extends { kind: "viewport/capture-snapshot" } ? { imageBase64: string; w: number; h: number } :
  R extends { kind: "viewport/input" }            ? Record<string, never> :
  R extends { kind: "spawner/start" }             ? Record<string, never> :
  R extends { kind: "spawner/trigger" }           ? Record<string, never> :
  R extends { kind: "spawner/stop" }              ? Record<string, never> :
  R extends { kind: "app/quit" }                  ? Record<string, never> :
  R extends { kind: "engine/action/reset-view-settings" } ? Record<string, never> :
  R extends { kind: "register-accelerators" }     ? Record<string, never> :
  never;

export type ResponseFor<R extends Request> =
  ResponseForA<R> extends never ? ResponseForB<R> : ResponseForA<R>;

// ============================================================================
// Event DTOs (host → JS push)
// ============================================================================

export type Event =
  | { kind: "engine/state/changed";   payload: EngineStateDto }
  | { kind: "emitters/tree/changed";  payload: EmitterTreeDto }
  | { kind: "emitters/selected";      payload: { id: number | null } }
  // `overload`: latched preview overload — engine is suppressing spawning
  // because the live particle/instance budget was exceeded.
  | { kind: "stats/tick";             payload: { fps: number; emitters: number; particles: number; instances: number; overload: boolean } }
  // [hard-guard] One-shot event emitted when the preemptive estimate gate
  // refuses a spawn. Polled by the dispatcher's 4 Hz stats path and used
  // to drive a transient (~5 s) refusal banner in the UI.
  | { kind: "engine/overload/refused"; payload: { estimated: number; cap: number; attemptedCount: number } }
  // T9] Emitted whenever stats/set-frozen flips. When
  // frozen=true, StatusBar clears its local state so the cells
  // render `—` placeholders. Used by a11y spec beforeEach for
  // deterministic UIA goldens.
  | { kind: "stats/frozen-changed";   payload: { frozen: boolean } }
  | { kind: "dirty/changed";          payload: { dirty: boolean } }
  | { kind: "recent/changed";         payload: { paths: string[] } }
  //: host emits this when the native frame-X / Alt-F4 is hit on a
  // dirty doc, so the app pops the same Save/Discard/Cancel prompt File→Exit
  // uses (the host can't render the React prompt itself).
  | { kind: "app/close-requested";    payload: Record<string, never> }
  | { kind: "undo/changed";           payload: { canUndo: boolean; canRedo: boolean; label?: string } }
  | { kind: "accelerator/pressed";    payload: { combo: string } }
  | { kind: "spawner/active-count";   payload: { count: number } }
  // (Group A): viewport mouse cursor's intersection with the
  // ground plane in world coords. Host throttles to ~30 Hz so the
  // status bar update doesn't saturate the bridge.
  | { kind: "cursor/position-3d";     payload: { x: number; y: number; z: number } }
  // In-drag readout: host emits at ~30 Hz while a reference-object
  // gizmo is being dragged. `nx`/`ny` are NORMALIZED [0,1] viewport coords
  // (origin top-left) for the projected gizmo origin; the React pill floats
  // up-right of it and hides on active:false / visible:false. MockBridge
  // never emits this (no engine in browser-mode).
  | { kind: "engine/manipulator/drag"; payload:
      | { active: false }
      | { active: true; kind: "translate" | "plane" | "rotate"; nx: number; ny: number;
          visible: boolean; labels: string[]; values: number[]; decimals: number } };

export type EventKind = Event["kind"];
export type EventOf<K extends EventKind> = Extract<Event, { kind: K }>;

// ============================================================================
// Bridge interface
// ============================================================================

export interface Bridge {
  request<R extends Request>(req: R): Promise<ResponseFor<R>>;
  on<K extends EventKind>(kind: K, handler: (event: EventOf<K>) => void): () => void;
}

// ============================================================================
// Wire envelopes (for the JSON protocol)
// ============================================================================

export type WireRequest<R extends Request = Request> = {
  type: "req";
  id: RequestId;
  kind: R["kind"];
  params: R["params"];
};

export type WireResponse<R extends Request = Request> =
  | { type: "res"; id: RequestId; ok: true; data: ResponseFor<R> }
  | { type: "res"; id: RequestId; ok: false; error: string };

export type WireEvent<E extends Event = Event> = {
  type: "evt";
  kind: E["kind"];
  payload: E["payload"];
};

export type WireMessage = WireRequest | WireResponse | WireEvent;
