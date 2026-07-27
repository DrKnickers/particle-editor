// Mod-stack layering against the REAL native host (--test-host): set two
// fixture layers, reorder them, clear — asserting the mods/set-layers response
// and the follow-up mods/list state after every step. Closes the "mod-stack
// switching has no automated coverage" gap (mod-layer stacking shipped
// without it).
//
// Test isolation: mods/set-layers persistence is registry-gated under
// --test-host (ModManager::SetLayerStack allowPersist=false via the standard
// m_testHost/m_settingsLive gate), so this spec cannot rewrite the daily
// driver's LastLayers/LastMod. The afterAll [] reset is in-session hygiene for
// subsequent specs, not persistence cleanup.
//
// Fixture layers are empty-but-present dirs (tests/fixtures/mods/mod-*) —
// SetLayerStack keeps any path that passes PathIsDirectory; no Data content
// is required for stack mechanics.
import { test, expect, chromium, type Page, type Browser } from "@playwright/test";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";
const fixturesDir = resolve(dirname(fileURLToPath(import.meta.url)), "fixtures", "mods");
const modAlpha = resolve(fixturesDir, "mod-alpha");
const modBeta = resolve(fixturesDir, "mod-beta");

type SetLayersResult = { ok: boolean; stack: string[] };
type ModsList = { layers?: unknown; stack: string[]; activePath: string | null };

let browser: Browser;
let page: Page;

type BridgeWindow = {
  bridge: { request: (req: { kind: string; params: object }) => Promise<unknown> };
};

async function setLayers(paths: string[]): Promise<SetLayersResult> {
  return page.evaluate(
    (p) =>
      (window as unknown as BridgeWindow).bridge.request({
        kind: "mods/set-layers",
        params: { paths: p },
      }) as Promise<SetLayersResult>,
    paths,
  );
}

async function modsList(): Promise<ModsList> {
  return page.evaluate(
    () =>
      (window as unknown as BridgeWindow).bridge.request({
        kind: "mods/list",
        params: {},
      }) as Promise<ModsList>,
  );
}

test.beforeAll(async () => {
  browser = await chromium.connectOverCDP(CDP_ENDPOINT);
  const context = browser.contexts()[0];
  if (!context) throw new Error("CDP: no browser contexts attached");
  page = context.pages()[0] ?? (await context.waitForEvent("page"));
  await page.waitForFunction(
    () => typeof (window as { bridge?: unknown }).bridge !== "undefined",
    null,
    { timeout: 15_000 },
  );
});

test.afterAll(async () => {
  // In-session hygiene: leave the host unmodded for whatever spec runs next.
  try {
    await setLayers([]);
  } catch (err) {
    // Don't fail teardown, but don't be silent either — a leaked layer stack
    // can surface as confusing failures in LATER spec files.
    console.warn(`[mod-stack] afterAll reset failed (later specs may see layers): ${err}`);
  }
  await browser?.close();
});

const endsWithDir = (p: string, dir: string) =>
  p.replace(/[\\/]+$/, "").toLowerCase().endsWith(dir.toLowerCase());

test("set two fixture layers → stack reflects both, in order", async () => {
  const r = await setLayers([modAlpha, modBeta]);
  expect(r.ok).toBe(true);
  expect(r.stack).toHaveLength(2);
  expect(endsWithDir(r.stack[0], "mod-alpha")).toBe(true);
  expect(endsWithDir(r.stack[1], "mod-beta")).toBe(true);

  const list = await modsList();
  expect(list.stack).toHaveLength(2);
  expect(endsWithDir(list.activePath ?? "", "mod-alpha")).toBe(true); // primary = front
});

test("reorder → front layer (primary) swaps", async () => {
  const r = await setLayers([modBeta, modAlpha]);
  expect(r.ok).toBe(true);
  expect(r.stack).toHaveLength(2);
  expect(endsWithDir(r.stack[0], "mod-beta")).toBe(true);

  const list = await modsList();
  expect(endsWithDir(list.activePath ?? "", "mod-beta")).toBe(true);
});

test("nonexistent layer paths are dropped, not errored", async () => {
  const ghost = resolve(fixturesDir, "mod-does-not-exist");
  const r = await setLayers([modAlpha, ghost]);
  expect(r.ok).toBe(true);
  expect(r.stack).toHaveLength(1); // ghost silently filtered by SetLayerStack
  expect(endsWithDir(r.stack[0], "mod-alpha")).toBe(true);
});

test("clear → unmodded", async () => {
  const r = await setLayers([]);
  expect(r.ok).toBe(true);
  expect(r.stack).toHaveLength(0);

  const list = await modsList();
  expect(list.stack).toHaveLength(0);
  expect(list.activePath).toBeNull(); // unmodded reports null, not ""
});
