// Task 2.4 contract tests: Background picker wired against the *real*
// native bridge inside ParticleEditor.exe --new-ui --test-host. Sibling
// of bridge-native.spec.ts (Task 2.2.1) — same CDP-attach harness, same
// `window.bridge` host-object channel, but exercising the surface the
// BackgroundPicker actually drives:
//
//   - engine/set/skydome-slot     (bundled slot mutation)
//   - engine/set/background       (COLORREF round-trip — Win32 byte order)
//   - engine/set/skydome-custom-path (custom slot persistence)
//   - undo/perform                (handler dispatch, Task 2.4 surface)
//
// Notes on TestHostBridge.on(): the host-object channel doesn't carry
// events, so TestHostBridge.on returns a no-op unsubscribe. Specs that
// would otherwise wait on `engine/state/changed` instead poll a fresh
// `engine/state/snapshot` after the mutation lands.
import { test, expect, chromium, type Page, type Browser } from "@playwright/test";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";

let browser: Browser;
let page: Page;

test.beforeAll(async () => {
  browser = await chromium.connectOverCDP(CDP_ENDPOINT);
  const context = browser.contexts()[0];
  if (!context) throw new Error("CDP: no browser contexts attached");
  const pages = context.pages();
  page = pages[0] ?? (await context.waitForEvent("page"));

  await page.waitForFunction(
    () => typeof (window as { bridge?: unknown }).bridge !== "undefined",
    null,
    { timeout: 15_000 }
  );
});

test.afterAll(async () => {
  await browser?.close();
});

test("Background popover opens from the toolbar dropdown trigger", async () => {
  // Task 2.2: the BackgroundPicker slide-in ToolPanel was replaced by a
  // Radix Popover triggered from the Toolbar's Group 4 dropdown. The
  // dropdown button still carries aria-label="Background", but the
  // mounted content is now a popover wrapper (data-radix-popper-content-wrapper)
  // rather than role="dialog". Slot count assertion is preserved —
  // BackgroundPickerBody still renders 12 aria-pressed slot buttons.
  const probe = await page.evaluate(async () => {
    const btn = document.querySelector<HTMLButtonElement>('button[aria-label="Background"]');
    if (!btn) return { clicked: false, popover: false, slots: 0 };
    btn.click();
    await new Promise((r) => setTimeout(r, 250));
    const popover = document.querySelector('[data-radix-popper-content-wrapper]');
    const slots = popover?.querySelectorAll("button[aria-pressed]").length ?? 0;
    return { clicked: true, popover: !!popover, slots };
  });
  expect(probe.clicked).toBe(true);
  expect(probe.popover).toBe(true);
  expect(probe.slots).toBe(12);
});

test("engine/set/skydome-slot mutates state (bundled slot 5)", async () => {
  const after = await page.evaluate(async () => {
    type AnyBridge = {
      request(r: { kind: string; params: object }): Promise<unknown>;
    };
    const b = (window as { bridge?: AnyBridge }).bridge;
    if (!b) throw new Error("window.bridge not attached");
    await b.request({ kind: "engine/set/skydome-slot", params: { slot: 5 } });
    const snap = (await b.request({
      kind: "engine/state/snapshot",
      params: {},
    })) as { skydomeSlot: number };
    return snap.skydomeSlot;
  });
  expect(after).toBe(5);
});

test("engine/set/background round-trips a COLORREF (orange = 0x000088ff)", async () => {
  // COLORREF in Win32 is 0x00BBGGRR — low byte is red, not blue. The
  // colorref.ts helpers in the React app handle the swap; this spec
  // exercises the wire format directly. 0x000088ff is "orange-ish":
  // R=0xff, G=0x88, B=0x00 — verifies the dispatcher doesn't reorder
  // bytes on the way through.
  const after = await page.evaluate(async () => {
    const b = (window as { bridge?: { request(r: { kind: string; params: object }): Promise<unknown> } })
      .bridge;
    if (!b) throw new Error("window.bridge not attached");
    await b.request({ kind: "engine/set/background", params: { rgb: 0x000088ff } });
    const snap = (await b.request({
      kind: "engine/state/snapshot",
      params: {},
    })) as { background: number };
    return snap.background;
  });
  expect(after).toBe(0x000088ff);
});

test("engine/set/skydome-custom-path persists across snapshots (slot 9)", async () => {
  const after = await page.evaluate(async () => {
    const b = (window as { bridge?: { request(r: { kind: string; params: object }): Promise<unknown> } })
      .bridge;
    if (!b) throw new Error("window.bridge not attached");
    await b.request({
      kind: "engine/set/skydome-custom-path",
      params: { slot: 9, path: "C:/fake/test.dds" },
    });
    const snap = (await b.request({
      kind: "engine/state/snapshot",
      params: {},
    })) as { skydomeCustomPaths: string[] };
    // The DTO flattens slots 9..11 to a 0..2 array (see
    // BuildEngineStateSnapshot in BridgeDispatcher.cpp).
    return snap.skydomeCustomPaths[0];
  });
  expect(after).toBe("C:/fake/test.dds");
});

test("undo/perform reports applied:false when no captures have been recorded", async () => {
  // Task 2.4 caveat: UndoStack is constructed in HostWindow and reachable
  // through the bridge, but engine setters are not yet wrapped in
  // Capture() calls. Until Phase 3 emitter work lands, every undo
  // request resolves with `applied:false`. This spec asserts the
  // surface is wired end-to-end without claiming functional undo of
  // engine state.
  const r = await page.evaluate(async () => {
    const b = (window as { bridge?: { request(r: { kind: string; params: object }): Promise<unknown> } })
      .bridge;
    if (!b) throw new Error("window.bridge not attached");
    return b.request({
      kind: "undo/perform",
      params: { direction: "undo" },
    });
  });
  expect(r).toMatchObject({ applied: false });
});
