import { test, expect, type Page } from "./helpers/cdp";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import { captureDomA11y } from "./helpers/a11y-dom-snapshot";
import { CUSTOM_PRIMITIVE_SURFACES, seedCanonicalUiState } from "./helpers/a11y-surfaces";
import "./helpers/toMatchJSONGolden";

// ESM-equivalent of __dirname (package is "type": "module").
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const FIXTURE_PATH = path.resolve(__dirname, "fixtures/a11y-base-state.alo");

let page: Page;

test.beforeAll(async ({ cdpPage }) => {
  page = cdpPage;
  await seedCanonicalUiState(page); // pin canonical UI state (light theme + Spawner visible)
});

test.afterAll(async () => {
  if (page) {
    await page.evaluate(async () => {
      const bridge = window.bridge;
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
  await page.evaluate(
    async (fixturePath) => {
      const bridge = window.bridge!;
      await bridge.request({ kind: "file/open", params: { path: fixturePath } });
      await bridge.request({ kind: "engine/set/paused", params: { paused: true } });
      await bridge.request({ kind: "stats/set-frozen", params: { frozen: true } });
    },
    FIXTURE_PATH
  );
});

test.describe("a11y/curve-spinner [composition]", () => {
  for (const surface of CUSTOM_PRIMITIVE_SURFACES) {
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
