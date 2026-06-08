// Vitest config — point at the in-process contract tests under src/.
// Excludes the Playwright spec tree under tests/, which is driven by
// the test:native harness (different runner, different transport).
import { defineConfig } from "vitest/config";
import path from "node:path";

export default defineConfig({
  resolve: {
    alias: { "@": path.resolve(__dirname, "./src") },
  },
  // Mirror the build-time `define` from vite.config.ts so specs that
  // assert on `import.meta.env.VITE_*` (e.g. AboutDialog reading the
  // baked-in version string) see the same values Vite injects at build.
  // Kept in sync by hand — when vite.config.ts adds a new VITE_* key,
  // add it here too.
  define: {
    "import.meta.env.VITE_APP_VERSION": JSON.stringify("1.5"),
    "import.meta.env.VITE_BUILD_DATE": JSON.stringify("test-build"),
  },
  test: {
    include: ["src/**/__tests__/**/*.{test,spec}.{ts,tsx}"],
    exclude: ["node_modules/**", "dist/**", "tests/**"],
    environment: "jsdom",
    setupFiles: ["./src/test-setup.ts"],
  },
});
