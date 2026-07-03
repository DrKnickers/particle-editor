import { test } from "node:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { SSIM_MIN, ssim } from "./ssim.mjs";

const has = (bin) => spawnSync(bin, ["-version"]).status === 0;
const ffmpegReady = has("ffmpeg");

function writeColorPng(path, color) {
  const r = spawnSync("ffmpeg", [
    "-y", "-f", "lavfi", "-i", `color=c=${color}:s=16x16:d=1`, "-frames:v", "1", path,
  ], { encoding: "utf8", shell: false });
  assert.equal(r.status, 0, `failed to generate ${color} PNG`);
}

test("identical images score at or above the calibrated SSIM threshold", { skip: !ffmpegReady && "ffmpeg not installed" }, () => {
  const dir = mkdtempSync(join(tmpdir(), "ssim-"));
  const red = join(dir, "red.png");
  writeColorPng(red, "red");

  const score = ssim(red, red);

  assert.equal(score.ok, true, score.why);
  assert.ok(score.all >= SSIM_MIN, `SSIM ${score.all} below ${SSIM_MIN}`);
  assert.ok(Math.abs(score.all - 1) < 1e-12, `SSIM ${score.all} not approximately 1`);
});

test("clearly different images score below the calibrated SSIM threshold", { skip: !ffmpegReady && "ffmpeg not installed" }, () => {
  const dir = mkdtempSync(join(tmpdir(), "ssim-"));
  const red = join(dir, "red.png");
  const blue = join(dir, "blue.png");
  writeColorPng(red, "red");
  writeColorPng(blue, "blue");

  const score = ssim(red, blue);

  assert.equal(score.ok, true, score.why);
  assert.ok(score.all < SSIM_MIN, `SSIM ${score.all} should be below ${SSIM_MIN}`);
});

test("missing input reports an ffmpeg SSIM failure", { skip: !ffmpegReady && "ffmpeg not installed" }, () => {
  const dir = mkdtempSync(join(tmpdir(), "ssim-"));
  const red = join(dir, "red.png");
  writeColorPng(red, "red");

  const score = ssim(red, join(dir, "missing.png"));

  assert.equal(score.ok, false);
  assert.match(score.why, /ffmpeg ssim failed/);
});
