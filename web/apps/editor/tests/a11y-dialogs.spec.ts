import { test, expect, chromium, type Page, type Browser } from "@playwright/test";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import { captureUIA, discoverHostHwnd } from "./helpers/uia";
import { normalize } from "./helpers/a11y-normalizer";
import { DIALOG_SURFACES, seedCanonicalUiState } from "./helpers/a11y-surfaces";
import allowlist from "./helpers/a11y-allowlist.json" with { type: "json" };
import "./helpers/toMatchJSONGolden";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";
const COMPOSITION_MODE = process.env.ALO_HOSTING_MODE !== "legacy" /* */;
// Absolute path because the editor's CWD is repo-root (per run-native-tests.mjs:66),
// not the tests dir — a relative path here would resolve to <repo>/tests/fixtures/...
// which doesn't exist. Resolve from this spec's __dirname instead. See T9.0 pre-flight.
// ESM-equivalent of __dirname (package is "type": "module").
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const FIXTURE_PATH = path.resolve(__dirname, "fixtures/a11y-base-state.alo");

let browser: Browser;
let page: Page;
let hostHwnd: bigint;

test.beforeAll(async () => {
  if (COMPOSITION_MODE) return;  // skip body; per-test skip handles individual tests
  browser = await chromium.connectOverCDP(CDP_ENDPOINT);
  const context = browser.contexts()[0];
  if (!context) throw new Error("CDP: no browser contexts attached");
  page = context.pages()[0] ?? (await context.waitForEvent("page"));
  await page.waitForFunction(
    () => typeof (window as { bridge?: unknown }).bridge !== "undefined",
    null,
    { timeout: 15_000 }
  );
  await seedCanonicalUiState(page); //: pin canonical UI state (light theme + Spawner visible)
  hostHwnd = await discoverHostHwnd();
});

test.afterAll(async () => {
  // Unfreeze stats so subsequent spec files (in the same host process)
  // get stats/tick again. Without this, the next file's app-shell /
  // status-bar tests time out waiting for FPS updates that never come.
  // Guard with page-undefined since afterAll may fire when beforeAll
  // never ran (e.g. composition-mode skip).
  if (page && !COMPOSITION_MODE) {
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
  // Don't close the CDP connection — all a11y HWND specs connect to the
  // same underlying WebView2 process. Closing here triggers async Target
  // cleanup that races with the next spec's beforeAll reconnect, causing
  // "Target page closed" mid-test. The host process is killed by
  // run-native-tests.mjs after the full suite finishes.
});

test.beforeEach(async ({}, testInfo) => {
  // Post-T0 re-plan: HWND-mode spec auto-skips under composition.
  // Composition coverage is via T10's a11y-*-composition.spec.ts using
  // page.accessibility.snapshot() (Win32 UIA can't reach the React tree
  // in composition mode — see tasks/phase-0-a11y-cross-mode-probe.md).
  if (COMPOSITION_MODE) {
    testInfo.annotations.push({
      type: "skip-reason",
      description:
        "ALO_HOSTING_MODE != 'legacy' (composition mode active) — HWND Win32 UIA spec " +
        "auto-skips in composition mode. Use a11y-dialogs-composition.spec.ts " +
        "(DOM snapshot) for the composition lane."
    });
    test.skip();
  }
  // Reset to known-clean state — close any open menus / dialogs left by
  // the previous test. Cheaper than relaunching the binary.
  await page.keyboard.press("Escape");
  await page.keyboard.press("Escape");
  // Move mouse to (0,0) so the cursor/position-3d emitter doesn't fire
  // mid-capture with non-deterministic coordinates.
  await page.mouse.move(0, 0);
  // Guard: a prior test's teardown may have triggered a window.location.href
  // navigation (e.g. dialog-mod-nickname resets the ?demo= query param),
  // which temporarily undefines window.bridge while the app re-mounts.
  // Wait for bridge to be available before issuing bridge.request() calls.
  await page.waitForFunction(
    () => typeof (window as { bridge?: unknown }).bridge !== "undefined",
    null,
    { timeout: 15_000 }
  );
  // Load deterministic base state from fixture. Absolute path resolves from
  // this spec's __dirname (see FIXTURE_PATH comment above).
  await page.evaluate(
    async (fixturePath) => {
      const bridge = (window as { bridge: { request: (req: { kind: string; params: unknown }) => Promise<unknown> } }).bridge;
      await bridge.request({ kind: "file/open", params: { path: fixturePath } });
      // Pause the particle simulation so live particle values don't appear
      // in UIA spinner/edit Name fields. See a11y-chrome.spec.ts beforeEach
      // for the full reasoning.
      await bridge.request({ kind: "engine/set/paused", params: { paused: true } });
      // T9] Freeze the StatusBar's 4 Hz stats stream — see the
      // matching comment in a11y-chrome.spec.ts beforeEach.
      await bridge.request({ kind: "stats/set-frozen", params: { frozen: true } });
    },
    FIXTURE_PATH
  );
});

test.describe("a11y/dialogs [hwnd]", () => {
  for (const surface of DIALOG_SURFACES) {
    test(`${surface.id} [hwnd]`, async () => {
      try {
        await surface.setup(page);
        const raw = await captureUIA(hostHwnd, surface.id);
        const normalized = normalize(raw, allowlist);
        expect(normalized).toMatchJSONGolden(
          `a11y-goldens/${surface.id}.golden.json`,
          { rawForDebug: raw }
        );
      } finally {
        await surface.teardown(page);
      }
    });
  }
});
