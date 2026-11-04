// Keep tracked first-party C++ in the repo's .clang-format style.
// Vendored code is skipped through .clang-format-ignore, which clang-format reads itself.

import { test } from "node:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");

// Output differs between clang-format majors, so the check pins the one the code was formatted with.
const REQUIRED_CLANG_FORMAT_MAJOR = 22;
const CPP_PATHSPECS = ["*.c", "*.cc", "*.cpp", "*.h", "*.hpp", "*.inl"];
// Windows caps one command line near 32k characters; batches stay well under it.
const FILES_PER_BATCH = 100;

const CANDIDATE_EXES = [
  "clang-format",
  "C:/Program Files/LLVM/bin/clang-format.exe",
  "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin/clang-format.exe",
  "C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/Llvm/x64/bin/clang-format.exe",
  "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Tools/Llvm/x64/bin/clang-format.exe",
];

function clangFormatMajor(exe) {
  const r = spawnSync(exe, ["--version"], { encoding: "utf8", windowsHide: true });
  const match = r.status === 0 && /clang-format version (\d+)\./.exec(r.stdout);
  return match ? Number(match[1]) : null;
}

function findClangFormat() {
  const found = [];
  for (const exe of CANDIDATE_EXES) {
    if (exe.includes("/") && !existsSync(exe)) continue;
    const major = clangFormatMajor(exe);
    if (major === REQUIRED_CLANG_FORMAT_MAJOR) return { exe, found };
    if (major !== null) found.push(`${exe} (${major})`);
  }
  return { exe: null, found };
}

function trackedCppFiles() {
  const r = spawnSync("git", ["ls-files", "-z", "--", ...CPP_PATHSPECS], { cwd: repoRoot, encoding: "utf8" });
  assert.equal(r.status, 0, "git ls-files failed: " + r.stderr);
  return r.stdout.split("\0").filter(Boolean);
}

test("tracked C++ matches .clang-format", () => {
  const { exe, found } = findClangFormat();
  assert.ok(exe, `clang-format ${REQUIRED_CLANG_FORMAT_MAJOR} not found (found: ${found.join(", ") || "none"}); ` +
    "install LLVM 22 or Visual Studio's bundled clang-format");

  const files = trackedCppFiles();
  assert.ok(files.length > 0, "no tracked C++ files found");
  const problems = [];
  for (let i = 0; i < files.length; i += FILES_PER_BATCH) {
    const batch = files.slice(i, i + FILES_PER_BATCH);
    const r = spawnSync(exe, ["--dry-run", "--Werror", "--style=file", ...batch],
      { cwd: repoRoot, encoding: "utf8", windowsHide: true, maxBuffer: 64 * 1024 * 1024 });
    if (r.status !== 0) problems.push(r.stderr.trim());
  }
  assert.deepEqual(problems, [], "run clang-format -i on the files below:\n" + problems.join("\n"));
});
