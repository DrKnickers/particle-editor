import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import { join } from "node:path";
import { allowlist, buildPack, inventory, pngHasText, stripPngText, stripRngLines, tileMap, verifyPack } from "./pack.mjs";
import { bundle, fakeFfmpeg, png, spec } from "../vfx-loop/testing.mjs";

function temporary(t) {
  const dir = fs.mkdtempSync(join(os.tmpdir(), "vfx-pack-"));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  return dir;
}
const SHARED = { "prompt.md": "p\n", "rules.md": "r\n", "findings.schema.json": "{}\n", "intent.md": "# i\n", "textures.json": "[]\n" };

function withText(buffer) {
  // A tEXt chunk inserted after IHDR (33 bytes in).
  const data = Buffer.from("Comment\0seed 7");
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const chunk = Buffer.concat([len, Buffer.from("tEXt"), data, Buffer.alloc(4)]);
  return Buffer.concat([buffer.subarray(0, 33), chunk, buffer.subarray(33)]);
}

test("stripRngLines drops the generator fields and keeps everything else", () => {
  const out = stripRngLines('{"frame":0,"particles":3,"rngState":9,"rngDrawCount":4,"perEmitter":[3]}\n{"frame":1,"particles":0,"rngState":1,"rngDrawCount":5,"perEmitter":[0]}\n');
  assert.equal(out, '{"frame":0,"particles":3,"perEmitter":[3]}\n{"frame":1,"particles":0,"perEmitter":[0]}\n');
});

test("stripPngText removes text chunks and leaves the image bytes alone", () => {
  const clean = png(10);
  const tagged = withText(clean);
  assert.equal(pngHasText(tagged), true);
  assert.deepEqual(stripPngText(tagged), clean);
  assert.equal(pngHasText(clean), false);
  assert.throws(() => stripPngText(Buffer.from("nope")), /not a PNG/);
});

test("tile maps, the texture inventory and the allowlist", () => {
  assert.deepEqual(tileMap([0, 15, 30, 45, 60, 75, 90, 105, 120]).tiles[8], { tile: 8, row: 1, column: 0, frame: 120 });
  assert.deepEqual(inventory([["b.tga", "a.tga"], ["a.tga", ""]]), ["a.tga", "b.tga"]);
  const one = allowlist({ bundles: ["bundle"], frames: { bundle: [0, 2] }, inspect: true });
  assert.ok(one.includes("inspect.json") && one.includes("frames/frame-00002.png"));
  const three = allowlist({ bundles: ["bundle-a", "bundle-b", "bundle-c"], frames: { "bundle-a": [0], "bundle-b": [0], "bundle-c": [0] }, inspect: false });
  assert.ok(three.includes("bundle-b/frames.jsonl") && !three.includes("inspect.json") && !three.includes("frames.jsonl"));
});

test("a single-bundle pack holds exactly the allowlist, stripped, timed and verified", async (t) => {
  const dir = temporary(t);
  const b = await bundle(join(dir, "bundle"), spec());
  const out = join(dir, "pack");
  const { manifest, frames } = await buildPack({ out, bundles: { bundle: b }, shared: SHARED, inspectJson: "{}\n", run: fakeFfmpeg() });
  const expected = allowlist({ bundles: ["bundle"], frames, inspect: true });
  assert.deepEqual(verifyPack(out, manifest, expected), []);
  assert.ok(!fs.readFileSync(join(out, "frames.jsonl"), "utf8").includes("rng"));
  fs.writeFileSync(join(out, "intent.md"), "# changed\n");
  const errors = verifyPack(out, manifest, expected);
  assert.match(errors.join(), /intent.md does not match/);
  assert.match(errors.join(), /intent.md time is not normalised/);
  fs.writeFileSync(join(out, "extra.txt"), "x");
  assert.match(verifyPack(out, manifest, expected).join(), /file set differs/);
});

test("a three-bundle pack keeps shared files at its root and frames per bundle; no inspect in bundle-only", async (t) => {
  const dir = temporary(t);
  const bundles = {};
  for (const [label, seed] of [["bundle-a", 3], ["bundle-b", 9], ["bundle-c", 14]]) bundles[label] = await bundle(join(dir, label), spec({ scenario: { seed } }));
  const out = join(dir, "pack");
  const { manifest, frames } = await buildPack({ out, bundles, shared: SHARED, run: fakeFfmpeg() });
  assert.deepEqual(verifyPack(out, manifest, allowlist({ bundles: Object.keys(bundles), frames, inspect: false })), []);
  assert.ok(fs.existsSync(join(out, "bundle-b", "selected.json")));
  await assert.rejects(buildPack({ out, bundles, shared: SHARED, run: fakeFfmpeg() }), /exists/);
});

test("alternate data streams reported by the probe fail verification", async (t) => {
  const dir = temporary(t);
  const b = await bundle(join(dir, "bundle"), spec());
  const out = join(dir, "pack");
  const { manifest, frames } = await buildPack({ out, bundles: { bundle: b }, shared: SHARED, run: fakeFfmpeg() });
  const errors = verifyPack(out, manifest, allowlist({ bundles: ["bundle"], frames, inspect: false }),
    { streams: (p) => (p.endsWith("rules.md") ? ["hidden"] : []) });
  assert.match(errors.join(), /rules.md carries an alternate data stream hidden/);
});
