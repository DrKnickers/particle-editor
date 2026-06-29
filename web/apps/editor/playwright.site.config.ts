// Playwright config for the landing-page smoke (spec §7). Serves the static repo-root
// site/ via a dependency-free node server (NOT Vite — the site has no toolchain).
// Separate from playwright.web.config.ts (testDir ./tests-web) so neither picks up the
// other's specs. webServer cwd is this package (web/apps/editor), so the command path is
// relative to it.
import { defineConfig, devices } from "@playwright/test";

const PORT = 5175;

export default defineConfig({
  testDir: "./tests-site",
  globalSetup: "./tests-site/global-setup.mjs",
  fullyParallel: true,
  workers: process.env.CI ? 1 : undefined,
  reporter: "list",
  timeout: 30_000,
  use: {
    baseURL: `http://localhost:${PORT}`,
    trace: "on-first-retry",
  },
  projects: [{ name: "chromium", use: { ...devices["Desktop Chrome"] } }],
  webServer: {
    command: `node tests-site/serve.mjs`,
    env: { PORT: String(PORT) },
    url: `http://localhost:${PORT}`,
    reuseExistingServer: !process.env.CI,
    timeout: 60_000,
  },
});
