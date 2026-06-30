import { test } from "node:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { computeDelta, isWithinTolerance, measureYavg, parseCropArg } from "./seam-match.mjs";

const FFMPEG_OK = spawnSync("ffmpeg", ["-version"], { encoding: "utf8" }).status === 0;

test("parseCropArg parses W:H:X:Y", () => {
  assert.deepEqual(parseCropArg("660:620:340:110"), { w: 660, h: 620, x: 340, y: 110 });
});

test("computeDelta returns the absolute YAVG difference", () => {
  assert.equal(computeDelta(10.5, 11.0), 0.5);
});

test("isWithinTolerance accepts deltas at or below tolerance", () => {
  assert.equal(isWithinTolerance(0.5, 1.0), true);
  assert.equal(isWithinTolerance(1.5, 1.0), false);
});

// Integration: drive the real ffmpeg YAVG glue (this is where the metadata-print
// `file=-` bug lived — the helper math above can't catch it). Skips if ffmpeg
// isn't installed. partial-scan.mjs shares the identical measureYavg path.
test("measureYavg extracts YAVG from a real PNG (ffmpeg glue regression)", { skip: !FFMPEG_OK }, () => {
  const dir = mkdtempSync(join(tmpdir(), "clipverify-"));
  try {
    const png = join(dir, "gray.png");
    const gen = spawnSync(
      "ffmpeg",
      ["-v", "error", "-f", "lavfi", "-i", "color=c=gray:s=120x120", "-frames:v", "1", png],
      { encoding: "utf8" },
    );
    assert.equal(gen.status, 0, "ffmpeg failed to generate the fixture PNG");
    const yavg = measureYavg(png, { w: 80, h: 80, x: 20, y: 20 });
    assert.ok(yavg > 100 && yavg < 150, "expected ~mid-gray luma, got " + yavg);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
