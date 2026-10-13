// [houdini] run_generator.py re-checks every job inside hython. Its rules use
// no Houdini, so a plain Python runs them here (plan §3.3 "Builder").
//
// -B keeps bytecode out of the shared interpreter (the public-history suite
// pins its closure), and -I keeps the environment and user site out.
import { test } from "node:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { sha256 } from "./cache.mjs";

const here = fileURLToPath(new URL(".", import.meta.url));
const runGenerator = join(here, "hython", "run_generator.py");
const registryBytes = readFileSync(join(here, "registry.json"));

function python() {
  if (process.platform === "win32") {
    assert.ok(process.env.LOCALAPPDATA, "LOCALAPPDATA is required to find Python");
    const command = join(process.env.LOCALAPPDATA, "Programs", "Python", "Python313", "python.exe");
    assert.ok(existsSync(command), `Python 3 is required at ${command}`);
    return command;
  }
  return process.env.PE_PYTHON ?? "/usr/bin/python3";
}
const run = (args, input = "") => {
  const result = spawnSync(python(), ["-B", "-I", ...args], { input, encoding: "utf8", windowsHide: true, timeout: 60000 });
  assert.equal(result.error, undefined);
  return result;
};

// Each case calls one function and reports "ok" or the rejection message.
const DRIVER = String.raw`
import importlib.util, json, math, os, shutil, sys, tempfile
spec = importlib.util.spec_from_file_location("run_generator", sys.argv[1])
rg = importlib.util.module_from_spec(spec); spec.loader.exec_module(rg)
registry = open(rg.REGISTRY_PATH, "rb").read()
good = {"schemaVersion": 1, "id": "debug-atlas", "params": {}, "seed": 7, "tier": "preview", "threads": 4,
        "registrySha256": rg.sha256(registry)}
SCHEMA = {"heat": {"type": "float", "min": 0.5, "max": 2.0, "default": 1.0},
          "count": {"type": "int", "min": 1, "max": 8, "default": 4},
          "palette": {"type": "enum", "values": ["orange", "blue"], "default": "orange"}}
def outcome(fn):
    try:
        return ["ok", fn()]
    except rg.Rejected as error:
        return ["rejected", str(error)]
def job(**change):
    value = dict(good); value.update(change); return value
def without(key):
    value = dict(good); del value[key]; return value
results = {}
results["good"] = outcome(lambda: rg.check_job(good, registry)[1])
for name, value in [("extra", job(exe="cmd")), ("missing", without("tier")), ("schema", job(schemaVersion=2)),
                    ("registry", job(registrySha256="0" * 64)), ("idMalformed", job(id="D:evil")),
                    ("idUnknown", job(id="nope")), ("idProto", job(id="__proto__")),
                    ("seedBool", job(seed=True)), ("seedNegative", job(seed=-1)), ("seedBig", job(seed=2 ** 31)),
                    ("seedFloat", job(seed=7.0)), ("tier", job(tier="ultra")), ("threadsZero", job(threads=0)),
                    ("threadsBig", job(threads=65)), ("threadsBool", job(threads=True)),
                    ("paramsUnknown", job(params={"heat": 1})), ("paramsList", job(params=[]))]:
    results[name] = outcome(lambda value=value: rg.check_job(value, registry))
ok = {"heat": 1.0, "count": 4, "palette": "orange"}
results["paramsGood"] = outcome(lambda: rg.check_params({"heat": 1, "count": 4.0, "palette": "blue"}, SCHEMA))
for name, change in [("inf", {"heat": math.inf}), ("nan", {"heat": math.nan}), ("range", {"heat": 2.5}),
                     ("bool", {"heat": True}), ("string", {"heat": "1"}), ("intFraction", {"count": 2.5}),
                     ("enum", {"palette": "red"}), ("missing", None), ("extra", {"other": 1})]:
    params = dict(ok)
    if change is None: del params["palette"]
    else: params.update(change)
    results["param_" + name] = outcome(lambda params=params: rg.check_params(params, SCHEMA))
entry = json.loads(registry)["generators"]["debug-atlas"]
results["builderGood"] = outcome(lambda: callable(rg.load_builder(entry)["generate"]))
for name, change in [("hash", {"builderSha256": "0" * 64}), ("dotdot", {"builder": "generators/../registry.py"}),
                     ("drive", {"builder": "generators/D:evil.py"}), ("outside", {"builder": "hython/run_generator.py"})]:
    value = dict(entry); value.update(change)
    results["builder_" + name] = outcome(lambda value=value: rg.load_builder(value))
work = tempfile.mkdtemp()
os.makedirs(os.path.join(work, "frames"))
names = ["frames/frame_%04d.exr" % (i + 1) for i in range(4)]
for i, name in enumerate(names[:3]):
    open(os.path.join(work, *name.split("/")), "wb").write(b"frame %d" % i)
results["manifestMissing"] = outcome(lambda: rg.frame_manifest(names, 4, work))
open(os.path.join(work, *names[3].split("/")), "wb").write(b"frame 3")
results["manifestGood"] = outcome(lambda: rg.frame_manifest(names, 4, work))
results["manifestCount"] = outcome(lambda: rg.frame_manifest(names[:3], 4, work))
results["manifestName"] = outcome(lambda: rg.frame_manifest(names[:3] + ["frames/../x.exr"], 4, work))
results["manifestType"] = outcome(lambda: rg.frame_manifest(None, 4, work))
shutil.rmtree(work)
# The builder regex keeps every name inside generators/; inside() is the
# second wall, against a link or junction that leads out of it.
if os.name == "nt":
    import _winapi
    base = tempfile.mkdtemp()
    gen, out = os.path.join(base, "generators"), os.path.join(base, "elsewhere")
    os.makedirs(gen); os.makedirs(out)
    _winapi.CreateJunction(out, os.path.join(gen, "link"))
    results["insideDirect"] = rg.inside(gen, os.path.join(gen, "a.py"))
    results["insideJunction"] = rg.inside(gen, os.path.join(gen, "link", "a.py"))
    results["insideBase"] = rg.inside(gen, gen)
    os.rmdir(os.path.join(gen, "link"))
    shutil.rmtree(base)
print(json.dumps(results))
`;

test("run_generator's checks accept the shipped job and reject every malformed one", () => {
  const result = run(["-c", DRIVER, runGenerator]);
  assert.equal(result.status, 0, result.stderr);
  const r = JSON.parse(result.stdout);
  assert.deepEqual(r.good, ["ok", { id: "debug-atlas", params: {}, seed: 7, tier: "preview", threads: 4 }]);
  const rejected = {
    extra: /unknown fields: exe/, missing: /missing fields: tier/, schema: /schema/, registry: /registry changed/,
    idMalformed: /malformed/, idUnknown: /unknown generator/, idProto: /malformed/,
    seedBool: /seed/, seedNegative: /seed/, seedBig: /seed/, seedFloat: /seed/, tier: /tier/,
    threadsZero: /threads/, threadsBig: /threads/, threadsBool: /threads/,
    paramsUnknown: /unknown parameters: heat/, paramsList: /not an object/,
    param_inf: /not finite/, param_nan: /not finite/, param_range: /out of range/, param_bool: /not a number/,
    param_string: /not a number/, param_intFraction: /not an integer/, param_enum: /not one of its values/,
    param_missing: /palette is missing/, param_extra: /unknown parameters: other/,
    builder_hash: /pinned hash/, builder_dotdot: /malformed/, builder_drive: /malformed/, builder_outside: /malformed/,
    manifestMissing: /frame is missing: frames\/frame_0004.exr/, manifestCount: /3 frames, not 4/,
    manifestName: /malformed/, manifestType: /0 frames, not 4/,
  };
  for (const [name, pattern] of Object.entries(rejected)) {
    assert.equal(r[name]?.[0], "rejected", `${name} was not rejected: ${JSON.stringify(r[name])}`);
    assert.match(r[name][1], pattern, name);
  }
  assert.deepEqual(r.paramsGood, ["ok", { count: 4, heat: 1.0, palette: "blue" }]);
  assert.deepEqual(r.builderGood, ["ok", true]);
  assert.equal(r.manifestGood[0], "ok");
  assert.deepEqual(r.manifestGood[1].frames.map((f) => [f.index, f.name]),
    [0, 1, 2, 3].map((i) => [i, `frames/frame_000${i + 1}.exr`]));
  assert.equal(r.manifestGood[1].frames[2].sha256, sha256(Buffer.from("frame 2")));
  if (process.platform === "win32") {
    assert.deepEqual([r.insideDirect, r.insideJunction, r.insideBase], [true, false, false],
      "inside() must follow a junction out of generators/");
  }
});

test("a rejected job exits 2 before Houdini is imported", () => {
  // Plain Python has no `hou`: reaching the import would fail with exit 1.
  const job = { schemaVersion: 1, id: "debug-atlas", params: {}, seed: -1, tier: "preview", threads: 4,
    registrySha256: sha256(registryBytes) };
  const result = run([runGenerator], JSON.stringify(job));
  assert.equal(result.status, 2, result.stderr);
  assert.deepEqual(JSON.parse(result.stdout.replace(/^\[houdini-run\] /, "")), { ok: false, rejected: "seed is out of range" });
  const large = run([runGenerator], " ".repeat(65537));
  assert.equal(large.status, 2);
  assert.match(large.stdout, /job is too large/);
  const garbage = run([runGenerator], "not json");
  assert.equal(garbage.status, 2);
  assert.match(garbage.stdout, /"ok": false/);
});
