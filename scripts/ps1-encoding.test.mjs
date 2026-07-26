// Guard: every repo .ps1 must be parseable by Windows PowerShell 5.1.
//
// PS 5.1 decodes a BOM-less file as the system ANSI codepage, not UTF-8. A
// single non-ASCII character (an em dash in a comment or a quoted string) then
// arrives as mojibake bytes and cascades into parse errors — sync-public.ps1
// hit exactly this: two U+2014 characters produced 13 errors and made the
// public-mirror sync unrunnable on the only shell this box has. PowerShell 7,
// which reads UTF-8 regardless, is not installed here.
//
// Rule: a .ps1 is safe if it is pure ASCII, OR it carries a UTF-8 BOM.
// (This is the .rc convention in AGENTS.md applied to the script surface.)

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, dirname, relative } from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), "..");
const SKIP_DIRS = new Set(["node_modules", ".git", "dist", "x64", "release-stage", ".claude", ".codex"]);

function collectPs1(dir, out = []) {
  for (const name of readdirSync(dir)) {
    if (SKIP_DIRS.has(name)) continue;
    const full = join(dir, name);
    const st = statSync(full);
    if (st.isDirectory()) collectPs1(full, out);
    else if (name.toLowerCase().endsWith(".ps1")) out.push(full);
  }
  return out;
}

test("every .ps1 is ASCII-only or UTF-8 BOM'd (Windows PowerShell 5.1 parseable)", () => {
  const offenders = [];
  for (const file of collectPs1(repoRoot)) {
    const buf = readFileSync(file);
    const hasBom = buf[0] === 0xef && buf[1] === 0xbb && buf[2] === 0xbf;
    if (hasBom) continue;
    const firstNonAscii = buf.findIndex((b) => b > 0x7f);
    if (firstNonAscii !== -1) {
      // Report the line so the fix is obvious: either de-accent it or add a BOM.
      const line = buf.subarray(0, firstNonAscii).toString("utf8").split("\n").length;
      offenders.push(`${relative(repoRoot, file)}:${line} (byte 0x${buf[firstNonAscii].toString(16)})`);
    }
  }
  assert.deepEqual(
    offenders,
    [],
    `BOM-less .ps1 files contain non-ASCII bytes; Windows PowerShell 5.1 will mis-decode them and fail to parse:\n  ${offenders.join("\n  ")}`,
  );
});
