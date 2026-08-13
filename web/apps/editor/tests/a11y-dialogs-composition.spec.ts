import { test, expect, chromium, type Page, type Browser } from "@playwright/test";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import { captureDomA11y } from "./helpers/a11y-dom-snapshot";
import { DIALOG_SURFACES, seedCanonicalUiState } from "./helpers/a11y-surfaces";
import "./helpers/toMatchJSONGolden";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";
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
});

test.beforeEach(async () => {
  await page.keyboard.press("Escape");
  await page.keyboard.press("Escape");
  await page.mouse.move(0, 0);
  // Guard: a prior test's teardown may have triggered a window.location.href
  // navigation (a demo route resetting the ?demo= query param), which
  // temporarily undefines window.bridge while the app re-mounts. Mirror
  // the HWND lane's a11y-dialogs.spec.ts beforeEach pattern.
  await page.waitForFunction(
    () => typeof (window as { bridge?: unknown }).bridge !== "undefined",
    null,
    { timeout: 15_000 }
  );
  await page.evaluate(
    async (fixturePath) => {
      const bridge = (window as { bridge: { request: (req: { kind: string; params: unknown }) => Promise<unknown> } }).bridge;
      await bridge.request({ kind: "file/open", params: { path: fixturePath } });
      await bridge.request({ kind: "engine/set/paused", params: { paused: true } });
      await bridge.request({ kind: "stats/set-frozen", params: { frozen: true } });
    },
    FIXTURE_PATH
  );
});

test.describe("a11y/dialogs [composition]", () => {
  for (const surface of DIALOG_SURFACES) {
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
