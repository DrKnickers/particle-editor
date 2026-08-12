// Guards for the gate's own trustworthiness, from the 2026-07 release audit's
// negative controls. Each of these was a demonstrated FALSE GREEN: the gate
// reported success while the thing it claimed to verify had not happened.

import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, writeFileSync, utimesSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import {
  staleBinaryNote, parseSkippedCount, recordSmokeVerdict, PNG_SIGNATURE,
  skipBudgetVerdict, SKIP_BUDGET,
} from "./run-all-tests.mjs";
import { selfSkipVerdict } from "./run-native-unit-tests.mjs";

test("staleBinaryNote flags an exe older than its sources (the stubbed-MSBuild false green)", () => {
  const dir = mkdtempSync(join(tmpdir(), "gate-stale-"));
  try {
    const exe = join(dir, "ParticleEditor.exe");
    writeFileSync(exe, "binary");
    // Source newer than the exe by an hour: this is exactly the state an
    // MSBuild that exits 0 without relinking leaves behind.
    const exeSeconds = Date.now() / 1000 - 3600;
    utimesSync(exe, exeSeconds, exeSeconds);
    const newestSrcMs = Date.now();

    const note = staleBinaryNote(exe, "msbuild-release", newestSrcMs);
    assert.ok(note, "expected a stale-binary note");
    assert.match(note, /stale binary/);
    assert.match(note, /OLDER than the newest source/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test("staleBinaryNote accepts a freshly linked exe (no false positive on a real build)", () => {
  const dir = mkdtempSync(join(tmpdir(), "gate-fresh-"));
  try {
    const exe = join(dir, "ParticleEditor.exe");
    writeFileSync(exe, "binary");
    // Sources an hour old, exe written just now — the normal post-build state,
    // and also the legitimate incremental no-op case (exe still newer).
    assert.equal(staleBinaryNote(exe, "msbuild-release", Date.now() - 3600_000), null);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test("staleBinaryNote reports a missing exe rather than passing", () => {
  const note = staleBinaryNote(join(tmpdir(), "definitely-absent-particle-editor.exe"), "msbuild-debug", Date.now());
  assert.ok(note);
  assert.match(note, /produced no/);
});

test("parseSkippedCount sees skipped tests inside an otherwise-green runner", () => {
  // The real playwright-native line the audit found: a PASSING lane whose
  // aggregate then printed "0 skipped".
  assert.equal(parseSkippedCount("  190 passed, 4 skipped (108.5s)"), 4);
  // The stub that made the whole gate report PASS with zero skips.
  assert.equal(parseSkippedCount("0 passed, 1 skipped"), 1);
  // Playwright's other phrasing for un-run tests.
  assert.equal(parseSkippedCount("12 passed, 3 did not run"), 3);
  // Clean runs make no such claim.
  assert.equal(parseSkippedCount("15 passed (13.5s)"), 0);
  assert.equal(parseSkippedCount("51 passed, 0 failed, 0 skipped"), 0);
});

test("parseSkippedCount reads `node --test`'s reversed word order (the scripts lane was always 0)", () => {
  // The real tail of `pnpm run test:scripts`. Matching only Playwright's
  // "<n> skipped" made this lane's skips unreadable, so the surfacing added for
  // the 2026-07 audit never fired for the one lane it was written for.
  const nodeSummary = ["ℹ pass 159", "ℹ fail 0", "ℹ cancelled 0", "ℹ skipped 1", "ℹ todo 0"].join("\n");
  assert.equal(parseSkippedCount(nodeSummary), 1);
  // A clean node run must still read 0 — the overreach direction, and the one
  // that would turn every green scripts lane red.
  assert.equal(parseSkippedCount(nodeSummary.replace("skipped 1", "skipped 0")), 0);
});

test("parseSkippedCount does not read a count off the NEXT line (\\s crosses newlines)", () => {
  // The real playwright-native tail, verbatim: the skip line comes FIRST and the
  // pass count is on the line below it. A \s-based reversed-order pattern reads
  // that as "skipped 196" and fails a healthy lane — which is what the full gate
  // did on the first attempt at this fix.
  const playwrightTail = "  1 skipped\n  196 passed (114.5s)\n";
  assert.equal(parseSkippedCount(playwrightTail), 1);
  // Same trap in the other direction: a line ending in a bare number above a
  // skip line must not be adopted as the count.
  assert.equal(parseSkippedCount("Slow test file: tests\\emitter-drag.spec.ts 42\n  3 skipped\n"), 3);
});

// ── record-smoke oracle (2026-07 audit) ──────────────────────────────────────
//
// This lane is how we claim headless recording works, and it could not fail:
// it captured the child's spawn result and then used `status` ONLY inside
// failure message strings, never as a condition, while discarding `error`,
// `signal` and stderr. Its entire oracle was 'a file named frame_N.png exists'
// plus 'the largest is over 20 KB'.

const OK_FRAME = { frames: ["frame_00000.png"], maxBytes: 120_000, biggest: "frame_00000.png", header: PNG_SIGNATURE };

test("recordSmokeVerdict passes a genuine run", () => {
  assert.equal(recordSmokeVerdict({ status: 0, ...OK_FRAME }), null);
});

test("recordSmokeVerdict FAILS a recorder that wrote frames and then crashed (the an-audit-finding false green)", () => {
  // Exactly the green-preserving mutation from the audit: good-looking frames,
  // nonzero exit. The old lane passed this.
  const v = recordSmokeVerdict({ status: 1, stderr: "D3D9 device lost; aborting", ...OK_FRAME });
  assert.ok(v, "expected a failure verdict");
  assert.match(v, /exited 1/);
  assert.match(v, /D3D9 device lost/, "stderr must reach the message — it used to be discarded");
});

test("recordSmokeVerdict FAILS a timed-out recorder even with frames on disk", () => {
  const v = recordSmokeVerdict({ status: null, signal: "SIGTERM", ...OK_FRAME });
  assert.ok(v);
  assert.match(v, /SIGTERM/);
});

test("recordSmokeVerdict FAILS when the process could not be spawned at all", () => {
  const v = recordSmokeVerdict({ error: new Error("ENOENT"), status: null, ...OK_FRAME });
  assert.ok(v);
  assert.match(v, /could not run/);
});

test("recordSmokeVerdict FAILS oversized garbage wearing a frame filename", () => {
  // Right name, right size, not a PNG — the size-only oracle accepted this.
  const v = recordSmokeVerdict({
    status: 0,
    frames: ["frame_00000.png"],
    maxBytes: 900_000,
    biggest: "frame_00000.png",
    header: Buffer.from("NOTAPNG!", "ascii"),
  });
  assert.ok(v);
  assert.match(v, /not a PNG/);
});

test("recordSmokeVerdict still catches the blank-frame and no-frame cases it always did", () => {
  assert.match(recordSmokeVerdict({ status: 0, frames: [], maxBytes: 0 }), /0 frames/);
  assert.match(
    recordSmokeVerdict({ status: 0, frames: ["frame_00000.png"], maxBytes: 900, header: PNG_SIGNATURE }),
    /look blank/,
  );
});

// ── skip-as-pass: internal skips (2026-07 audit) ───────────────────────
// One mechanism at two levels. A capability probe fails, the test skips, the
// exit code stays 0. #687 made both VISIBLE and stopped there, so the gate
// could still go green around coverage that did not run. These verdicts are
// what turn that visibility into a gate.

test("skipBudgetVerdict FAILS a lane that skipped more tests than it declared (the an-audit-finding false green)", () => {
  // The FFmpeg-dependent script tests self-skipping on a box that HAS FFmpeg.
  // Explicit EMPTY budget: this pins the verdict logic, and the LIVE table now
  // legitimately declares a scripts entry on the public mirror (conditional on
  // the private clip data being absent), which would mask the case.
  const v = skipBudgetVerdict("scripts", 2, new Set(), {});
  assert.equal(v.ok, false);
  // The specific numbers, not just "something was skipped": a note that cannot
  // name the count cannot tell a new skip from the old one.
  assert.match(v.note, /2 skipped test\(s\), budget 0/);
});

test("skipBudgetVerdict budgets ZERO for a lane with no declared entry", () => {
  // The default for a lane nobody thought about must be "runs everything".
  assert.equal(skipBudgetVerdict("vitest", 1, new Set()).ok, false);
  assert.equal(skipBudgetVerdict("vitest", 0, new Set()).ok, true);
});

// ---- overreach guards: the budget must not fail what it was told to allow ----

// Budget fixture, deliberately NOT the live SKIP_BUDGET: these cases pin the
// verdict logic, and must keep doing so whether or not any lane currently
// declares a skip. The live table gets its own invariant test below.
const BUDGET_FIXTURE = { "demo-lane": { max: 1, why: "a genuinely absent capability" } };

test("skipBudgetVerdict PASSES a lane sitting exactly on its declared budget", () => {
  // The case that separates "policing drift" from "banning skips" — a fix that
  // failed here would turn a legitimately-budgeted lane red.
  const v = skipBudgetVerdict("demo-lane", 1, new Set(), BUDGET_FIXTURE);
  assert.equal(v.ok, true);
  assert.equal(v.note, null, "at-budget must be quiet, not merely non-fatal");
});

test("skipBudgetVerdict PASSES under budget but flags the budget as loose", () => {
  const v = skipBudgetVerdict("demo-lane", 0, new Set(), BUDGET_FIXTURE);
  assert.equal(v.ok, true);
  assert.match(v.note, /tighten SKIP_BUDGET\.demo-lane/);
});

test("every live SKIP_BUDGET entry carries a reason", () => {
  // The reason is the load-bearing half. The table's first and only entry was a
  // skip everyone had recorded as a legitimate capability skip; having to write
  // the reason down is what exposed it as a spec with an impossible
  // precondition, and the entry came back out. A bare number is unfalsifiable.
  for (const [lane, entry] of Object.entries(SKIP_BUDGET)) {
    assert.equal(typeof entry.max, "number", `${lane}: max must be a number`);
    assert.ok(entry.why && entry.why.trim().length > 20, `${lane}: needs a real reason, not a placeholder`);
  }
});

test("skipBudgetVerdict honours --allow-missing for the machine that genuinely can't run it", () => {
  // Explicit empty budget, same reason as the over-budget case above.
  const v = skipBudgetVerdict("scripts", 3, new Set(["scripts"]), {});
  assert.equal(v.ok, true);
  // Downgraded to a SKIP, never to silence — the count still has to appear.
  assert.match(v.note, /3 skipped test\(s\)/);
  assert.match(v.note, /allowed via --allow-missing/);
});

test("selfSkipVerdict FAILS a binary that printed SKIP: and exited 0 (the an-audit-finding false green)", () => {
  // test_clip_save_confinement's junction case going quiet, which is
  // indistinguishable in the exit code from junction rejection still working.
  const v = selfSkipVerdict(1, false);
  assert.equal(v.ok, false);
  assert.match(v.note, /1 case\(s\) SKIPPED/);
  assert.match(v.note, /did NOT run/);
});

test("selfSkipVerdict leaves a clean binary alone", () => {
  const v = selfSkipVerdict(0, false);
  assert.equal(v.ok, true);
  assert.equal(v.note, "");
  // ...and the allow flag must not invent a note where there is nothing to say.
  assert.equal(selfSkipVerdict(0, true).note, "");
});

test("selfSkipVerdict downgrades to a VISIBLE skip under --allow-missing-capabilities", () => {
  // The over-eager fix here suppresses the note along with the failure, which
  // restores exactly the silence an-audit-finding is about.
  const v = selfSkipVerdict(2, true);
  assert.equal(v.ok, true);
  assert.match(v.note, /2 case\(s\) SKIPPED/);
});
