// Guards for the gate's own trustworthiness, from the 2026-07 release audit's
// negative controls. Each of these was a demonstrated FALSE GREEN: the gate
// reported success while the thing it claimed to verify had not happened.

import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, writeFileSync, utimesSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { staleBinaryNote, parseSkippedCount, recordSmokeVerdict, PNG_SIGNATURE } from "./run-all-tests.mjs";

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

// ── record-smoke oracle (audit an-audit-finding) ──────────────────────────────────────
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
