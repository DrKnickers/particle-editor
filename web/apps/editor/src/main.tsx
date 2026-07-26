import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { App } from "@/App";
import { ErrorBoundary } from "@/components/ErrorBoundary";
import { applyMode, readStoredMode } from "@/lib/theme";
import "@/styles/globals.css";

// Apply the persisted theme SYNCHRONOUSLY, before the first render — the
// App-level effect ran after the initial paint, so a light-theme user got a
// dark first frame (pre-PR review 2026-07-18). applyMode's first call skips
// the theme-transition cross-fade (no prior data-theme), and App.tsx keeps
// the live prefers-color-scheme listener for "system" mode.
applyMode(readStoredMode());

// DEV-ONLY: install the Playwright layout-lane test seam (window.__atlasTest).
// The dynamic import behind import.meta.env.DEV means `vite build` excludes the
// seam module from the production bundle entirely (see src/dev/atlas-test-seam.ts).
if (import.meta.env.DEV) {
  void import("@/dev/atlas-test-seam").then((m) => m.installAtlasTestSeam());
  // DEV-ONLY: palette layout seam (window.__paletteTest) — seeds the mock
  // palette so the #683 layout spec can measure a populated popover.
  void import("@/dev/palette-test-seam").then((m) => m.installPaletteTestSeam());
  // DEV-ONLY: install the React Profiler audit seam (window.__profilerAudit).
  // Same dynamic-import stripping guarantee (see src/dev/profiler-audit.ts).
  void import("@/dev/profiler-audit").then((m) => m.installProfilerAuditSeam());
}

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <ErrorBoundary>
      <App />
    </ErrorBoundary>
  </StrictMode>
);
