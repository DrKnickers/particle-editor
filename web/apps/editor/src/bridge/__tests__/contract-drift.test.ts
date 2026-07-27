// Bridge contract-drift guard.
//
// The schema's `Request` union is the source of truth for every request the
// host can receive. MockBridge must handle each kind (so browser-mode + the
// Vitest contract suite stay faithful) OR explicitly defer it on the
// commented allowlist below. This test fails when a NEW request kind is added
// to the schema without a MockBridge handler — catching the drift at the
// point it's introduced rather than as a runtime "unknown request kind"
// throw deep in a Playwright run.
//
// A TS union has no runtime form to enumerate, so — like the contrast guard —
// we parse the two source files directly:
//   - the schema's `Request` union for every `kind: "..."` literal, bounded
//     to the union block so the sibling `Event` / response unions don't leak;
//   - MockBridge's `switch (req.kind)` for every `case "...":` label.
// (Vitest runs from web/apps/editor, so these relative paths resolve.)

import { describe, it, expect } from "vitest";
import { readFileSync } from "node:fs";

const schemaSrc = readFileSync(
  "../../packages/bridge-schema/src/index.ts",
  "utf8",
);
const mockSrc = readFileSync("src/bridge/mock.ts", "utf8");

/** Slice the `Request` union block: from `export type Request =` to the next
 *  top-level `export type ` declaration (which begins the response/event
 *  unions whose own `kind: "..."` literals must NOT be counted as requests). */
function requestUnionBlock(src: string): string {
  const start = src.indexOf("export type Request =");
  if (start < 0) throw new Error("`export type Request =` not found in schema");
  const rest = src.slice(start + "export type Request =".length);
  const end = rest.indexOf("\nexport type ");
  if (end < 0) throw new Error("end of Request union not found");
  return rest.slice(0, end);
}

/** Every `kind: "..."` literal in the Request union — one per request arm. */
function requestKinds(src: string): string[] {
  const block = requestUnionBlock(src);
  const re = /\bkind:\s*"([^"]+)"/g;
  const kinds = new Set<string>();
  let m: RegExpExecArray | null;
  while ((m = re.exec(block)) !== null) kinds.add(m[1]!);
  return [...kinds].sort();
}

/** Every `case "...":` label in MockBridge's dispatch switch. */
function mockCaseLabels(src: string): Set<string> {
  const re = /^\s*case\s*"([^"]+)":/gm;
  const labels = new Set<string>();
  let m: RegExpExecArray | null;
  while ((m = re.exec(src)) !== null) labels.add(m[1]!);
  return labels;
}

// Explicit, commented DEFERRED allowlist — request kinds the schema declares
// that MockBridge intentionally does NOT implement (browser mode lacks the
// engine machinery they need). Each is still REACHED by the mock's switch as a
// `throw "... not implemented"` arm, so a caller in browser mode
// fails loudly rather than silently. Removing a kind from the schema, or
// adding a real mock handler for it, must also remove it here (the "no stale
// allowlist entries" assertion enforces that).
const DEFERRED: readonly string[] = [
  // Per-emitter property patch via the legacy update path — superseded in the
  // mock by emitters/set-properties; the native host owns the real update.
  "emitters/update",
  // Real .alo import requires FileManager + ParticleSystem the browser-mode
  // host doesn't own (the preview path, emitters/preview-from-file, IS mocked).
  "emitters/import-from-file",
  // Live-simulation counters come from the real Engine; browser mode runs no
  // simulation, so the mock throws rather than returning meaningless zeros.
  "engine/query/live-instances",
  // Frameless title-bar window controls act on the native HWND — the native
  // host owns them; browser mode has no window to minimize/maximize/close.
  "window/minimize",
  "window/maximize",
  "window/close",
];

describe("bridge contract drift (schema Request kinds vs MockBridge)", () => {
  const kinds = requestKinds(schemaSrc);
  const handled = mockCaseLabels(mockSrc);
  const deferred = new Set(DEFERRED);

  it("parses a plausible number of request kinds from the schema", () => {
    // Sanity floor — guards against the union-block slice silently matching
    // nothing (which would make the parity check vacuously pass).
    expect(kinds.length).toBeGreaterThan(80);
  });

  it("every schema Request kind is mock-handled or explicitly deferred", () => {
    const unaccounted = kinds.filter(
      (k) => !handled.has(k) && !deferred.has(k),
    );
    expect(unaccounted).toEqual([]);
  });

  it("every DEFERRED kind still exists in the schema (no stale allowlist)", () => {
    const known = new Set(kinds);
    const stale = DEFERRED.filter((k) => !known.has(k));
    expect(stale).toEqual([]);
  });

  it("no DEFERRED kind has quietly gained a real mock handler", () => {
    // A deferred kind reaching a `throw` arm is fine; but if it gains a real
    // handler it should graduate off the allowlist. The mock routes both via
    // `case`, so we distinguish by what the label actually falls through to.
    //
    // This used to scan a fixed 400-character window after the label for the
    // text "not implemented". That made the guard depend on how verbose the
    // surrounding comments happened to be — adding a comment inside the throw
    // cluster pushed the throw out of range and failed the sibling labels —
    // and it would equally have PASSED on a real handler that merely mentioned
    // "not implemented" in a nearby comment. Match the intent instead: after
    // the label, skipping sibling `case` labels and comments, the next
    // STATEMENT must be the shared not-implemented throw.
    const graduated = DEFERRED.filter((k) => {
      const label = `case "${k}":`;
      const idx = mockSrc.indexOf(label);
      if (idx < 0) return false; // not handled at all → fine (still deferred)
      const rest = mockSrc.slice(idx + label.length);
      // Consume any run of whitespace / line + block comments / sibling labels.
      const skip = /^(?:\s|\/\/[^\n]*|\/\*[\s\S]*?\*\/|case\s*"[^"]+":)*/;
      const next = rest.replace(skip, "");
      return !/^throw new Error\(\s*`MockBridge: '\$\{req\.kind\}' not implemented`/.test(next);
    });
    expect(graduated).toEqual([]);
  });
});
