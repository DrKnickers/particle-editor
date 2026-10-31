// Opt-in diagnostic; share the mock app server and browser settings.
import { defineConfig } from "@playwright/test";
import webConfig from "./playwright.web.config";

export default defineConfig({
  ...webConfig,
  testIgnore: [],
  testMatch: "**/profiler-audit.spec.ts",
});
