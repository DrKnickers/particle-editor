// Pure drift-decision logic for the a11y goldens drift check. No I/O — fed
// `git status --porcelain` text + the two capture passes, returns the verdict.
// Unit-tested via `node --test` (scripts/ has no Vitest coverage; Vitest only
// collects src/**, and tsc -b only typechecks src/, so this stays plain ESM).

const GOLDENS_PREFIX = "web/apps/editor/tests/a11y-goldens/";

/**
 * Extract changed a11y-golden paths from `git status --porcelain` output.
 * Porcelain lines are `XY<space>path` (path at column 3). Regeneration only
 * modifies/adds goldens (never renames), so column-3-onward is the path.
 * Returns repo-relative paths under the goldens dir, deduped + sorted.
 */
export function parseChangedGoldens(porcelainText) {
  const paths = new Set();
  for (const raw of porcelainText.split("\n")) {
    const line = raw.replace(/\r$/, "");
    if (line.trim() === "") continue;
    const path = line.slice(3).trim();
    if (path.startsWith(GOLDENS_PREFIX)) paths.add(path);
  }
  return [...paths].sort();
}

/**
 * Split two capture passes' changed-golden lists into `stable` (changed in
 * BOTH — real drift) and `noisy` (changed in only one — run-to-run
 * nondeterminism that slipped past the normalizer).
 */
export function stableDrift(firstChanged, secondChanged) {
  const a = new Set(firstChanged);
  const b = new Set(secondChanged);
  const stable = [...a].filter((p) => b.has(p)).sort();
  const noisy = [...new Set([...a, ...b])]
    .filter((p) => !(a.has(p) && b.has(p)))
    .sort();
  return { stable, noisy };
}

/**
 * Map a stable/noisy split to the harness verdict + process exit code:
 *   stable non-empty            -> drift (exit 2; also lists any noisy files)
 *   stable empty, noisy present -> noise (exit 0, normalizer gap)
 *   both empty                  -> none  (exit 0)
 */
export function verdict({ stable, noisy }) {
  if (stable.length > 0) {
    const noisyNote = noisy.length > 0
      ? `\nA11Y-DRIFT: also nondeterministic (normalizer gap)\n${noisy.join("\n")}`
      : "";
    return {
      kind: "drift",
      exitCode: 2,
      message: `A11Y-DRIFT: ${stable.length} file(s)\n${stable.join("\n")}${noisyNote}`,
    };
  }
  if (noisy.length > 0) {
    return {
      kind: "noise",
      exitCode: 0,
      message: `A11Y-DRIFT: nondeterministic (normalizer gap)\n${noisy.join("\n")}`,
    };
  }
  return { kind: "none", exitCode: 0, message: "A11Y-DRIFT: none" };
}
