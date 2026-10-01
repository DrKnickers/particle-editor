// mods/* bridge surface contract test against the live host.
//
// The Mods menu's UI is exercised in vitest (jsdom can render Radix
// menus); these specs verify the wire contract holds end-to-end:
// schema-declared response shapes match what the C++ dispatcher
// emits, and mods/set-layers round-trips through ModManager into the
// snapshot's activeModPath field.
//
// The dev machine's installed-mod list is not fixed (whatever you
// have in <gameRoot>/{corruption,GameData}/Mods determines content),
// so these specs assert on *shape*, not specific entries. A CI
// machine with no mods installed will return an empty mods array;
// the contract is the same.

import { test, expect, type Page } from "./helpers/cdp";

let page: Page;

test.beforeAll(async ({ cdpPage }) => {
  page = cdpPage;
});

test("mods/list returns the expected shape", async () => {
  const r = await page.evaluate(async () => {
    const b = window.bridge;
    if (!b) throw new Error("window.bridge not attached");
    return b.request({ kind: "mods/list", params: {} });
  });
  for (const k of ["mods", "layers", "stack", "activePath"]) expect(r).toHaveProperty(k);
  const t = r as { mods: unknown; layers: unknown; stack: unknown; activePath: unknown };
  expect(Array.isArray(t.mods) && Array.isArray(t.layers) && Array.isArray(t.stack)).toBe(true);
  expect(t.activePath === null || typeof t.activePath === "string").toBe(true);
});

test("mods/set-layers [] lands as activeModPath:null in snapshot", async () => {
  const after = await page.evaluate(async () => {
    const b = window.bridge;
    if (!b) throw new Error("window.bridge not attached");
    await b.request({ kind: "mods/set-layers", params: { paths: [] } });
    const snap = (await b.request({ kind: "engine/state/snapshot", params: {} })) as { activeModPath: string | null };
    return snap.activeModPath;
  });
  expect(after).toBe(null);
});

test("mods/refresh returns the same shape as mods/list", async () => {
  const r = await page.evaluate(async () => {
    const b = window.bridge;
    if (!b) throw new Error("window.bridge not attached");
    return b.request({ kind: "mods/refresh", params: {} });
  });
  for (const k of ["mods", "layers", "stack", "activePath"]) expect(r).toHaveProperty(k);
});
