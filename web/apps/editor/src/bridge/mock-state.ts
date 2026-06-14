// Zustand-backed in-memory mirror of `EngineStateDto` used by the
// MockBridge when the React app runs outside a WebView2 host (browser-
// mode design iteration). Defaults intentionally mirror
// `Engine::ResetParameters` / the engine constructor in
// `src/engine.cpp` so the mock state is indistinguishable from a
// freshly-launched native session.
//
// Colour encoding note: `groundSolidColor` and `background` are Win32
// COLORREFs (0x00BBGGRR). RGB(r,g,b) = `r | (g<<8) | (b<<16)`.
//   RGB(128,128,128) = 0x00808080  → flat-grey ground solid colour
//   RGB(0x14,0x08,0x34) = 0x00340814  → dark-purple background

import { create } from "zustand";
import type {
  EmitterPropertiesDto,
  EmitterTreeDto,
  EmitterTreeNode,
  EngineStateDto,
  GroupDto,
  InterpolationType,
  LightDto,
  SpawnerParamsDto,
  TrackDto,
  TrackKey,
  TrackName,
} from "@particle-editor/bridge-schema";
import { TRACK_NAMES, ZERO_SPAWN } from "@particle-editor/bridge-schema";

/** Defaults mirror `SpawnerConfig()` at [src/SpawnerDriver.h:18]:
 *  Auto mode + disabled + burst 1 + 0 s spacing + 10 s interval + origin
 *  + 5 s lifetime + zero jitter. */
export function makeDefaultSpawnerParams(): SpawnerParamsDto {
  return {
    mode: "auto",
    enabled: false,
    burstSize: 1,
    spacingSec: 0,
    intervalSec: 10,
    position: [0, 0, 0],
    velocity: [0, 0, 0],
    maxLifetimeSec: 5,
    jitterPosition: [0, 0, 0],
    acceleration: [0, 0, 0],
    squiggleAmplitude: [0, 0, 0],
    squiggleFrequency: 1,
  };
}

export const GROUND_SLOT_COUNT = 8;       // matches Engine::kGroundTextureCount
export const SKYDOME_SLOT_COUNT = 12;     // matches Engine::kSkydomeSlotCount
export const SKYDOME_FIRST_CUSTOM = 9;    // matches Engine::kSkydomeFirstCustomSlot
export const SKYDOME_CUSTOM_COUNT =
  SKYDOME_SLOT_COUNT - SKYDOME_FIRST_CUSTOM;  // 3

const zeroLight: LightDto = {
  diffuse:   [0, 0, 0, 0],
  specular:  [0, 0, 0, 0],
  position:  [0, 0, 0, 0],
  direction: [0, 0, 0, 0],
};

/** Build a fresh defaults object every time so the test reset hook can
 *  splat it into the store without sharing references. */
export function makeDefaultEngineState(): EngineStateDto {
  return {
    // Editor-level state (Screen 8 Batch 3): a freshly-launched mock
    // session is untitled (no path) and clean (no edits since load).
    currentFilePath: null,
    dirty: false,

    ground: true,
    groundZ: 0,
    groundTexture: 0,
    groundSolidColor: 0x00808080,
    groundSlotCustomPaths: Array.from({ length: GROUND_SLOT_COUNT }, () => ""),

    skydomeSlot: 0,
    skydomeCustomPaths: Array.from({ length: SKYDOME_CUSTOM_COUNT }, () => ""),
    // game-dome environment (no dome selected by default)
    skydomeContext: "space",
    skydomePrimaryName: "",
    skydomeSecondaryName: "",

    // imported reference object + unit grid (nothing selected by default)
    referenceObjectName: "",
    referenceObjectVisible: true,
    referenceObjectPosition: [0, 0, 0],
    referenceObjectRotation: [0, 0, 0],
    referenceObjectStatus: "none",
    gridVisible: false,
    gridSpacing: 20,

    background: 0x00340814,

    lights: {
      sun:   { ...zeroLight },
      fill1: { ...zeroLight },
      fill2: { ...zeroLight },
    },
    ambient: [0, 0, 0, 0],
    shadow:  [0, 0, 0, 0],

    bloom: false,
    bloomAvailable: true,    // mock pretends the shader is loaded
    bloomStrength: 0.0,
    bloomCutoff:   0.9,
    bloomSize:     0.1,

    // Task 2.7 — leave particles after instance death. Default true
    // matches the native ParticleSystem constructor at
    // [ParticleSystem.cpp:956].
    leaveParticles: true,

    heatDebug: false,

    paused: false,

    camera: {
      position: [0, -250, 125],
      target:   [0, 0, 0],
      up:       [0, 0, 1],
    },

    wind:    [0, 0, 0],
    gravity: [0, 0, -1],

    spawner: makeDefaultSpawnerParams(),

    // Screen 4 Batch A: nothing selected by default. Single-select only
    // in Batch A; multi-select is Batch B.
    selectedEmitterId: null,

    // D6: no active mod by default. Browser-mode MockBridge has
    // no disk to scan; selecting a mock entry updates this field via
    // mods/select for menu check-mark parity.
    activeModPath: null,

    // Browser-mode mock has no undo stack today (undo/perform is a
    // documented no-op — see mock.ts). Both flags stay false so the
    // Edit menu's Undo/Redo render as disabled in browser mode,
    // which is the correct UX given the no-op behaviour.
    canUndo: false,
    canRedo: false,
  };
}

type EngineStore = EngineStateDto & {
  applyPatch: (p: Partial<EngineStateDto>) => void;
  reset: () => void;
};

export const useMockEngineState = create<EngineStore>((set) => ({
  ...makeDefaultEngineState(),
  applyPatch: (p) => set(p as Partial<EngineStore>),
  reset: () => set(makeDefaultEngineState() as Partial<EngineStore>),
}));

/** Returns the engine-state slice of the store with the action methods
 *  stripped — i.e. exactly what should be serialised over the bridge. */
export function snapshotEngineState(): EngineStateDto {
  const { applyPatch: _a, reset: _r, ...rest } = useMockEngineState.getState();
  return rest;
}

// ─── Recent files registry (Screen 8 Batch 3) ───────────────────────
//
// Lives outside the EngineStateDto because it's host state, not engine
// state — the native host backs this with the Windows registry under
// `HKEY_CURRENT_USER\Software\AloParticleEditor` (matches legacy's
// AddToHistory / GetHistory at [src/main.cpp:650-768]). The mock stores
// the same list in-memory; the contract is the order (most-recent
// first), the cap (9 entries — `NUM_HISTORY_ITEMS` in legacy main.cpp),
// and the dedupe rule (a re-saved path moves to the front, not a
// duplicate entry).

export const MAX_RECENT_FILES = 9;

type RecentFilesStore = {
  paths: string[];
  setPaths: (paths: string[]) => void;
  /** Push to front; dedupes (case-insensitive) and caps at 9. */
  push: (path: string) => string[];
  reset: () => void;
};

// ─── Emitter-tree fixture (Screen 4 Batch A) ─────────────────────────
//
// Three roots covering the role + link-group combinations that the
// EmitterTree component needs to render:
//   - root "Smoke"   (linkGroup 1) — has a lifetime child + death child
//   - root "Sparks"  (linkGroup 1) — has a lifetime child only
//   - root "Flash"   (linkGroup 0) — leaf
// One linked pair (Smoke + Sparks share group 1) so the link-group dot
// styling is exercised. All emitters are visible=true; the disabled
// glyph state isn't reachable in Batch A (no visibility toggle yet).
//
// IDs are flat 0..5 and stable across resets so test assertions can
// pin to specific rows.

// Stable per-emitter identity (reorder glide), mirroring the host's
// process-monotonic counter (ParticleSystem.cpp s_nextEmitterStableId).
// DELIBERATELY offset from the mock's node ids (which are themselves stable —
// unlike the host's positional ids) so web code that confuses `id` with
// `stableId` fails fast in mock-driven tests instead of accidentally working.
// 0 is reserved for the synthetic root.
let s_nextStableId = 1001;
export function nextStableId(): number {
  return s_nextStableId++;
}

export function makeDefaultEmitterTree(): EmitterTreeDto {
  return {
    root: {
      id: -1,
      stableId: 0,
      name: "",
      role: "root",
      linkGroup: 0,
      visible: true,
      spawn: ZERO_SPAWN,
      children: [
        {
          id: 0, stableId: nextStableId(), name: "Smoke", role: "root", linkGroup: 1, visible: true, spawn: ZERO_SPAWN,
          children: [
            { id: 1, stableId: nextStableId(), name: "Smoke embers", role: "lifetime", linkGroup: 0, visible: true, spawn: ZERO_SPAWN, children: [] },
            { id: 2, stableId: nextStableId(), name: "Smoke puff",   role: "death",    linkGroup: 0, visible: true, spawn: ZERO_SPAWN, children: [] },
          ],
        },
        {
          id: 3, stableId: nextStableId(), name: "Sparks", role: "root", linkGroup: 1, visible: true, spawn: ZERO_SPAWN,
          children: [
            { id: 4, stableId: nextStableId(), name: "Spark trail", role: "lifetime", linkGroup: 0, visible: true, spawn: ZERO_SPAWN, children: [] },
          ],
        },
        {
          id: 5, stableId: nextStableId(), name: "Flash", role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN,
          children: [],
        },
      ],
    },
  };
}

type EmitterTreeStore = {
  tree: EmitterTreeDto;
  setTree: (tree: EmitterTreeDto) => void;
  reset: () => void;
};

export const useMockEmitterTree = create<EmitterTreeStore>((set) => ({
  tree: makeDefaultEmitterTree(),
  setTree: (tree) => set({ tree }),
  reset: () => set({ tree: makeDefaultEmitterTree() }),
}));

// ─── Link-group exempt-field fixture (Screen 4 Batch B1) ────────────
//
// The native side persists per-group LinkExemptFlags bitfields;
// the wire shape is `string[]` of field names that are exempt
// (per-emitter). MockBridge owns an in-memory map so the React modal
// can round-trip a fixture without the C++ host. Defaults to "name +
// colorTexture + normalTexture + trackIndex" — matches
// GetDefaultLinkExemptFlags() at [src/LinkGroup.cpp].

type LinkGroupExemptStore = {
  exempts: Map<number, string[]>;
  get: (groupId: number) => string[];
  set: (groupId: number, fields: string[]) => void;
  reset: (groupId: number) => void;
  resetAll: () => void;
};

export const DEFAULT_LINK_EXEMPT_FIELDS: readonly string[] = Object.freeze([
  "colorTexture",
  "normalTexture",
  "trackIndex",
  // Note: "name" is intrinsically exempt at the data-model layer (see
  // LinkExemptFlags::name) but legacy's settings dialog hides it from
  // the field table. Mock fixture mirrors that — the wire surface
  // returns only fields the user can toggle.
]);

export const useMockLinkGroupExempt = create<LinkGroupExemptStore>(
  (set, get) => ({
    exempts: new Map(),
    get: (groupId) => {
      const explicit = get().exempts.get(groupId);
      if (explicit !== undefined) return [...explicit];
      return [...DEFAULT_LINK_EXEMPT_FIELDS];
    },
    set: (groupId, fields) => {
      const next = new Map(get().exempts);
      next.set(groupId, [...fields]);
      set({ exempts: next });
    },
    reset: (groupId) => {
      const next = new Map(get().exempts);
      next.delete(groupId);
      set({ exempts: next });
    },
    resetAll: () => set({ exempts: new Map() }),
  }),
);

// ─── join-conflict seam (mock only) ──────────────────────────
//
// The native host computes a join's field disagreements from the real
// emitter params via `DiffNonExemptParams`. The MockBridge has only the
// tree DTO (no per-emitter params), so it can't diff for real — instead
// `linkGroups/diff-membership` returns whatever this store is seeded with
// (default: none). Tests drive the SetLinkGroupDialog confirm flow by
// seeding conflicts here; the real field-level correctness is a native
// (user) verification (web-lane-vs-native split).

export type LinkJoinConflict = { id: number; fields: string[] };

type LinkGroupConflictStore = {
  conflicts: LinkJoinConflict[];
  setConflicts: (conflicts: LinkJoinConflict[]) => void;
  resetAll: () => void;
};

export const useMockLinkGroupConflicts = create<LinkGroupConflictStore>(
  (set) => ({
    conflicts: [],
    setConflicts: (conflicts) =>
      set({ conflicts: conflicts.map((c) => ({ ...c, fields: [...c.fields] })) }),
    resetAll: () => set({ conflicts: [] }),
  }),
);

// ─── Tree-mutation helpers (Screen 4 Batch B1) ──────────────────────
//
// MockBridge invokes these to mutate the fixture in-place while
// preserving the parent-pointer / role invariants. Each helper returns
// the new tree (immutable swap) so the Zustand setTree triggers
// subscribers and the contract test can assert on the resulting shape.

function cloneNode(n: EmitterTreeNode): EmitterTreeNode {
  return {
    ...n,
    children: n.children.map(cloneNode),
  };
}

/** Walk and apply a transform to every node matching `id`. Returns a
 *  new tree (structurally cloned) with the transform applied. */
function mapNode(
  tree: EmitterTreeDto,
  id: number,
  transform: (n: EmitterTreeNode) => EmitterTreeNode,
): EmitterTreeDto {
  const walk = (n: EmitterTreeNode): EmitterTreeNode => {
    if (n.id === id) return transform(n);
    return { ...n, children: n.children.map(walk) };
  };
  return { root: walk(tree.root) };
}

/** Returns the highest `id` currently present in the tree. -1 means
 *  the tree is empty (only the synthetic root). */
function maxIdIn(tree: EmitterTreeDto): number {
  let m = -1;
  const visit = (n: EmitterTreeNode) => {
    if (n.id > m) m = n.id;
    n.children.forEach(visit);
  };
  visit(tree.root);
  return m;
}

/** Generate a duplicate-suffix name. Mirrors `GenerateDuplicateName`
 *  at [src/UI/EmitterList.cpp:309]: strips a trailing `_<digits>` if
 *  present, then appends `_<next>` where next is `max+1` across all
 *  emitters whose name shares the same base. */
export function generateDuplicateName(
  tree: EmitterTreeDto,
  sourceName: string,
): string {
  // Strip trailing _<digits> from the base.
  let base = sourceName;
  const underscore = base.lastIndexOf("_");
  if (underscore !== -1) {
    const tail = base.slice(underscore + 1);
    if (tail.length > 0 && /^\d+$/.test(tail)) {
      base = base.slice(0, underscore);
    }
  }
  // Walk tree, find max N among names matching `<base>` or `<base>_<N>`.
  let maxN = 0;
  const visit = (n: EmitterTreeNode) => {
    if (n.id === -1) {
      n.children.forEach(visit);
      return;
    }
    if (n.name === base) {
      // n=0; maxN already starts there.
    } else if (
      n.name.length > base.length + 1 &&
      n.name.slice(0, base.length) === base &&
      n.name[base.length] === "_"
    ) {
      const tail = n.name.slice(base.length + 1);
      if (/^\d+$/.test(tail)) {
        const num = Number.parseInt(tail, 10);
        if (num > maxN) maxN = num;
      }
    }
    n.children.forEach(visit);
  };
  visit(tree.root);
  return `${base}_${maxN + 1}`;
}

/** Duplicate the emitter at `id` as a fresh root (matches legacy
 *  `EmitterList_DuplicateEmitter`). The duplicate's subtree gets new
 *  ids; the duplicate itself + descendants are inserted as a top-level
 *  child of the synthetic root. Returns the new tree + the id of the
 *  duplicated root. Returns null when the source id isn't in the tree
 *  (or is the synthetic root). */
export function duplicateEmitter(
  tree: EmitterTreeDto,
  id: number,
): { tree: EmitterTreeDto; newId: number } | null {
  if (id === -1) return null;
  const source = findEmitterNode(tree, id);
  if (source === null) return null;

  let nextId = maxIdIn(tree) + 1;
  const newRootId = nextId;
  const reassign = (n: EmitterTreeNode): EmitterTreeNode => {
    const idForThis = nextId++;
    return {
      ...n,
      id: idForThis,
      role: "root",   // top-level — legacy duplicates land as roots
      children: n.children.map((c) => ({
        ...cloneNode(c),
        id: nextId++,
      })),
    };
  };
  // Build the duplicate subtree with fresh ids. Walk via depth-first
  // so id assignment matches the visit order. A duplicate is a NEW
  // emitter → fresh stableId too (mirrors the C++ copy-ctor rule).
  const reassignAll = (n: EmitterTreeNode): EmitterTreeNode => ({
    ...n,
    id: nextId++,
    stableId: nextStableId(),
    children: n.children.map(reassignAll),
  });
  // Reset nextId; we want the cloned root to take `newRootId`.
  nextId = maxIdIn(tree) + 1;
  const clone = reassignAll(source);
  // The cloned root becomes a root in the synthetic-root children.
  clone.role = "root";
  clone.name = generateDuplicateName(tree, source.name);

  const newTree: EmitterTreeDto = {
    root: {
      ...tree.root,
      children: [...tree.root.children, clone],
    },
  };
  // Note: reassign() helper above is unused — kept inline reassignAll
  // for clarity; the void reference here satisfies lint.
  void reassign;
  return { tree: newTree, newId: newRootId };
}

/** Delete the emitter at `id` (and its subtree). Returns the new tree
 *  or null when the id isn't found / is the synthetic root.
 *  Post-deletion, runs `enforceSingleMemberLinkGroups` () so the
 *  survivor of a 2-member group whose other member was just deleted
 *  drops to `linkGroup = 0` automatically. */
export function deleteEmitter(
  tree: EmitterTreeDto,
  id: number,
): EmitterTreeDto | null {
  if (id === -1) return null;
  const prune = (n: EmitterTreeNode): EmitterTreeNode | null => {
    if (n.id === id) return null;
    return {
      ...n,
      children: n.children.flatMap((c) => {
        const p = prune(c);
        return p === null ? [] : [p];
      }),
    };
  };
  const next = prune(tree.root);
  if (next === null) return null;  // can't prune synthetic root
  return enforceSingleMemberLinkGroups({ root: next });
}

/** Rename the emitter at `id`. Returns the new tree (always defined;
 *  no-op if id not found). */
export function renameEmitter(
  tree: EmitterTreeDto,
  id: number,
  name: string,
): EmitterTreeDto {
  return mapNode(tree, id, (n) => ({ ...n, name }));
}

/** Increment-duplicate: same as `duplicateEmitter` but the new name's
 *  numeric suffix is bumped by `delta`. Legacy
 *  `EmitterList_DuplicateEmitter(hWnd, indexDelta)` shifts the
 *  TRACK_INDEX track; the mock has no track data to mutate, so we just
 *  record the duplicate + the suffix in the name. */
export function duplicateWithIndexIncrement(
  tree: EmitterTreeDto,
  id: number,
  delta: number,
): { tree: EmitterTreeDto; newId: number } | null {
  const dup = duplicateEmitter(tree, id);
  if (dup === null) return null;
  // delta is for the index-track shift in legacy. The duplicate's name
  // already carries the auto-suffix from generateDuplicateName; we tag
  // an additional `(+N)` marker so tests can assert the delta arrived.
  // (The contract test only checks the wire round-trip; the actual
  // index-track shift is host-side.)
  const annotated = mapNode(dup.tree, dup.newId, (n) => ({
    ...n,
    name: `${n.name} (+${delta})`,
  }));
  return { tree: annotated, newId: dup.newId };
}

// ─── Batch B2 helpers — Add child / Move / Link-group membership ────

/** Add a lifetime child under `parentId`. Refused when the parent
 *  already has a lifetime child (the underlying engine slot is a
 *  single pointer, not a list). Returns null on missing parent or
 *  already-filled slot. */
export function addLifetimeChildEmitter(
  tree: EmitterTreeDto,
  parentId: number,
): { tree: EmitterTreeDto; newId: number } | null {
  if (parentId === -1) return null;
  const parent = findEmitterNode(tree, parentId);
  if (parent === null) return null;
  if (parent.children.some((c) => c.role === "lifetime")) return null;
  const newId = maxIdIn(tree) + 1;
  const child: EmitterTreeNode = {
    id: newId,
    stableId: nextStableId(),
    name: "",
    role: "lifetime",
    linkGroup: 0,
    visible: true,
    spawn: ZERO_SPAWN,
    children: [],
  };
  const next = mapNode(tree, parentId, (n) => ({
    ...n,
    // Lifetime child renders before death child in the legacy tree
    // (during-life before on-death). Splice it in at index 0 so the
    // ordering remains stable when both children exist.
    children: [child, ...n.children],
  }));
  return { tree: next, newId };
}

/** Add a new empty root emitter (Phase 4.1 Fix dispatch 5).
 *
 *  Mirrors `ParticleSystem::addRootEmitter()` with the default empty
 *  Emitter argument: the new root has no name, link group 0, visible,
 *  no children. Always succeeds (the engine has no "max roots" cap).
 *  New id = `maxIdIn(tree) + 1` to match the existing id-allocator
 *  pattern used by the other add helpers. Appended at the end of the
 *  root child list — mirrors the engine's push-back insertion. */
export function addRootEmitterMock(
  tree: EmitterTreeDto,
): { tree: EmitterTreeDto; newId: number } {
  const newId = maxIdIn(tree) + 1;
  const child: EmitterTreeNode = {
    id: newId,
    stableId: nextStableId(),
    name: "",
    role: "root",
    linkGroup: 0,
    visible: true,
    spawn: ZERO_SPAWN,
    children: [],
  };
  return {
    tree: { root: { ...tree.root, children: [...tree.root.children, child] } },
    newId,
  };
}

/** Add a death child under `parentId`. Refused when the parent
 *  already has a death child. */
export function addDeathChildEmitter(
  tree: EmitterTreeDto,
  parentId: number,
): { tree: EmitterTreeDto; newId: number } | null {
  if (parentId === -1) return null;
  const parent = findEmitterNode(tree, parentId);
  if (parent === null) return null;
  if (parent.children.some((c) => c.role === "death")) return null;
  const newId = maxIdIn(tree) + 1;
  const child: EmitterTreeNode = {
    id: newId,
    stableId: nextStableId(),
    name: "",
    role: "death",
    linkGroup: 0,
    visible: true,
    spawn: ZERO_SPAWN,
    children: [],
  };
  const next = mapNode(tree, parentId, (n) => ({
    ...n,
    // Death child renders after lifetime child.
    children: [...n.children, child],
  }));
  return { tree: next, newId };
}

/** Paste the first clipboard buffer entry as a child of `parentId` in the
 *  given slot (legacy Paste As ▸ Lifetime/Death — one emitter into one
 *  slot). Returns null on an empty buffer, an unknown parent, or an
 *  already-occupied slot (slot single-occupancy — same refusal as the
 *  add-child helpers). The seeded child keeps the copied subtree but is
 *  re-id'd to the next free id and re-roled to the target slot. */
export function pasteAsChildFromClipboard(
  tree: EmitterTreeDto,
  buffer: EmitterTreeNode[],
  parentId: number,
  slot: "lifetime" | "death",
): { tree: EmitterTreeDto; newId: number } | null {
  if (parentId === -1 || buffer.length === 0) return null;
  const parent = findEmitterNode(tree, parentId);
  if (parent === null) return null;
  if (parent.children.some((c) => c.role === slot)) return null;
  const newId = maxIdIn(tree) + 1;
  // Clone + re-id the WHOLE subtree from newId (depth-first) so no pasted
  // node collides with an existing id; then re-role the top to the slot.
  const child = cloneNode(buffer[0]);
  reassignIdsInPlace(child, newId);
  child.role = slot;
  const next = mapNode(tree, parentId, (n) => ({
    ...n,
    // Lifetime renders before death (during-life before on-death).
    children: slot === "lifetime" ? [child, ...n.children] : [...n.children, child],
  }));
  return { tree: next, newId };
}

/** Swap the emitter at `id` with its adjacent sibling in `direction`.
 *  Returns null on missing id, non-root emitter, or move past edge —
 *  matches `ParticleSystem::moveEmitter` semantics (children can't be
 *  reordered since each parent has fixed-role slots). */
export function moveEmitterInTree(
  tree: EmitterTreeDto,
  id: number,
  direction: "up" | "down",
): EmitterTreeDto | null {
  if (id === -1) return null;
  // Only root emitters can be moved; find the target in the root list.
  const idx = tree.root.children.findIndex((c) => c.id === id);
  if (idx === -1) return null;
  const swapIdx = direction === "up" ? idx - 1 : idx + 1;
  if (swapIdx < 0 || swapIdx >= tree.root.children.length) return null;
  const next = [...tree.root.children];
  [next[idx], next[swapIdx]] = [next[swapIdx]!, next[idx]!];
  return { root: { ...tree.root, children: next } };
}

/** Flip a single emitter's `visible` flag. Returns the new tree on
 *  success or null when the id isn't found. Matches the legacy
 *  `EmitterList_ToggleEmitterVisibility` which flips the selected
 *  emitter only — children are untouched. */
export function setEmitterVisibleMock(
  tree: EmitterTreeDto,
  id: number,
  visible: boolean,
): EmitterTreeDto | null {
  let found = false;
  const walk = (n: EmitterTreeNode): EmitterTreeNode => {
    if (n.id === id) {
      found = true;
      return { ...n, visible };
    }
    return { ...n, children: n.children.map(walk) };
  };
  const next: EmitterTreeDto = { root: walk(tree.root) };
  return found ? next : null;
}

/** Recursively set `visible` on every non-virtual emitter in the tree
 *  (excludes the synthetic root). Matches the legacy
 *  `EmitterList_SetAllEmitterVisibility`. */
export function setAllEmittersVisibleMock(
  tree: EmitterTreeDto,
  visible: boolean,
): EmitterTreeDto {
  const walk = (n: EmitterTreeNode): EmitterTreeNode => ({
    ...n,
    visible,
    children: n.children.map(walk),
  });
  return { root: { ...tree.root, children: tree.root.children.map(walk) } };
}

/** Find the smallest unused positive linkGroup id in the tree. Starts
 *  at 1; matches the host's "smallest unused positive uint32_t" rule. */
export function findUnusedLinkGroupId(tree: EmitterTreeDto): number {
  let maxGroup = 0;
  const visit = (n: EmitterTreeNode) => {
    if (n.linkGroup > maxGroup) maxGroup = n.linkGroup;
    n.children.forEach(visit);
  };
  visit(tree.root);
  return maxGroup + 1;
}

/** Update linkGroup membership on a batch of emitters. `groupId === -1`
 *  means "create a new group" (picks the smallest unused positive id
 *  via `findUnusedLinkGroupId`). `null` or `0` clears membership.
 *  Post-mutation, runs `enforceSingleMemberLinkGroups` to keep the
 *  data layer aligned with the render-layer filter — see. */
export function setLinkGroupMembership(
  tree: EmitterTreeDto,
  ids: number[],
  groupId: number | null,
): EmitterTreeDto {
  let resolved: number;
  if (groupId === null || groupId === 0) {
    resolved = 0;
  } else if (groupId === -1) {
    resolved = findUnusedLinkGroupId(tree);
  } else {
    resolved = groupId;
  }
  const idSet = new Set(ids);
  const walk = (n: EmitterTreeNode): EmitterTreeNode => {
    const transformed = idSet.has(n.id)
      ? { ...n, linkGroup: resolved }
      : n;
    return { ...transformed, children: transformed.children.map(walk) };
  };
  return enforceSingleMemberLinkGroups({ root: walk(tree.root) });
}

/** Sweep the tree and demote any positive `linkGroup` with
 *  exactly one member to 0. Idempotent — a second call produces no
 *  further change. Mirrors the host-side `EnforceSingleMemberLinkGroups`
 *  in `src/host/BridgeDispatcher.cpp` so data and view agree end-to-end
 *  (a single-member group renders no group indicator). Pure function —
 *  returns a new tree
 *  DTO; doesn't mutate the input. */
export function enforceSingleMemberLinkGroups(
  tree: EmitterTreeDto,
): EmitterTreeDto {
  // Pass 1: count members per positive linkGroup.
  const counts = new Map<number, number>();
  const visit = (n: EmitterTreeNode) => {
    if (n.linkGroup > 0) {
      counts.set(n.linkGroup, (counts.get(n.linkGroup) ?? 0) + 1);
    }
    n.children.forEach(visit);
  };
  visit(tree.root);

  // Short-circuit: if every group has ≥ 2 members already, no rewrite.
  // (Common case once enforcement has been running — a no-op call.)
  let anySingleton = false;
  counts.forEach((c) => { if (c === 1) anySingleton = true; });
  if (!anySingleton) return tree;

  // Pass 2: rebuild with singletons demoted to linkGroup=0.
  const demote = (n: EmitterTreeNode): EmitterTreeNode => {
    const shouldDemote =
      n.linkGroup > 0 && counts.get(n.linkGroup) === 1;
    const next = shouldDemote ? { ...n, linkGroup: 0 } : n;
    return { ...next, children: next.children.map(demote) };
  };
  return { root: demote(tree.root) };
}

// ─── Batch B3 helpers — drag/drop reorder + reparent ────────────────

/** Find the parent of `id` in the tree (returns the synthetic root if
 *  the id is a top-level root). Returns null when the id isn't found. */
function findParentNode(
  tree: EmitterTreeDto,
  id: number,
): EmitterTreeNode | null {
  const visit = (n: EmitterTreeNode): EmitterTreeNode | null => {
    for (const c of n.children) {
      if (c.id === id) return n;
      const hit = visit(c);
      if (hit) return hit;
    }
    return null;
  };
  return visit(tree.root);
}

/** Reorder a root-level emitter to `rootIndex`. Mirrors
 *  `ParticleSystem::moveEmitterToRootIndex(emitter, gap)`'s contract:
 *  gap K means "land before position K in the new list". Returns the
 *  mutated tree, or null when the source isn't a root or the no-op
 *  case fires (gap K equals sourceIdx or sourceIdx+1). */
export function reorderRootEmitter(
  tree: EmitterTreeDto,
  id: number,
  rootIndex: number,
): EmitterTreeDto | null {
  const roots = tree.root.children;
  const sourceIdx = roots.findIndex((c) => c.id === id);
  if (sourceIdx === -1) return null;          // not a root
  if (rootIndex < 0 || rootIndex > roots.length) return null;
  // No-op detection: gap [sourceIdx, sourceIdx+1] is the source's
  // own position. The C++ side returns false here too.
  if (rootIndex === sourceIdx || rootIndex === sourceIdx + 1) return null;
  const next = [...roots];
  const [moved] = next.splice(sourceIdx, 1);
  // After removal, indices above sourceIdx shift down by 1.
  const insertAt = rootIndex > sourceIdx ? rootIndex - 1 : rootIndex;
  next.splice(insertAt, 0, moved!);
  return { root: { ...tree.root, children: next } };
}

/** Reorder a SET of root emitters so they land contiguous at gap `rootIndex`,
 *  preserving their current top-to-bottom order; non-contiguous selections
 *  collapse together. Mirrors `ParticleSystem::reorderManyRootsToIndex`.
 *  Returns the mutated tree, or null on: out-of-range gap, empty selection,
 *  any non-root id, or an own-footprint no-op (a contiguous block dropped
 *  anywhere in [first, last+1]). */
export function reorderManyRoots(
  tree: EmitterTreeDto,
  ids: number[],
  rootIndex: number,
): EmitterTreeDto | null {
  const roots = tree.root.children;
  const N = roots.length;
  if (rootIndex < 0 || rootIndex > N) return null; // out of range (gap is 0..N)
  const pos = new Map<number, number>();
  roots.forEach((c, i) => pos.set(c.id, i));
  const idxs: number[] = [];
  for (const id of new Set(ids)) {
    const i = pos.get(id);
    if (i === undefined) return null; // missing or non-root
    idxs.push(i);
  }
  if (idxs.length === 0) return null;
  idxs.sort((a, b) => a - b);
  const M = idxs.length;
  const first = idxs[0]!, last = idxs[M - 1]!;
  if (last - first + 1 === M && rootIndex >= first && rootIndex <= last + 1) {
    return null;
  }
  const selSet = new Set(idxs);
  const rest = roots.filter((_, i) => !selSet.has(i));
  const block = idxs.map((i) => roots[i]!);
  let removedBeforeGap = 0;
  for (const i of idxs) if (i < rootIndex) removedBeforeGap++;
  const insertAt = rootIndex - removedBeforeGap;
  const next = [...rest.slice(0, insertAt), ...block, ...rest.slice(insertAt)];
  return { root: { ...tree.root, children: next } };
}

/** Reparent `id` under `targetId` in the named slot. Refuses when:
 *    - source or target missing,
 *    - source === target,
 *    - target is in source's subtree (cycle),
 *    - target's chosen slot is already filled. */
export function reparentEmitterInTree(
  tree: EmitterTreeDto,
  id: number,
  targetId: number,
  slot: "lifetime" | "death",
): EmitterTreeDto | null {
  if (id === targetId) return null;
  const source = findEmitterNode(tree, id);
  const target = findEmitterNode(tree, targetId);
  if (source === null || target === null) return null;
  // Cycle check: target must not be in source's subtree.
  const inSubtree = (n: EmitterTreeNode): boolean => {
    if (n.id === targetId) return true;
    return n.children.some(inSubtree);
  };
  if (inSubtree(source)) return null;
  // Slot occupancy check.
  if (target.children.some((c) => c.role === slot)) return null;
  // Already a direct child of target via *some* slot? Refuse — matches
  // the engine's "slot-switching under the same parent is refused" rule.
  const parent = findParentNode(tree, id);
  if (parent !== null && parent.id === targetId) return null;
  // Detach source from its current parent + attach as `slot` child of
  // target. Walk the tree once, transforming each touched node.
  const clonedSource: EmitterTreeNode = {
    ...source,
    role: slot,
    // Keep source's own children; they ride along with the move.
    children: source.children.map(cloneNode),
  };
  const walk = (n: EmitterTreeNode): EmitterTreeNode => {
    // Detach: drop source from any children list it appears in.
    let nextChildren = n.children.filter((c) => c.id !== id);
    // Attach: when we're the target, splice source in. Lifetime
    // renders before death; insert at the front for "lifetime", append
    // for "death" — matches the legacy ordering convention.
    if (n.id === targetId) {
      nextChildren = slot === "lifetime"
        ? [clonedSource, ...nextChildren]
        : [...nextChildren, clonedSource];
    }
    return { ...n, children: nextChildren.map(walk) };
  };
  return { root: walk(tree.root) };
}

// ─── Batch C helpers — clipboard (copy / cut / paste) ───────────────
//
// The mock clipboard is a plain `EmitterTreeNode[]` array of cloned
// subtrees. `copy` deep-clones the named nodes into the clipboard,
// `cut` does the same then deletes the originals, `paste` deep-clones
// the clipboard back into the tree as new roots with fresh ids. The
// shape mirrors the native C++ host's `std::vector<std::vector<uint8_t>>`
// — one serialised buffer per copied subtree.

/** In-memory clipboard. Survives across copy → paste; cleared on
 *  every fresh `copy` / `cut`. The Zustand store exposes setters so
 *  the bridge handler can mutate without React rerender storms. */
type EmitterClipboardStore = {
  buffer: EmitterTreeNode[];
  set: (buffer: EmitterTreeNode[]) => void;
  reset: () => void;
};

export const useMockEmitterClipboard = create<EmitterClipboardStore>(
  (set) => ({
    buffer: [],
    set: (buffer) => set({ buffer: buffer.map(cloneNode) }),
    reset: () => set({ buffer: [] }),
  }),
);

/** Reassign ids in a (deep-cloned) subtree starting at `startId`,
 *  walking depth-first. Returns the new root id + the mutated subtree
 *  (in-place — caller must have already cloned). */
function reassignIdsInPlace(n: EmitterTreeNode, startId: number): number {
  let next = startId;
  const walk = (m: EmitterTreeNode) => {
    m.id = next++;
    // A pasted node is a NEW emitter → fresh stableId (C++ copy-ctor rule).
    m.stableId = nextStableId();
    m.children.forEach(walk);
  };
  walk(n);
  return next;
}

/** Copy the named nodes' subtrees into the clipboard. Returns the
 *  list of cloned subtrees (order matches `ids`). Missing ids are
 *  silently skipped. */
export function copyEmittersToClipboard(
  tree: EmitterTreeDto,
  ids: number[],
): EmitterTreeNode[] {
  const out: EmitterTreeNode[] = [];
  for (const id of ids) {
    if (id === -1) continue;
    const n = findEmitterNode(tree, id);
    if (n === null) continue;
    out.push(cloneNode(n));
  }
  return out;
}

/** Paste the clipboard buffer as new roots. Reassigns ids from
 *  `max+1`. If `afterId` is provided AND matches a current root, the
 *  pasted roots land directly after it; else they append at the end
 *  of roots. Returns the new tree + the list of newly-assigned root
 *  ids in the order they were pasted. */
export function pasteEmittersFromClipboard(
  tree: EmitterTreeDto,
  clipboard: EmitterTreeNode[],
  afterId: number | null,
): { tree: EmitterTreeDto; newIds: number[] } {
  if (clipboard.length === 0) {
    return { tree, newIds: [] };
  }
  let nextId = maxIdIn(tree) + 1;
  const newIds: number[] = [];
  const newRoots: EmitterTreeNode[] = clipboard.map((src) => {
    const clone = cloneNode(src);
    const rootId = nextId;
    nextId = reassignIdsInPlace(clone, nextId);
    // Pasted nodes always land as roots (legacy default; paste-as-
    // Lifetime/Death is a future polish).
    clone.role = "root";
    newIds.push(rootId);
    return clone;
  });
  const roots = tree.root.children;
  let insertAt = roots.length;
  if (afterId !== null) {
    const idx = roots.findIndex((c) => c.id === afterId);
    if (idx !== -1) insertAt = idx + 1;
  }
  const nextRoots = [...roots.slice(0, insertAt), ...newRoots, ...roots.slice(insertAt)];
  return {
    tree: { root: { ...tree.root, children: nextRoots } },
    newIds,
  };
}

/** Walks the fixture and returns the node with the matching id, or null
 *  when the id isn't present in the tree. The synthetic id=-1 root
 *  matches too — callers that explicitly forbid the synthetic root must
 *  guard themselves. */
export function findEmitterNode(tree: EmitterTreeDto, id: number): EmitterTreeNode | null {
  const visit = (n: EmitterTreeNode): EmitterTreeNode | null => {
    if (n.id === id) return n;
    for (const c of n.children) {
      const hit = visit(c);
      if (hit) return hit;
    }
    return null;
  };
  return visit(tree.root);
}

export const useMockRecentFiles = create<RecentFilesStore>((set, get) => ({
  paths: [],
  setPaths: (paths) => set({ paths }),
  push: (path) => {
    const lower = path.toLowerCase();
    const filtered = get().paths.filter((p) => p.toLowerCase() !== lower);
    const next = [path, ...filtered].slice(0, MAX_RECENT_FILES);
    set({ paths: next });
    return next;
  },
  reset: () => set({ paths: [] }),
}));

// ─── Track fixtures (Screen 6 Batch A) ───────────────────────────────
//
// Deterministic per-emitter-id tracks so the mock surfaces something
// visible in the CurveEditor for any selected emitter. The shape is
// always 7 tracks in `TRACK_NAMES` order with small key counts that
// match the expected real-world usage (<20 keys/track) — keeps the
// SVG-vs-canvas profiling vehicle honest.
//
// The seed is the emitter id so different selections show distinct
// curves. Each track's interpolation rotates through linear / smooth /
// step so the toolbar's interpolation-state visual (Batch B will wire
// the actual toggle) shows variety in screenshots.

const TRACK_INTERPOLATIONS: readonly InterpolationType[] = Object.freeze([
  "linear", "smooth", "linear", "linear", "smooth", "step", "linear",
]);

/** Tiny LCG-ish hash so the keys for emitter id N differ from id M but
 *  stay deterministic per id. Don't use for anything that needs
 *  cryptographic properties — this is fixture seeding only. */
function seededFloat(id: number, salt: number, trackIdx: number): number {
  const v = Math.abs(Math.sin((id + 1) * 13.37 + salt * 7.11 + trackIdx * 3.19));
  return v - Math.floor(v);
}

/** Build a single track for the given (emitter id, track index) pair.
 *  Key shapes differ per track for visual variety:
 *   - Red (0):           ramp up 0→1 then down
 *   - Green (1):         hold 0 then bump
 *   - Blue (2):          steady decline
 *   - Alpha (3):         classic fade-in-fade-out
 *   - Scale (4):         expand 1→3
 *   - Index (5):         step 0→3→7
 *   - RotationSpeed (6): symmetric around 0 */
function buildFixtureTrack(id: number, trackIdx: number): TrackDto {
  const name: TrackName = TRACK_NAMES[trackIdx]!;
  const jitter = seededFloat(id, 1, trackIdx) * 0.15; // ±0–15% jitter
  let keys: TrackKey[] = [];
  switch (trackIdx) {
    case 0: // red
      keys = [
        { time: 0,   value: 0 },
        { time: 30,  value: 0.8 + jitter * 0.2 },
        { time: 60,  value: 1.0 },
        { time: 100, value: 0 },
      ];
      break;
    case 1: // green
      keys = [
        { time: 0,   value: 0 },
        { time: 50,  value: 0 },
        { time: 75,  value: 0.6 + jitter * 0.3 },
        { time: 100, value: 0.2 },
      ];
      break;
    case 2: // blue
      keys = [
        { time: 0,   value: 0.9 },
        { time: 100, value: 0.1 },
      ];
      break;
    case 3: // alpha
      keys = [
        { time: 0,   value: 0 },
        { time: 20,  value: 1 },
        { time: 80,  value: 1 },
        { time: 100, value: 0 },
      ];
      break;
    case 4: // scale
      keys = [
        { time: 0,   value: 1 },
        { time: 100, value: 3 + jitter * 2 },
      ];
      break;
    case 5: // index
      keys = [
        { time: 0,   value: 0 },
        { time: 33,  value: 3 },
        { time: 66,  value: 7 },
        { time: 100, value: 7 },
      ];
      break;
    case 6: // rotation speed
      keys = [
        { time: 0,   value: -2 + jitter },
        { time: 50,  value: 0 },
        { time: 100, value: 2 - jitter },
      ];
      break;
  }
  return {
    name,
    keys,
    interpolation: TRACK_INTERPOLATIONS[trackIdx]!,
    // Fixture defaults: no channel is locked. Mutators below override
    // via `setTrackLockInOverlay`.
    lockedTo: null,
  };
}

/** Build the 7-track DTO array for an emitter. Always returns 7
 *  entries in `TRACK_NAMES` order — that fixed shape is the contract,
 *  not a per-emitter override. */
export function makeFixtureTracks(id: number): TrackDto[] {
  return TRACK_NAMES.map((_n, i) => buildFixtureTrack(id, i));
}

// ─── Mutable per-emitter track overrides (Screen 5 / Screen 6 Batch B-α)
//
// `makeFixtureTracks` is a pure-function generator. To make the mock
// observe track mutations (delete-key, set-interpolation) across
// successive `emitters/get-tracks` calls, we layer a tiny overlay
// store keyed by emitter id. The overlay holds the FULL 7-track
// array for any emitter that's been mutated; missing entries fall
// back to the fixture generator on demand.
//
// Mutations always read-modify-write the overlay (seeding it from the
// fixture on first touch). Reset wipes the overlay so the fixture
// shines through again — used by the contract-test `beforeEach`.

type TrackOverlayStore = {
  /** id → 7-track DTO array. Present entry wins over the fixture. */
  overlay: Map<number, TrackDto[]>;
  /** Read-merge: returns the live tracks for `id`, seeding from the
   *  fixture if the overlay doesn't carry this id yet. Pure read —
   *  does not mutate. */
  read: (id: number) => TrackDto[];
  /** Replace the full 7-track array for `id`. The mutators below
   *  build the next array off `read(id)` then call `write`. */
  write: (id: number, tracks: TrackDto[]) => void;
  reset: () => void;
};

export const useMockTrackOverlay = create<TrackOverlayStore>((set, get) => ({
  overlay: new Map(),
  read: (id) => {
    const explicit = get().overlay.get(id);
    if (explicit !== undefined) return explicit.map((t) => ({
      ...t,
      keys: t.keys.map((k) => ({ ...k })),
    }));
    return makeFixtureTracks(id);
  },
  write: (id, tracks) => {
    const next = new Map(get().overlay);
    next.set(id, tracks.map((t) => ({
      ...t,
      keys: t.keys.map((k) => ({ ...k })),
    })));
    set({ overlay: next });
  },
  reset: () => set({ overlay: new Map() }),
}));

/** Delete the named-time keys from `track` on emitter `id`. Border
 *  keys (first + last by time) are silently skipped — they define the
 *  track's time range and aren't deletable per legacy semantics. The
 *  match tolerance for `time` is exact float equality (the React side
 *  carries the key's wire-shipped `time` value directly, so equality
 *  holds within IEEE-754 round-trip rules). Returns the count of
 *  keys actually removed (0 = nothing matched / all border keys). */
export function deleteTrackKeysInOverlay(
  id: number,
  trackName: TrackName,
  times: number[],
): number {
  const cur = useMockTrackOverlay.getState().read(id);
  const trackIdx = cur.findIndex((t) => t.name === trackName);
  if (trackIdx === -1) return 0;
  const target = cur[trackIdx]!;
  if (target.keys.length === 0) return 0;
  // Border keys = first + last in time order. The fixture wire
  // contract is keys-ascending-by-time, so first/last are simply
  // indices 0 and length-1.
  const borderTimes = new Set<number>([
    target.keys[0]!.time,
    target.keys[target.keys.length - 1]!.time,
  ]);
  const toDelete = new Set<number>();
  for (const t of times) {
    if (!borderTimes.has(t)) toDelete.add(t);
  }
  if (toDelete.size === 0) return 0;
  const nextKeys = target.keys.filter((k) => !toDelete.has(k.time));
  const removed = target.keys.length - nextKeys.length;
  if (removed === 0) return 0;
  const nextTracks = cur.map((t, i) =>
    i === trackIdx ? { ...t, keys: nextKeys } : t,
  );
  useMockTrackOverlay.getState().write(id, nextTracks);
  return removed;
}

/** Set the lock state on (emitter `id`, channel `channel`). `lockTo`
 *  is the *earlier* channel name to lock onto, or null to unlock. The
 *  mock mirrors the native semantics:
 *    - Only RGBA channels participate (channelIdx 0..3).
 *    - Only earlier-channel targets are valid (channelIdx > targetIdx,
 *      both in 0..3). Anything else is silently treated as unlock.
 *    - The overlay stores ONLY `lockedTo`; the canonical keys are
 *      NEVER overwritten by a lock/unlock. Locked-channel views are
 *      derived at read time (see `deriveLockViews`), matching the
 *      native pointer-alias semantics where `tracks[i] = &trackContents[j]`
 *      — the master's edits are instantly visible through the follower,
 *      and `trackContents[i]` is preserved intact throughout the lock.
 *  Returns true when the state actually changed (write happened). */
export function setTrackLockInOverlay(
  id: number,
  channel: TrackName,
  lockTo: TrackName | null,
): boolean {
  const cur = useMockTrackOverlay.getState().read(id);
  const channelIdx = cur.findIndex((t) => t.name === channel);
  if (channelIdx === -1 || channelIdx >= 4) return false;

  let resolvedLockTo: TrackName | null = null;
  if (lockTo !== null) {
    const targetIdx = cur.findIndex((t) => t.name === lockTo);
    if (targetIdx >= 0 && targetIdx < 4 && targetIdx < channelIdx) {
      resolvedLockTo = lockTo;
    }
  }

  const target = cur[channelIdx]!;
  if (target.lockedTo === resolvedLockTo) return false; // no-op

  // Write ONLY the lockedTo field — canonical keys are untouched in both the
  // lock and unlock paths. The live view is derived at the get-tracks read
  // boundary via `deriveLockViews`.
  const nextTracks = cur.map((t, i) =>
    i === channelIdx ? { ...t, lockedTo: resolvedLockTo } : t,
  );
  useMockTrackOverlay.getState().write(id, nextTracks);
  return true;
}

/** Present locked channels as views of their master's CANONICAL
 *  content — the mock equivalent of the native pointer alias
 *  (tracks[i] = &trackContents[j]). Pure; applied at the
 *  emitters/get-tracks read boundary ONLY. Mutators must keep
 *  operating on canonical overlay data — deriving inside the
 *  overlay's read() would bake mirrors into canonical on the next
 *  read-modify-write. */
export function deriveLockViews(tracks: TrackDto[]): TrackDto[] {
  return tracks.map((t, i) => {
    if (i >= 4 || t.lockedTo == null) return t;
    const src = tracks.find((s) => s.name === t.lockedTo);
    if (src === undefined) return t;
    return {
      ...t,
      keys: src.keys.map((k) => ({ ...k })),
      interpolation: src.interpolation,
    };
  });
}

/** Set `track.interpolation = interp` on emitter `id`. Always succeeds
 *  (when the track is known) — no refusal path on the wire. */
export function setTrackInterpolationInOverlay(
  id: number,
  trackName: TrackName,
  interp: InterpolationType,
): boolean {
  const cur = useMockTrackOverlay.getState().read(id);
  const trackIdx = cur.findIndex((t) => t.name === trackName);
  if (trackIdx === -1) return false;
  const nextTracks = cur.map((t, i) =>
    i === trackIdx ? { ...t, interpolation: interp } : t,
  );
  useMockTrackOverlay.getState().write(id, nextTracks);
  return true;
}

/** Move the key at `oldTime` on (emitter `id`, track `trackName`) to
 *  `(newTime, newValue)`. Border keys (first + last by time) override
 *  `newTime = oldTime` — only the value moves — matching the drag-time-
 *  fixed rule + native `set-track-key` semantics.
 *
 *  Returns true when a matching key was found and mutated; false when
 *  the track or the key at `oldTime` doesn't exist. The wire contract
 *  doesn't surface that distinction (both produce a no-op response)
 *  but tests want to assert the mutation actually landed. */
export function setTrackKeyInOverlay(
  id: number,
  trackName: TrackName,
  oldTime: number,
  newTime: number,
  newValue: number,
): boolean {
  const cur = useMockTrackOverlay.getState().read(id);
  const trackIdx = cur.findIndex((t) => t.name === trackName);
  if (trackIdx === -1) return false;
  const target = cur[trackIdx]!;
  if (target.keys.length === 0) return false;
  const keyIdx = target.keys.findIndex((k) => k.time === oldTime);
  if (keyIdx === -1) return false;
  // Border-key detection: oldTime is border iff it matches the first
  // or last key in time order (the wire contract is ascending by time).
  const isBorder =
    oldTime === target.keys[0]!.time ||
    oldTime === target.keys[target.keys.length - 1]!.time;
  const effectiveTime = isBorder ? oldTime : newTime;
  // Build the next keys list: drop the old key, insert the new one in
  // time order. The wire contract is ascending-by-time so callers can
  // trust ordering after the round trip.
  const remaining = target.keys.filter((_, i) => i !== keyIdx);
  const inserted: TrackKey = { time: effectiveTime, value: newValue };
  // Find insertion index (ascending by time).
  let ins = remaining.findIndex((k) => k.time > effectiveTime);
  if (ins === -1) ins = remaining.length;
  const nextKeys = [...remaining.slice(0, ins), inserted, ...remaining.slice(ins)];
  const nextTracks = cur.map((t, i) =>
    i === trackIdx ? { ...t, keys: nextKeys } : t,
  );
  useMockTrackOverlay.getState().write(id, nextTracks);
  return true;
}

// ─── Emitter properties fixture + overlay (Phase 4.1 Fix dispatch 1) ─
//
// Same pattern as `useMockTrackOverlay`: a deterministic generator
// (`makeFixtureProperties`) supplies the baseline; a per-id overlay
// Map layers user mutations on top. `read(id)` merges them.
//
// The fixture's *defaults* mirror `ParticleSystem::Emitter`'s zero-init
// (mostly zeros + the name "Emitter <id>" + nParticlesPerSecond=10 so
// continuous-spawn mode looks alive in design iteration). Different
// emitter ids get slightly different starting lifetimes / blendModes
// so multiple selections show distinct values.

function makeDefaultGroup(): GroupDto {
  return {
    type: 0,                  // GT_EXACT
    min: [0, 0, 0],
    max: [0, 0, 0],
    sideLength: 0,
    sphereRadius: 0,
    sphereEdge: 0,
    cylinderRadius: 0,
    cylinderEdge: 0,
    cylinderHeight: 0,
    val: [0, 0, 0],
  };
}

export function makeFixtureProperties(id: number): EmitterPropertiesDto {
  // Per-id deterministic perturbation so different selections show
  // different starting values without sharing a global counter.
  const lifetimeSeed = ((Math.abs(id) % 5) + 1);  // 1..5 s
  return {
    // Basic
    name: id === -1 ? "" : `Emitter ${id}`,
    lifetime: lifetimeSeed,
    initialDelay: 0,
    useBursts: false,
    nBursts: 1,
    burstDelay: 0,
    nParticlesPerBurst: 10,
    nParticlesPerSecond: 10,
    randomLifetimePerc: 0,
    randomScalePerc: 0,
    randomRotation: false,
    randomRotationDirection: false,
    randomRotationAverage: 0,
    randomRotationVariance: 0,
    freezeTime: 0,
    skipTime: 0,
    linkToSystem: false,
    parentLinkStrength: 1,
    index: id < 0 ? 0 : id,

    // Appearance
    colorTexture: "",
    normalTexture: "",
    blendMode: 0,
    textureSize: 1,
    nTriangles: 1,
    doColorAddGrayscale: false,
    randomColors: [0, 0, 0, 0],
    hasTail: false,
    tailSize: 0,
    isHeatParticle: false,
    isWorldOriented: false,
    noDepthTest: false,
    affectedByWind: false,

    // Physics
    acceleration: [0, 0, 0],
    gravity: 0,
    inwardSpeed: 0,
    inwardAcceleration: 0,
    objectSpaceAcceleration: false,
    bounciness: 0,
    groundBehavior: 0,
    emitFromMesh: 0,
    emitFromMeshOffset: 0,
    isWeatherParticle: false,
    weatherCubeSize: 0,
    weatherCubeDistance: 0,
    weatherFadeoutDistance: 0,

    groups: [makeDefaultGroup(), makeDefaultGroup(), makeDefaultGroup()],
  };
}

type PropertyOverlayStore = {
  /** id → full EmitterPropertiesDto. Present entry wins over the fixture. */
  overlay: Map<number, EmitterPropertiesDto>;
  /** Read-merge: returns the live properties for `id`, seeding from the
   *  fixture if the overlay doesn't yet carry this id. */
  read: (id: number) => EmitterPropertiesDto;
  /** Replace the full DTO for `id`. */
  write: (id: number, props: EmitterPropertiesDto) => void;
  /** Apply a partial patch on top of the current value for `id`. */
  patch: (id: number, patch: Partial<EmitterPropertiesDto>) => void;
  reset: () => void;
};

export const useMockEmitterProperties = create<PropertyOverlayStore>((set, get) => ({
  overlay: new Map(),
  read: (id) => {
    const explicit = get().overlay.get(id);
    if (explicit !== undefined) {
      // Return a defensive shallow clone so external consumers can't
      // mutate the store's value directly.
      return {
        ...explicit,
        acceleration: [...explicit.acceleration] as [number, number, number],
        randomColors: [...explicit.randomColors] as [number, number, number, number],
        groups: explicit.groups.map((g) => ({
          ...g,
          min: [...g.min] as [number, number, number],
          max: [...g.max] as [number, number, number],
          val: [...g.val] as [number, number, number],
        })),
      };
    }
    return makeFixtureProperties(id);
  },
  write: (id, props) => {
    const next = new Map(get().overlay);
    next.set(id, props);
    set({ overlay: next });
  },
  patch: (id, p) => {
    const cur = get().read(id);
    const merged = { ...cur, ...p };
    const next = new Map(get().overlay);
    next.set(id, merged);
    set({ overlay: next });
  },
  reset: () => set({ overlay: new Map() }),
}));

/** Insert a new key at `(time, value)` on the named track. If a key
 *  already exists at the exact `time`, bumps `time` by 0.001 (matching
 *  the native host's dedupe-by-epsilon rule). Returns the actual
 *  inserted (time, value) — the React side auto-selects the new key
 *  using the returned time so a collision doesn't break selection. */
export function addTrackKeyInOverlay(
  id: number,
  trackName: TrackName,
  time: number,
  value: number,
): { time: number; value: number } | null {
  const cur = useMockTrackOverlay.getState().read(id);
  const trackIdx = cur.findIndex((t) => t.name === trackName);
  if (trackIdx === -1) return null;
  const target = cur[trackIdx]!;
  let effectiveTime = time;
  // Dedupe-by-epsilon: bump until the time is unique. The bump is
  // small (0.001) and the loop is bounded by the key count so a
  // pathological dataset can't lock the dispatch thread.
  const times = new Set(target.keys.map((k) => k.time));
  while (times.has(effectiveTime)) {
    effectiveTime += 0.001;
  }
  const inserted: TrackKey = { time: effectiveTime, value };
  let ins = target.keys.findIndex((k) => k.time > effectiveTime);
  if (ins === -1) ins = target.keys.length;
  const nextKeys = [...target.keys.slice(0, ins), inserted, ...target.keys.slice(ins)];
  const nextTracks = cur.map((t, i) =>
    i === trackIdx ? { ...t, keys: nextKeys } : t,
  );
  useMockTrackOverlay.getState().write(id, nextTracks);
  return { time: effectiveTime, value };
}
