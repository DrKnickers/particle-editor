import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import http from "node:http";
import os from "node:os";
import { join } from "node:path";
import { createBlindServer, packFile, session, writeTruth } from "./blind-page.mjs";
import { manifestOf } from "./pack.mjs";
import { store } from "./store.mjs";
import { png } from "../vfx-loop/testing.mjs";

const EMITTERS = [{ index: 0, name: "Fireball" }, { index: 1, name: "Smoke" }];

function setUp(t) {
  const root = fs.mkdtempSync(join(os.tmpdir(), "vfx-blind-"));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const s = store(root);
  for (const id of ["d1", "c1"]) {
    const files = {
      "intent.md": "# Fireball\n", "selected.json": JSON.stringify({ columns: 8, tiles: [{ tile: 0, row: 0, column: 0, frame: 0 }, { tile: 1, row: 0, column: 1, frame: 15 }] }),
      "summary.json": JSON.stringify({ emitters: EMITTERS.map((e) => ({ ...e, firstActive: 0 })) }),
      "frames/frame-00000.png": png(10), "frames/frame-00015.png": png(20), "inspect.json": "{}",
    };
    for (const [rel, bytes] of Object.entries(files)) s.write(["packs", id, ...rel.split("/")], bytes);
    s.writeJson(["packs", `${id}.manifest.json`], manifestOf(files));
  }
  s.write(["corpus.json"], "{}"); // replaced below
  s.writeJson(["corpus.json"], { state: "draft", cases: [{ id: "d1", kind: "defect" }, { id: "c1", kind: "control" }], order: ["c1", "d1"] });
  return { root, s };
}

const answer = (over = {}) => ({
  bundleId: "bundle", category: "duration", direction: "too-short", emitters: [EMITTERS[0]],
  windows: { bundle: { image: [15], telemetry: [] } }, policy: { primary: "frames", "bundle-only": "frames" },
  correction: { kind: "scalar", emitter: EMITTERS[0], field: "lifetime" }, ...over,
});
const truthOf = (variants, visible = true) => ({ transports: { frames: { visible, variants }, "contact-sheets": { visible, variants } } });

test("packs come in the recorded order with their bundles, frames and emitters", (t) => {
  const { root } = setUp(t);
  const s = session(root);
  assert.deepEqual(s.packs.map((p) => [p.n, p.id, p.done]), [[1, "c1", false], [2, "d1", false]]);
  assert.deepEqual(s.packs[1].bundles, ["bundle"]);
  assert.deepEqual(s.packs[1].frames, { bundle: [0, 15] });
  assert.deepEqual(s.packs[1].emitters, EMITTERS);
});

test("only manifest-listed pack files are served", (t) => {
  const { root } = setUp(t);
  assert.equal(packFile(root, "d1", "intent.md").toString(), "# Fireball\n");
  for (const rel of ["../corpus.json", "../../corpus.json", "frames/../intent.md", "frames", "d1.manifest.json", "./intent.md"]) assert.equal(packFile(root, "d1", rel), null, rel);
  assert.equal(packFile(root, "nope", "intent.md"), null);
});

test("a complete answer is written; an incomplete one is refused", (t) => {
  const { root, s } = setUp(t);
  writeTruth(root, "d1", truthOf([answer(), answer({ category: "timing", direction: "too-late", correction: { kind: "scalar", emitter: EMITTERS[1], field: "initialDelay" } })]));
  assert.equal(s.json("cases", "d1", "truth.json").transports.frames.variants.length, 2);
  assert.equal(session(root).packs[1].done, true);
  const refused = (variants, pattern, visible = true) => assert.throws(() => writeTruth(root, "d1", truthOf(variants, visible)), pattern);
  refused([answer({ correction: undefined })], /correction is one of the direction's options/);
  refused([answer({ correction: { kind: "scalar", emitter: EMITTERS[0], field: "randomScalePerc" } })], /correction is one of/);
  refused([answer({ emitters: [] })], /names its emitters/);
  refused([answer({ emitters: [{ index: 0, name: "Smoke" }] })], /names its emitters/);
  refused([answer({ direction: "too-loud" })], /direction too-loud/);
  refused([answer({ bundleId: "bundle-a" })], /bundle bundle-a is not in the pack/);
  refused([answer({ windows: { bundle: { image: [-1], telemetry: [] } } })], /lists image and telemetry frames/);
  refused([answer({ policy: { primary: "frames" } })], /bundle-only condition/);
  refused([answer({ windows: { bundle: { image: [], telemetry: [] } } })], /needs a non-empty window/);
  refused([answer({ category: "draw-order", direction: Object.keys(JSON.parse(fs.readFileSync(new URL("./rubric.json", import.meta.url))).categories["draw-order"].directions)[0] })], /both emitters of its relation/);
  refused([answer()], /not-visible annotation carries no answer/, false);
  assert.throws(() => writeTruth(root, "d1", { transports: { frames: { visible: true, variants: [] } } }), /no contact-sheets annotation/);
  assert.throws(() => writeTruth(root, "zz", truthOf([])), /no case zz/);
  // A control the owner flags is recorded; lock then rules its base unsuitable.
  writeTruth(root, "c1", truthOf([answer()], false));
});

test("a locked corpus is refused at start and at every write", (t) => {
  const { root, s } = setUp(t);
  const server = createBlindServer({ root });
  server.close();
  s.writeJson(["corpus.json"], { ...s.json("corpus.json"), state: "locked" });
  assert.throws(() => createBlindServer({ root }), /locked/);
  assert.throws(() => writeTruth(root, "d1", truthOf([answer()])), /locked/);
  assert.throws(() => packFile(root, "d1", "intent.md"), /locked/);
});

function request(port, method, path, { body, type } = {}) {
  return new Promise((resolve, reject) => {
    const req = http.request({ host: "127.0.0.1", port, method, path, headers: type ? { "content-type": type } : {} }, (res) => {
      const parts = [];
      res.on("data", (c) => parts.push(c));
      res.on("end", () => resolve({ status: res.statusCode, type: res.headers["content-type"], body: Buffer.concat(parts) }));
    });
    req.on("error", reject);
    req.end(body);
  });
}

test("over HTTP: the page, the session, pack files, and the truth write", async (t) => {
  const { root, s } = setUp(t);
  const server = createBlindServer({ root });
  await new Promise((r) => server.listen(0, "127.0.0.1", r));
  t.after(() => server.close());
  const { port } = server.address();
  const page = await request(port, "GET", "/");
  assert.equal(page.status, 200);
  assert.match(page.body.toString(), /Blind pass/);
  assert.deepEqual(JSON.parse((await request(port, "GET", "/api/session")).body).packs.map((p) => p.id), ["c1", "d1"]);
  const frame = await request(port, "GET", "/pack/d1/frames/frame-00015.png");
  assert.equal(frame.status, 200);
  assert.equal(frame.type, "image/png");
  assert.equal((await request(port, "GET", "/pack/d1/../corpus.json")).status, 404);
  assert.equal((await request(port, "GET", "/pack/d1/..%2F..%2Fcorpus.json")).status, 404);
  assert.equal((await request(port, "GET", "/pack/d1/%2e%2e/%2e%2e/corpus.json")).status, 404);
  const body = JSON.stringify(truthOf([answer()]));
  assert.equal((await request(port, "POST", "/api/truth/d1", { body, type: "text/plain" })).status, 415);
  assert.equal((await request(port, "POST", "/api/truth/d1", { body: "{", type: "application/json" })).status, 400);
  assert.equal((await request(port, "POST", "/api/truth/d1", { body: "x".repeat(300 * 1024), type: "application/json" })).status, 413);
  assert.equal((await request(port, "POST", "/api/truth/d1", { body, type: "application/json" })).status, 200);
  assert.equal(JSON.parse((await request(port, "GET", "/api/truth/d1")).body).transports.frames.variants.length, 1);
  s.writeJson(["corpus.json"], { ...s.json("corpus.json"), state: "locked" });
  assert.equal((await request(port, "POST", "/api/truth/d1", { body, type: "application/json" })).status, 409);
  assert.equal((await request(port, "GET", "/pack/d1/intent.md")).status, 409);
});
