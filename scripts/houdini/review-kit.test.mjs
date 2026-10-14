// [houdini] The in-game review kit, with the entry, the editor and the game
// data faked: the XML patch, the kit's files, its refusals and --record.
// A real kit is staged in the `houdini` lane.
import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { sha256 } from "./cache.mjs";
import { MARKER, MARKER_FRAMES, REVIEW_LIFT, checklist, focRoot, markerFrames, parseArgs, patchParticlesXml, recordVerdict, stage } from "./review-kit.mjs";

const XML = Buffer.from([
  '<?xml version="1.0"?>', "<Particles>",
  '\t<Particle Name="GUI_Move_Command_Particle">',
  "\t\t <Land_Model_Name>P_MOUSE_CLICK.ALO</Land_Model_Name>",
  "\t\t <Particle_Lifetime_Frames>30</Particle_Lifetime_Frames>",
  "\t</Particle>",
  '\t<Particle Name="Other_Particle">',
  "\t\t <Land_Model_Name>P_OTHER.ALO</Land_Model_Name>",
  "\t\t <Particle_Lifetime_Frames>30</Particle_Lifetime_Frames>",
  "\t</Particle>", "</Particles>", "",
].join("\r\n"), "latin1");

test("parseArgs: --entry stages, --record records, never both", () => {
  assert.deepEqual(Object.keys(parseArgs(["--entry", "C:\\g\\cache\\k"])), ["entry"]);
  assert.deepEqual(Object.keys(parseArgs(["--entry", "C:\\g\\cache\\k", "--exe", "C:\\x.exe"])).sort(), ["entry", "exe"]);
  const recorded = parseArgs(["--record", "C:\\g\\review\\k", "--status", "passed", "--notes", "looks right"]);
  assert.deepEqual([recorded.status, recorded.notes], ["passed", "looks right"]);
  for (const argv of [[], ["--exe", "a"], ["--entry", "a", "--entry", "b"], ["--entry", "a", "--status", "passed"],
    ["--record", "k", "--status", "passed"], ["--record", "k", "--status", "fine", "--notes", "x"],
    ["--record", "k", "--status", "passed", "--notes", "  "], ["--record", "k", "--status", "passed", "--notes", "x".repeat(2001)],
    ["--record", "k", "--status", "passed", "--notes", "x", "--entry", "e"], ["--entry", "a", "--seed", "1"], ["--entry"]])
    assert.throws(() => parseArgs(argv), (error) => error.exitCode === 2, JSON.stringify(argv));
});

test("the XML patch changes only the click marker's lifetime", () => {
  const patched = patchParticlesXml(XML).toString("latin1");
  assert.equal(patched, XML.toString("latin1").replace("<Particle_Lifetime_Frames>30", `<Particle_Lifetime_Frames>${MARKER_FRAMES}`));
  assert.match(patched, /P_OTHER\.ALO<\/Land_Model_Name>\r\n\t\t <Particle_Lifetime_Frames>30</);
  const text = XML.toString("latin1");
  for (const [broken, pattern] of [
    [text.replace("GUI_Move_Command_Particle", "Renamed"), /0 GUI_Move_Command_Particle entries/],
    [text.replace("Other_Particle", "GUI_Move_Command_Particle"), /2 GUI_Move_Command_Particle entries/],
    [text.replace("P_MOUSE_CLICK.ALO", "P_SOMETHING.ALO"), /shows P_SOMETHING\.ALO/],
    [text.replace("<Particle_Lifetime_Frames>30</Particle_Lifetime_Frames>", ""), /no single Particle_Lifetime_Frames/],
  ]) assert.throws(() => patchParticlesXml(Buffer.from(broken, "latin1")), (error) => error.exitCode === 1 && pattern.test(error.message));
});

test("the kit needs the Forces of Corruption install", () => {
  assert.equal(focRoot(["D:\\Steam\\EaW\\GameData", "D:\\Steam\\EaW\\corruption"]), "D:\\Steam\\EaW\\corruption");
  assert.throws(() => focRoot(["D:\\Steam\\EaW\\GameData"]), (error) => error.exitCode === 1);
  assert.throws(() => focRoot([]), (error) => error.exitCode === 1);
});

const TEXTURE = Buffer.from("tga bytes"), NAME = `pe_gen_static-energy-ring_${sha256(TEXTURE).slice(0, 8)}.tga`;
const provenance = (overrides = {}) => ({
  generatorId: "static-energy-ring", generatorVersion: 1, seed: 1, parameters: { radius: 0.6 }, tier: "final",
  cacheKey: "c".repeat(64), contentHash: sha256(TEXTURE), outputProfile: "eaw-additive",
  texture: { name: NAME, blend: "additive", grid: 1, size: 512 }, ...overrides,
});

function harness(t, { entry = provenance(), shown = {}, xml = XML } = {}) {
  const root = mkdtempSync(join(tmpdir(), "pe-houdini-kit-"));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  const calls = [];
  const seams = {
    open: () => ({ provenance: entry, root, published: TEXTURE }),
    roots: () => ["D:\\Steam\\EaW\\GameData", "D:\\Steam\\EaW\\corruption"],
    stockFile: (roots, relative) => { calls.push(["stock", roots, relative]); return xml; },
    fixture: (exe, kind, name, path) => {
      calls.push(["fixture", kind, name, path]);
      writeFileSync(path, "alo");
      return { kind, reloaded: true, colorTexture: name, lift: REVIEW_LIFT, lifetime: 2, ...shown };
    },
  };
  return { root, calls, kit: join(root, "review", `static-energy-ring-${sha256(TEXTURE).slice(0, 8)}`),
    run: () => stage({ entry: join(root, "cache", "c".repeat(64)), exe: "C:\\pe.exe" }, seams) };
}

test("a kit holds the texture, the lifted fixture, the patched XML, the checklist and review.json", (t) => {
  const h = harness(t);
  const record = h.run();
  assert.deepEqual([record.ok, record.exitCode, record.kit, record.kind, record.texture], [true, 0, h.kit, "ring", NAME]);
  assert.deepEqual(readdirSync(h.kit).sort(), ["Data", "REVIEW.md", "review.json"]);
  assert.deepEqual(readFileSync(join(h.kit, "Data", "Art", "Textures", NAME)), TEXTURE);
  assert.deepEqual(h.calls, [
    ["stock", ["D:\\Steam\\EaW\\corruption"], "XML\\PARTICLES.XML"],
    ["fixture", "ring", NAME, join(h.kit, "Data", "Art", "Models", MARKER)],
  ]);
  assert.deepEqual(readFileSync(join(h.kit, "Data", "XML", "PARTICLES.XML")), patchParticlesXml(XML));
  const review = JSON.parse(readFileSync(join(h.kit, "review.json"), "utf8"));
  assert.deepEqual(review, {
    schemaVersion: 1, generatorId: "static-energy-ring", generatorVersion: 1, seed: 1, parameters: { radius: 0.6 },
    tier: "final", cacheKey: "c".repeat(64), contentHash: sha256(TEXTURE), texture: NAME,
    fixture: { kind: "ring", lifetime: 2, lift: REVIEW_LIFT }, markerFrames: MARKER_FRAMES, gameVerification: "not-run",
  });
  const text = readFileSync(join(h.kit, "REVIEW.md"), "utf8");
  assert.match(text, new RegExp(`Modpath=${h.kit.replace(/\\/g, "\\\\")}\\r\\n`));
  assert.match(text, /corruption\\swfoc\.exe" LANGUAGE=ENGLISH -WINDOWED/);
  assert.match(text, /--record ".+" --status passed\|failed/);
  // A second staging of the same texture is refused, and leaves the first alone.
  assert.throws(() => h.run(), (error) => error.exitCode === 2 && /already exists/.test(error.message));
  assert.deepEqual(readFileSync(join(h.kit, "Data", "Art", "Textures", NAME)), TEXTURE);
});

test("only a final-tier texture is staged, and a failed staging leaves no kit", (t) => {
  for (const tier of ["preview", "validate"]) {
    const h = harness(t, { entry: provenance({ tier }) });
    assert.throws(() => h.run(), (error) => error.exitCode === 2 && /final-tier/.test(error.message));
    assert.equal(existsSync(join(h.root, "review")), false);
  }
  for (const shown of [{ lift: 0 }, { colorTexture: "other.tga" }, { reloaded: false }, { kind: "puff" }]) {
    const h = harness(t, { shown });
    assert.throws(() => h.run(), (error) => error.exitCode === 1 && /does not show/.test(error.message), JSON.stringify(shown));
    assert.equal(existsSync(h.kit), false, JSON.stringify(shown));
  }
  const h = harness(t, { xml: null });
  assert.throws(() => h.run(), (error) => error.exitCode === 1 && /no PARTICLES\.XML/.test(error.message));
});

test("--record writes the verdict against the bytes the kit still holds", (t) => {
  const h = harness(t);
  h.run();
  const record = recordVerdict({ record: h.kit, status: "passed", notes: "  arrives as intended  " }, { today: () => "2026-10-14" });
  assert.deepEqual(record.gameVerification, { status: "passed", date: "2026-10-14", notes: "arrives as intended" });
  const review = JSON.parse(readFileSync(join(h.kit, "review.json"), "utf8"));
  assert.deepEqual(review.gameVerification, record.gameVerification);
  assert.equal(review.contentHash, sha256(TEXTURE));
  writeFileSync(join(h.kit, "Data", "Art", "Textures", NAME), "altered");
  assert.throws(() => recordVerdict({ record: h.kit, status: "failed", notes: "x" }), (error) => error.exitCode === 4 && /no longer matches/.test(error.message));
  const empty = join(h.root, "empty");
  mkdirSync(empty);
  assert.throws(() => recordVerdict({ record: empty, status: "passed", notes: "x" }), (error) => error.exitCode === 2);
  writeFileSync(join(empty, "review.json"), JSON.stringify({ schemaVersion: 1, texture: "..\\..\\x.tga" }));
  assert.throws(() => recordVerdict({ record: empty, status: "passed", notes: "x" }), (error) => error.exitCode === 2);
  // A DDS kit records the same way.
  const dds = join(h.root, "dds"), bytes = Buffer.from("dds bytes"), name = `pe_gen_smoke_${sha256(bytes).slice(0, 8)}.dds`;
  mkdirSync(join(dds, "Data", "Art", "Textures"), { recursive: true });
  writeFileSync(join(dds, "Data", "Art", "Textures", name), bytes);
  writeFileSync(join(dds, "review.json"), JSON.stringify({ schemaVersion: 1, texture: name, contentHash: sha256(bytes) }));
  assert.equal(recordVerdict({ record: dds, status: "passed", notes: "x" }, { today: () => "2026-10-14" }).texture, name);
});

test("the checklist names the kit, the lifetime and what to look for", () => {
  const text = checklist({ kit: "C:\\gen\\review\\k", game: null, provenance: provenance(), kind: "ring", lifetime: 2 });
  assert.match(text, /^# In-game review: static-energy-ring\r\n/);
  assert.match(text, /"swfoc\.exe" LANGUAGE=ENGLISH -WINDOWED Modpath=C:\\gen\\review\\k/);
  assert.match(text, /for 2 s each time a move order is given/);
  for (const item of ["black or white box", "dark fringe", "frames in order", "blooms", "zoomed out", "Keep the game in front",
    "WidescreenAspectRatio", "[single](1280 / 720)"])
    assert.ok(text.includes(item), item);
});

test("the marker lives long enough for every fixture: 150 frames for 4 s, in proportion beyond", () => {
  assert.deepEqual(["puff", "ring", "fireball", "smoke4", "smoke8"].map(markerFrames), [150, 150, 150, 150, 240]);
  assert.equal(patchParticlesXml(XML, 240).toString("latin1"),
    XML.toString("latin1").replace("<Particle_Lifetime_Frames>30", "<Particle_Lifetime_Frames>240"));
});
