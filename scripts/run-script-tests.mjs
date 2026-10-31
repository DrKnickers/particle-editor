// Keep the slow, pinned-interpreter publication suite in its own gate lane.
import { spawnSync } from "node:child_process";
import { existsSync, readdirSync } from "node:fs";
import { join, resolve, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");

export function scriptTestFiles(root = repoRoot) {
  const dirs = [
    "web/apps/editor/scripts/lib", "scripts/lib", "scripts/clip-encode",
    "scripts/clip-verify", "scripts/clip-author", "scripts",
    "scripts/vfx-loop", "scripts/vfx-index",
    "scripts/vfx-build", "scripts/mcp-adapter", "scripts/houdini",
    "scripts/wiki-media/__tests__",
  ];
  // A folder can be absent on the public mirror.
  return dirs.filter((dir) => existsSync(join(root, dir))).flatMap((dir) => readdirSync(join(root, dir))
    .filter((name) => name.endsWith(".test.mjs") && !(dir === "scripts" && name === "public-history.test.mjs"))
    .sort().map((name) => join(root, dir, name)));
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const r = spawnSync(process.execPath, ["--test", ...process.argv.slice(2), ...scriptTestFiles()], {
    cwd: join(repoRoot, "web", "apps", "editor"), stdio: "inherit", windowsHide: true,
  });
  if (r.error) console.error(r.error.message);
  process.exit(r.status ?? 1);
}
