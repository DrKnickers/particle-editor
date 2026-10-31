// Contract tests for MockBridge — engine/state, engine/set|query|action, and settings/lighting.
// Split by namespace from the former single bridge-contract.test.ts. Each
// request is exercised end-to-end: request → store mutation → event →
// follow-up read, keeping the schema and the MockBridge implementation honest.

import { describe, it, expect, beforeEach } from "vitest";
import { MockBridge } from "../mock";
import { makeMockBridgeWithListener } from "@/test/mock-bridge";
import {
  useMockEngineState,
} from "../mock-state";
import type {
  EngineStateDto,
} from "@particle-editor/bridge-schema";
import { resetMockState } from "@/test/mock-state";

// Reset the shared mock stores between tests so state mutations don't leak.
beforeEach(resetMockState);

describe("MockBridge contract — engine/* and settings/*", () => {
  it("engine/set/ground-z patches state and fires state/changed", async () => {
    let last: EngineStateDto | null = null;
    const { b, off } = makeMockBridgeWithListener("engine/state/changed", (e) => { last = e.payload; });

    await b.request({ kind: "engine/set/ground-z", params: { z: 12.5 } });

    expect(last).not.toBeNull();
    expect(last!.groundZ).toBe(12.5);
    const fresh = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(fresh.groundZ).toBe(12.5);
    off();
  });

  it("background slot setters return exact actual slots without dirtying refused requests or no-ops", async () => {
    const b = new MockBridge();

    // Ground's valid same-slot no-op is applied, but slot 0 -> 0 must stay
    // clean. This fails if dirtiness is keyed to `applied` instead of delta.
    const groundNoop = await b.request({
      kind: "engine/set/ground-texture",
      params: { slot: 0 },
    });
    expect(groundNoop).toEqual({ slot: 0, applied: true });
    const afterGroundNoop = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(afterGroundNoop.dirty).toBe(false);

    // Valid no-op: applied is true, but slot 0 -> 0 is not a mutation.
    const skyNoop = await b.request({
      kind: "engine/set/skydome-slot",
      params: { slot: 0 },
    });
    expect(skyNoop).toEqual({ slot: 0, applied: true });

    // Exact wrong values: invalid 12 must not echo 12, and empty custom slot
    // 9 must not report applied:true. Both leave the already-Off mock clean.
    const skyInvalid = await b.request({
      kind: "engine/set/skydome-slot",
      params: { slot: 12 },
    });
    expect(skyInvalid).toEqual({ slot: 0, applied: false });
    const skyEmptyCustom = await b.request({
      kind: "engine/set/skydome-slot",
      params: { slot: 9 },
    });
    expect(skyEmptyCustom).toEqual({ slot: 0, applied: false });

    const groundInvalid = await b.request({
      kind: "engine/set/ground-texture",
      params: { slot: 8 },
    });
    expect(groundInvalid).toEqual({ slot: 0, applied: false });
    const groundEmptyCustom = await b.request({
      kind: "engine/set/ground-texture",
      params: { slot: 5 },
    });
    expect(groundEmptyCustom).toEqual({ slot: 0, applied: false });

    const clean = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(clean.dirty).toBe(false);

    // Overreach controls: a procedural ground slot and bundled skydome slot
    // apply exactly, so a guard that rejects every request fails here.
    const groundBundled = await b.request({
      kind: "engine/set/ground-texture",
      params: { slot: 4 },
    });
    expect(groundBundled).toEqual({ slot: 4, applied: true });
    const skyBundled = await b.request({
      kind: "engine/set/skydome-slot",
      params: { slot: 5 },
    });
    expect(skyBundled).toEqual({ slot: 5, applied: true });
  });

  it("a failed custom skydome request that falls back from bundled to Off dirties the actual change", async () => {
    const b = new MockBridge();
    await b.request({ kind: "engine/set/skydome-slot", params: { slot: 5 } });
    useMockEngineState.getState().applyPatch({ dirty: false });

    const fallback = await b.request({
      kind: "engine/set/skydome-slot",
      params: { slot: 9 },
    });
    expect(fallback).toEqual({ slot: 0, applied: false });
    const after = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(after.skydomeSlot).toBe(0);
    expect(after.dirty).toBe(true);
  });

  // One representative case per scalar setter — proves the kind→field
  // routing for the entire setter ladder. The list intentionally covers
  // every primitive shape (boolean / number / Color / Vec4 / nested
  // light record / Vec3 / Camera) so a regression in any single arm of
  // the dispatch surfaces here.
  it.each([
    ["engine/set/ground",             { enabled: false },            "ground",           false],
    ["engine/set/ground-z",           { z: 42 },                      "groundZ",          42],
    ["engine/set/ground-texture",     { slot: 3 },                    "groundTexture",    3],
    ["engine/set/ground-solid-color", { rgb: 0x00ff8800 },            "groundSolidColor", 0x00ff8800],
    ["engine/set/skydome-slot",       { slot: 5 },                    "skydomeSlot",      5],
    ["engine/set/background",         { rgb: 0x00112233 },            "background",       0x00112233],
    ["engine/set/bloom",              { enabled: true },              "bloom",            true],
    ["engine/set/bloom-strength",     { v: 2.5 },                     "bloomStrength",    2.5],
    ["engine/set/bloom-cutoff",       { v: 0.5 },                     "bloomCutoff",      0.5],
    ["engine/set/bloom-size",         { v: 0.25 },                    "bloomSize",        0.25],
    ["engine/set/heat-debug",         { enabled: true },              "heatDebug",        true],
    ["engine/set/paused",             { paused: true },               "paused",           true],
  ] as const)("%s mutates the snapshot", async (kind, params, field, expected) => {
    const b = new MockBridge();
    await b.request({ kind, params } as any);
    const s = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect((s as any)[field]).toEqual(expected);
  });

  it("engine/set/ground-slot-custom-path writes one slot in-place", async () => {
    const b = new MockBridge();
    await b.request({
      kind: "engine/set/ground-slot-custom-path",
      params: { slot: 5, path: "C:/textures/foo.tga" },
    });
    const s = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(s.groundSlotCustomPaths[5]).toBe("C:/textures/foo.tga");
    expect(s.groundSlotCustomPaths[0]).toBe("");  // others untouched
  });

  it("engine/set/skydome-custom-path writes the 9..11 slot via custom-array index", async () => {
    const b = new MockBridge();
    await b.request({
      kind: "engine/set/skydome-custom-path",
      params: { slot: 10, path: "C:/sky/foo.dds" },
    });
    const s = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(s.skydomeCustomPaths[1]).toBe("C:/sky/foo.dds");  // 10 - 9 = 1
    expect(s.skydomeCustomPaths[0]).toBe("");
    expect(s.skydomeCustomPaths[2]).toBe("");
  });

  it("engine/set/camera replaces the camera record", async () => {
    const b = new MockBridge();
    await b.request({
      kind: "engine/set/camera",
      params: { position: [1, 2, 3], target: [4, 5, 6], up: [0, 0, 1] },
    });
    const s = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(s.camera.position).toEqual([1, 2, 3]);
    expect(s.camera.target).toEqual([4, 5, 6]);
    expect(s.camera.up).toEqual([0, 0, 1]);
  });

  it("engine/set/light updates only the named light slot", async () => {
    const b = new MockBridge();
    await b.request({
      kind: "engine/set/light",
      params: {
        which: "sun",
        diffuse:   [1, 0.5, 0.25, 1],
        specular:  [0, 0, 0, 0],
        position:  [10, 0, 0, 0],
        direction: [-1, 0, 0, 0],
      },
    });
    const s = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(s.lights.sun.diffuse).toEqual([1, 0.5, 0.25, 1]);
    expect(s.lights.fill1.diffuse).toEqual([0, 0, 0, 0]);  // untouched
  });

  it("engine/set/ambient and engine/set/shadow swap the Vec4 in-place", async () => {
    const b = new MockBridge();
    await b.request({ kind: "engine/set/ambient", params: { color: [0.1, 0.2, 0.3, 1.0] } });
    await b.request({ kind: "engine/set/shadow",  params: { color: [0.4, 0.4, 0.4, 1.0] } });
    const s = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(s.ambient).toEqual([0.1, 0.2, 0.3, 1.0]);
    expect(s.shadow).toEqual([0.4, 0.4, 0.4, 1.0]);
  });

  it("engine/query/ground-slot-empty respects bundled / custom rules", async () => {
    const b = new MockBridge();
    // Dirt (0) + procedural solid (4) are structurally non-empty. Game-sourced
    // slots 1..3 (grass/sand/snow) have no editor-owned asset -> empty here, even
    // though they render from the game install (availability is a separate query).
    for (const slot of [0, 4]) {
      const empty = await b.request({ kind: "engine/query/ground-slot-empty", params: { slot } });
      expect(empty).toBe(false);
    }
    for (const slot of [1, 2, 3]) {
      const empty = await b.request({ kind: "engine/query/ground-slot-empty", params: { slot } });
      expect(empty).toBe(true);
    }
    // Slot 5 starts empty (no custom path set).
    const before = await b.request({ kind: "engine/query/ground-slot-empty", params: { slot: 5 } });
    expect(before).toBe(true);
    // Once a path is assigned, the slot is no longer empty.
    await b.request({
      kind: "engine/set/ground-slot-custom-path",
      params: { slot: 5, path: "C:/x.tga" },
    });
    const after = await b.request({ kind: "engine/query/ground-slot-empty", params: { slot: 5 } });
    expect(after).toBe(false);
  });

  it("engine/query/skydome-slot-empty respects bundled / custom rules", async () => {
    const b = new MockBridge();
    // Bundled slots (0..8) are never empty.
    const bundled = await b.request({ kind: "engine/query/skydome-slot-empty", params: { slot: 3 } });
    expect(bundled).toBe(false);
    // Custom slot 9 starts empty.
    const before = await b.request({ kind: "engine/query/skydome-slot-empty", params: { slot: 9 } });
    expect(before).toBe(true);
    await b.request({
      kind: "engine/set/skydome-custom-path",
      params: { slot: 9, path: "C:/sky.dds" },
    });
    const after = await b.request({ kind: "engine/query/skydome-slot-empty", params: { slot: 9 } });
    expect(after).toBe(false);
  });

  // game-dome environment: setter mutates the snapshot and emits state.
  it("engine/set/skydome-environment patches context + the two Names and fires state/changed", async () => {
    let last: EngineStateDto | null = null;
    const { b, off } = makeMockBridgeWithListener("engine/state/changed", (e) => { last = e.payload; });
    await b.request({
      kind: "engine/set/skydome-environment",
      params: { context: "land", primaryName: "Day_Blue_Sky", secondaryName: "Planet_Rings00" },
    });
    expect(last).not.toBeNull();
    expect(last!.skydomeContext).toBe("land");
    expect(last!.skydomePrimaryName).toBe("Day_Blue_Sky");
    expect(last!.skydomeSecondaryName).toBe("Planet_Rings00");
    // resolvable Names report "ok"; an empty slot reports "none".
    expect(last!.skydomePrimaryStatus).toBe("ok");
    expect(last!.skydomeSecondaryStatus).toBe("ok");
    const fresh = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(fresh.skydomePrimaryName).toBe("Day_Blue_Sky");
    off();
  });

  // A chosen-but-unloadable dome reports "load-failed" so the picker can
  // surface it instead of silently falling back to the solid background.
  it("engine/set/skydome-environment reports load-failed for a missing dome", async () => {
    const b = new MockBridge();
    await b.request({
      kind: "engine/set/skydome-environment",
      params: { context: "space", primaryName: "Broken_Sky", secondaryName: "" },
    });
    const s = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(s.skydomePrimaryStatus).toBe("load-failed");
    expect(s.skydomeSecondaryStatus).toBe("none");
  });

  // reference object + unit grid: setters mutate the snapshot + fire
  // state/changed; the skinned canned Name drives the "skinned" status.
  it("engine/set/reference-object patches name + status and fires state/changed", async () => {
    let last: EngineStateDto | null = null;
    const { b, off } = makeMockBridgeWithListener("engine/state/changed", (e) => { last = e.payload; });
    await b.request({ kind: "engine/set/reference-object", params: { name: "AT_AT_Walker" } });
    expect(last).not.toBeNull();
    expect(last!.referenceObjectName).toBe("AT_AT_Walker");
    expect(last!.referenceObjectStatus).toBe("ok");
    // A canned skinned object reports "skinned".
    await b.request({ kind: "engine/set/reference-object", params: { name: "Stormtrooper_Squad" } });
    expect(last!.referenceObjectStatus).toBe("skinned");
    // A Name whose model file is absent reports "model-missing".
    await b.request({ kind: "engine/set/reference-object", params: { name: "Sensor_Array_NoModel" } });
    expect(last!.referenceObjectStatus).toBe("model-missing");
    // Empty clears the selection.
    await b.request({ kind: "engine/set/reference-object", params: { name: "" } });
    expect(last!.referenceObjectName).toBe("");
    expect(last!.referenceObjectStatus).toBe("none");
    off();
  });

  it("engine/set/reference-object-transform + -visible round-trip through the snapshot", async () => {
    const b = new MockBridge();
    await b.request({
      kind: "engine/set/reference-object-transform",
      params: { position: [1, 2, 3], rotation: [45, 0, 90] },
    });
    await b.request({ kind: "engine/set/reference-object-visible", params: { visible: false } });
    const snap = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(snap.referenceObjectPosition).toEqual([1, 2, 3]);
    expect(snap.referenceObjectRotation).toEqual([45, 0, 90]);
    expect(snap.referenceObjectVisible).toBe(false);
  });

  it("engine/set/grid-visible + -grid-spacing round-trip; spacing clamps to > 0", async () => {
    const b = new MockBridge();
    await b.request({ kind: "engine/set/grid-visible", params: { visible: true } });
    await b.request({ kind: "engine/set/grid-spacing", params: { spacing: 50 } });
    let snap = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(snap.gridVisible).toBe(true);
    expect(snap.gridSpacing).toBe(50);
    // A non-positive spacing is clamped (mirrors Engine::SetGridSpacing).
    await b.request({ kind: "engine/set/grid-spacing", params: { spacing: 0 } });
    snap = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(snap.gridSpacing).toBeGreaterThan(0);
  });

  it("engine/set/snap-enabled round-trips to snapshot.snapEnabled", async () => {
    const b = new MockBridge();
    await b.request({ kind: "engine/set/snap-enabled", params: { enabled: true } });
    const snap = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(snap.snapEnabled).toBe(true);
  });

  it("engine/action/rescale-system round-trips with empty body and fires state/changed", async () => {
    let count = 0;
    const { b, off } = makeMockBridgeWithListener("engine/state/changed", () => { count++; });
    const r = await b.request({
      kind: "engine/action/rescale-system",
      params: { durationScalePercent: 150, sizeScalePercent: 200 },
    });
    expect(r).toEqual({});
    expect(count).toBe(1);
    off();
  });

  it("engine/action/clear fires state/changed without mutating fields", async () => {
    let count = 0;
    const { b, off } = makeMockBridgeWithListener("engine/state/changed", () => { count++; });
    await b.request({ kind: "engine/action/clear", params: {} });
    expect(count).toBe(1);
    off();
  });

  it("engine setter sets dirty=true and emits dirty/changed once", async () => {
    const b = new MockBridge();
    let dirtyEvents = 0;
    let lastDirty: boolean | null = null;
    const off = b.on("dirty/changed", (e) => {
      dirtyEvents++;
      lastDirty = e.payload.dirty;
    });

    await b.request({ kind: "engine/set/ground-z", params: { z: 1 } });
    expect(lastDirty).toBe(true);
    expect(dirtyEvents).toBe(1);

    // Second mutation while already dirty should NOT re-emit (debounce).
    await b.request({ kind: "engine/set/ground-z", params: { z: 2 } });
    expect(dirtyEvents).toBe(1);

    off();
  });

  it("engine/set/overload-guard round-trips and stores the config on the mock", async () => {
    const b = new MockBridge();
    const res = await b.request({
      kind: "engine/set/overload-guard",
      params: { enabled: false, maxParticles: 50_000 },
    });
    expect(res).toEqual({});
    expect(b.lastOverloadGuard).toEqual({ enabled: false, maxParticles: 50_000 });
  });

  it("engine/set/overload-guard is view-only (does not mark the doc dirty)", async () => {
    const b = new MockBridge();
    await b.request({ kind: "engine/set/overload-guard", params: { enabled: true, maxParticles: 25_000 } });
    // Dirty state is read via engine/state/snapshot.dirty in this file
    // (no file/state kind exists in the schema).
    const snap = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(snap.dirty).toBe(false);
  });

  it("engine/set/ground is view-only (does not mark the doc dirty) (#617)", async () => {
    // Ground visibility is a global VIEW preference (registry-persisted, not
    // part of the .alo document); native host no longer marks dirty, so the
    // mock must match. Contrast engine/set/ground-z (a real doc property),
    // which still dirties above.
    const b = new MockBridge();
    await b.request({ kind: "engine/set/ground", params: { enabled: false } });
    const snap = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(snap.dirty).toBe(false);
    // The state patch still applies — only the dirty side-effect is gone.
    expect(snap.ground).toBe(false);
  });

  it("engine/set/estimated-load is view-only (does not mark the doc dirty)", async () => {
    const b = new MockBridge();
    await b.request({ kind: "engine/set/estimated-load", params: { perInstance: 500 } });
    const snap = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(snap.dirty).toBe(false);
  });

  it("engine/action/rescale-emitter round-trips with empty body and fires state/changed", async () => {
    let stateChanges = 0;
    const { b, off } = makeMockBridgeWithListener("engine/state/changed", () => { stateChanges += 1; });

    const r = await b.request({
      kind: "engine/action/rescale-emitter",
      params: { id: 0, durationScalePercent: 150, sizeScalePercent: 75 },
    });
    expect(r).toEqual({});
    expect(stateChanges).toBeGreaterThanOrEqual(1);
    off();
  });

  // ─── Settings — cross-mode registry (raw lighting + Force Align) ──
  //
  // `settings/lighting` returns the raw lighting split (intensity/colour
  // kept separate, angles in degrees) the panel seeds from; the native
  // host reads it from the registry. The `forceAlign` field reflects the
  // live flag, and `…/set` round-trips it within a MockBridge instance.
  it("settings/lighting-force-align/set flips the forceAlign field on the next get", async () => {
    const b = new MockBridge();
    const setRes = await b.request({
      kind: "settings/lighting-force-align/set",
      params: { enabled: false },
    });
    expect(setRes).toEqual({});
    const after = await b.request({ kind: "settings/lighting", params: {} });
    expect(after.forceAlign).toBe(false);
  });

  it("settings/lighting/set persists the full raw split for the next get", async () => {
    const b = new MockBridge();
    const rgb = (r: number, g: number, bl: number) => r | (g << 8) | (bl << 16);
    const dto = {
      sun:   { intensity: 0.8, az: 30, alt: 60, diffuse: rgb(10, 20, 30), specular: rgb(40, 50, 60) },
      fill1: { intensity: 0.3, az: 90, alt: -5, diffuse: rgb(1, 2, 3),    specular: 0 },
      fill2: { intensity: 0.2, az: 270, alt: -5, diffuse: rgb(4, 5, 6),   specular: 0 },
      ambient: rgb(7, 8, 9),
      shadow:  rgb(11, 12, 13),
      forceAlign: false,
    };
    const setRes = await b.request({ kind: "settings/lighting/set", params: dto });
    expect(setRes).toEqual({});
    const after = await b.request({ kind: "settings/lighting", params: {} });
    // The written ambient (the field that regressed in the field report) and
    // the rest of the split round-trip verbatim.
    expect(after.ambient).toBe(rgb(7, 8, 9));
    expect(after.sun).toEqual(dto.sun);
    expect(after.shadow).toBe(rgb(11, 12, 13));
    expect(after.forceAlign).toBe(false);
  });
});
