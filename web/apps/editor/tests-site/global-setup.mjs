// Generates gitignored placeholder media for the landing smoke ONCE, before workers.
// Fails loud in CI if ffmpeg is missing (so a missing dep can't green the suite by skip).
import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, copyFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { resolve } from "node:path";
const SITE = fileURLToPath(new URL("../../../../site", import.meta.url));
const MEDIA = resolve(SITE, "media-local");
const CLIPS = ["hero.mp4", "faithful.mp4"];
const POSTERS = ["hero-poster.jpg", "faithful-poster.jpg", "preview-poster.jpg", "workspace-poster.jpg"];
const REQUIRED = [...CLIPS, ...POSTERS].map((f) => resolve(MEDIA, f));
export default function () {
  if (REQUIRED.every((p) => existsSync(p))) return;
  mkdirSync(MEDIA, { recursive: true });
  try {
    const c0 = resolve(MEDIA, "hero.mp4"), p0 = resolve(MEDIA, "hero-poster.jpg");
    execFileSync("ffmpeg", ["-y", "-f", "lavfi", "-i", "testsrc=duration=1:size=1280x720:rate=30", "-pix_fmt", "yuv420p", "-movflags", "+faststart", c0], { stdio: "ignore" });
    execFileSync("ffmpeg", ["-y", "-i", c0, "-frames:v", "1", p0], { stdio: "ignore" });
    for (const f of CLIPS) if (f !== "hero.mp4") copyFileSync(c0, resolve(MEDIA, f));
    for (const f of POSTERS) if (f !== "hero-poster.jpg") copyFileSync(p0, resolve(MEDIA, f));
  } catch (e) {
    if (process.env.CI) throw new Error("Landing smoke requires ffmpeg to generate placeholder media in CI: " + e.message);
    console.warn("[landing-smoke] ffmpeg unavailable — playback/pause tests will skip locally");
  }
}
