// Generates gitignored placeholder media for the landing smoke ONCE, before workers.
// Fails loud in CI if ffmpeg is missing (so a missing dep can't green the suite by skip).
//
// NON-DESTRUCTIVE by design: media-local/ doubles as the user's REAL clip staging dir
// (the VPS demo deploys from it), so this never overwrites an existing file — the
// placeholder is rendered to a temp name and only copied into missing slots. (The old
// version re-rendered hero.mp4 in place whenever ANY placeholder was missing, which
// would have clobbered the real hero clip.)
import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, copyFileSync, rmSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { resolve } from "node:path";

const SITE = fileURLToPath(new URL("../../../../site", import.meta.url));
const MEDIA = resolve(SITE, "media-local");
// Must match the data-clip/data-poster names in site/index.html.
const STEMS = ["hero", "faith", "f04", "spawner", "f02-reorder", "f02"];
const CLIPS = STEMS.map((s) => `${s}.mp4`);
const POSTERS = STEMS.map((s) => `${s}-poster.jpg`);
const STILLS = ["interface-theme-dark.jpg", "interface-theme-light.jpg"];

export default function () {
  const missing = [...CLIPS, ...POSTERS, ...STILLS].filter((f) => !existsSync(resolve(MEDIA, f)));
  if (!missing.length) return;
  mkdirSync(MEDIA, { recursive: true });
  const tmpClip = resolve(MEDIA, ".placeholder-clip.tmp.mp4");
  const tmpPoster = resolve(MEDIA, ".placeholder-poster.tmp.jpg");
  try {
    execFileSync("ffmpeg", ["-y", "-f", "lavfi", "-i", "testsrc=duration=1:size=1280x960:rate=30", "-pix_fmt", "yuv420p", "-movflags", "+faststart", tmpClip], { stdio: "ignore" });
    execFileSync("ffmpeg", ["-y", "-i", tmpClip, "-frames:v", "1", tmpPoster], { stdio: "ignore" });
    for (const f of missing) copyFileSync(f.endsWith(".mp4") ? tmpClip : tmpPoster, resolve(MEDIA, f));
  } catch (e) {
    if (process.env.CI) throw new Error("Landing smoke requires ffmpeg to generate placeholder media in CI: " + e.message);
    console.warn("[landing-smoke] ffmpeg unavailable — playback/pause tests will skip locally");
  } finally {
    rmSync(tmpClip, { force: true });
    rmSync(tmpPoster, { force: true });
  }
}
