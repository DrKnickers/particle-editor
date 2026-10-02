import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
// The HostWindowImpl constructor is defined in-class in the private header.
const hostSource = readFileSync(resolve(repoRoot, "src/host/HostWindowImpl.h"), "utf8");

test("automation isolates the texture palette before restoring the saved mod stack", () => {
  const constructor = hostSource.indexOf("HostWindowImpl(HINSTANCE inst");
  const restore = hostSource.indexOf("modManager->RestoreLastLayerStack();", constructor);
  const isolate = hostSource.indexOf(
    "TexturePalette::Store::Instance().SetEphemeral(true);",
    constructor,
  );

  assert.notEqual(constructor, -1, "HostWindowImpl constructor not found");
  assert.notEqual(restore, -1, "saved mod-stack restore not found");
  assert.notEqual(isolate, -1, "palette automation isolation not found");
  assert.match(
    hostSource.slice(constructor, isolate),
    /if\s*\(IsAutomationMode\(\)\)\s*$/,
    "palette isolation must be gated by the host's automation mode",
  );
  assert.match(
    hostSource,
    /bool IsAutomationMode\(\) const\s*\{\s*return m_runMode == RunMode::Drive \|\| m_runMode == RunMode::Record;\s*\}/,
    "both drive and record must isolate the palette",
  );
  assert.ok(
    isolate < restore,
    "SetEphemeral(true) must run before RestoreLastLayerStack can load a persisted palette",
  );
});
