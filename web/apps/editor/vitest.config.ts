// Vitest config — point at the in-process contract tests under src/.
// Excludes the Playwright spec tree under tests/, which is driven by
// the test:native harness (different runner, different transport).
import { defineConfig } from "vitest/config";
import path from "node:path";
import { readAppVersion } from "./app-version";

export default defineConfig({
  resolve: {
    alias: { "@": path.resolve(__dirname, "./src") },
  },
  // Mirror the build-time `define` from vite.config.ts so specs that assert
  // on `import.meta.env.VITE_*` (e.g. AboutDialog reading the baked-in version
  // string) see the same values Vite injects at build. The version is read
  // from the same canonical header via the same parser as the real build, so
  // it can't drift; BUILD_DATE stays a fixed sentinel for golden stability.
  // When vite.config.ts adds a new VITE_* key, add it here too.
  define: {
    "import.meta.env.VITE_APP_VERSION": JSON.stringify(
      readAppVersion(path.resolve(__dirname, "../../../src/version.h")),
    ),
    "import.meta.env.VITE_BUILD_DATE": JSON.stringify("test-build"),
  },
  test: {
    include: ["src/**/__tests__/**/*.{test,spec}.{ts,tsx}"],
    exclude: ["node_modules/**", "dist/**", "tests/**"],
    environment: "jsdom",
    setupFiles: ["./src/test-setup.ts"],
    // Report-only coverage (no thresholds yet — see tasks/todo.md follow-ups).
    // Run via `pnpm test:coverage`; plain `pnpm test` skips instrumentation.
    // Scoped to src/ so Playwright helpers and node scripts don't dilute the
    // numbers. Only true test-support files are excluded — shipped-but-
    // jsdom-unreachable code (bridge/test-host.ts, PrimitivesGallery) stays
    // IN so the report never overstates coverage of production code; their
    // real coverage lives in the native Playwright lane.
    coverage: {
      provider: "v8",
      include: ["src/**"],
      exclude: ["src/**/__tests__/**", "src/test-setup.ts"],
      reporter: ["text-summary", "html", "json-summary"],
    },
  },
});
