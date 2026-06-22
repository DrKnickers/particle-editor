import { test } from "node:test";
import assert from "node:assert/strict";
import { readdirSync, readFileSync, existsSync } from "node:fs";
import { fileURLToPath } from "node:url";

// The DEV-only Playwright layout-lane seam (window.__atlasTest, from
// src/dev/atlas-test-seam.ts) is loaded behind `import.meta.env.DEV` via a dynamic
// import in main.tsx, so `vite build` must dead-code-eliminate it from the
// production bundle the host ships. This guards that — it runs in `test:scripts`,
// which CI's web job runs AFTER `vite build`, so dist/ exists there. Locally it
// skips when dist/ hasn't been built. (Locally it can also scan a STALE dist if you
// edited the seam without rebuilding; CI always builds fresh first, so the guard is
// authoritative only there.)
const distAssets = fileURLToPath(new URL("../../dist/assets/", import.meta.url));
const distMissing = !existsSync(distAssets);

// `__atlasTest` is a window-property string literal (survives minification);
// `atlas-test-seam` would be the dynamic-import chunk name if it were ever emitted.
const FORBIDDEN = ["__atlasTest", "atlas-test-seam"];

test(
  "the DEV test seam is absent from the production bundle",
  // Skip locally when dist/ hasn't been built — but NEVER skip under CI: the web
  // job builds before test:scripts, so a missing dist there means the build order
  // broke and the guard must FAIL loudly rather than silently pass-by-skip.
  { skip: distMissing && !process.env.CI ? "no dist/assets — run `pnpm build` first" : false },
  () => {
    assert.ok(!distMissing, "dist/assets missing under CI — `vite build` must run before test:scripts");
    const js = readdirSync(distAssets).filter((f) => f.endsWith(".js"));
    assert.ok(js.length > 0, "expected built JS assets in dist/assets/");
    for (const f of js) {
      const src = readFileSync(new URL(`../../dist/assets/${f}`, import.meta.url), "utf8");
      for (const marker of FORBIDDEN) {
        assert.ok(
          !src.includes(marker),
          `production bundle ${f} leaked the DEV test seam marker "${marker}" — the import.meta.env.DEV guard in main.tsx is not eliminating src/dev/atlas-test-seam.ts`,
        );
      }
    }
  },
);
