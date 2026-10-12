import { spawnSync } from "node:child_process";

export const SSIM_MIN = 0.9995;

export function ssim(a, b) {
  // ffmpeg prints "SSIM R:… G:… B:… All:<x> (…)" on stderr.
  const r = spawnSync(
    "ffmpeg",
    ["-hide_banner", "-i", a, "-i", b, "-filter_complex", "ssim", "-f", "null", "-"],
    { encoding: "utf8", shell: false },
  );
  if (r.error) return { ok: false, why: `ffmpeg spawn failed (${r.error.code}) — is ffmpeg on PATH?` };
  const text = `${r.stderr || ""}${r.stdout || ""}`;
  // Full float syntax incl. scientific notation: a badly mismatched image can
  // emit "All:1.23e-05", which a bare [\d.]+ would truncate to a PASSING 1.23.
  const m = text.match(/All:([0-9.]+(?:[eE][+-]?[0-9]+)?)/);
  if (r.status !== 0 || !m || !Number.isFinite(Number(m[1]))) {
    // Dimension mismatches surface here: ffmpeg's ssim filter refuses
    // differently-sized inputs and exits nonzero.
    const dims = text.match(/Input link.*parameters.*do not match|width|height/i);
    return { ok: false, why: `ffmpeg ssim failed (exit ${r.status})${dims ? " — likely dimension mismatch" : ""}` };
  }
  return { ok: true, all: Number(m[1]) };
}

// The largest change in mean luminance over any 16x16 block, 0-255: a local
// measure, where whole-frame SSIM averages a small change away. Same-sized
// images only; ffmpeg's blend refuses others.
export function blockDiff(a, b) {
  const r = spawnSync(
    "ffmpeg",
    ["-hide_banner", "-loglevel", "error", "-i", a, "-i", b, "-filter_complex",
      "[0]format=gray[x];[1]format=gray[y];[x][y]blend=all_mode=difference,scale=iw/16:ih/16:flags=area",
      "-f", "rawvideo", "-pix_fmt", "gray", "-"],
    { shell: false, maxBuffer: 1 << 26 },
  );
  if (r.error) return { ok: false, why: `ffmpeg spawn failed (${r.error.code}) — is ffmpeg on PATH?` };
  if (r.status !== 0 || !r.stdout?.length) return { ok: false, why: `ffmpeg block difference failed (exit ${r.status}): ${String(r.stderr ?? "").trim()}` };
  let peak = 0;
  for (const v of r.stdout) if (v > peak) peak = v;
  return { ok: true, peak };
}
