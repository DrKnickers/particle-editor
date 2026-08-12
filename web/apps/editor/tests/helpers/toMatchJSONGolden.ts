// Custom Playwright matcher `expect(x).toMatchJSONGolden(path, opts)` for
// the a11y goldens.
//
// Behavior:
//   - String `received`: write as-is, no transformation. Used by the
//     composition lane, which captures Playwright's
//     `locator.ariaSnapshot()` YAML output. Goldens end in `.golden.yaml`.
//   - Object `received`: serialize as `JSON.stringify(value, null, 2) + "\n"`
//     and compare byte-for-byte. Used by the HWND lane, which
//     captures the normalized UIA tree from `uia_inspector.exe`. Goldens
//     end in `.golden.json`.
//   - With env `UPDATE_A11Y_GOLDENS=1`: write the serialized value to the
//     golden path instead of comparing. The matcher always reports `pass`
//     in update mode so a full spec run can refresh every golden in one
//     pass without spurious failures.
//   - On mismatch with `options.rawForDebug`, dump the raw pre-normalization
//     value (JSON-stringified) to `tests/a11y-failures/<basename>.raw.json`
//     (gitignored) so the developer can tell whether a diff originated
//     from the normalizer or from the underlying UIA / DOM tree.
//
// Despite the name, the matcher handles BOTH JSON (HWND lane) and YAML
// strings (composition lane). The "JSON" naming is historic; renaming
// would churn 4 HWND specs unnecessarily.
//
// Imported (side-effect) by every spec that uses `toMatchJSONGolden` — the
// `expect.extend` call and the global type augmentation only take effect
// once the module is loaded. The HWND and composition spec templates
// include the import at the top of each file.

import { expect, type MatcherReturnType } from "@playwright/test";
import * as fs from "node:fs";
import * as path from "node:path";
import { fileURLToPath } from "node:url";

// ESM-equivalent of __dirname (package is "type": "module").
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const UPDATE = process.env.UPDATE_A11Y_GOLDENS === "1";
// __dirname is .../tests/helpers, so FAILURE_DIR resolves to .../tests/a11y-failures.
const FAILURE_DIR = path.join(__dirname, "..", "a11y-failures");

// Volatile-content normalization — applied to BOTH the live snapshot and
// the committed golden before byte-comparison (and to the written value
// in UPDATE mode so the placeholder is what lands on disk).
//
// The canonical case is the About dialog's "Build date". It's baked from
// HEAD's commit date at build time (web/apps/editor/vite.config.ts) so
// that the dialog shows a meaningful, per-commit-stable date instead of
// "the day someone ran pnpm build". But that creates an unwinnable chase
// for the golden: the golden is committed in a LATER commit than the one
// whose date it records, so its baked date can NEVER equal the build date
// of the commit that contains it. Whatever date we freeze into the golden,
// the act of committing it advances HEAD to a newer date, and the next
// rebuild's BUILD_DATE no longer matches.
//
// Resolution: treat the date as volatile and normalize it to a stable
// placeholder, exactly like the JSON normalizer's `volatile` property
// list and the StatusBar source-side freeze. The About dialog
// still shows the real commit date to users; the test simply doesn't
// assert the specific value. Covers both lanes: composition (ariaSnapshot
// YAML, inline "Build date: YYYY-MM-DD") and HWND (UIA tree JSON, where
// the date is its own `"Name": "YYYY-MM-DD"` text node).
function normalizeVolatile(s: string): string {
  return s
    .replace(/Build date: \d{4}-\d{2}-\d{2}/g, "Build date: <DATE>")
    .replace(/"Name": "\d{4}-\d{2}-\d{2}"/g, '"Name": "<DATE>"');
}

expect.extend({
  toMatchJSONGolden(
    received: unknown,
    goldenPath: string,
    options?: { rawForDebug?: unknown },
  ): MatcherReturnType {
    // goldenPath is relative to tests/, e.g. "a11y-goldens/menubar.golden.json"
    // or "a11y-goldens/menubar.composition.golden.yaml" (composition lane).
    const absPath = path.resolve(__dirname, "..", goldenPath);
    // String inputs (composition lane, ariaSnapshot YAML) are written as-is;
    // object inputs (HWND lane, UIA tree) are JSON-stringified with 2-space
    // indent + trailing newline. Both end with "\n" so the golden file is
    // text-editor-friendly.
    const serialized = typeof received === "string"
      ? (received.endsWith("\n") ? received : received + "\n")
      : JSON.stringify(received, null, 2) + "\n";

    if (UPDATE) {
      // A gate run must NEVER take this branch. Update mode overwrites the
      // golden and returns pass unconditionally, so any regression reachable
      // while it is active gets blessed into the baseline and the suite goes
      // green on the broken output (2026-07 audit). Refusing here means
      // the blessing stays a deliberate, human-initiated act — which is what
      // it was always meant to be.
      if (process.env.PE_GATE_NO_REUSE) {
        return {
          pass: false,
          message: () =>
            `golden UPDATE mode is refused during a gate run (${goldenPath}). ` +
            `Update mode rewrites the baseline and always passes, so a gate ` +
            `that allowed it could not fail. Run the a11y update by hand.`,
        };
      }
      fs.mkdirSync(path.dirname(absPath), { recursive: true });
      // Write the normalized form so the committed golden stores the
      // `<DATE>` placeholder explicitly — self-documenting, rather than a
      // stale literal date that's silently normalized away on read.
      fs.writeFileSync(absPath, normalizeVolatile(serialized), "utf8");
      return {
        pass: true,
        message: () => `wrote golden: ${goldenPath}`,
      };
    }

    if (!fs.existsSync(absPath)) {
      return {
        pass: false,
        message: () =>
          `A11y golden missing: ${goldenPath}\n` +
          `  Hint: run \`pnpm a11y:update\` to create it.`,
      };
    }

    const expected = fs.readFileSync(absPath, "utf8");
    // Normalize both sides so a golden that still holds a literal date
    // (committed before this normalizer existed) matches a freshly-built
    // snapshot with a different date.
    const pass = normalizeVolatile(expected) === normalizeVolatile(serialized);
    if (!pass && options?.rawForDebug !== undefined) {
      fs.mkdirSync(FAILURE_DIR, { recursive: true });
      // Strip the trailing extension regardless of which lane (.json / .yaml).
      const base = path.basename(goldenPath).replace(/\.golden\.(json|yaml)$/, "");
      fs.writeFileSync(
        path.join(FAILURE_DIR, `${base}.raw.json`),
        JSON.stringify(options.rawForDebug, null, 2) + "\n",
        "utf8",
      );
    }

    return {
      pass,
      message: () => {
        if (pass) return `Matched: ${goldenPath}`;
        const base = path.basename(goldenPath).replace(/\.golden\.(json|yaml)$/, "");
        return (
          `A11y golden mismatch: ${goldenPath}\n` +
          `  Expected (committed) vs Received (current run) differ.\n` +
          `  Hint: if intended, run \`pnpm a11y:update --grep "<surface>"\`\n` +
          (options?.rawForDebug
            ? `  Raw pre-normalization written to a11y-failures/${base}.raw.json\n`
            : "")
        );
      },
    };
  },
});

// Type augmentation — Playwright extends matchers via the global
// PlaywrightTest.Matchers interface (see playwright/types/test.d.ts:8551).
// `declare module "@playwright/test"` does NOT work because the public
// .d.ts re-exports from "playwright/test" and never declares a `Matchers`
// interface in its own module namespace.
declare global {
  // eslint-disable-next-line @typescript-eslint/no-namespace
  namespace PlaywrightTest {
    interface Matchers<R> {
      toMatchJSONGolden(
        goldenPath: string,
        options?: { rawForDebug?: unknown },
      ): R;
    }
  }
}
