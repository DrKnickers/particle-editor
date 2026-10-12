import { test } from "node:test";
import assert from "node:assert/strict";
import { assetSet, checkAttestation, checkBundle, checkDependencies } from "./provenance.mjs";

const RENDER = { adapter: { description: "GPU", vendorId: 4318, deviceId: 7, driverVersion: "31.0.15.1" }, displayFormat: 22, depthStencilFormat: 75,
  multisample: { type: 0, quality: 0 }, vertexProcessing: "HWVP" };
const shader = { type: "shader", identity: "ENGINE\\PRIMALPHA.FX", requests: ["Engine\\PrimAlpha.fx"], attempts: [], outcome: "resolved",
  winner: { kind: "archive", path: "C:/Game/Data/Shaders.meg", member: "PRIMALPHA.FX", start: 0, size: 9 }, sha256: "c".repeat(64) };
const texture = (name, over = {}) => ({ type: "texture", identity: name.toUpperCase(), requests: [name], attempts: [], outcome: "resolved",
  winner: { kind: "loose", path: `C:/Game/Data/Art/Textures/${name.toUpperCase()}` }, sha256: "a".repeat(64), ...over });
const capture = (over = {}) => ({
  content: { gameRoots: ["C:/Game/"], selectedModPaths: [], loadedAssets: [shader, texture("smoke.tga")], resolvedTextures: ["smoke.tga"], unresolvedTextures: [], ...over.content },
  render: over.render === undefined ? RENDER : over.render,
});
const draft = () => ({ contentRoots: ["C:\\Game"], render: null, assets: new Map() });

test("the first bundle sets the render profile and asset map; agreeing bundles pass under one root form", () => {
  const d = draft();
  assert.deepEqual(checkBundle(capture(), d, { label: "base" }), []);
  assert.deepEqual(d.render, RENDER);
  assert.equal(d.assets.size, 2);
  // Another alias of the same record (a different request spelling) is the same asset.
  const alias = capture({ content: { loadedAssets: [shader, texture("smoke.tga", { requests: ["SMOKE.TGA", "Smoke.tga"] })] } });
  assert.deepEqual(checkBundle(alias, d, { label: "case" }), []);
});

test("mixed provenance: roots, mod paths, every render field, and any asset record difference", () => {
  const d = draft();
  checkBundle(capture(), d, { label: "base" });
  const one = (c, pattern, opts = {}) => assert.match(checkBundle(c, d, { label: "x", ...opts }).join("\n"), pattern);
  one(capture({ content: { gameRoots: ["C:/Game/", "D:/Mod/"] } }), /game roots/);
  one(capture({ content: { gameRoots: ["D:/Game/"] } }), /game roots/);
  one(capture({ content: { selectedModPaths: ["Mods/X"] } }), /selected mod paths/);
  one(capture({ render: null }), /no render profile/);
  one(capture({ render: { ...RENDER, adapter: { ...RENDER.adapter, driverVersion: "31.0.15.2" } } }), /render adapter differs/);
  one(capture({ render: { ...RENDER, vertexProcessing: "SWVP" } }), /render vertexProcessing differs/);
  one(capture({ render: { ...RENDER, multisample: { type: 4, quality: 0 } } }), /render multisample differs/);
  one(capture({ content: { loadedAssets: [{ ...shader, sha256: "d".repeat(64) }] } }), /SHADER.*PRIMALPHA|shader:ENGINE.*differs/);
  // An asset that stops resolving is a difference, not an absence.
  const substituted = texture("smoke.tga", { outcome: "default-substituted", winner: { kind: "embedded", resourceId: "IDB_MISSING" } });
  one(capture({ content: { loadedAssets: [shader, substituted] } }), /texture:SMOKE.TGA differs/);
  // A change confined to a rejected attempt's bytes is a difference too.
  const rejected = texture("smoke.tga", { attempts: [{ name: "SMOKE.TGA", result: "decode-failed", winner: { kind: "loose", path: "x" }, sha256: "e".repeat(64) }] });
  one(capture({ content: { loadedAssets: [shader, rejected] } }), /texture:SMOKE.TGA differs/);
  assert.deepEqual(checkBundle(capture({ content: { loadedAssets: [shader, substituted] } }), d, { label: "case 1", allowAssets: ["texture:SMOKE.TGA"] }), []);
  one(capture({ content: { loadedAssets: undefined } }), /no asset ledger/);
});

test("dependencies are set-equal with the base's, except the recipe's allowance", () => {
  const base = assetSet(capture());
  assert.deepEqual(base, ["shader:ENGINE\\PRIMALPHA.FX", "texture:SMOKE.TGA"]);
  assert.deepEqual(checkDependencies(capture(), base, { label: "c" }), []);
  const extra = capture({ content: { loadedAssets: [shader, texture("smoke.tga"), texture("fire.tga")] } });
  assert.match(checkDependencies(extra, base, { label: "c" }).join(), /FIRE.TGA was not loaded by its base/);
  const fewer = capture({ content: { loadedAssets: [shader] } });
  assert.match(checkDependencies(fewer, base, { label: "c" }).join(), /SMOKE.TGA is missing against its base/);
  const swapped = capture({ content: { loadedAssets: [shader, texture("smoke_absent.tga")] } });
  assert.deepEqual(checkDependencies(swapped, base, { label: "c", allow: ["texture:SMOKE.TGA", "texture:SMOKE_ABSENT.TGA"] }), []);
});

test("texture attestation: bases resolve every name; the planted case holds exactly its name unresolved", () => {
  const inspect = { emitters: [{ colorTexture: "smoke.tga", normalTexture: "" }, { colorTexture: "fire.tga", normalTexture: "fire_n.tga" }] };
  const resolvedAll = capture({ content: { resolvedTextures: ["fire.tga", "fire_n.tga", "smoke.tga"], unresolvedTextures: [] } });
  assert.deepEqual(checkAttestation(resolvedAll, inspect, { label: "base" }), []);
  assert.match(checkAttestation(capture({ content: { resolvedTextures: ["fire.tga", "smoke.tga"] } }), inspect, { label: "base" }).join(), /fire_n.tga is unverified/);
  assert.match(checkAttestation(capture({ content: { resolvedTextures: ["fire.tga", "fire_n.tga"], unresolvedTextures: ["smoke.tga"] } }), inspect, { label: "base" }).join(), /unresolved textures smoke.tga/);
  assert.match(checkAttestation(resolvedAll, inspect, { label: "case", base: capture() }).join(), /differ from the base's/);
  assert.deepEqual(checkAttestation(capture({ content: { resolvedTextures: [], unresolvedTextures: [] } }), { emitters: [] }, { label: "x" }), []);
  assert.match(checkAttestation({ content: {} }, inspect, { label: "x" }).join(), /no texture attestation/);

  const planted = { emitters: [{ colorTexture: "smoke_absent.tga", normalTexture: "" }, { colorTexture: "fire.tga", normalTexture: "" }] };
  const ok = capture({ content: { resolvedTextures: ["fire.tga"], unresolvedTextures: ["smoke_absent.tga"] } });
  assert.deepEqual(checkAttestation(ok, planted, { label: "case 1", planted: "smoke_absent.tga" }), []);
  const twoMissing = capture({ content: { resolvedTextures: [], unresolvedTextures: ["fire.tga", "smoke_absent.tga"] } });
  assert.match(checkAttestation(twoMissing, planted, { label: "case 1", planted: "smoke_absent.tga" }).join(), /must be exactly smoke_absent.tga/);
  const resolvedPlant = capture({ content: { resolvedTextures: ["fire.tga", "smoke_absent.tga"], unresolvedTextures: [] } });
  assert.match(checkAttestation(resolvedPlant, planted, { label: "case 1", planted: "smoke_absent.tga" }).join("\n"), /must be exactly/);
});
