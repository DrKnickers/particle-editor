// Compatibility wrapper for the app icon generator.
// The source of truth is build.py so the icon can be regenerated without
// installing a project-local rasterizer package.
const { spawnSync } = require("child_process");
const path = require("path");

const script = path.join(__dirname, "build.py");
const result = spawnSync("python", [script], { stdio: "inherit" });

if (result.error) {
  console.error(result.error.message);
  process.exit(1);
}

process.exit(result.status ?? 1);
