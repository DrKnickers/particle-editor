// Playwright config for the WEB layout lane — real-browser DOM-layout tests that
// jsdom can't do (centering, geometry). Drives the MOCK app via the Vite dev
// server in headless Chromium, so it runs in CI on Linux with NO native host and
// NO CDP-viewport conflict (the specs measure React DOM only).
//
// Deliberately separate from playwright.config.ts (which CDP-connects to a running
// ParticleEditor.exe and is NOT in CI). Specs live in ./tests-web so the native
// config's testDir (./tests) never picks them up.
import { defineConfig, devices } from "@playwright/test";

const PORT = 5174;

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
    reuseExistingServer: !process.env.CI,
    timeout: 120_000,
  },
});
