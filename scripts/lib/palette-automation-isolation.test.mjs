import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const hostSource = readFileSync(resolve(repoRoot, "src/host/HostWindow.cpp"), "utf8");

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
  assert.ok(
    isolate < restore,
    "SetEphemeral(true) must run before RestoreLastLayerStack can load a persisted palette",
  );
});
