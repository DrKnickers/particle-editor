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
    /bool IsAutomationMode\(\) const\s*\{\s*return m_runMode == RunMode::Drive \|\| m_runMode == RunMode::Record \|\|\s*m_runMode == RunMode::ReplayProof \|\| m_runMode == RunMode::Observe;\s*\}/,
    "drive, record, replay proof and observe must isolate the palette",
  );
  assert.ok(
    isolate < restore,
    "SetEphemeral(true) must run before RestoreLastLayerStack can load a persisted palette",
  );
});

test("observe shares proof's constructor and boot isolation at every entry point", () => {
  const host = readFileSync(resolve(repoRoot, "src/host/HostWindow.cpp"), "utf8");
  const main = readFileSync(resolve(repoRoot, "src/main.cpp"), "utf8");
  assert.match(hostSource, /m_captureGoldenProfile\(options\.captureGoldenProfile \|\| !options\.replayScenarioPath\.empty\(\) \|\| !options\.observeSpecPath\.empty\(\)\)/);
  assert.match(hostSource, /!options\.replayScenarioPath\.empty\(\) \|\| !options\.observeSpecPath\.empty\(\),\s*options\.useTestHost/);
  assert.match(hostSource, /m_runMode\(!options\.observeSpecPath\.empty\(\) \? RunMode::Observe/);
  assert.match(host, /SetReplayProofOwnsSettings\(m_runMode == RunMode::ReplayProof \|\| m_runMode == RunMode::Observe\)/);
  assert.match(host, /RenderComposedFrame\(m_runMode != RunMode::ReplayProof && m_runMode != RunMode::Observe\)/);
  assert.match(host, /if \(m_webDead && !IsFullyInteractive\(\) && m_runMode != RunMode::ReplayProof && m_runMode != RunMode::Observe\)/);
  assert.match(host, /RunMode::Observe \? L"-observe-"/);
  assert.match(host, /RunMode::Observe \? "observe"/);
  assert.match(host, /if \(m_runMode == RunMode::Observe\) return observeExitCode;/);
  assert.match(main, /const bool automationMode = driveMode \|\| recordMode \|\| replayMode \|\| observeMode;/);
  assert.match(main, /!recordScriptPath\.empty\(\) \|\| !replayScenarioPath\.empty\(\) \|\| !observeSpecPath\.empty\(\),/);
  const validation = main.indexOf("host::ParseObservationSpec(text, spec)");
  const fileManager = main.indexOf("FileManager* fileManager = createFileManager(NULL");
  assert.ok(validation !== -1 && validation < fileManager, "bad spec must fail before data-path discovery or host boot");
  const early = main.slice(main.lastIndexOf("if (!observeSpecPath.empty())", validation), fileManager);
  assert.match(early, /ValidateObservationSpec\(spec\)/);
  assert.match(early, /ParticleRandom::Seed\(spec.scenario.seed\)/);
  assert.doesNotMatch(early, /create_directories|AtomicWrite|ofstream|MoveFileEx/, "bad specs write stderr only");
});
