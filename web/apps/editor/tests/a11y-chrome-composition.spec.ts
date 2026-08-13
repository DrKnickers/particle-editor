import { test, expect, chromium, type Page, type Browser } from "@playwright/test";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import { captureDomA11y } from "./helpers/a11y-dom-snapshot";
import { CHROME_SURFACES, seedCanonicalUiState } from "./helpers/a11y-surfaces";
import "./helpers/toMatchJSONGolden";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";
// Absolute path because the editor's CWD is repo-root (per run-native-tests.mjs:66),
// not the tests dir — see the matching comment in a11y-chrome.spec.ts (HWND lane).
// ESM-equivalent of __dirname (package is "type": "module").
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const FIXTURE_PATH = path.resolve(__dirname, "fixtures/a11y-base-state.alo");

let browser: Browser;
let page: Page;

test.beforeAll(async () => {
  browser = await chromium.connectOverCDP(CDP_ENDPOINT);
  const context = browser.contexts()[0];
  if (!context) throw new Error("CDP: no browser contexts attached");
  page = context.pages()[0] ?? (await context.waitForEvent("page"));
  await page.waitForFunction(
    () => typeof (window as { bridge?: unknown }).bridge !== "undefined",
    null,
    { timeout: 15_000 }
  );
  await seedCanonicalUiState(page); // pin canonical UI state (light theme + Spawner visible)
});

test.afterAll(async () => {
  // Mirror the HWND lane's afterAll: unfreeze stats + reset to a fresh
  // one-root-emitter state so subsequent spec files in the same host
  // process don't see contamination from this lane's beforeEach.
  if (page) {
    await page.evaluate(async () => {
      const bridge = (window as { bridge?: { request: (req: { kind: string; params: unknown }) => Promise<unknown> } }).bridge;
      if (bridge) {
        await bridge.request({ kind: "stats/set-frozen", params: { frozen: false } });
        // beforeEach pauses the preview clock; revert it or every later
        // spec file in the shared host runs with frozen sim time.
        await bridge.request({ kind: "engine/set/paused", params: { paused: false } });
        await bridge.request({ kind: "file/new", params: {} });
      }
    });
  }
  // Don't close the CDP connection — see the matching comment in
  // a11y-chrome.spec.ts (HWND lane).
});

test.beforeEach(async () => {
  // Reset to known-clean state — close any open menus / dialogs left by
  // the previous test. Cheaper than relaunching the binary.
  await page.keyboard.press("Escape");
  await page.keyboard.press("Escape");
  // Move mouse to (0,0) so the cursor/position-3d emitter doesn't fire
  // mid-capture with non-deterministic coordinates.
  await page.mouse.move(0, 0);
  // Load deterministic base state from fixture. Absolute path resolves from
  // this spec's __dirname (see FIXTURE_PATH comment above).
  await page.evaluate(
    async (fixturePath) => {
      const bridge = (window as { bridge: { request: (req: { kind: string; params: unknown }) => Promise<unknown> } }).bridge;
      await bridge.request({ kind: "file/open", params: { path: fixturePath } });
      // Pause the particle simulation so live values don't leak into the
      // snapshot. See a11y-chrome.spec.ts (HWND lane) for full reasoning.
      await bridge.request({ kind: "engine/set/paused", params: { paused: true } });
      // Freeze the StatusBar's 4 Hz stats stream so cells
      // render `—` placeholders deterministically.
      await bridge.request({ kind: "stats/set-frozen", params: { frozen: true } });
    },
    FIXTURE_PATH
  );
});

test.describe("a11y/chrome [composition]", () => {
  for (const surface of CHROME_SURFACES) {
    test(`${surface.id} [composition]`, async () => {
      try {
        await surface.setup(page);
        const snap = await captureDomA11y(page);
        expect(snap).toMatchJSONGolden(
          `a11y-goldens/${surface.id}.composition.golden.yaml`
        );
      } finally {
        await surface.teardown(page);
      }
    });
  }
});
