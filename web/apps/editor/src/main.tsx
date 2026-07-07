import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { App } from "@/App";
import { ErrorBoundary } from "@/components/ErrorBoundary";
import "@/styles/globals.css";

// DEV-ONLY: install the Playwright layout-lane test seam (window.__atlasTest).
// The dynamic import behind import.meta.env.DEV means `vite build` excludes the
// seam module from the production bundle entirely (see src/dev/atlas-test-seam.ts).
if (import.meta.env.DEV) {
  void import("@/dev/atlas-test-seam").then((m) => m.installAtlasTestSeam());
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
