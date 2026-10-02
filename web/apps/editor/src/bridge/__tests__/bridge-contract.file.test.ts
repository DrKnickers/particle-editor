// Contract tests for MockBridge — document file ops, the recent-files list, and the mod stack.
// Split by namespace from the former single bridge-contract.test.ts. Each
// request is exercised end-to-end: request → store mutation → event →
// follow-up read, keeping the schema and the MockBridge implementation honest.

import { describe, it, expect, beforeEach } from "vitest";
import { MockBridge } from "../mock";
import { resetMockState } from "@/test/mock-state";

// Reset the shared mock stores between tests so state mutations don't leak.
beforeEach(resetMockState);

describe("MockBridge contract — file/*, recent/*, mods/*", () => {
  // Layer stacking: mods/list surfaces a flat layer catalog + the
  // ordered stack; set-layers replaces the whole stack in precedence order,
  // dropping unknown paths + deduping; [] = Unmodded.
  it("mods/list surfaces a layer catalog + stack; set-layers round-trips in order", async () => {
    const b = new MockBridge();
    let list = await b.request({ kind: "mods/list", params: {} });
    expect(Array.isArray(list.layers)).toBe(true);
    expect(list.layers.length).toBeGreaterThan(1);
    expect(list.stack).toEqual([]);
    expect(list.activePath).toBe(null);

    const a = list.layers[0].path;  // "C:/mock/corruption/Mods/FoCMod"
    const c = list.layers[1].path;  // a nested layer
    const r = await b.request({ kind: "mods/set-layers", params: { paths: [c, a] } });
    expect(r).toMatchObject({ ok: true, stack: [c, a] });
    list = await b.request({ kind: "mods/list", params: {} });
    expect(list.stack).toEqual([c, a]);
    expect(list.activePath).toBe(c);

    // Unknown paths dropped; dedup; order preserved.
    await b.request({ kind: "mods/set-layers", params: { paths: [a, "C:/bogus", a, c] } });
    list = await b.request({ kind: "mods/list", params: {} });
    expect(list.stack).toEqual([a, c]);

    // [] = Unmodded.
    await b.request({ kind: "mods/set-layers", params: { paths: [] } });
    list = await b.request({ kind: "mods/list", params: {} });
    expect(list.stack).toEqual([]);
    expect(list.activePath).toBe(null);
  });

  // file/save / file/recent/list are now
  // implemented in the mock (and the native host). The old "throws not
  // implemented" assertion has been replaced by round-trip specs below
  // covering file/new, file/save-as, recent/changed.

  it("file/new round-trips, resets state, and re-selects the default root (legacy parity)", async () => {
    const b = new MockBridge();
    let lastSelected: { id: number | null } | null = null;
    b.on("emitters/selected", (e) => { lastSelected = e.payload; });

    // Pre-dirty + move selection OFF the default root, so file/new's reset to 0
    // is observable (not a coincidence of the default).
    await b.request({ kind: "engine/set/ground-z", params: { z: 12 } });
    await b.request({ kind: "emitters/select", params: { id: 1 } });
    expect((await b.request({ kind: "engine/state/snapshot", params: {} })).dirty)
      .toBe(true);

    // Like the native host, a dirty document is only replaced once the user
    // chose Don't Save.
    const refused = await b.request({ kind: "file/new", params: {} });
    expect(refused).toEqual({ ok: false, error: "unsaved-changes" });
    expect((await b.request({ kind: "engine/state/snapshot", params: {} })).dirty).toBe(true);

    const r = await b.request({ kind: "file/new", params: { discardUnsaved: true } });
    expect(r).toEqual({});
    const snap = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(snap.dirty).toBe(false);
    expect(snap.currentFilePath).toBeNull();
    expect(snap.groundZ).toBe(0);  // reset to default
    // Legacy parity: file/new re-selects the seeded root (id 0) AND announces it
    // via emitters/selected (so the EmitterTree's selection atom updates live).
    expect(snap.selectedEmitterId).toBe(0);
    expect(lastSelected).not.toBeNull();
    expect(lastSelected!.id).toBe(0);
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

  // file/open is no longer a hard reject. The native handler
  // shows GetOpenFileNameW; the mock resolves with the schema's
  // cancellation shape so the React handler's request chain aborts
  // cleanly in browser mode without surfacing a raw rejection.
  it("resolves file/open with ok:false in browser mode", async () => {
    const b = new MockBridge();
    const r = await b.request({ kind: "file/open", params: {} } as any);
    expect(r).toEqual({ ok: false, error: "browser-mode" });
  });

  it("file/open of an .alo over unsaved work is refused until the discard is confirmed", async () => {
    const b = new MockBridge();
    await b.request({ kind: "engine/set/ground-z", params: { z: 3 } });   // dirty
    expect(await b.request({ kind: "file/open", params: { path: "C:/a.alo" } }))
      .toEqual({ ok: false, error: "unsaved-changes" });
    let snap = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(snap.dirty).toBe(true);
    expect(snap.currentFilePath).toBeNull();

    // Texture pickers never touch the document, so they are never refused.
    expect(await b.request({ kind: "file/open", params: { filter: "skydome" } }))
      .toEqual({ ok: false, error: "browser-mode" });

    expect(await b.request({ kind: "file/open", params: { path: "C:/a.alo", discardUnsaved: true } }))
      .toEqual({ ok: true, path: "C:/a.alo" });
    snap = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(snap.dirty).toBe(false);
    expect(snap.currentFilePath).toBe("C:/a.alo");
  });

  it("file/new on a clean document needs no discard flag", async () => {
    const b = new MockBridge();
    expect(await b.request({ kind: "file/new", params: {} })).toEqual({});
  });
});
