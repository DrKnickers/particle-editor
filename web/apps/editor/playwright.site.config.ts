// Playwright config for the landing-page smoke (spec §7). Serves the static repo-root
// site/ via a dependency-free node server (NOT Vite — the site has no toolchain).
// Separate from playwright.web.config.ts (testDir ./tests-web) so neither picks up the
// other's specs. webServer cwd is this package (web/apps/editor), so the command path is
// relative to it.
import { defineConfig, devices } from "@playwright/test";

// SITE_PORT override: with a fixed port and reuseExistingServer, two worktrees running this
// suite concurrently silently attach to whichever server started first — and test the OTHER
// worktree's site/. (Observed live: a parallel session's server made this suite report its
// checkout's content.) Concurrent sessions should set distinct SITE_PORTs.
const PORT = Number(process.env.SITE_PORT ?? 5175);

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
    // See the WEB_PORT note in playwright.web.config.ts: reuse is a developer
    // convenience, never acceptable for a gate run. The SITE_PORT advice above
    // only helps someone who remembers to follow it; PE_GATE_NO_REUSE makes the
    // gate correct by default (audit an-audit-finding).
    reuseExistingServer: !process.env.CI && !process.env.PE_GATE_NO_REUSE,
    timeout: 60_000,
  },
});
