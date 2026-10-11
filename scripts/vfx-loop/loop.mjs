// [vfx-loop] The Phase 5 loop coordinator (design §18.1): observe a source
// effect, apply a bounded patch through the exe's --tool verbs, observe the
// candidate under the same scenario, and compare the two (compare.mjs).
//
//   node scripts/vfx-loop/loop.mjs --task <task.json> --source <effect.alo>
//     --patch <patch-set.json> --out <task-root> [--exe path] [--game-root path ...]
//
// The loop never writes the source: it works on a copy inside a fresh task
// root, and every step leaves its artifact there (docs/VFX_LOOP.md). It decides
// nothing about taste. The comparison it ends with is evidence for a human.
import * as fs from "node:fs";
import { basename, dirname, isAbsolute, join, relative, resolve, win32 } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { observe, sha256, spawnWithDeadline } from "../observe.mjs";
import { resolveReplayGameRoots } from "../replay-proof.mjs";
import { METRICS, writeComparison } from "./compare.mjs";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const serialise = (value) => JSON.stringify(value, null, 2) + "\n";
const fail = (message, exitCode = 1) => Object.assign(new Error(message), { exitCode });

// 0 complete; 1 a step failed (exe, ffmpeg, untrustworthy evidence);
// 2 usage or a malformed task / patch set; 3 the source or candidate is invalid;
// 4 the patch was refused; 7 the observations are misaligned; 13 task-root IO.
export const EXIT = { ok: 0, step: 1, input: 2, invalid: 3, refused: 4, misaligned: 7, io: 13 };

// §18.3. The loop carries findings into the task root for the human; it acts
// only on the requests.
const FINDING_KEYS = ["id", "severity", "category", "claim", "evidence", "suspectedEmitter", "suggestedChange", "confidence"];

export function checkTask(task) {
  const problems = [];
  const object = (value) => value !== null && typeof value === "object" && !Array.isArray(value);
  if (!object(task) || task.schemaVersion !== 1) return ["task: schemaVersion must be 1"];
  if (typeof task.brief !== "string" || !task.brief.trim()) problems.push("task: brief must be a non-empty string");
  const observation = task.observation;
  if (!object(observation) || !object(observation.scenario) || !object(observation.lifecycle) || !object(observation.output))
    problems.push("task: observation needs scenario, lifecycle and output objects");
  else if ("aloPath" in observation.scenario) problems.push("task: observation.scenario must not name aloPath; the loop sets it");
  if (!Array.isArray(task.budgets)) problems.push("task: budgets must be an array");
  else {
    const ids = new Set();
    for (const [i, budget] of task.budgets.entries()) {
      if (!object(budget) || typeof budget.id !== "string" || !budget.id || ids.has(budget.id))
        problems.push(`task: budgets[${i}] needs a unique id`);
      else ids.add(budget.id);
      if (!METRICS.includes(budget?.metric)) problems.push(`task: budgets[${i}].metric must be one of ${METRICS.join(", ")}`);
      if (!Number.isFinite(budget?.maxDelta)) problems.push(`task: budgets[${i}].maxDelta must be a finite number`);
    }
  }
  return problems;
}

// Returns problems as [message, exitCode] pairs: a request authored against
// another source is a refusal (as --tool patch exit 4), the rest are malformed.
export function checkPatchSet(set, sourceHash) {
  const object = (value) => value !== null && typeof value === "object" && !Array.isArray(value);
  if (!object(set) || set.schema !== 1) return [["patch set: schema must be 1", EXIT.input]];
  if (!Array.isArray(set.requests) || set.requests.length === 0) return [["patch set: requests must be a non-empty array", EXIT.input]];
  const problems = [], targets = new Set();
  for (const [i, request] of set.requests.entries()) {
    const selector = request?.selector;
    if (!object(request) || !object(selector) || !Array.isArray(selector.rolePath) || !Array.isArray(request.mutations)) {
      problems.push([`patch set: requests[${i}] needs a selector with a rolePath, and mutations`, EXIT.input]);
      continue;
    }
    // The chain rebinds later requests to intermediate hashes, so each must
    // still prove it was authored against this exact source.
    if (selector.sourceDocumentHash !== sourceHash)
      problems.push([`patch set: requests[${i}] was authored against ${selector.sourceDocumentHash}, not the source ${sourceHash}`, EXIT.refused]);
    // A second request for one emitter would select it by a fingerprint the
    // first request may have changed. One request per emitter, then.
    const target = JSON.stringify(selector.rolePath);
    if (targets.has(target)) problems.push([`patch set: requests[${i}] targets ${target} again; put one emitter's edits in one request`, EXIT.input]);
    targets.add(target);
  }
  if (set.findings !== undefined) {
    if (!Array.isArray(set.findings)) problems.push(["patch set: findings must be an array", EXIT.input]);
    else for (const [i, finding] of set.findings.entries())
      for (const key of FINDING_KEYS) if (!object(finding) || !(key in finding))
        problems.push([`patch set: findings[${i}] is missing ${key} (§18.3)`, EXIT.input]);
  }
  return problems;
}

// Lexical confinement plus a real-path check of the parent: nothing the loop
// writes may land outside the task root, through `..` or a junction.
export function taskPath(root, name, io = fs) {
  if (typeof name !== "string" || !name || name.includes("\\") || name.includes(":") || isAbsolute(name) ||
      win32.isAbsolute(name) || name.split("/").some((v) => !v || v === "." || v === ".."))
    throw fail(`path is not task-relative: ${name}`, EXIT.io);
  const path = resolve(root, name);
  io.mkdirSync(dirname(path), { recursive: true });
  const rel = relative(io.realpathSync(root), io.realpathSync(dirname(path)));
  if (rel === ".." || rel.startsWith(`..${win32.sep}`) || rel.startsWith("../") || isAbsolute(rel))
    throw fail(`path escapes the task root: ${name}`, EXIT.io);
  return path;
}

const TOOL_EXIT = { 2: EXIT.input, 3: EXIT.invalid, 4: EXIT.refused, 5: EXIT.refused, 6: EXIT.refused, 13: EXIT.io };

export async function runLoop({ task, source, patchSet, out, exe = join(repoRoot, "x64/Release/ParticleEditor.exe"),
  roots = [], run = spawnWithDeadline, io = fs, say = (line) => console.log(line) } = {}) {
  const taskProblems = checkTask(task);
  if (taskProblems.length) throw fail(taskProblems.join("; "), EXIT.input);
  if (io.existsSync(out) && (io.lstatSync(out).isSymbolicLink() || io.readdirSync(out).length))
    throw fail(`task root must be absent or empty: ${out}`, EXIT.io);
  io.mkdirSync(out, { recursive: true });
  const at = (name) => taskPath(out, name, io);
  const dirAt = (name) => dirname(at(`${name}/_`)); // a confined, created directory
  const steps = [];
  let current = "start";
  const log = (step, fields) => {
    steps.push({ step, ...fields });
    say(`[vfx-loop] step=${step} ${Object.entries(fields).map(([k, v]) => `${k}=${v}`).join(" ")}`);
  };
  const tool = async (args, label) => {
    const result = await run(exe, ["--tool", ...args], { timeoutMs: 120000 });
    if (result.code !== 0 && !(result.code in TOOL_EXIT))
      throw fail(`${label}: --tool exit ${result.code}: ${result.stderr}`, EXIT.step);
    return result.code;
  };
  const readJson = (path) => JSON.parse(io.readFileSync(path, "utf8"));
  const record = (value) => {
    const path = at("loop.json"), temp = at("loop.json.tmp");
    io.writeFileSync(temp, serialise(value)); io.renameSync(temp, path);
  };
  const name = basename(source);
  const sourceBytes = io.readFileSync(source), sourceHash = sha256(sourceBytes);
  const summary = { schemaVersion: 1, brief: task.brief, source: { path: resolve(source), sha256: sourceHash } };
  try {
    // The --tool verbs warn when a file's name differs from the effect's
    // internal name, so the copy and every candidate keep the source's name.
    current = "source";
    const copy = at(`source/${name}`);
    io.writeFileSync(copy, sourceBytes);
    log(current, { sha256: sourceHash, copy: `source/${name}` });

    current = "evidence";
    if (await tool(["inspect", copy, "--out", at("evidence/inspect.json")], "inspect") !== 0)
      throw fail("inspect refused the source", EXIT.invalid);
    if (readJson(at("evidence/inspect.json")).sourceDocumentHash !== sourceHash)
      throw fail("inspect hashed different source bytes", EXIT.step);
    const validated = await tool(["validate", copy, "--out", at("evidence/validate.json")], "validate source");
    if (validated !== 0) throw fail(`the source is invalid (validate exit ${validated}); see evidence/validate.json`, TOOL_EXIT[validated]);
    log(current, { inspect: "evidence/inspect.json", validate: "evidence/validate.json" });

    current = "patch-set";
    const problems = checkPatchSet(patchSet, sourceHash);
    if (problems.length) throw fail(problems.map(([message]) => message).join("; "), Math.max(...problems.map(([, code]) => code)));
    io.writeFileSync(at("patch/patch-set.json"), serialise(patchSet));
    log(current, { requests: patchSet.requests.length, findings: patchSet.findings?.length ?? 0 });

    const spec = (aloPath) => ({ schemaVersion: 1, scenario: { schemaVersion: 1, ...task.observation.scenario, aloPath },
      lifecycle: task.observation.lifecycle, output: task.observation.output });
    const observeRun = async (label, aloPath, expectedHash) => {
      const specPath = at(`observations/${label}.observation.json`);
      io.writeFileSync(specPath, serialise(spec(aloPath)));
      const directory = dirAt(`observations/${label}`);
      await observe(specPath, directory, { exe, roots, run, io });
      const capture = readJson(join(directory, "capture.json"));
      if (capture.source.sha256 !== expectedHash) throw fail(`${label} observation saw different source bytes`, EXIT.step);
      return directory;
    };
    current = "baseline";
    const baselineDir = await observeRun("baseline", copy, sourceHash);
    log(current, { bundle: "observations/baseline" });

    // Request k applies to step k-1's verified output, rebound to its hash.
    let input = copy, hash = sourceHash;
    for (const [i, original] of patchSet.requests.entries()) {
      const stepName = String(i + 1).padStart(2, "0");
      current = `patch-${stepName}`;
      const request = structuredClone(original);
      request.selector.sourceDocumentHash = hash;
      const requestPath = at(`patch/${stepName}/request.json`), output = at(`patch/${stepName}/${name}`);
      const reportPath = at(`patch/${stepName}/report.json`);
      io.writeFileSync(requestPath, serialise(request));
      const code = await tool(["patch", input, requestPath, "--out", output, "--report", reportPath], current);
      const report = io.existsSync(reportPath) ? readJson(reportPath) : null;
      if (code !== 0) {
        const refusal = report?.refusal ? ` (${report.refusal.category}: ${report.refusal.reason})` : "";
        throw fail(`request ${i + 1} refused with --tool exit ${code}${refusal}; see patch/${stepName}/report.json`, TOOL_EXIT[code]);
      }
      const bytes = io.readFileSync(output);
      if (report?.outcome !== "ok" || report.candidateHash !== sha256(bytes) || !Object.values(report.checks ?? {}).every(Boolean))
        throw fail(`request ${i + 1}: the patch report does not vouch for patch/${stepName}/${name}`, EXIT.step);
      input = output; hash = report.candidateHash;
      log(current, { emitter: report.selector?.name ?? "?", candidateHash: hash });
    }

    current = "candidate";
    const candidate = at(`candidate/${name}`);
    io.copyFileSync(input, candidate);
    const candidateValid = await tool(["validate", candidate, "--out", at("candidate/validate.json")], "validate candidate");
    if (candidateValid !== 0)
      throw fail(`the candidate is invalid (validate exit ${candidateValid}); see candidate/validate.json`, TOOL_EXIT[candidateValid]);
    summary.candidate = { path: `candidate/${name}`, sha256: hash };
    log(current, { sha256: hash, validate: "candidate/validate.json" });

    current = "candidate-observation";
    const candidateDir = await observeRun("candidate", candidate, hash);
    log(current, { bundle: "observations/candidate" });

    current = "compare";
    const comparison = dirAt("comparison");
    const { metrics, schedule } = await writeComparison(baselineDir, candidateDir, comparison,
      { budgets: task.budgets, run, io });
    log(current, { budgetsPass: metrics.budgetsPass, firstRandomDivergence: schedule.firstDivergentFrame });

    current = "source-check";
    for (const [label, path] of [["source", source], ["source copy", copy]])
      if (sha256(io.readFileSync(path)) !== sourceHash) throw fail(`the ${label} changed during the loop`, EXIT.step);
    log(current, { unchanged: true });

    summary.technicalGate = { pass: metrics.budgetsPass, failed: metrics.budgets.filter((v) => !v.pass).map((v) => v.id) };
    summary.steps = steps;
    summary.failure = null;
    record(summary);
    return summary;
  } catch (error) {
    const exitCode = error.exitCode ?? EXIT.step;
    summary.steps = steps;
    summary.failure = { step: current, exitCode, message: error.message };
    try { record(summary); } catch { /* the original failure is the one to report */ }
    throw Object.assign(error, { exitCode });
  }
}

export async function main(argv = process.argv.slice(2)) {
  const usage = "usage: node scripts/vfx-loop/loop.mjs --task <task.json> --source <effect.alo> --patch <patch-set.json> --out <task-root> [--exe path] [--game-root path ...]";
  const options = {}, overrides = [];
  for (let i = 0; i < argv.length; i += 2) {
    const flag = argv[i], value = argv[i + 1];
    if (!["--task", "--source", "--patch", "--out", "--exe", "--game-root"].includes(flag) || !value || value.startsWith("--"))
      throw fail(`bad argument ${flag}\n${usage}`, EXIT.input);
    if (flag === "--game-root") overrides.push(resolve(value)); else options[flag.slice(2)] = resolve(value);
  }
  for (const key of ["task", "source", "patch", "out"]) if (!options[key]) throw fail(`missing --${key}\n${usage}`, EXIT.input);
  const exe = options.exe ?? join(repoRoot, "x64/Release/ParticleEditor.exe");
  if (!fs.existsSync(exe)) throw fail(`missing executable ${exe}; this script builds nothing`, EXIT.input);
  const roots = resolveReplayGameRoots({ overrides });
  if (!roots.length) throw fail("missing game data: host lookup found no usable MEG archive", EXIT.input);
  const parse = (path) => {
    try { return JSON.parse(fs.readFileSync(path, "utf8")); }
    catch (error) { throw fail(`cannot read ${path}: ${error.message}`, EXIT.input); }
  };
  const summary = await runLoop({ task: parse(options.task), source: options.source, patchSet: parse(options.patch),
    out: options.out, exe, roots });
  console.log(`[vfx-loop] completed ${options.out}: technical gate ${summary.technicalGate.pass ? "pass" : `FAIL (${summary.technicalGate.failed.join(", ")})`}`);
  return EXIT.ok;
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  try { process.exitCode = await main(); }
  catch (error) { console.error(`[vfx-loop] ${error.message}`); process.exitCode = error.exitCode ?? EXIT.step; }
}
