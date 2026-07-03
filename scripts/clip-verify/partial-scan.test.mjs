import { test } from "node:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync, copyFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { detectIsolatedDips, parseYavgAll } from "./partial-scan.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const cli = join(here, "partial-scan.mjs");
const ffmpegReady = spawnSync("ffmpeg", ["-version"], { encoding: "utf8" }).status === 0;

// Tiny solid-gray PNG fixture at the given luma (16x16).
function makePng(path, luma) {
  const r = spawnSync("ffmpeg", ["-f", "lavfi", "-i", `color=c=0x${luma.toString(16).padStart(2, "0").repeat(3)}:s=16x16`, "-frames:v", "1", "-y", path], { encoding: "utf8" });
  assert.equal(r.status, 0, `fixture png failed: ${r.stderr}`);
}

test("detectIsolatedDips flags a frame below both neighbors", () => {
  assert.deepEqual(detectIsolatedDips([10, 10, 2, 10, 10], 3.0), [
    { index: 2, yavg: 2, prev: 10, next: 10 },
  ]);
});

test("detectIsolatedDips returns no dips for a flat sequence", () => {
  assert.deepEqual(detectIsolatedDips([10, 10, 10], 3.0), []);
});

test("detectIsolatedDips never flags the first or last frame", () => {
  assert.deepEqual(detectIsolatedDips([2, 10, 10], 3.0), []);
  assert.deepEqual(detectIsolatedDips([10, 10, 2], 3.0), []);
});

test("parseYavgAll extracts every YAVG in frame order", () => {
  const out = "frame:0 pts:0\nlavfi.signalstats.YAVG=12.5\n"
            + "frame:1 pts:1\nlavfi.signalstats.YAVG=3\n"
            + "frame:2 pts:2\nlavfi.signalstats.YAVG=-0.25\n";
  assert.deepEqual(parseYavgAll(out), [12.5, 3, -0.25]);
});

test("parseYavgAll returns empty for output with no YAVG lines", () => {
  assert.deepEqual(parseYavgAll("no metadata here"), []);
});

test("parseYavgAll handles scientific-notation YAVG (near-black frames)", () => {
  const out = "lavfi.signalstats.YAVG=1.23e-05\nlavfi.signalstats.YAVG=2.5E+01\n";
  assert.deepEqual(parseYavgAll(out), [0.0000123, 25]);
});

// End-to-end: a numbering GAP defeats the single-invocation sequence fast
// path (ffmpeg image2 stops at the gap -> count mismatch -> null), so the
// per-file fallback must still find the dip. Follows seam-match.test.mjs's
// ffmpeg-fixture precedent; skips without ffmpeg like ssim.test.mjs.
test("CLI finds a dip across a numbering gap via the per-file fallback",
     { skip: !ffmpegReady && "ffmpeg not installed" }, () => {
  const dir = mkdtempSync(join(tmpdir(), "partial-scan-gap-"));
  try {
    // frames 0,1,2,4 (gap at 3); frame 1 is the isolated dark dip.
    makePng(join(dir, "frame_00000.png"), 0x80);
    makePng(join(dir, "frame_00001.png"), 0x10);
    makePng(join(dir, "frame_00002.png"), 0x80);
    copyFileSync(join(dir, "frame_00002.png"), join(dir, "frame_00004.png"));
    const r = spawnSync(process.execPath, [cli, "--frames", dir, "--start", "0", "--crop", "16:16:0:0"], { encoding: "utf8" });
    assert.equal(r.status, 1, `expected dip exit 1, got ${r.status}: ${r.stderr}`);
    assert.match(r.stdout, /^frame 1: /m);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
