// Playwright config for Task 2.2 contract tests against the native bridge.
//
// The harness (scripts/run-native-tests.mjs) launches
// ParticleEditor.exe --new-ui --test-host in the background, waits for
// CDP to come up on :9222, then runs these tests. Specs connect to that
// shared CDP endpoint — there's only one host process at a time, so
// fullyParallel + multiple workers would race against a single page.
import { defineConfig } from "@playwright/test";

export default defineConfig({
  testDir: "./tests",
  fullyParallel: false,
  workers: 1,
  reporter: "list",
  use: { trace: "on-first-retry" },
  timeout: 30_000,
});
