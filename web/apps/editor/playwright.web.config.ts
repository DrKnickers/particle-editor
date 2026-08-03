// Playwright config for the WEB layout lane — real-browser DOM-layout tests that
// jsdom can't do (centering, geometry). Drives the MOCK app via the Vite dev
// server in headless Chromium, so it runs in CI on Linux with NO native host and
// NO CDP-viewport conflict (the specs measure React DOM only).
//
// Deliberately separate from playwright.config.ts (which CDP-connects to a running
// ParticleEditor.exe and is NOT in CI). Specs live in ./tests-web so the native
// config's testDir (./tests) never picks them up.
import { defineConfig, devices } from "@playwright/test";

// WEB_PORT override, mirroring SITE_PORT in playwright.site.config.ts: with a
// fixed port AND reuseExistingServer, two worktrees running this lane at once
// would have the second silently reuse the FIRST checkout's Vite server — and
// therefore test the first checkout's app. Concurrent sessions should set
// distinct WEB_PORTs (2026-07 audit, G-1).
const PORT = Number(process.env.WEB_PORT ?? 5174);

export default defineConfig({
  testDir: "./tests-web",
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
    command: `pnpm exec vite --port ${PORT} --strictPort`,
    url: `http://localhost:${PORT}`,
    // Reuse is a developer convenience, never acceptable for a gate run: the
    // whole point of the lane is to test THIS checkout. PE_GATE_NO_REUSE is set
    // by scripts/run-all-tests.mjs, so `pnpm test:web` by hand keeps the fast
    // path while the gate always gets its own server (audit G-1). A port
    // already in use then fails the lane loudly instead of testing someone
    // else's app — set WEB_PORT to run two worktrees concurrently.
    reuseExistingServer: !process.env.CI && !process.env.PE_GATE_NO_REUSE,
    timeout: 120_000,
  },
});
