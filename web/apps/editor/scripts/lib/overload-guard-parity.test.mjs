import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

function parseSeparatedInteger(source, pattern, description) {
  const match = source.match(pattern);
  assert.ok(match, `could not find ${description}`);
  const literal = match[1];
  assert.match(literal, /^[\d'_]+$/, `${description} must be an integer literal`);
  return Number.parseInt(literal.replace(/['_]/g, ""), 10);
}

test("the web overload-cap default matches Engine's preview-particle cap", () => {
  const engineHeader = readFileSync(new URL("../../../../../src/engine.h", import.meta.url), "utf8");
  const overloadGuard = readFileSync(new URL("../../src/lib/overload-guard.ts", import.meta.url), "utf8");
  const engineDefault = parseSeparatedInteger(
    engineHeader,
    /\bkDefaultMaxPreviewParticles\b\s*=\s*([\d'_]+)\s*;/,
    "kDefaultMaxPreviewParticles",
  );
  const webDefault = parseSeparatedInteger(
    overloadGuard,
    /\bOVERLOAD_GUARD_DEFAULT\b\s*:\s*OverloadGuardConfig\s*=\s*Object\.freeze\(\{\s*enabled:\s*true,\s*maxParticles:\s*([\d'_]+),\s*\}\)/,
    "OVERLOAD_GUARD_DEFAULT.maxParticles",
  );

  assert.equal(webDefault, engineDefault);
});
