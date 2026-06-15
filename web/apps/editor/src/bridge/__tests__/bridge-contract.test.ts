// Contract tests for MockBridge. Each `engine/set/*` is exercised
// end-to-end: request → store mutation → engine/state/changed event →
// follow-up snapshot read. Keeps the schema (`EngineStateDto`) and the
// MockBridge implementation honest as the bridge surface grows.

import { describe, it, expect, beforeEach } from "vitest";
import { MockBridge } from "../mock";
import {
  useMockEmitterClipboard,
  useMockEmitterProperties,
  useMockEmitterTree,
  useMockEngineState,
  useMockLinkGroupExempt,
  useMockLinkGroupConflicts,
  useMockRecentFiles,
  useMockTrackOverlay,
  makeDefaultEmitterTree,
  makeDefaultEngineState,
} from "../mock-state";
import type {
  EmitterTreeDto,
  EmitterTreeNode,
  EngineStateDto,
  Event,
} from "@particle-editor/bridge-schema";
import { ZERO_SPAWN } from "@particle-editor/bridge-schema";

beforeEach(() => {
  // Reset the store between tests so state mutations don't leak.
  useMockEngineState.setState(makeDefaultEngineState());
  useMockRecentFiles.getState().reset();
  useMockEmitterTree.setState({ tree: makeDefaultEmitterTree() });
  useMockLinkGroupExempt.getState().resetAll();
  useMockLinkGroupConflicts.getState().resetAll();
  useMockEmitterClipboard.getState().reset();
  useMockTrackOverlay.getState().reset();
  useMockEmitterProperties.getState().reset();
});

describe("MockBridge contract", () => {
  it("engine/state/snapshot returns the full DTO shape", async () => {
    const b = new MockBridge();
    const s = await b.request({ kind: "engine/state/snapshot", params: {} });
    // Editor-level state (Screen 8 Batch 3).
    expect(s).toHaveProperty("currentFilePath");
    expect(s).toHaveProperty("dirty");
    expect(s.currentFilePath).toBeNull();
    expect(s.dirty).toBe(false);
    // Spot-check every top-level field.
    expect(s).toHaveProperty("ground");
    expect(s).toHaveProperty("groundZ");
    expect(s).toHaveProperty("groundTexture");
    expect(s).toHaveProperty("groundSolidColor");
    expect(s).toHaveProperty("groundSlotCustomPaths");
    expect(s).toHaveProperty("skydomeSlot");
    expect(s).toHaveProperty("skydomeCustomPaths");
    expect(s).toHaveProperty("background");
    expect(s).toHaveProperty("lights.sun.diffuse");
    expect(s.lights.sun.diffuse).toHaveLength(4);
    expect(s).toHaveProperty("ambient");
    expect(s).toHaveProperty("shadow");
    expect(s).toHaveProperty("bloom");
    expect(s).toHaveProperty("bloomAvailable");
    expect(s).toHaveProperty("bloomStrength");
    expect(s).toHaveProperty("bloomCutoff");
    expect(s).toHaveProperty("bloomSize");
    expect(s).toHaveProperty("heatDebug");
    expect(s).toHaveProperty("paused");
    expect(s).toHaveProperty("camera.position");
    expect(s.camera.position).toHaveLength(3);
    expect(s).toHaveProperty("wind");
    expect(s).toHaveProperty("gravity");
    // Screen 4 Batch A: selected-emitter scalar.
    expect(s).toHaveProperty("selectedEmitterId");
    expect(s.selectedEmitterId).toBeNull();
  });

  it("engine/set/ground-z patches state and fires state/changed", async () => {
    const b = new MockBridge();
    let last: EngineStateDto | null = null;
    const off = b.on("engine/state/changed", (e) => { last = e.payload; });

    await b.request({ kind: "engine/set/ground-z", params: { z: 12.5 } });

    expect(last).not.toBeNull();
    expect(last!.groundZ).toBe(12.5);
    const fresh = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(fresh.groundZ).toBe(12.5);
    off();
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
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    await b.request({ kind, params } as any);
    const s = await b.request({ kind: "engine/state/snapshot", params: {} });
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
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
    // Bundled slots (0..4) are never empty.
    for (const slot of [0, 1, 2, 3, 4]) {
      const empty = await b.request({ kind: "engine/query/ground-slot-empty", params: { slot } });
      expect(empty).toBe(false);
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

  it("engine/query/bloom-available returns the flag", async () => {
    const b = new MockBridge();
    const v = await b.request({ kind: "engine/query/bloom-available", params: {} });
    expect(v).toBe(true);
  });

  // game-dome environment: setter mutates the snapshot, query enumerates.
  it("engine/set/skydome-environment patches context + the two Names and fires state/changed", async () => {
    const b = new MockBridge();
    let last: EngineStateDto | null = null;
    const off = b.on("engine/state/changed", (e) => { last = e.payload; });
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

  it("engine/query/skydome-list returns primary + secondary Name lists per context", async () => {
    const b = new MockBridge();
    const space = await b.request({ kind: "engine/query/skydome-list", params: { context: "space" } });
    expect(Array.isArray(space.primary)).toBe(true);
    expect(Array.isArray(space.secondary)).toBe(true);
    expect(space.primary).toContain("Stars_Low");
    const land = await b.request({ kind: "engine/query/skydome-list", params: { context: "land" } });
    expect(land.primary).toContain("Day_Blue_Sky");
    // The two contexts enumerate different lists.
    expect(land.primary).not.toEqual(space.primary);
  });

  // reference object + unit grid: setters mutate the snapshot + fire
  // state/changed; the list query enumerates Name + category; the skinned
  // canned Name drives the "skinned" status.
  it("engine/set/reference-object patches name + status and fires state/changed", async () => {
    const b = new MockBridge();
    let last: EngineStateDto | null = null;
    const off = b.on("engine/state/changed", (e) => { last = e.payload; });
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

  it("engine/query/reference-object-list returns Name + category entries", async () => {
    const b = new MockBridge();
    const r = await b.request({ kind: "engine/query/reference-object-list", params: {} });
    expect(Array.isArray(r.objects)).toBe(true);
    const turret = r.objects.find((o) => o.name === "Empire_Anti_Aircraft_Turret");
    expect(turret?.category).toBe("Turret");
  });

  // Submods: mods/list surfaces them under the active mod; set-submods
  // stacks them in precedence order; selecting a different mod resets the stack;
  // unmodded => none + a set-submods is inert.
  it("mods/list surfaces submods only under an active mod; set-submods round-trips in order", async () => {
    const b = new MockBridge();

    // Unmodded: no submods.
    await b.request({ kind: "mods/select", params: { path: null } });
    let list = await b.request({ kind: "mods/list", params: {} });
    expect(list.submods).toEqual([]);
    expect(list.activeSubmods).toEqual([]);

    // A set-submods while unmodded is inert — submods belong to an active mod
    // (matches the native ModManager::SelectSubmods no-mod guard).
    await b.request({ kind: "mods/set-submods", params: { names: ["Mod"] } });
    list = await b.request({ kind: "mods/list", params: {} });
    expect(list.activeSubmods).toEqual([]);

    // Activate a mod: submods appear, none selected yet.
    await b.request({ kind: "mods/select", params: { path: "C:/mock/corruption/Mods/FoCMod" } });
    list = await b.request({ kind: "mods/list", params: {} });
    expect(list.submods.length).toBeGreaterThan(1);
    expect(list.activeSubmods).toEqual([]);

    // Stack two submods in a specific precedence order; the order round-trips.
    const a = list.submods[0];
    const c = list.submods[1];
    const r = await b.request({ kind: "mods/set-submods", params: { names: [c, a] } });
    expect(r).toMatchObject({ ok: true, activeSubmods: [c, a] });
    list = await b.request({ kind: "mods/list", params: {} });
    expect(list.activeSubmods).toEqual([c, a]);

    // Unknown / duplicate names are dropped; order of the valid ones is kept.
    await b.request({ kind: "mods/set-submods", params: { names: [a, "Bogus", a, c] } });
    list = await b.request({ kind: "mods/list", params: {} });
    expect(list.activeSubmods).toEqual([a, c]);

    // Case-insensitive: a lower-cased request canonicalizes to the on-disk casing
    // (mirrors native ModManager::SelectSubmods, which the strict menu check needs).
    expect(a).not.toBe(a.toLowerCase());   // guard: the fixture name has upper-case
    await b.request({ kind: "mods/set-submods", params: { names: [a.toLowerCase()] } });
    list = await b.request({ kind: "mods/list", params: {} });
    expect(list.activeSubmods).toEqual([a]);

    // Switching mods resets the stack.
    await b.request({ kind: "mods/select", params: { path: "C:/mock/GameData/Mods/BaseGameMod" } });
    list = await b.request({ kind: "mods/list", params: {} });
    expect(list.activeSubmods).toEqual([]);
  });

  it("engine/action/step-frames resolves with an empty body in browser mode", async () => {
    const b = new MockBridge();
    // Pause first; the request is a response-only no-op either way, but
    // mirrors how the React Toolbar dispatches it (pause → step).
    await b.request({ kind: "engine/set/paused", params: { paused: true } });
    const r = await b.request({ kind: "engine/action/step-frames", params: { frames: 1 } });
    expect(r).toEqual({});
  });

  it("engine/action/rescale-system round-trips with empty body and fires state/changed", async () => {
    const b = new MockBridge();
    let count = 0;
    const off = b.on("engine/state/changed", () => { count++; });
    const r = await b.request({
      kind: "engine/action/rescale-system",
      params: { durationScalePercent: 150, sizeScalePercent: 200 },
    });
    expect(r).toEqual({});
    expect(count).toBe(1);
    off();
  });

  it("engine/action/clear fires state/changed without mutating fields", async () => {
    const b = new MockBridge();
    let count = 0;
    const off = b.on("engine/state/changed", () => { count++; });
    await b.request({ kind: "engine/action/clear", params: {} });
    expect(count).toBe(1);
    off();
  });

  it("rejects unimplemented emitters/* requests (mutations) as not implemented", async () => {
    const b = new MockBridge();
    // emitters/list and emitters/select are implemented as of Screen 4
    // Batch A; emitters/import-from-file landed with audit-G1 (native
    // handler + the emitter-import a11y spec). emitters/update remains
    // Phase 3+ work and is the one asserted unimplemented here.
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    await expect(b.request({ kind: "emitters/update", params: { id: 0, patch: {} } } as any))
      .rejects.toThrow(/not implemented/);
  });

  // Phase 3 Screen 8 Batch 3: file/save / file/recent/list are now
  // implemented in the mock (and the native host). The old "throws not
  // implemented" assertion has been replaced by round-trip specs below
  // covering file/new, file/save-as, recent/changed.

  it("file/new round-trips through MockBridge and returns {}", async () => {
    const b = new MockBridge();
    // Pre-dirty the state so file/new has something to clear.
    await b.request({ kind: "engine/set/ground-z", params: { z: 12 } });
    expect((await b.request({ kind: "engine/state/snapshot", params: {} })).dirty)
      .toBe(true);

    const r = await b.request({ kind: "file/new", params: {} });
    expect(r).toEqual({});
    const snap = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(snap.dirty).toBe(false);
    expect(snap.currentFilePath).toBeNull();
    expect(snap.groundZ).toBe(0);  // reset to default
  });

  it("file/save-as round-trips and returns { ok: true, path }", async () => {
    const b = new MockBridge();
    const r = await b.request({ kind: "file/save-as", params: {} });
    expect(r).toEqual({ ok: true, path: "/mock/saved-as.alo" });
    const snap = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(snap.currentFilePath).toBe("/mock/saved-as.alo");
    expect(snap.dirty).toBe(false);
  });

  it("recent/changed event fires when file/save adds a new path", async () => {
    const b = new MockBridge();
    let last: { paths: string[] } | null = null;
    const off = b.on("recent/changed", (e) => { last = e.payload; });

    await b.request({ kind: "file/save", params: { path: "C:/foo/bar.alo" } });

    expect(last).not.toBeNull();
    expect(last!.paths).toContain("C:/foo/bar.alo");

    // file/recent/list now mirrors the recent-files list.
    const list = await b.request({ kind: "file/recent/list", params: {} });
    expect(list.paths).toEqual(last!.paths);
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

  it("engine/set/estimated-load resolves ok via the mock", async () => {
    const b = new MockBridge();
    const res = await b.request({
      kind: "engine/set/estimated-load",
      params: { perInstance: 1234.5 },
    });
    expect(res).toEqual({});
  });

  it("engine/set/estimated-load is view-only (does not mark the doc dirty)", async () => {
    const b = new MockBridge();
    await b.request({ kind: "engine/set/estimated-load", params: { perInstance: 500 } });
    const snap = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(snap.dirty).toBe(false);
  });

  // Task 2.4: file/open is no longer a hard reject. The native handler
  // shows GetOpenFileNameW; the mock resolves with the schema's
  // cancellation shape so the React handler's request chain aborts
  // cleanly in browser mode without surfacing a raw rejection.
  it("resolves file/open with ok:false in browser mode", async () => {
    const b = new MockBridge();
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const r = await b.request({ kind: "file/open", params: {} } as any);
    expect(r).toEqual({ ok: false, error: "browser-mode" });
  });

  // ─── Phase 3 Screen 8 Batch 4: spawner + emitters/preview-from-file
  it("spawner/start round-trips through MockBridge; snapshot reflects new config", async () => {
    const b = new MockBridge();
    let lastSnap: { spawner?: { burstSize: number; mode: string } } | null = null;
    const off = b.on("engine/state/changed", (e) => {
      lastSnap = e.payload;
    });

    await b.request({
      kind: "spawner/start",
      params: {
        mode: "manual",
        enabled: false,
        burstSize: 7,
        spacingSec: 0.5,
        intervalSec: 3,
        position: [1, 2, 3],
        velocity: [0, 0, 0],
        maxLifetimeSec: 12,
        jitterPosition: [0, 0, 0],
        acceleration: [0, 0, 0],
        squiggleAmplitude: [0, 0, 0],
        squiggleFrequency: 1,
      },
    });

    expect(lastSnap).not.toBeNull();
    expect(lastSnap!.spawner!.burstSize).toBe(7);
    expect(lastSnap!.spawner!.mode).toBe("manual");

    const snap = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(snap.spawner.burstSize).toBe(7);
    expect(snap.spawner.mode).toBe("manual");
    off();
  });

  it("spawner/trigger returns {} and emits spawner/active-count", async () => {
    const b = new MockBridge();
    let count: number | null = null;
    const off = b.on("spawner/active-count", (e) => {
      count = e.payload.count;
    });

    const r = await b.request({ kind: "spawner/trigger", params: {} });
    expect(r).toEqual({});
    // Mock starts the count at 0 and bumps by burstSize (1 by default).
    expect(count).toBe(1);
    off();
  });

  // ─── host-state plumbing: round-trips for the handlers whose
  // C++ side moved from forward-deferred no-ops to real implementations.
  // The MockBridge handlers are unchanged; these specs are belt-and-
  // braces coverage so a future MockBridge regression that breaks the
  // schema-level round-trip surfaces here rather than in Playwright.
  it("spawner/stop round-trips and resets the active-count to 0", async () => {
    const b = new MockBridge();
    // Pre-fire a trigger to bump the active count above 0.
    let lastCount: number | null = null;
    const off = b.on("spawner/active-count", (e) => { lastCount = e.payload.count; });
    await b.request({ kind: "spawner/trigger", params: {} });
    expect(lastCount).toBeGreaterThan(0);

    const r = await b.request({ kind: "spawner/stop", params: {} });
    expect(r).toEqual({});
    // MockBridge emits a spawner/active-count event with 0 on stop;
    // the native handler emits engine/state/changed with
    // spawner.enabled=false. Both shapes are valid stop signals; the
    // mock spec asserts the mock-specific behaviour.
    expect(lastCount).toBe(0);
    off();
  });

  it("engine/action/rescale-system round-trips and emits state/changed", async () => {
    const b = new MockBridge();
    let stateChanges = 0;
    const off = b.on("engine/state/changed", () => { stateChanges += 1; });

    const r = await b.request({
      kind: "engine/action/rescale-system",
      params: { durationScalePercent: 200, sizeScalePercent: 100 },
    });
    expect(r).toEqual({});
    // MockBridge fires engine/state/changed after the rescale (parity
    // with the C++ host); the test just observes that at least one
    // arrived during the dispatch window.
    expect(stateChanges).toBeGreaterThanOrEqual(1);
    off();
  });

  it("emitters/preview-from-file returns a mock tree for any path", async () => {
    const b = new MockBridge();
    const r = await b.request({
      kind: "emitters/preview-from-file",
      params: { path: "/anywhere/foo.alo" },
    });
    expect(r.ok).toBe(true);
    if (r.ok) {
      expect(r.tree).toBeDefined();
      expect(r.tree.children.length).toBeGreaterThanOrEqual(3);
      // Names from the mock tree.
      const names = r.tree.children.map((c) => c.name);
      expect(names).toEqual(expect.arrayContaining(["Smoke", "Sparks", "Flash"]));
    }
  });

  // ─── Screen 4 Batch A — emitter tree + selection ──────────────────
  it("emitters/list returns the fixture tree with the synthetic root + populated real roots", async () => {
    const b = new MockBridge();
    const tree = await b.request({ kind: "emitters/list", params: {} });
    // Synthetic root has id=-1 and is the only level the rest descends from.
    expect(tree.root.id).toBe(-1);
    expect(tree.root.role).toBe("root");
    // Three real roots from the fixture.
    expect(tree.root.children).toHaveLength(3);
    const names = tree.root.children.map((c) => c.name);
    expect(names).toEqual(["Smoke", "Sparks", "Flash"]);
    // Smoke has one lifetime child + one death child.
    const smoke = tree.root.children[0];
    expect(smoke.linkGroup).toBe(1);
    expect(smoke.children).toHaveLength(2);
    expect(smoke.children[0].role).toBe("lifetime");
    expect(smoke.children[1].role).toBe("death");
    // Sparks has just a lifetime child.
    const sparks = tree.root.children[1];
    expect(sparks.children).toHaveLength(1);
    expect(sparks.children[0].role).toBe("lifetime");
    // Flash is a bare leaf with no link group.
    const flash = tree.root.children[2];
    expect(flash.children).toHaveLength(0);
    expect(flash.linkGroup).toBe(0);
    // All emitters in the fixture are visible.
    expect(smoke.visible).toBe(true);
  });

  it("emitters/select updates snapshot.selectedEmitterId and fires emitters/selected", async () => {
    const b = new MockBridge();
    let lastEvent: { id: number | null } | null = null;
    const off = b.on("emitters/selected", (e) => { lastEvent = e.payload; });

    // Initial snapshot: no selection.
    const before = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(before.selectedEmitterId).toBeNull();

    // Select Smoke (id=0).
    await b.request({ kind: "emitters/select", params: { id: 0 } });
    expect(lastEvent).not.toBeNull();
    expect(lastEvent!.id).toBe(0);
    const after = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(after.selectedEmitterId).toBe(0);

    // Selecting null clears.
    await b.request({ kind: "emitters/select", params: { id: null } });
    expect(lastEvent!.id).toBeNull();
    const cleared = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(cleared.selectedEmitterId).toBeNull();
    off();
  });

  // ─── Screen 4 Batch B1 — emitter mutations + Screen-8 dialogs ─────
  it("emitters/duplicate clones the emitter as a new root and returns ok:true + newId", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });

    const r = await b.request({ kind: "emitters/duplicate", params: { id: 0 } });
    if (!r.ok) throw new Error("expected ok:true");
    expect(r.newId).toBeGreaterThan(0);

    expect(lastTree).not.toBeNull();
    // Synthetic root now has 4 children (Smoke / Sparks / Flash / duplicate).
    expect(lastTree!.root.children).toHaveLength(4);
    // The duplicate inherits the source name with a `_<n>` suffix.
    const dup = lastTree!.root.children[3];
    expect(dup.name.startsWith("Smoke_")).toBe(true);
    off();
  });

  it("emitters/delete removes the emitter + subtree and clears selection if it was selected", async () => {
    const b = new MockBridge();
    // Pre-select the target so we observe the auto-clear.
    await b.request({ kind: "emitters/select", params: { id: 0 } });

    let lastTree: EmitterTreeDto | null = null;
    let lastSelected: number | null | "untouched" = "untouched";
    const offTree = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    const offSel  = b.on("emitters/selected",     (e) => { lastSelected = e.payload.id; });

    await b.request({ kind: "emitters/delete", params: { id: 0 } });

    expect(lastTree).not.toBeNull();
    expect(lastTree!.root.children).toHaveLength(2);  // Smoke + its 2 kids gone
    expect(lastTree!.root.children.map((c) => c.name)).toEqual(["Sparks", "Flash"]);
    expect(lastSelected).toBeNull();
    offTree();
    offSel();
  });

  it("emitters/rename updates the node name in the tree", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });

    await b.request({
      kind: "emitters/rename",
      params: { id: 0, name: "Smoke (renamed)" },
    });
    expect(lastTree).not.toBeNull();
    expect(lastTree!.root.children[0].name).toBe("Smoke (renamed)");
    off();
  });

  it("emitters/duplicate-with-index-increment returns the new id and clones the subtree", async () => {
    const b = new MockBridge();
    const r = await b.request({
      kind: "emitters/duplicate-with-index-increment",
      params: { id: 0, delta: 5 },
    });
    expect(r.newId).toBeGreaterThan(0);
    // Snapshot the tree and find the duplicate; its name carries the
    // delta marker so we can assert the round-trip preserved `delta`.
    const tree = await b.request({ kind: "emitters/list", params: {} });
    const dup = tree.root.children[tree.root.children.length - 1];
    expect(dup.name).toContain("(+5)");
  });

  it("engine/action/rescale-emitter round-trips with empty body and fires state/changed", async () => {
    const b = new MockBridge();
    let stateChanges = 0;
    const off = b.on("engine/state/changed", () => { stateChanges += 1; });

    const r = await b.request({
      kind: "engine/action/rescale-emitter",
      params: { id: 0, durationScalePercent: 150, sizeScalePercent: 75 },
    });
    expect(r).toEqual({});
    expect(stateChanges).toBeGreaterThanOrEqual(1);
    off();
  });

  it("linkGroups/list-exempt-fields returns the v1 default exempt set for an unknown group", async () => {
    const b = new MockBridge();
    const r = await b.request({
      kind: "linkGroups/list-exempt-fields",
      params: { groupId: 1 },
    });
    // Defaults: colorTexture + normalTexture + trackIndex.
    expect(r.fields).toEqual(expect.arrayContaining([
      "colorTexture", "normalTexture", "trackIndex",
    ]));
  });

  it("linkGroups/set-exempt-fields replaces the per-group set", async () => {
    const b = new MockBridge();
    await b.request({
      kind: "linkGroups/set-exempt-fields",
      params: { groupId: 1, fields: ["lifetime", "gravity"] },
    });
    const r = await b.request({
      kind: "linkGroups/list-exempt-fields",
      params: { groupId: 1 },
    });
    expect(r.fields).toEqual(["lifetime", "gravity"]);
  });

  // ─── Screen 4 Batch B2 — add-child + move + link-group membership ─

  it("emitters/add-lifetime-child adds a lifetime child under the parent and returns its newId", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    // Flash (id=5) starts as a leaf — no children. Adding a lifetime
    // child should succeed and produce a child with role: "lifetime".
    const r = await b.request({
      kind: "emitters/add-lifetime-child",
      params: { parentId: 5 },
    });
    expect(r.newId).toBeGreaterThan(0);
    expect(lastTree).not.toBeNull();
    const flash = lastTree!.root.children[2];
    expect(flash.children).toHaveLength(1);
    expect(flash.children[0].role).toBe("lifetime");
    expect(flash.children[0].id).toBe(r.newId);
    off();
  });

  it("emitters/add-root appends a new empty root emitter and returns its newId ()", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    const before = await b.request({ kind: "emitters/list", params: {} });
    const rootCountBefore = before.root.children.length;
    const r = await b.request({ kind: "emitters/add-root", params: {} });
    expect(r.newId).toBeGreaterThan(0);
    expect(lastTree).not.toBeNull();
    expect(lastTree!.root.children).toHaveLength(rootCountBefore + 1);
    // New root is appended at the end with role: "root", empty name,
    // no children, link group 0.
    const last = lastTree!.root.children[lastTree!.root.children.length - 1]!;
    expect(last.id).toBe(r.newId);
    expect(last.role).toBe("root");
    expect(last.children).toHaveLength(0);
    expect(last.linkGroup).toBe(0);
    off();
  });

  it("emitters/add-death-child adds a death child under the parent and returns its newId", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    // Sparks (id=3) has one lifetime child but no death child. Add a
    // death child and verify role + position (death child renders
    // after lifetime).
    const r = await b.request({
      kind: "emitters/add-death-child",
      params: { parentId: 3 },
    });
    expect(r.newId).toBeGreaterThan(0);
    expect(lastTree).not.toBeNull();
    const sparks = lastTree!.root.children[1];
    expect(sparks.children).toHaveLength(2);
    expect(sparks.children[1].role).toBe("death");
    expect(sparks.children[1].id).toBe(r.newId);
    off();
  });

  it("emitters/move swaps adjacent roots", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    // Fixture roots in order: Smoke(0), Sparks(3), Flash(5). Move
    // Sparks (middle) up — should swap with Smoke.
    await b.request({
      kind: "emitters/move",
      params: { id: 3, direction: "up" },
    });
    expect(lastTree).not.toBeNull();
    const names = lastTree!.root.children.map((c) => c.name);
    expect(names).toEqual(["Sparks", "Smoke", "Flash"]);
    off();
  });

  it("emitters/set-visible flips a single emitter's visible flag ()", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    await b.request({
      kind: "emitters/set-visible",
      params: { id: 0, visible: false },
    });
    expect(lastTree).not.toBeNull();
    const smoke = lastTree!.root.children.find((c) => c.id === 0)!;
    expect(smoke.visible).toBe(false);
    // Siblings unchanged.
    const sparks = lastTree!.root.children.find((c) => c.id === 3)!;
    expect(sparks.visible).toBe(true);
    off();
  });

  it("emitters/set-all-visible recurses through the tree ()", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    await b.request({
      kind: "emitters/set-all-visible",
      params: { visible: false },
    });
    expect(lastTree).not.toBeNull();
    // Every emitter — roots + children of every depth — should be hidden.
    const allHidden = (n: EmitterTreeNode): boolean =>
      n.visible === false && n.children.every(allHidden);
    expect(lastTree!.root.children.every(allHidden)).toBe(true);

    // Bulk-show puts them all back.
    await b.request({
      kind: "emitters/set-all-visible",
      params: { visible: true },
    });
    const allVisible = (n: EmitterTreeNode): boolean =>
      n.visible === true && n.children.every(allVisible);
    expect(lastTree!.root.children.every(allVisible)).toBe(true);
    off();
  });

  it("linkGroups/set-membership applies groupId to each id; -1 + single id auto-demotes to 0 (path 3); null clears", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });

    // Fixture: Smoke (id=0) + Sparks (id=3) share linkGroup=1.
    // Flash (id=5) is unlinked. Add Flash to a new group via the
    // -1 sentinel — the resolution rule picks group 2 (smallest
    // unused positive), then's `enforceSingleMemberLinkGroups`
    // fires immediately after the mutation and demotes Flash back to
    // 0 because group 2 has exactly one member. Net result: a -1 +
    // single-id dispatch is effectively a no-op (data layer matches
    // render layer, which already filters single-member groups).
    await b.request({
      kind: "linkGroups/set-membership",
      params: { ids: [5], groupId: -1 },
    });
    expect(lastTree).not.toBeNull();
    // Find Flash by walking the synthetic root.
    const flashAfter = lastTree!.root.children.find((c) => c.id === 5)!;
    expect(flashAfter.linkGroup).toBe(0);

    // Clear Smoke + Sparks via null — both should drop to 0.
    await b.request({
      kind: "linkGroups/set-membership",
      params: { ids: [0, 3], groupId: null },
    });
    const smokeAfter  = lastTree!.root.children.find((c) => c.id === 0)!;
    const sparksAfter = lastTree!.root.children.find((c) => c.id === 3)!;
    expect(smokeAfter.linkGroup).toBe(0);
    expect(sparksAfter.linkGroup).toBe(0);

    // Assign Flash + Smoke to an explicit group id — should land
    // exactly there with no scan/rewrite. Group 7 has 2 members so
    //'s enforcement doesn't fire.
    await b.request({
      kind: "linkGroups/set-membership",
      params: { ids: [0, 5], groupId: 7 },
    });
    expect(lastTree!.root.children.find((c) => c.id === 0)!.linkGroup).toBe(7);
    expect(lastTree!.root.children.find((c) => c.id === 5)!.linkGroup).toBe(7);
    off();
  });

  // ─── — engine-side single-member link-group enforcement ─────
  //
  // Five tests covering the three mutation paths from ROADMAP §1.1
  // that can leave a group with exactly one member, plus a regression
  // guard against over-eager demotion and an undo round-trip.

  it("path 1: leaving a 2-member group demotes BOTH members to 0", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });

    // Fixture: Smoke (id=0) + Sparks (id=3) share linkGroup=1. Leave
    // Smoke via groupId=null — Sparks is now alone in group 1, so
    // demotes Sparks too.
    await b.request({
      kind: "linkGroups/set-membership",
      params: { ids: [0], groupId: null },
    });
    expect(lastTree).not.toBeNull();
    const smoke  = lastTree!.root.children.find((c) => c.id === 0)!;
    const sparks = lastTree!.root.children.find((c) => c.id === 3)!;
    expect(smoke.linkGroup).toBe(0);
    expect(sparks.linkGroup).toBe(0);
    off();
  });

  it("path 1b: joining a different group that shrinks the previous group demotes the lone survivor", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });

    // Fixture: Smoke (id=0) + Sparks (id=3) share linkGroup=1. Flash
    // (id=5) is unlinked. Move Smoke into a 2-member-with-Flash group
    // (say groupId=7) via a single dispatch — leaves Sparks alone in
    // group 1, which demotes.
    //
    // Use two dispatches to set this up clearly: first add Flash to
    // group 7 (creates singleton — gets demoted), then bulk-assign
    // [Smoke, Flash] to 7 (creates valid 2-member group AND shrinks
    // group 1 to {Sparks} → Sparks demoted).
    await b.request({
      kind: "linkGroups/set-membership",
      params: { ids: [0, 5], groupId: 7 },
    });
    expect(lastTree).not.toBeNull();
    const smoke  = lastTree!.root.children.find((c) => c.id === 0)!;
    const sparks = lastTree!.root.children.find((c) => c.id === 3)!;
    const flash  = lastTree!.root.children.find((c) => c.id === 5)!;
    expect(smoke.linkGroup).toBe(7);
    expect(flash.linkGroup).toBe(7);
    expect(sparks.linkGroup).toBe(0);  // demoted — was alone in group 1
    off();
  });

  it("path 2: deleting one member of a 2-member group demotes the survivor", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });

    // Fixture: Smoke (id=0) + Sparks (id=3) share linkGroup=1.
    // Delete Smoke; Sparks is now alone in group 1 → demoted.
    await b.request({
      kind: "emitters/delete",
      params: { id: 0 },
    });
    expect(lastTree).not.toBeNull();
    const sparks = lastTree!.root.children.find((c) => c.id === 3)!;
    expect(sparks.linkGroup).toBe(0);
    off();
  });

  it("regression guard: deleting from a 3-member group keeps the remaining 2 in their group", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });

    // Build a 3-member group first: Smoke + Sparks + Flash all in
    // group 9. (Smoke + Sparks were already in group 1 — overwrite.)
    await b.request({
      kind: "linkGroups/set-membership",
      params: { ids: [0, 3, 5], groupId: 9 },
    });
    expect(lastTree!.root.children.find((c) => c.id === 0)!.linkGroup).toBe(9);
    // Delete Smoke; Sparks + Flash still in group 9 (count = 2) — no
    // demotion should fire.
    await b.request({
      kind: "emitters/delete",
      params: { id: 0 },
    });
    const sparks = lastTree!.root.children.find((c) => c.id === 3)!;
    const flash  = lastTree!.root.children.find((c) => c.id === 5)!;
    expect(sparks.linkGroup).toBe(9);
    expect(flash.linkGroup).toBe(9);
    off();
  });

  it("idempotence: a no-op dispatch on an already-enforced tree changes nothing", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });

    // First dispatch: assign [Smoke, Sparks] to group 7 (already
    // sharing group 1, but explicit reassignment is fine — both end
    // at 7, group 7 has 2 members, no demotion). Confirms the
    // fixture's group 1 → group 7 reassignment doesn't accidentally
    // demote anyone.
    await b.request({
      kind: "linkGroups/set-membership",
      params: { ids: [0, 3], groupId: 7 },
    });
    expect(lastTree!.root.children.find((c) => c.id === 0)!.linkGroup).toBe(7);
    expect(lastTree!.root.children.find((c) => c.id === 3)!.linkGroup).toBe(7);

    // Second dispatch: re-assign the same ids to the same group.
    // Enforcement runs again but the tree state is already correct →
    // no change.
    await b.request({
      kind: "linkGroups/set-membership",
      params: { ids: [0, 3], groupId: 7 },
    });
    expect(lastTree!.root.children.find((c) => c.id === 0)!.linkGroup).toBe(7);
    expect(lastTree!.root.children.find((c) => c.id === 3)!.linkGroup).toBe(7);
    off();
  });

  // ─── Screen 4 Batch B3 — drag/drop reorder + reparent ────────────

  it("emitters/drop { mode: 'reorder' } reorders the fixture roots", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    // Fixture roots: Smoke(0), Sparks(3), Flash(5). Drop Smoke at
    // gap 3 (after Flash) → expected post-order [Sparks, Flash, Smoke].
    const r = await b.request({
      kind: "emitters/drop",
      params: { mode: "reorder", id: 0, rootIndex: 3 },
    });
    expect(r.ok).toBe(true);
    expect(lastTree).not.toBeNull();
    const namesAfter = lastTree!.root.children.map((c) => c.name);
    expect(namesAfter).toEqual(["Sparks", "Flash", "Smoke"]);
    off();
  });

  it("emitters/drop { mode: 'reparent' } reparents under the target in the named slot", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    // Fixture: Flash (id=5) is an unlinked leaf with NO children;
    // Sparks (id=3) is a root with only a lifetime child. Reparent
    // Flash under Sparks in the death slot — Sparks should gain Flash
    // as a death child; the root list should shrink by one.
    const r = await b.request({
      kind: "emitters/drop",
      params: { mode: "reparent", id: 5, targetId: 3, slot: "death" },
    });
    expect(r.ok).toBe(true);
    expect(lastTree).not.toBeNull();
    // Flash should no longer be a root.
    const rootIds = lastTree!.root.children.map((c) => c.id);
    expect(rootIds).not.toContain(5);
    // Sparks should now have Flash as a death-role child.
    const sparks = lastTree!.root.children.find((c) => c.id === 3)!;
    const flashUnderSparks = sparks.children.find((c) => c.id === 5);
    expect(flashUnderSparks).toBeDefined();
    expect(flashUnderSparks!.role).toBe("death");
    off();
  });

  // ─── Screen 4 Batch C — clipboard (copy / cut / paste) ───────────

  it("emitters/copy stashes the named subtrees and emits no tree change", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    // Copy Smoke (id=0) — should NOT emit tree/changed (read-only op).
    const r = await b.request({ kind: "emitters/copy", params: { ids: [0] } });
    expect(r).toEqual({});
    expect(lastTree).toBeNull();
    // Tree is untouched.
    const after = await b.request({ kind: "emitters/list", params: {} });
    expect(after.root.children.map((c) => c.name)).toEqual(["Smoke", "Sparks", "Flash"]);
    off();
  });

  it("emitters/cut serialises + deletes + a follow-up paste restores the subtree as a new root", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    // Cut Sparks (id=3) — has a lifetime child (id=4). After cut, the
    // root list should be [Smoke, Flash] (Sparks gone), with the
    // clipboard holding the Sparks subtree.
    await b.request({ kind: "emitters/cut", params: { ids: [3] } });
    expect(lastTree).not.toBeNull();
    expect(lastTree!.root.children.map((c) => c.name)).toEqual(["Smoke", "Flash"]);
    // Paste back — clipboard buffer is consumed by reference (deep
    // clone on every set), so a fresh paste should produce a Sparks
    // subtree with fresh ids appended at the end of roots.
    const r = await b.request({ kind: "emitters/paste", params: {} });
    expect(r.newIds).toHaveLength(1);
    expect(lastTree).not.toBeNull();
    const names = lastTree!.root.children.map((c) => c.name);
    expect(names).toEqual(["Smoke", "Flash", "Sparks"]);
    // The pasted Sparks should carry the child (Spark trail).
    const pastedSparks = lastTree!.root.children.find((c) => c.name === "Sparks")!;
    expect(pastedSparks.children).toHaveLength(1);
    expect(pastedSparks.children[0]!.name).toBe("Spark trail");
    // Pasted ids are fresh (above the pre-cut max=5).
    expect(pastedSparks.id).toBeGreaterThan(5);
    off();
  });

  it("emitters/paste with afterId splices the pasted roots after the named root", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    // Copy Flash (id=5), then paste with afterId=0 (Smoke). Expected
    // root order after paste: [Smoke, Flash-copy, Sparks, Flash].
    await b.request({ kind: "emitters/copy", params: { ids: [5] } });
    await b.request({ kind: "emitters/paste", params: { afterId: 0 } });
    expect(lastTree).not.toBeNull();
    const names = lastTree!.root.children.map((c) => c.name);
    expect(names).toEqual(["Smoke", "Flash", "Sparks", "Flash"]);
    off();
  });

  it("linkGroups/reset-exempt-fields drops the per-group entry back to defaults", async () => {
    const b = new MockBridge();
    await b.request({
      kind: "linkGroups/set-exempt-fields",
      params: { groupId: 1, fields: ["lifetime"] },
    });
    await b.request({
      kind: "linkGroups/reset-exempt-fields",
      params: { groupId: 1 },
    });
    const r = await b.request({
      kind: "linkGroups/list-exempt-fields",
      params: { groupId: 1 },
    });
    expect(r.fields).toEqual(expect.arrayContaining([
      "colorTexture", "normalTexture", "trackIndex",
    ]));
  });

  // ─── — linkGroups/diff-membership (join-conflict preview) ──

  it("linkGroups/diff-membership returns the seeded conflicts for a real-group join", async () => {
    const b = new MockBridge();
    useMockLinkGroupConflicts.getState().setConflicts([
      { id: 4, fields: ["lifetime", "gravity"] },
    ]);
    const r = await b.request({
      kind: "linkGroups/diff-membership",
      params: { ids: [4], groupId: 1 },
    });
    expect(r.conflicts).toEqual([{ id: 4, fields: ["lifetime", "gravity"] }]);
  });

  it("linkGroups/diff-membership reports no conflicts when leaving (groupId null/0)", async () => {
    const b = new MockBridge();
    useMockLinkGroupConflicts.getState().setConflicts([
      { id: 4, fields: ["lifetime"] },
    ]);
    const left = await b.request({
      kind: "linkGroups/diff-membership",
      params: { ids: [4], groupId: null },
    });
    expect(left.conflicts).toEqual([]);
    const zero = await b.request({
      kind: "linkGroups/diff-membership",
      params: { ids: [4], groupId: 0 },
    });
    expect(zero.conflicts).toEqual([]);
  });

  // ─── Screen 6 Batch A — emitters/get-tracks ─────────────────────
  //
  // Read-only DTO contract. The wire shape is always 7 tracks in
  // `TRACK_NAMES` order; an unknown id returns empty-keys placeholders
  // rather than an error so the React panel can render a "no data"
  // stub without special-casing failure.

  it("emitters/get-tracks returns 7 tracks in TRACK_NAMES order", async () => {
    const b = new MockBridge();
    const r = await b.request({
      kind: "emitters/get-tracks",
      params: { id: 0 },  // Smoke (fixture root)
    });
    expect(r.tracks).toHaveLength(7);
    expect(r.tracks.map((t) => t.name)).toEqual([
      "red", "green", "blue", "alpha",
      "scale", "index", "rotationSpeed",
    ]);
    for (const t of r.tracks) {
      expect(["linear", "smooth", "step"]).toContain(t.interpolation);
      expect(Array.isArray(t.keys)).toBe(true);
      // Keys are sorted ascending by time.
      for (let i = 1; i < t.keys.length; i++) {
        expect(t.keys[i]!.time).toBeGreaterThanOrEqual(t.keys[i - 1]!.time);
      }
    }
    // Fixture sanity: alpha track has the classic fade-in / fade-out
    // four-key shape used in the panel screenshots.
    const alpha = r.tracks.find((t) => t.name === "alpha");
    expect(alpha?.keys).toHaveLength(4);
  });

  it("emitters/get-tracks returns 7 empty-keys tracks for an unknown id", async () => {
    const b = new MockBridge();
    const r = await b.request({
      kind: "emitters/get-tracks",
      params: { id: 9999 },
    });
    expect(r.tracks).toHaveLength(7);
    for (const t of r.tracks) {
      expect(t.keys).toEqual([]);
    }
  });

  // ─── Screen 5 / Screen 6 Batch B-α — track mutations ────────────
  //
  // delete-track-keys removes the named keys (silently skipping border
  // keys — first + last in time order); set-track-interpolation
  // overrides the track's interpolation type. Both round-trip through
  // a subsequent `emitters/get-tracks` to prove the overlay is wired.

  it("emitters/delete-track-keys removes the specified non-border key from the track", async () => {
    const b = new MockBridge();
    // Alpha on id=0 (Smoke) has 4 keys: time 0, 20, 80, 100.
    // Border keys are time 0 and 100. Deleting time=20 should leave
    // 3 keys (times 0, 80, 100); deleting time=0 is a no-op (border).
    await b.request({
      kind: "emitters/delete-track-keys",
      params: { id: 0, track: "alpha", times: [20] },
    });
    const after = await b.request({
      kind: "emitters/get-tracks",
      params: { id: 0 },
    });
    const alpha = after.tracks.find((t) => t.name === "alpha");
    expect(alpha?.keys).toHaveLength(3);
    expect(alpha?.keys.map((k) => k.time)).toEqual([0, 80, 100]);

    // Border-key delete attempt is a silent no-op.
    await b.request({
      kind: "emitters/delete-track-keys",
      params: { id: 0, track: "alpha", times: [0, 100] },
    });
    const after2 = await b.request({
      kind: "emitters/get-tracks",
      params: { id: 0 },
    });
    const alpha2 = after2.tracks.find((t) => t.name === "alpha");
    expect(alpha2?.keys).toHaveLength(3);
  });

  it("emitters/set-track-interpolation updates the track's interpolation type", async () => {
    const b = new MockBridge();
    // The Smoke fixture's alpha track is "linear" by default. Flip to
    // "smooth" and re-read.
    await b.request({
      kind: "emitters/set-track-interpolation",
      params: { id: 0, track: "alpha", interpolation: "smooth" },
    });
    const after = await b.request({
      kind: "emitters/get-tracks",
      params: { id: 0 },
    });
    const alpha = after.tracks.find((t) => t.name === "alpha");
    expect(alpha?.interpolation).toBe("smooth");

    // Round-trip to "step".
    await b.request({
      kind: "emitters/set-track-interpolation",
      params: { id: 0, track: "alpha", interpolation: "step" },
    });
    const after2 = await b.request({
      kind: "emitters/get-tracks",
      params: { id: 0 },
    });
    expect(after2.tracks.find((t) => t.name === "alpha")?.interpolation).toBe("step");
  });

  // ─── Screen 6 Batch B-β — track key mutations ────────────────────
  //
  // set-track-key moves an existing key (erase oldTime, insert
  // newTime/newValue). Border keys silently fix newTime = oldTime.
  // add-track-key inserts a new key, dedupes by epsilon-bump on
  // exact-time collisions.

  it("emitters/set-track-key moves an interior key to (newTime, newValue)", async () => {
    const b = new MockBridge();
    // Alpha on Smoke (id=0) has 4 keys: time 0, 20, 80, 100. Move
    // time=20 → (25, 0.5).
    await b.request({
      kind: "emitters/set-track-key",
      params: {
        id: 0,
        track: "alpha",
        oldTime: 20,
        newTime: 25,
        newValue: 0.5,
      },
    });
    const after = await b.request({
      kind: "emitters/get-tracks",
      params: { id: 0 },
    });
    const alpha = after.tracks.find((t) => t.name === "alpha");
    expect(alpha?.keys.map((k) => k.time)).toEqual([0, 25, 80, 100]);
    const moved = alpha?.keys.find((k) => k.time === 25);
    expect(moved?.value).toBe(0.5);
    // Border-key time change is silently downgraded to value-only.
    await b.request({
      kind: "emitters/set-track-key",
      params: {
        id: 0,
        track: "alpha",
        oldTime: 0,
        newTime: 5,           // would change the border key's time
        newValue: 0.25,
      },
    });
    const after2 = await b.request({
      kind: "emitters/get-tracks",
      params: { id: 0 },
    });
    const alpha2 = after2.tracks.find((t) => t.name === "alpha");
    expect(alpha2?.keys[0]?.time).toBe(0);    // time unchanged
    expect(alpha2?.keys[0]?.value).toBe(0.25); // value moved
  });

  it("emitters/add-track-key inserts a new key and returns the actual inserted time", async () => {
    const b = new MockBridge();
    // Insert time=40 on Alpha (which has 0, 20, 80, 100 by default).
    const r = await b.request({
      kind: "emitters/add-track-key",
      params: { id: 0, track: "alpha", time: 40, value: 0.6 },
    });
    expect(r.time).toBe(40);
    expect(r.value).toBe(0.6);
    const after = await b.request({
      kind: "emitters/get-tracks",
      params: { id: 0 },
    });
    const alpha = after.tracks.find((t) => t.name === "alpha");
    expect(alpha?.keys.map((k) => k.time)).toEqual([0, 20, 40, 80, 100]);
    // Collision at exact time → host bumps by 0.001.
    const r2 = await b.request({
      kind: "emitters/add-track-key",
      params: { id: 0, track: "alpha", time: 40, value: 0.9 },
    });
    expect(r2.time).toBeCloseTo(40.001, 4);
    const after2 = await b.request({
      kind: "emitters/get-tracks",
      params: { id: 0 },
    });
    const alpha2 = after2.tracks.find((t) => t.name === "alpha");
    expect(alpha2?.keys.length).toBe(6);
  });

  // ─── Phase 4.1 Fix dispatch 1 — emitters/get-properties + set-properties

  it("emitters/get-properties returns a full EmitterPropertiesDto with Basic/Appearance/Physics fields", async () => {
    const b = new MockBridge();
    const r = await b.request({
      kind: "emitters/get-properties",
      params: { id: 0 },
    });
    expect(r).toHaveProperty("properties");
    const p = r.properties;
    // Basic fields
    expect(p).toHaveProperty("name");
    expect(p).toHaveProperty("lifetime");
    expect(p).toHaveProperty("useBursts");
    expect(p).toHaveProperty("nParticlesPerSecond");
    expect(p).toHaveProperty("randomRotation");
    expect(p).toHaveProperty("parentLinkStrength");
    expect(p).toHaveProperty("index");
    // Appearance fields
    expect(p).toHaveProperty("colorTexture");
    expect(p).toHaveProperty("blendMode");
    expect(p).toHaveProperty("hasTail");
    expect(p).toHaveProperty("randomColors");
    expect(p.randomColors).toHaveLength(4);
    // Physics fields
    expect(p).toHaveProperty("acceleration");
    expect(p.acceleration).toHaveLength(3);
    expect(p).toHaveProperty("gravity");
    expect(p).toHaveProperty("groundBehavior");
    expect(p).toHaveProperty("weatherCubeSize");
    // Groups
    expect(p).toHaveProperty("groups");
    expect(p.groups).toHaveLength(3);
    expect(p.groups[0]).toHaveProperty("type");
    expect(p.groups[0]).toHaveProperty("min");
    expect(p.groups[0].min).toHaveLength(3);
  });

  it("emitters/set-properties applies a partial patch that is observable via get-properties", async () => {
    const b = new MockBridge();
    // Verify baseline differs from the patch values so the round-trip
    // assertion is meaningful.
    const before = await b.request({
      kind: "emitters/get-properties",
      params: { id: 0 },
    });
    expect(before.properties.lifetime).not.toBe(5.0);
    expect(before.properties.useBursts).toBe(false);

    await b.request({
      kind: "emitters/set-properties",
      params: { id: 0, patch: { lifetime: 5.0, useBursts: true, nBursts: 3 } },
    });

    const after = await b.request({
      kind: "emitters/get-properties",
      params: { id: 0 },
    });
    expect(after.properties.lifetime).toBe(5.0);
    expect(after.properties.useBursts).toBe(true);
    expect(after.properties.nBursts).toBe(3);
    // Untouched fields stay at their fixture values.
    expect(after.properties.nParticlesPerSecond).toBe(before.properties.nParticlesPerSecond);
  });

  // ─── Settings — cross-mode registry (raw lighting + Force Align) ──
  //
  // `settings/lighting` returns the raw lighting split (intensity/colour
  // kept separate, angles in degrees) the panel seeds from; the native
  // host reads it from the registry. The `forceAlign` field reflects the
  // live flag, and `…/set` round-trips it within a MockBridge instance.
  it("settings/lighting returns the raw split and forceAlign defaults to true", async () => {
    const b = new MockBridge();
    const s = await b.request({ kind: "settings/lighting", params: {} });
    expect(s.forceAlign).toBe(true);
    // Raw split: intensity kept separate from colour (not folded).
    expect(s.sun.intensity).toBe(0.5);
    expect(s.sun.az).toBe(0);
    expect(s.sun.alt).toBe(45);
    expect(s.fill1.az).toBe(120);
    expect(s.fill2.az).toBe(210);
    // Colours are packed COLORREFs (low byte = R).
    expect(s.sun.diffuse & 0xff).toBe(180);
    expect(s).toHaveProperty("ambient");
    expect(s).toHaveProperty("shadow");
  });

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

  it("on() returns a working unsubscribe", async () => {
    const b = new MockBridge();
    const seen: Event[] = [];
    const off = b.on("engine/state/changed", (e) => { seen.push(e); });
    await b.request({ kind: "engine/set/ground-z", params: { z: 1 } });
    off();
    await b.request({ kind: "engine/set/ground-z", params: { z: 2 } });
    expect(seen).toHaveLength(1);
  });
});

describe("MockBridge emitters/move-many (preserve order at the edge)", () => {
  function rootsTree(names: string[]): EmitterTreeDto {
    return {
      root: {
        id: -1, stableId: 0, name: "", role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN,
        children: names.map((name, i) => ({
          id: i, stableId: 100 + i, name, role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN, children: [],
        })),
      },
    };
  }
  const rootNames = () =>
    useMockEmitterTree.getState().tree.root.children.map((c) => c.name);

  it("moves a contiguous block together", async () => {
    useMockEmitterTree.setState({ tree: rootsTree(["A", "B", "C", "D"]) });
    const b = new MockBridge();
    const r = await b.request({ kind: "emitters/move-many", params: { ids: [1, 2], direction: "up" } });
    expect(rootNames()).toEqual(["B", "C", "A", "D"]);
    expect(r.newIds).toEqual([1, 2]); // mock keeps ids stable through a move
  });

  it("freezes a contiguous selection pinned at the top edge", async () => {
    useMockEmitterTree.setState({ tree: rootsTree(["A", "B", "C", "D"]) });
    const b = new MockBridge();
    await b.request({ kind: "emitters/move-many", params: { ids: [0, 1], direction: "up" } });
    expect(rootNames()).toEqual(["A", "B", "C", "D"]); // nothing moved
  });

  it("freezes a NON-contiguous selection whose lead is pinned (preserve, no compacting)", async () => {
    useMockEmitterTree.setState({ tree: rootsTree(["A", "B", "C", "D"]) });
    const b = new MockBridge();
    await b.request({ kind: "emitters/move-many", params: { ids: [0, 2], direction: "up" } });
    // A is pinned at the top → the whole selection freezes. (Compacting would
    // have produced ["A", "C", "B", "D"]; order is preserved instead.)
    expect(rootNames()).toEqual(["A", "B", "C", "D"]);
  });

  it("translates a non-contiguous selection that is not edge-pinned", async () => {
    useMockEmitterTree.setState({ tree: rootsTree(["A", "B", "C", "D"]) });
    const b = new MockBridge();
    await b.request({ kind: "emitters/move-many", params: { ids: [1, 3], direction: "up" } });
    expect(rootNames()).toEqual(["B", "A", "D", "C"]);
  });

  it("freezes at the bottom edge for move down", async () => {
    useMockEmitterTree.setState({ tree: rootsTree(["A", "B", "C", "D"]) });
    const b = new MockBridge();
    await b.request({ kind: "emitters/move-many", params: { ids: [2, 3], direction: "down" } });
    expect(rootNames()).toEqual(["A", "B", "C", "D"]);
  });
});

describe("MockBridge stableId semantics (reorder-glide identity contract)", () => {
  // The glide keys React rows + FLIP maps by stableId. Three invariants the
  // host (ParticleSystem.cpp ctors/copySharedParamsFrom) and the mock must
  // both uphold; the mock is the only automatable side, so pin it here:
  //   presence+uniqueness, stability across reorders, freshness on copies.
  const flatten = (n: EmitterTreeNode): Array<[string, number]> =>
    [[n.name, n.stableId] as [string, number], ...n.children.flatMap(flatten)];
  const stableIdsByName = () =>
    new Map(useMockEmitterTree.getState().tree.root.children.flatMap(flatten));
  const allStableIds = () =>
    flatten(useMockEmitterTree.getState().tree.root).map(([, s]) => s);

  it("emitters/list: synthetic root has stableId 0; every node carries a unique positive stableId", async () => {
    const b = new MockBridge();
    const tree = await b.request({ kind: "emitters/list", params: {} });
    expect(tree.root.stableId).toBe(0);
    const ids = tree.root.children.flatMap(flatten).map(([, s]) => s);
    expect(ids.every((s) => s > 0)).toBe(true);
    expect(new Set(ids).size).toBe(ids.length);
  });

  it("stableIds survive reorder-many and move-many (same emitter, same id)", async () => {
    const b = new MockBridge();
    const before = stableIdsByName();
    await b.request({ kind: "emitters/reorder-many", params: { ids: [5], rootIndex: 0 } });
    await b.request({ kind: "emitters/move-many", params: { ids: [3], direction: "down" } });
    const after = stableIdsByName();
    for (const [name, sid] of before) expect(after.get(name), name).toBe(sid);
  });

  it("duplicate-many yields FRESH stableIds; the tree stays unique", async () => {
    const b = new MockBridge();
    const before = new Set(allStableIds());
    const r = await b.request({ kind: "emitters/duplicate-many", params: { ids: [0] } });
    expect(r.ok).toBe(true);
    const after = allStableIds();
    expect(new Set(after).size).toBe(after.length); // still unique
    const fresh = after.filter((s) => !before.has(s));
    expect(fresh.length).toBeGreaterThan(0); // the copies got new identities
  });
});

describe("MockBridge dirty-bit for batch structural mutations", () => {
  // isMutating must flag emitters/move-many and emitters/duplicate-many so the
  // mock's dirty-bit / save-prompt gate matches the native host's markDirty
  // rule. Both shipped in PR #104 but were missing from the allowlist, so a
  // multi-select move/duplicate left the document falsely clean in the mock.
  it("emitters/move-many marks the document dirty", async () => {
    const b = new MockBridge();
    expect((await b.request({ kind: "engine/state/snapshot", params: {} })).dirty).toBe(false);
    // Default fixture roots: Smoke(0), Sparks(3), Flash(5). Moving Smoke down
    // is a real structural mutation.
    await b.request({ kind: "emitters/move-many", params: { ids: [0], direction: "down" } });
    expect((await b.request({ kind: "engine/state/snapshot", params: {} })).dirty).toBe(true);
  });

  it("emitters/duplicate-many marks the document dirty", async () => {
    const b = new MockBridge();
    expect((await b.request({ kind: "engine/state/snapshot", params: {} })).dirty).toBe(false);
    await b.request({ kind: "emitters/duplicate-many", params: { ids: [0] } });
    expect((await b.request({ kind: "engine/state/snapshot", params: {} })).dirty).toBe(true);
  });

  // Audit fix D — the mock fired markDirty UNCONDITIONALLY for any mutating
  // kind, so a REFUSED or NO-OP drag-commit (which the native host leaves
  // clean, marking dirty only on a real mutation) falsely dirtied the doc.
  it("a REFUSED emitters/drop (own-footprint reorder) leaves the document clean", async () => {
    const b = new MockBridge();
    // Smoke is root 0; dropping it at root index 0 is its own position → refused.
    const r = await b.request({ kind: "emitters/drop", params: { mode: "reorder", id: 0, rootIndex: 0 } });
    expect((r as { ok: boolean }).ok).toBe(false);
    expect((await b.request({ kind: "engine/state/snapshot", params: {} })).dirty).toBe(false);
  });

  it("a REFUSED emitters/reorder-many (own-footprint) leaves the document clean", async () => {
    const b = new MockBridge();
    // Selecting root 0 and landing at gap 0 is the block's own footprint → refused.
    const r = await b.request({ kind: "emitters/reorder-many", params: { ids: [0], rootIndex: 0 } });
    expect((r as { ok: boolean }).ok).toBe(false);
    expect((await b.request({ kind: "engine/state/snapshot", params: {} })).dirty).toBe(false);
  });

  it("a NO-OP emitters/move-many (edge-pinned) leaves the document clean", async () => {
    const b = new MockBridge();
    // Smoke is the TOP root; moving it up is pinned → nothing moves.
    await b.request({ kind: "emitters/move-many", params: { ids: [0], direction: "up" } });
    expect((await b.request({ kind: "engine/state/snapshot", params: {} })).dirty).toBe(false);
  });
});

describe("emitters/set-track-lock — read aliasing (native parity)", () => {
  // These tests pin the DERIVE-AT-READ semantics: a locked channel is a live
  // view of its master's CANONICAL content, not a stale copy taken at lock
  // time. Three invariants that must all hold simultaneously:
  //   1. Master edits AFTER the lock are visible through the follower.
  //   2. Unlock restores the follower's own pre-lock canonical content.
  //   3. Chained locks (blue→green while green→red) present green's
  //      CANONICAL content, not the mirror green currently displays.

  it("Mirror after master edit: green reflects red keys added after lock", async () => {
    const b = new MockBridge();
    // Lock green → red on emitter id 1.
    await b.request({
      kind: "emitters/set-track-lock",
      params: { id: 1, channel: "green", lockTo: "red" },
    });
    // Add a key on RED after the lock (time=42, value=0.42).
    await b.request({
      kind: "emitters/add-track-key",
      params: { id: 1, track: "red", time: 42, value: 0.42 },
    });
    const r = await b.request({ kind: "emitters/get-tracks", params: { id: 1 } });
    const green = r.tracks.find((t) => t.name === "green");
    const red   = r.tracks.find((t) => t.name === "red");
    expect(green?.lockedTo).toBe("red");
    // green must mirror red's current keys exactly.
    expect(green?.keys).toEqual(red?.keys);
    // The key added after the lock must appear in green.
    expect(green?.keys.some((k) => k.time === 42)).toBe(true);
  });

  it("Unlock restores: green regains its own pre-lock canonical keys", async () => {
    const b = new MockBridge();
    // Capture green's canonical keys BEFORE locking.
    const before = await b.request({ kind: "emitters/get-tracks", params: { id: 1 } });
    const preLockGreenKeys = before.tracks.find((t) => t.name === "green")!.keys;

    // Lock green → red.
    await b.request({
      kind: "emitters/set-track-lock",
      params: { id: 1, channel: "green", lockTo: "red" },
    });
    // Add a key on red so the locked state diverges from the pre-lock snapshot.
    await b.request({
      kind: "emitters/add-track-key",
      params: { id: 1, track: "red", time: 77, value: 0.77 },
    });

    // Unlock green (lockTo: null).
    await b.request({
      kind: "emitters/set-track-lock",
      params: { id: 1, channel: "green", lockTo: null },
    });

    const after = await b.request({ kind: "emitters/get-tracks", params: { id: 1 } });
    const greenAfter = after.tracks.find((t) => t.name === "green");
    expect(greenAfter?.lockedTo).toBeNull();
    // green's keys must match the pre-lock snapshot exactly.
    expect(greenAfter?.keys).toEqual(preLockGreenKeys);
  });

  it("Chained lock: blue→green presents green's CANONICAL keys (not red mirror)", async () => {
    const b = new MockBridge();
    // Capture green's pre-lock canonical keys.
    const before = await b.request({ kind: "emitters/get-tracks", params: { id: 1 } });
    const greenCanonicalKeys = before.tracks.find((t) => t.name === "green")!.keys;

    // Lock green → red (green now displays red's content).
    await b.request({
      kind: "emitters/set-track-lock",
      params: { id: 1, channel: "green", lockTo: "red" },
    });
    // Mutate red so green's displayed mirror diverges from green's canonical.
    await b.request({
      kind: "emitters/add-track-key",
      params: { id: 1, track: "red", time: 55, value: 0.55 },
    });

    // Lock blue → green. The alias must follow green's CANONICAL content
    // (what trackContents[green] holds), NOT the red mirror green displays.
    await b.request({
      kind: "emitters/set-track-lock",
      params: { id: 1, channel: "blue", lockTo: "green" },
    });

    const r = await b.request({ kind: "emitters/get-tracks", params: { id: 1 } });
    const blue = r.tracks.find((t) => t.name === "blue");
    expect(blue?.lockedTo).toBe("green");
    // blue must show green's pre-lock (canonical) keys, NOT red's keys.
    expect(blue?.keys).toEqual(greenCanonicalKeys);
  });
});

describe("emitter tree spawn params ()", () => {
  //: tree nodes must mirror the LIVE spawn values from the
  // properties overlay (fixture defaults + set-properties patches), not
  // the frozen ZERO_SPAWN placeholder the tree-store literals carry for
  // type satisfaction. Decoration happens at the mock's emit/return
  // choke points.
  it("emitters/list nodes mirror the properties overlay's spawn fields", async () => {
    const b = new MockBridge();
    const tree = await b.request({ kind: "emitters/list", params: {} });
    const first = tree.root.children[0]!;
    const { properties } = await b.request({
      kind: "emitters/get-properties", params: { id: first.id },
    });
    expect(first.spawn).toEqual({
      lifetime: properties.lifetime,
      useBursts: properties.useBursts,
      nBursts: properties.nBursts,
      burstDelay: properties.burstDelay,
      nParticlesPerSecond: properties.nParticlesPerSecond,
      nParticlesPerBurst: properties.nParticlesPerBurst,
    });
  });

  it("a set-properties spawn patch is reflected in the next tree/changed payload", async () => {
    const b = new MockBridge();
    const events: EmitterTreeDto[] = [];
    const off = b.on("emitters/tree/changed", (e) => { events.push(e.payload); });
    await b.request({
      kind: "emitters/set-properties",
      params: { id: 0, patch: { nParticlesPerSecond: 4242 } },
    });
    off();
    expect(events.length).toBeGreaterThan(0);
    const node = events.at(-1)!.root.children.find((n) => n.id === 0);
    expect(node?.spawn.nParticlesPerSecond).toBe(4242);
  });
});
