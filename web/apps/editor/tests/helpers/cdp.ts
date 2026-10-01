// Shared CDP connection for the native specs.
//
// scripts/run-native-tests.mjs launches ONE ParticleEditor.exe --test-host and
// runs every native spec against its CDP endpoint (workers: 1). The connection
// is worker-scoped: it is opened once, shared by every spec file in the worker,
// and closed at worker teardown. Specs read the page in their own beforeAll:
//
//   let page: Page;
//   test.beforeAll(async ({ cdpPage }) => {
//     page = cdpPage;
//   });
//
// One connection per worker also retires the per-file close/reconnect cycle,
// whose async Target cleanup could race the next file's reconnect and surface
// as "Target page closed" mid-test.
import { test as base, chromium, type Browser, type Page } from "@playwright/test";
import "./bridge-request"; // declares window.bridge

export { expect, type Browser, type Locator, type Page } from "@playwright/test";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";

type CdpFixtures = {
  cdpBrowser: Browser;
  cdpPage: Page;
};

export const test = base.extend<{}, CdpFixtures>({
  cdpBrowser: [
    async ({}, use) => {
      const browser = await chromium.connectOverCDP(CDP_ENDPOINT);
      await use(browser);
      await browser.close();
    },
    { scope: "worker" },
  ],
  cdpPage: [
    async ({ cdpBrowser }, use) => {
      const context = cdpBrowser.contexts()[0];
      if (!context) throw new Error("CDP: no browser contexts attached");
      const pages = context.pages();
      // Pick the page that actually has window.bridge (skip DevTools targets).
      let found: Page | null = null;
      for (const p of pages) {
        try {
          if (await p.evaluate(() => typeof window.bridge !== "undefined")) {
            found = p;
            break;
          }
        } catch {
          /* page not evaluable (e.g. devtools) — skip */
        }
      }
      const page = found ?? pages[0] ?? (await context.waitForEvent("page"));
      // The WebView2 navigation is async vs. the host launch; wait until
      // `window.bridge` is attached by App.tsx before any spec runs.
      await page.waitForFunction(() => typeof window.bridge !== "undefined", null, {
        timeout: 15_000,
      });
      await use(page);
    },
    { scope: "worker" },
  ],
});
