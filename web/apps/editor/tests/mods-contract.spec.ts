// D6: mods/* bridge surface contract test against the live host.
//
// The Mods menu's UI is exercised in vitest (jsdom can render Radix
// menus); these specs verify the wire contract holds end-to-end:
// schema-declared response shapes match what the C++ dispatcher
// emits, and mods/select round-trips through ModManager into the
// snapshot's activeModPath field.
//
// The dev machine's installed-mod list is not fixed (whatever you
// have in <gameRoot>/{corruption,GameData}/Mods determines content),
// so these specs assert on *shape*, not specific entries. A CI
// machine with no mods installed will return an empty mods array;
// the contract is the same.

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
    { timeout: 15_000 },
  );
});

test.afterAll(async () => {
  await browser?.close();
});

test("mods/list returns the expected shape", async () => {
  const r = await page.evaluate(async () => {
    type AnyBridge = {
      request(r: { kind: string; params: object }): Promise<unknown>;
    };
    const b = (window as { bridge?: AnyBridge }).bridge;
    if (!b) throw new Error("window.bridge not attached");
    return b.request({ kind: "mods/list", params: {} });
  });
  // Shape: { mods: ModDescriptor[]; activePath: string | null }.
  expect(r).toHaveProperty("mods");
  expect(r).toHaveProperty("activePath");
  const typed = r as { mods: unknown; activePath: unknown };
  expect(Array.isArray(typed.mods)).toBe(true);
  expect(typed.activePath === null || typeof typed.activePath === "string").toBe(true);
});

test("mods/select with path:null lands as activeModPath:null in snapshot", async () => {
  const after = await page.evaluate(async () => {
    type AnyBridge = {
      request(r: { kind: string; params: object }): Promise<unknown>;
    };
    const b = (window as { bridge?: AnyBridge }).bridge;
    if (!b) throw new Error("window.bridge not attached");
    await b.request({ kind: "mods/select", params: { path: null } });
    const snap = (await b.request({
      kind: "engine/state/snapshot",
      params: {},
    })) as { activeModPath: string | null };
    return snap.activeModPath;
  });
  expect(after).toBe(null);
});

test("mods/refresh returns the same shape as mods/list", async () => {
  const r = await page.evaluate(async () => {
    type AnyBridge = {
      request(r: { kind: string; params: object }): Promise<unknown>;
    };
    const b = (window as { bridge?: AnyBridge }).bridge;
    if (!b) throw new Error("window.bridge not attached");
    return b.request({ kind: "mods/refresh", params: {} });
  });
  expect(r).toHaveProperty("mods");
  expect(r).toHaveProperty("activePath");
});
