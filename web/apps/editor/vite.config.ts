import { defineConfig, type Plugin } from "vite";
import react from "@vitejs/plugin-react";
import tailwindcss from "@tailwindcss/vite";
import path from "node:path";
import { execSync } from "node:child_process";
import { writeFileSync } from "node:fs";
import { readAppVersion } from "./app-version";

// Build-time app version for the About dialog. Single source of truth is the
// C header src/version.h (PE_VERSION_STR), shared with the binary's
// VS_VERSION_INFO and the legacy Win32 About — so the React About always
// matches the download. vitest.config.ts mirrors this via the same parser.
const APP_VERSION = readAppVersion(
  path.resolve(__dirname, "../../../src/version.h"),
);

// BUILD_DATE: the committer date of HEAD in YYYY-MM-DD form. Stable
// across rebuilds of the same commit, so the About dialog's
// "Build date" reflects when the code was actually committed rather
// than when somebody happened to run `pnpm build`. Using `new Date()`
// here was the source of handoff item 16's lone real golden-drift
// surface (dialog-about) — every rebuild on a different day shifted
// the value and broke the a11y golden.
//
// Fallback path: if we can't reach git (release tarball, detached
// build environment without .git/), fall back to today's date so the
// dialog still renders. The fallback is acceptable because the only
// place anyone reads BUILD_DATE is the About dialog; the goldens
// only matter inside a git checkout, where the primary path runs.
const BUILD_DATE = (() => {
  try {
    return execSync("git show -s --format=%cs HEAD", {
      encoding: "utf8",
      cwd: __dirname,
    }).trim();
  } catch {
    return new Date().toISOString().slice(0, 10);
  }
})();

// Stamp dist/build-meta.json after the bundle is written so the native
// test harness (scripts/run-native-tests.mjs) can verify the baked
// hosting mode matches the lane it's about to run. The mode is otherwise
// constant-folded inline into the minified bundle (not greppable), so an
// explicit marker is the only robust source of truth for "which mode is
// this dist/?". `hostingMode` is the field the harness compares; `commit`
// + `builtAt` are diagnostic-only (surfaced in the gate's fail-fast
// message). `builtAt` is intentionally volatile — it's never byte-
// compared by any golden (it lives in a gitignored file), so unlike the
// About dialog's BUILD_DATE it needs no normalizer (cf.).
function buildMetaPlugin(): Plugin {
  return {
    name: "alo-build-meta",
    closeBundle() {
      const hostingMode =
        process.env.VITE_HOSTING_MODE === "legacy" ? "legacy" : "composition";
      let commit = "unknown";
      try {
        commit = execSync("git rev-parse --short HEAD", {
          encoding: "utf8",
          cwd: __dirname,
        }).trim();
      } catch {
        // Release tarball / detached build without .git — leave "unknown".
      }
      const meta = { hostingMode, commit, builtAt: new Date().toISOString() };
      writeFileSync(
        path.resolve(__dirname, "dist/build-meta.json"),
        JSON.stringify(meta, null, 2) + "\n",
      );
    },
  };
}

export default defineConfig({
  plugins: [react(), tailwindcss(), buildMetaPlugin()],
  resolve: {
    alias: { "@": path.resolve(__dirname, "./src") },
  },
  base: "./",
  server: {
    port: 5174,        // 5173 is used by viewport-poc; pick a fresh port for the real app
    strictPort: true,
  },
  build: { outDir: "dist", emptyOutDir: true },
  define: {
    "import.meta.env.VITE_APP_VERSION": JSON.stringify(APP_VERSION),
    "import.meta.env.VITE_BUILD_DATE": JSON.stringify(BUILD_DATE),
  },
});
