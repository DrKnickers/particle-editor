// Mock counterpart of src/host/BridgeDispatch_Engine.cpp.

import type {
  Request,
  EngineStateDto,
  LightDto,
  ReferenceObjectStatus,
  SkydomeSlotStatus,
} from "@particle-editor/bridge-schema";
import {
  makeDefaultEngineState,
  useMockEmitterTree,
  useMockEngineState,
  snapshotEngineState,
} from "../mock-state";
import type { MockDispatchHost } from "./host";

type EngineKind =
  | "engine/state/snapshot"
  | "engine/set/ground"
  | "engine/set/ground-z"
  | "engine/set/ground-texture"
  | "engine/set/ground-solid-color"
  | "engine/set/ground-slot-custom-path"
  | "engine/set/skydome-slot"
  | "engine/set/skydome-custom-path"
  | "engine/set/skydome-environment"
  | "engine/set/reference-object"
  | "engine/set/reference-object-visible"
  | "engine/set/reference-object-lock"
  | "engine/set/reference-object-transform"
  | "engine/set/grid-visible"
  | "engine/set/grid-spacing"
  | "engine/set/snap-enabled"
  | "engine/set/background"
  | "engine/set/bloom"
  | "engine/set/bloom-strength"
  | "engine/set/bloom-cutoff"
  | "engine/set/bloom-size"
  | "engine/set/leave-particles"
  | "engine/set/heat-debug"
  | "engine/set/camera"
  | "engine/set/light"
  | "engine/set/ambient"
  | "engine/set/shadow"
  | "engine/set/paused"
  | "engine/set/overload-guard"
  | "engine/set/msaa-level"
  | "engine/set/model-shadows"
  | "engine/set/soft-shadows"
  | "engine/set/estimated-load"
  | "engine/action/clear"
  | "engine/action/reload-shaders"
  | "engine/action/reload-textures"
  | "engine/action/on-particle-system-changed"
  | "engine/action/step-frames"
  | "engine/action/reset-view-settings"
  | "engine/action/rescale-system"
  | "engine/query/ground-slot-empty"
  | "engine/query/skydome-slot-empty"
  | "engine/query/skydome-list"
  | "engine/query/reference-object-list"
  | "engine/query/bloom-available"
  | "engine/query/msaa-levels"
  | "engine/action/rescale-emitter";

type EngineRequest = Extract<Request, { kind: EngineKind }>;

// Browser mode can't probe a real .alo, so these canned Names stand in
// for "skinned / unsupported" objects — selecting one drives the picker's
// "not supported" status path. Must match a Name in the reference-object-list.
const MOCK_SKINNED_REFS = new Set<string>(["Stormtrooper_Squad"]);

// Names that resolve in the catalog but whose model file is absent from the
// mod/base (getFile miss) — selecting one drives the "model file not found" status
// path (distinct from skinned / corrupt). Must match a Name in the catalog list.
// A structure (the picker now lists units + structures only; the earlier prop
// example would be filtered out, so the missing-model case rides a kept category).
const MOCK_MISSING_MODELS = new Set<string>(["Sensor_Array_NoModel"]);

// Browser mode can't load a real .alo, so these canned dome Names stand in
// for "chosen but the .alo wouldn't load" — selecting one drives the picker's
// load-failed status path + the solid-colour fallback indicator.
const MOCK_MISSING_DOMES = new Set<string>(["Broken_Sky"]);

// Representative average colour per built-in ground slot — the mock has no real
// textures, so it stands in for the host's GetGroundColor() (which averages the
// loaded texture). Slot 4 is the solid-colour slot (uses groundSolidColor).
const MOCK_GROUND_TEXTURE_COLOR: Record<number, number> = {
  0: 0x00334455, // dirt — brown, dark
  1: 0x00204a2a, // grass — green, medium-dark
  2: 0x0060a8c8, // sand — tan, light
  3: 0x00f0f0f0, // snow — near-white
};
function mockGroundColor(slot: number, solidColor: number): number {
  if (slot === 4) return solidColor; // kGroundSolidColorSlot
  return MOCK_GROUND_TEXTURE_COLOR[slot] ?? 0x00808080; // custom/unknown → mid-grey
}

export function dispatchEngine(this: MockDispatchHost, req: EngineRequest): unknown {
  switch (req.kind) {
    // ---------------- engine state ----------------
    case "engine/state/snapshot":
      return snapshotEngineState();

    // ---------------- engine setters: ground ----------------
    case "engine/set/ground":
      this.patchAndBroadcast({ ground: req.params.enabled });
      return {};

    case "engine/set/ground-z":
      this.patchAndBroadcast({ groundZ: req.params.z });
      return {};

    case "engine/set/ground-texture": {
      const requestedSlot = req.params.slot;
      const before = snapshotEngineState();
      const inRange = requestedSlot >= 0 && requestedSlot < before.groundSlotCustomPaths.length;
      // Browser mode treats built-in slots 0..4 as available. Custom slots
      // mirror the native engine and require a path before selection.
      const available =
        inRange &&
        (requestedSlot <= 4 || (before.groundSlotCustomPaths[requestedSlot] ?? "") !== "");
      if (available) {
        this.patchAndBroadcast({
          groundTexture: requestedSlot,
          groundColor: mockGroundColor(requestedSlot, before.groundSolidColor),
        });
      } else {
        this.patchAndBroadcast({});
      }
      const actualSlot = available ? requestedSlot : before.groundTexture;
      return { slot: actualSlot, applied: available };
    }

    case "engine/set/ground-solid-color":
      this.patchAndBroadcast(
        snapshotEngineState().groundTexture === 4
          ? { groundSolidColor: req.params.rgb, groundColor: req.params.rgb }
          : { groundSolidColor: req.params.rgb },
      );
      return {};

    case "engine/set/ground-slot-custom-path": {
      const { slot, path } = req.params;
      const snap = snapshotEngineState();
      const paths = [...snap.groundSlotCustomPaths];
      if (slot >= 0 && slot < paths.length) paths[slot] = path;
      const patch: Partial<EngineStateDto> = { groundSlotCustomPaths: paths };
      // Mirror the host: changing the SELECTED slot's texture refreshes the floor colour.
      if (slot === snap.groundTexture) patch.groundColor = mockGroundColor(slot, snap.groundSolidColor);
      this.patchAndBroadcast(patch);
      return {};
    }

    // ---------------- engine setters: skydome / background ----------------
    case "engine/set/skydome-slot": {
      const requestedSlot = req.params.slot;
      const before = snapshotEngineState();
      const inRange = requestedSlot >= 0 && requestedSlot < 12;
      const customPath =
        requestedSlot >= 9 ? (before.skydomeCustomPaths[requestedSlot - 9] ?? "") : "";
      const applied = inRange && (requestedSlot < 9 || customPath !== "");
      // The native setter falls back to Off when a valid custom slot cannot
      // load. Invalid indices leave the prior slot untouched.
      const actualSlot = applied ? requestedSlot : inRange ? 0 : before.skydomeSlot;
      this.patchAndBroadcast({ skydomeSlot: actualSlot });
      return { slot: actualSlot, applied };
    }

    case "engine/set/skydome-custom-path": {
      const { slot, path } = req.params;
      const customPaths = [...snapshotEngineState().skydomeCustomPaths];
      // slot is the absolute engine slot index (9..11); map to 0..2 in the
      // custom-only array.
      const idx = slot - 9;
      if (idx >= 0 && idx < customPaths.length) customPaths[idx] = path;
      this.patchAndBroadcast({ skydomeCustomPaths: customPaths });
      return {};
    }

    // game-dome environment: store context + the two chosen Names.
    // derive each slot's load outcome so the picker can surface a
    // chosen-but-unloadable dome (mirrors the native RebuildSkydomeMeshes).
    case "engine/set/skydome-environment": {
      const slotStatus = (name: string): SkydomeSlotStatus =>
        name === "" ? "none" : MOCK_MISSING_DOMES.has(name) ? "load-failed" : "ok";
      this.patchAndBroadcast({
        skydomeContext: req.params.context,
        skydomePrimaryName: req.params.primaryName,
        skydomeSecondaryName: req.params.secondaryName,
        skydomePrimaryStatus: slotStatus(req.params.primaryName),
        skydomeSecondaryStatus: slotStatus(req.params.secondaryName),
      });
      return {};
    }

    // imported reference object: select by Name (browser mode can't probe
    // a real .alo, so a canned skinned-set drives the "not supported" status),
    // toggle visibility, set transform; plus the unit grid toggle/spacing.
    case "engine/set/reference-object": {
      const name = req.params.name;
      const status: ReferenceObjectStatus =
        name === ""                      ? "none"
        : MOCK_MISSING_MODELS.has(name)  ? "model-missing"
        : MOCK_SKINNED_REFS.has(name)    ? "skinned"
        :                                  "ok";
      this.patchAndBroadcast({ referenceObjectName: name, referenceObjectStatus: status });
      return {};
    }

    case "engine/set/reference-object-visible":
      this.patchAndBroadcast({ referenceObjectVisible: req.params.visible });
      return {};

    case "engine/set/reference-object-lock":
      this.patchAndBroadcast({ referenceObjectLocked: req.params.locked });
      return {};

    case "engine/set/reference-object-transform":
      // Mirror the native bridge: a locked object drops
      // UI-routed transform requests (the picker also disables the inputs).
      if (useMockEngineState.getState().referenceObjectLocked) return {};
      this.patchAndBroadcast({
        referenceObjectPosition: req.params.position,
        referenceObjectRotation: req.params.rotation,
      });
      return {};

    case "engine/set/grid-visible":
      this.patchAndBroadcast({ gridVisible: req.params.visible });
      return {};

    case "engine/set/grid-spacing":
      this.patchAndBroadcast({
        gridSpacing: req.params.spacing > 0 ? req.params.spacing : 1,
      });
      return {};

    case "engine/set/snap-enabled":
      this.patchAndBroadcast({ snapEnabled: req.params.enabled });
      return {};

    case "engine/set/background":
      this.patchAndBroadcast({ background: req.params.rgb });
      return {};

    // ---------------- engine setters: bloom ----------------
    case "engine/set/bloom":
      this.patchAndBroadcast({ bloom: req.params.enabled });
      return {};

    case "engine/set/bloom-strength":
      this.patchAndBroadcast({ bloomStrength: req.params.v });
      return {};

    case "engine/set/bloom-cutoff":
      this.patchAndBroadcast({ bloomCutoff: req.params.v });
      return {};

    case "engine/set/bloom-size":
      this.patchAndBroadcast({ bloomSize: req.params.v });
      return {};

    // Leave particles after instance death. Mirrors the
    // native ParticleSystem::setLeaveParticles handler in
    // BridgeDispatcher.cpp.
    case "engine/set/leave-particles":
      this.patchAndBroadcast({ leaveParticles: req.params.enabled });
      return {};

    // ---------------- engine setters: debug / camera / lighting ----------------
    case "engine/set/heat-debug":
      this.patchAndBroadcast({ heatDebug: req.params.enabled });
      return {};

    case "engine/set/camera":
      this.patchAndBroadcast({ camera: { ...req.params } });
      return {};

    case "engine/set/light": {
      const { which, diffuse, specular, position, direction } = req.params;
      const next: LightDto = { diffuse, specular, position, direction };
      const lights = { ...snapshotEngineState().lights, [which]: next };
      this.patchAndBroadcast({ lights });
      return {};
    }

    case "engine/set/ambient":
      this.patchAndBroadcast({ ambient: req.params.color });
      return {};

    case "engine/set/shadow":
      this.patchAndBroadcast({ shadow: req.params.color });
      return {};

    // ---------------- engine setters: view state (preview clock) ----------------
    case "engine/set/paused":
      this.patchAndBroadcast({ paused: req.params.paused });
      return {};

    case "engine/set/overload-guard":
      // View-only preview config; the mock has no simulation to govern —
      // store it so contract tests can assert the round-trip.
      this.lastOverloadGuard = { ...req.params };
      return {};

    case "engine/set/msaa-level":
      // MockBridge: no GPU to configure — accept as a no-op.
      return {};

    case "engine/set/model-shadows":
      // MockBridge: no engine renderer — accept as a no-op.
      return {};

    case "engine/set/soft-shadows":
      // MockBridge: no engine renderer — accept as a no-op.
      return {};

    case "engine/set/estimated-load":
      // [hard-guard] The browser preview has no engine sim, so the
      // estimated-load value has no effect here; accept as a no-op so
      // web code paths are identical to native.
      return {};

    // ---------------- engine actions ----------------
    case "engine/action/clear":
      // No engine-state mutation; emit anyway so any UI watching for the
      // post-action redraw cue still fires.
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return {};

    case "engine/action/reload-shaders":
    case "engine/action/reload-textures":
    case "engine/action/on-particle-system-changed":
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return {};

    // Step one or more frames. In browser mode there's no engine clock
    // to advance; the response-only no-op keeps the schema reachable so
    // UI surfaces can wire the dispatch without a runtime error.
    case "engine/action/step-frames":
      return {};

    // Cascade-reset background, ground, bloom,
    // skydome, lighting back to engine defaults. The mock applies
    // a patch of just the view-setting fields (background / ground
    // / skydome / bloom) so editor state — currentFilePath, dirty
    // flag — is preserved across the reset. Emits one
    // engine/state/changed at the end.
    case "engine/action/reset-view-settings": {
      const defaults = makeDefaultEngineState();
      useMockEngineState.getState().applyPatch({
        background:    defaults.background,
        ground:        defaults.ground,
        groundZ:       defaults.groundZ,
        groundTexture: defaults.groundTexture,
        skydomeSlot:   defaults.skydomeSlot,
        bloom:         defaults.bloom,
        bloomStrength: defaults.bloomStrength,
        bloomCutoff:   defaults.bloomCutoff,
        bloomSize:     defaults.bloomSize,
      });
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return {};
    }

    // Rescale the whole particle system by a duration / size percentage.
    // MockBridge has no ParticleSystem to mutate; the handler logs the
    // call (so Vitest can assert on it via a spy) and emits the standard
    // post-action state/changed cue. Returns {} per schema.
    case "engine/action/rescale-system":
      console.log(
        "[MockBridge] engine/action/rescale-system",
        req.params,
      );
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return {};

    // ---------------- engine queries ----------------
    case "engine/query/ground-slot-empty": {
      const { slot } = req.params;
      const state = snapshotEngineState();
      const paths = state.groundSlotCustomPaths;
      // Mirrors Engine::IsGroundSlotEmpty: a slot is "empty" (no editor-owned
      // asset) unless it's dirt (slot 0, bundled IDB_GROUND), the procedural solid
      // colour (slot 4), or has a user custom path. Game-sourced slots 1..3
      // (grass/sand/snow) have NO bundled resource, so they read empty here even
      // though they render from the game install -- availability is a separate query.
      const hasBuiltin = slot === 0 || slot === 4;  // dirt + procedural solid only
      const hasCustom  = slot >= 0 && slot < paths.length && (paths[slot] ?? "") !== "";
      return !(hasBuiltin || hasCustom);
    }

    case "engine/query/skydome-slot-empty": {
      const { slot } = req.params;
      const state = snapshotEngineState();
      // Slots 0..8 are bundled (slot 0 = Off, never "empty" in the
      // picker sense — single-click commits it). Slots 9..11 are
      // empty iff their custom path is empty.
      if (slot >= 0 && slot < 9) return false;
      const idx = slot - 9;
      const paths = state.skydomeCustomPaths;
      if (idx < 0 || idx >= paths.length) return true;
      return (paths[idx] ?? "") === "";
    }

    // Enumerate selectable game-dome Names. Browser mode has no disk
    // to read *Skydomes.xml from, so return a small canned set per context
    // (real Names from vanilla FoC) to exercise the picker dispatch surface.
    case "engine/query/skydome-list": {
      const lists = {
        space: {
          // "Broken_Sky" is in MOCK_MISSING_DOMES — selecting it drives the
          // load-failed status path (browser-mode stand-in for an .alo
          // that won't load).
          primary: ["Stars_Low", "Stars_Medium", "Stars_High", "Stars_Cinematic", "Broken_Sky"],
          secondary: ["Star_Backdrop_Blue", "Star_Backdrop_Green", "Nebula_Field_Blue"],
        },
        land: {
          primary: ["Day_Blue_Sky", "Day_Clear_Sky", "Day_Storm_Sky", "Night_Stars"],
          secondary: ["Planet_Rings00", "Horizon_Haze00"],
        },
      };
      return req.params.context === "land" ? lists.land : lists.space;
    }

    // Enumerate selectable game objects. Browser mode has no disk to read
    // GameObjectFiles.xml from, so return a small canned set (real vanilla Names
    // across categories) to exercise the picker dispatch + grouping surface.
    // Units + structures only — mirrors Engine::EnumerateReferenceObjects
    // (props/projectiles/uncategorized are filtered out engine-side); `building`
    // is always false here (browser mode has no off-thread catalog build).
    case "engine/query/reference-object-list":
      return {
        building: false,
        // {domain, role, bucket} drive the collapsible tree; affiliation drives the faction chips.
        objects: [
          { name: "AT_AT_Walker", domain: "Ground", role: "Unit", bucket: "Vehicle", affiliation: "Empire" },
          { name: "AT_ST_Walker", domain: "Ground", role: "Unit", bucket: "Vehicle", affiliation: "Empire" },
          { name: "Stormtrooper_Squad", domain: "Ground", role: "Unit", bucket: "Infantry", affiliation: "Empire" }, // MOCK_SKINNED_REFS
          { name: "Rebel_Barracks", domain: "Ground", role: "Structure", bucket: "Structure", affiliation: "Rebel" },
          { name: "Empire_Anti_Aircraft_Turret", domain: "Ground", role: "Structure", bucket: "Structure", affiliation: "Empire" },
          { name: "Sensor_Array_NoModel", domain: "Ground", role: "Structure", bucket: "Structure", affiliation: "Rebel" }, // MOCK_MISSING_MODELS
          { name: "Imperial_Bunker_Capturable", domain: "Ground", role: "Unit", bucket: "Other", affiliation: "" }, // no affiliation -> only under "All"
          { name: "Star_Destroyer", domain: "Space", role: "Unit", bucket: "Capital", affiliation: "Empire" },
          { name: "Nebulon_B_Frigate", domain: "Space", role: "Unit", bucket: "Frigate", affiliation: "Rebel, Empire" }, // multi-faction
          { name: "TIE_Fighter", domain: "Space", role: "Unit", bucket: "Fighter", affiliation: "Empire" },
          { name: "Imperial_Star_Base", domain: "Space", role: "Structure", bucket: "Structure", affiliation: "Empire" },
          { name: "Darth_Vader", domain: "Ground", role: "Hero", bucket: "Hero", affiliation: "Empire" },
          { name: "Emperor_Palpatine", domain: "Space", role: "Hero", bucket: "Hero", affiliation: "Empire" },
          // Props + templates: their own flat sections, exempt from the fieldable gate.
          { name: "Asteroid_Field_Prop", domain: "Space", role: "Prop", bucket: "Other", affiliation: "" },
          { name: "Rebel_Crate_Prop", domain: "Ground", role: "Prop", bucket: "Other", affiliation: "Rebel" },
          { name: "Generic_Frigate_Template", domain: "Space", role: "Template", bucket: "Other", affiliation: "" },
        ],
      };

    case "engine/query/bloom-available":
      return snapshotEngineState().bloomAvailable;

    case "engine/query/msaa-levels":
      // MockBridge: report 0/2/4 as supported (8 deliberately excluded
      // so tests can verify that the UI clamps/hides unsupported levels).
      return { levels: [0, 2, 4], current: 4 };

    // ---------------- engine/action/rescale-emitter --
    //
    // Per-emitter rescale. The mock has no engine state to mutate so
    // the handler is a logging stub; the dirty-bit ride-along via
    // isMutating still fires, matching the native host's contract.
    case "engine/action/rescale-emitter": {
      console.log(
        "[MockBridge] engine/action/rescale-emitter",
        req.params,
      );
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      // The rescale changes per-emitter scalars but not the tree's
      // structural shape; still emit tree/changed so future inspector
      // panels relying on it re-fetch.
      this.emit({
        kind: "emitters/tree/changed",
        payload: useMockEmitterTree.getState().tree,
      });
      return {};
    }

    default: {
      // Exhaustiveness check — TS forces this to be `never`.
      const _exhaustive: never = req;
      throw new Error(`MockBridge: unknown request kind: ${JSON.stringify(_exhaustive)}`);
    }
  }
}
