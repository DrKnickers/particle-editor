import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import { join } from "node:path";
import { prepare, verify } from "./canonicalise.mjs";
import { pinFiles, sha256, store } from "./store.mjs";
import { effectBytes, emitter, fakeBenchExe } from "./testing.mjs";

function setUp(t, state = "draft") {
  const root = fs.mkdtempSync(join(os.tmpdir(), "vfx-canon-"));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const s = store(root);
  s.write(["bases", "A", "effect.alo"], effectBytes([emitter(0, "Fireball")]));
  s.writeJson(["corpus.json"], { schemaVersion: 1, state, bases: [{ id: "A" }] });
  const exe = join(root, "fake.exe");
  fs.writeFileSync(exe, "fake exe");
  return { root, s, exe };
}

test("an unchanged in-place re-save is recorded as the base's proof and the copy is removed", async (t) => {
  const { root, s, exe } = setUp(t);
  const hash = prepare({ root, base: "A" });
  assert.ok(s.exists("bases", "A", "effect.first.alo"));
  // A proof in flight is never pinned: --verify deletes it.
  assert.deepEqual(Object.keys(pinFiles(root)), ["bases/A/effect.alo"]);
  assert.equal(await verify({ root, base: "A", exe, run: fakeBenchExe() }), hash);
  assert.equal(s.json("corpus.json").bases[0].canonical, hash);
  assert.deepEqual(fs.readdirSync(s.at("bases", "A")), ["effect.alo"]);
});

test("an edited re-save, a missing preparation, a changed copy and a locked corpus are refused", async (t) => {
  const { root, s, exe } = setUp(t);
  await assert.rejects(verify({ root, base: "A", exe, run: fakeBenchExe() }), /no prepared copy/);
  prepare({ root, base: "A" });
  s.write(["bases", "A", "effect.alo"], effectBytes([emitter(0, "Fireball", { lifetime: 2 })]));
  await assert.rejects(verify({ root, base: "A", exe, run: fakeBenchExe() }), /not byte-identical/);
  assert.ok(s.exists("bases", "A", "effect.first.alo"), "a refused proof keeps the copy");
  assert.equal(s.json("corpus.json").bases[0].canonical, undefined);
  s.write(["bases", "A", "effect.first.alo"], s.read("bases", "A", "effect.alo"));
  await assert.rejects(verify({ root, base: "A", exe, run: fakeBenchExe() }), /changed since --prepare/);
  assert.throws(() => prepare({ root, base: "B" }), /no base B/);

  const locked = setUp(t, "locked");
  assert.throws(() => prepare({ root: locked.root, base: "A" }), /locked/);
});

test("a byte-identical re-save whose inspect output differs is refused", async (t) => {
  const { root, s, exe } = setUp(t);
  prepare({ root, base: "A" });
  const real = fakeBenchExe();
  const run = async (bin, args, options) => {
    const result = await real(bin, args, options);
    if (args[1] === "inspect" && args[2].endsWith("effect.alo")) {
      const out = JSON.parse(fs.readFileSync(args[4], "utf8"));
      out.leaveParticles = false;
      fs.writeFileSync(args[4], JSON.stringify(out));
    }
    return result;
  };
  await assert.rejects(verify({ root, base: "A", exe, run }), /not inspect-identical/);
  assert.equal(sha256(s.read("bases", "A", "effect.alo")), s.json("bases", "A", "effect.first.json").sha256);
  assert.ok(!s.exists("bases", "A", "effect.second.inspect.json"));
});
