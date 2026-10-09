// [tool-smoke] Drives the built exe's window-free --tool verbs end to end, the
// way an agent or a script would: inspect, validate, then patch, on the guide's
// shipped example effect. The C++ tests cover each verb's edge cases
// (tests/test_tool_cli.cpp, tests/test_tool_patch.cpp); this lane proves the
// three chain together from Node against the exe the gate just built.
//
//   node scripts/tool-smoke.mjs [--exe <ParticleEditor.exe>]
//
// Exits 0 when every step holds, 1 otherwise. Needs no game install and opens
// no window.
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { copyFileSync, existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { isDeepStrictEqual } from "node:util";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
export const FIXTURE = join(repoRoot, "site", "guide", "downloads", "P_EXPLOSION_EXAMPLE.ALO");

const sha256 = (bytes) => createHash("sha256").update(bytes).digest("hex");

// The exe is a Windows program: spawnSync waits for it, where PowerShell's `&`
// would not. A deadline turns a hang (a dialog, say) into a failure.
export function runTool(exe, args) {
  const r = spawnSync(exe, ["--tool", ...args], { encoding: "utf8", windowsHide: true, timeout: 60000 });
  return { code: r.error || r.signal ? -1 : r.status, stderr: `${r.stderr ?? ""}${r.error ? r.error.message : ""}` };
}

// A patch request for the root emitter `name`, selected by the source's hash.
export function rootRequest(hash, name, mutations) {
  return { schema: 1, selector: { sourceDocumentHash: hash, rolePath: [{ role: "root", name }], expected: { name } }, mutations };
}

// A copy of an inspect document with the given paths ("emitters[6].groups[1]")
// and the source hash removed.
export function outside(doc, paths) {
  const copy = structuredClone(doc);
  delete copy.sourceDocumentHash;
  for (const path of paths) {
    const keys = path.match(/[^.[\]]+/g);
    let node = copy;
    for (const key of keys.slice(0, -1)) node = node?.[key];
    if (node && typeof node === "object") delete node[keys.at(-1)];
  }
  return copy;
}

export function smoke(exe, dir, say = console.log) {
  const failures = [];
  const check = (ok, what) => { say(`  ${ok ? "ok:  " : "FAIL:"} ${what}`); if (!ok) failures.push(what); };
  const json = (name) => { try { return JSON.parse(readFileSync(join(dir, name), "utf8")); } catch { return null; } };
  const at = (name) => join(dir, name);

  // The internal-name warning compares the name with the file's, so keep it.
  const source = at("p_explosion_example.alo");
  copyFileSync(FIXTURE, source);
  const bytes = readFileSync(source);
  const hash = sha256(bytes);

  say("inspect");
  check(runTool(exe, ["inspect", source, "--out", at("inspect.json")]).code === 0, "exits 0");
  const before = json("inspect.json");
  check(before?.sourceDocumentHash === hash, "sourceDocumentHash is the SHA-256 of the input");
  const fire = before?.emitters?.find((e) => e.name === "Fire");
  check(fire !== undefined && fire.linkGroup === 0 && fire.parent === null, "lists the unlinked root emitter 'Fire'");

  say("validate");
  check(runTool(exe, ["validate", source, "--out", at("validate.json")]).code === 0, "exits 0");
  const v = json("validate.json");
  check(v?.valid === true && v?.canonical === true, "the shipped example is valid and canonical");

  say("patch");
  const patch = (request, out, report) => {
    writeFileSync(at(`${report}.request.json`), JSON.stringify(request));
    return runTool(exe, ["patch", source, at(`${report}.request.json`), "--out", at(out), "--report", at(report)]).code;
  };
  check(patch(rootRequest(hash, "Fire", []), "zero.alo", "zero.json") === 0 &&
        readFileSync(at("zero.alo")).equals(bytes), "a zero-op patch of a canonical file writes it back byte for byte");

  const edit = [{ op: "setProperty", field: "lifetime", value: 0.5 }, { op: "rename", name: "Fire Core" }];
  check(patch(rootRequest(hash, "Fire", edit), "edited.alo", "edited.json") === 0, "a two-op patch exits 0");
  const report = json("edited.json");
  check(report?.outcome === "ok" && report?.candidateHash === sha256(readFileSync(at("edited.alo"))),
        "the report says ok and hashes the candidate");
  check(["1", "2", "3", "4", "5"].every((k) => report?.checks?.[k] === true), "every postcondition passed");

  // Read the candidate back. Setting lifetime also moves the Lifetime group,
  // as the editor does, so the report's footprint names three paths; outside
  // them nothing may change.
  check(runTool(exe, ["inspect", at("edited.alo"), "--out", at("after.json")]).code === 0, "the candidate inspects");
  const after = json("after.json");
  const index = before?.emitters?.indexOf(fire) ?? -1;
  const edited = after?.emitters?.[index];
  check(edited?.name === "Fire Core" && edited?.lifetime === 0.5, "the candidate holds the new name and lifetime");
  const footprint = report?.footprint ?? [];
  check(footprint.includes(`emitters[${index}].lifetime`) && footprint.includes(`emitters[${index}].name`),
        "the footprint names both edited fields");
  check(before !== null && after !== null &&
        isDeepStrictEqual(outside(after, footprint), outside(before, footprint)), "nothing outside the footprint changed");

  check(patch(rootRequest(hash, "Debris", edit.slice(0, 1)), "linked.alo", "linked.json") === 5 &&
        json("linked.json")?.refusal?.category === "linked-target" && !existsSync(at("linked.alo")),
        "a linked emitter is refused (exit 5) and nothing is written");
  check(patch(rootRequest("0".repeat(64), "Fire", edit), "stale.alo", "stale.json") === 4 && !existsSync(at("stale.alo")),
        "a stale hash is refused (exit 4) and nothing is written");
  check(readFileSync(source).equals(bytes), "the source is unchanged");
  return failures;
}

function main(argv) {
  const i = argv.indexOf("--exe");
  const exe = i >= 0 ? argv[i + 1] : join(repoRoot, "x64", "Release", "ParticleEditor.exe");
  if (!exe || !existsSync(exe)) { console.error(`[tool-smoke] exe not found: ${exe}`); return 2; }
  const dir = mkdtempSync(join(tmpdir(), "pe-tool-smoke-"));
  try {
    const failures = smoke(exe, dir);
    console.log(failures.length === 0 ? "[tool-smoke] all checks passed" : `[tool-smoke] ${failures.length} check(s) failed`);
    return failures.length === 0 ? 0 : 1;
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) process.exitCode = main(process.argv.slice(2));
