// [vfx-loop] The loop coordinator: observe a source
// effect, then play the author's rounds. Each round applies a bounded patch set
// through the exe's --tool verbs, observes the candidate under the same
// scenario, and compares it with the source (compare.mjs). The last accepted
// candidate goes through the four-seed review (four-seed.mjs), the run ends in
// one outcome, and the human promotion package is written (package.mjs).
//
//   node scripts/vfx-loop/loop.mjs --task <task.json> --source <effect.alo>
//     --patch <round-1.json> [--patch <round-2.json> ...] --out <task-root>
//     [--exe path] [--game-root path ...]
//
// The loop never writes the source: it works on a copy inside a fresh task
// root, and every step leaves its artifact there. It decides nothing about
// taste. What it ends with is evidence for a human.
import * as fs from "node:fs";
import { basename, dirname, isAbsolute, join, relative, resolve, win32 } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { observe, sha256, spawnWithDeadline } from "../observe.mjs";
import { resolveReplayGameRoots } from "../replay-proof.mjs";
import { METRICS, writeComparison } from "./compare.mjs";
import { checkVariationSeeds, fourSeedReview, variationSeeds } from "./four-seed.mjs";
import { writePackage } from "./package.mjs";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const serialise = (value) => JSON.stringify(value, null, 2) + "\n";
const fail = (message, exitCode = 1) => Object.assign(new Error(message), { exitCode });
const pad = (n) => String(n).padStart(2, "0");

// Run outcomes, in the order decideOutcome checks them.
export const OUTCOME = { tooling: "BLOCKED_BY_TOOLING", asset: "BLOCKED_BY_ASSET", technical: "FAILED_TECHNICAL_GATE",
  judgment: "NEEDS_HUMAN_JUDGMENT", pass: "PASS_FOR_HUMAN_REVIEW" };

// 0 the run reached an outcome through its stop conditions (read loop.json);
// 1 blocked by tooling (exe, ffmpeg, untrustworthy evidence); 2 usage or a
// malformed task; 3 blocked by an asset (source unreadable or invalid, an
// effect --observe cannot load); 7 the observations are misaligned; 13 task-root IO.
export const EXIT = { ok: 0, tooling: 1, input: 2, asset: 3, misaligned: 7, io: 13 };

// A hard stop: the run ends here with this outcome, whatever the rounds did.
const blocked = (outcome, message, exitCode) => Object.assign(fail(message, exitCode), { outcome });

// The loop carries findings into the task root for the human; it acts
// only on the requests.
const FINDING_KEYS = ["id", "severity", "category", "claim", "evidence", "suspectedEmitter", "suggestedChange", "confidence"];
export const MAX_ROUNDS = 3;

export function checkTask(task) {
  const problems = [];
  const object = (value) => value !== null && typeof value === "object" && !Array.isArray(value);
  if (!object(task) || task.schemaVersion !== 1) return ["task: schemaVersion must be 1"];
  if (typeof task.brief !== "string" || !task.brief.trim()) problems.push("task: brief must be a non-empty string");
  const observation = task.observation;
  if (!object(observation) || !object(observation.scenario) || !object(observation.lifecycle) || !object(observation.output))
    problems.push("task: observation needs scenario, lifecycle and output objects");
  else {
    if ("aloPath" in observation.scenario) problems.push("task: observation.scenario must not name aloPath; the loop sets it");
    const seed = observation.scenario.seed;
    if (!Number.isSafeInteger(seed) || seed < 0 || seed > 0xffffffff)
      problems.push("task: observation.scenario.seed must be a 32-bit unsigned integer (the four-seed review starts from it)");
    else problems.push(...checkVariationSeeds(task.variationSeeds, seed));
  }
  if (task.maxRounds !== undefined && !(Number.isInteger(task.maxRounds) && task.maxRounds >= 1 && task.maxRounds <= 10))
    problems.push("task: maxRounds must be an integer from 1 to 10");
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

// A patch set JSON cannot parse is still the author's round: it is rejected
// with the parse error, and the author's bytes are kept as written.
export const UNREADABLE = Symbol("unreadable");
export function parsePatchSet(text) {
  try { return JSON.parse(text); }
  catch (error) { return { [UNREADABLE]: { text, error: error.message } }; }
}

// Problems with one round's patch set. Any problem rejects the round: the
// author wrote it, so it is the author's to fix, not a reason to stop the run.
export function checkPatchSet(set, inputHash) {
  const object = (value) => value !== null && typeof value === "object" && !Array.isArray(value);
  if (set?.[UNREADABLE]) return [`patch set: not JSON (${set[UNREADABLE].error})`];
  if (!object(set) || set.schema !== 1) return ["patch set: schema must be 1"];
  if (!Array.isArray(set.requests) || set.requests.length === 0) return ["patch set: requests must be a non-empty array"];
  const problems = [], targets = new Set();
  for (const [i, request] of set.requests.entries()) {
    const selector = request?.selector;
    if (!object(request) || !object(selector) || !Array.isArray(selector.rolePath) || !Array.isArray(request.mutations)) {
      problems.push(`patch set: requests[${i}] needs a selector with a rolePath, and mutations`);
      continue;
    }
    // The chain rebinds later requests to intermediate hashes, so each must
    // still prove it was authored against this round's exact input.
    if (selector.sourceDocumentHash !== inputHash)
      problems.push(`patch set: requests[${i}] was authored against ${selector.sourceDocumentHash}, not this round's input ${inputHash}`);
    // A second request for one emitter would select it by a fingerprint the
    // first request may have changed. One request per emitter, then.
    const target = JSON.stringify(selector.rolePath);
    if (targets.has(target)) problems.push(`patch set: requests[${i}] targets ${target} again; put one emitter's edits in one request`);
    targets.add(target);
  }
  // null, like an absent key, means "none".
  if (set.findings != null) {
    if (!Array.isArray(set.findings)) problems.push("patch set: findings must be an array");
    else for (const [i, finding] of set.findings.entries())
      for (const key of FINDING_KEYS) if (!object(finding) || !(key in finding))
        problems.push(`patch set: findings[${i}] is missing ${key}`);
  }
  if (set.needsHumanJudgment != null && (typeof set.needsHumanJudgment !== "string" || !set.needsHumanJudgment.trim()))
    problems.push("patch set: needsHumanJudgment must be a non-empty reason");
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

// Picks the single OUTCOME for a run that reached its stop conditions.
export function decideOutcome({ accepted, stop, fourSeedPass }) {
  if (!accepted) return OUTCOME.technical;
  if (!fourSeedPass) return OUTCOME.technical;
  if (stop !== "author-finished") return OUTCOME.judgment;
  return OUTCOME.pass;
}

// --tool exits that mean "the author's request or candidate was refused"; any
// other nonzero exit is the tool failing, which blocks the run.
const REFUSALS = new Set([2, 3, 4, 5, 6]);

export async function runLoop({ task, source, patchSets, out, exe = join(repoRoot, "x64/Release/ParticleEditor.exe"),
  roots = [], run = spawnWithDeadline, io = fs, say = (line) => console.log(line) } = {}) {
  const taskProblems = checkTask(task);
  if (taskProblems.length) throw fail(taskProblems.join("; "), EXIT.input);
  if (!Array.isArray(patchSets) || patchSets.length === 0) throw fail("at least one patch set (round) is required", EXIT.input);
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
    if (result.code === 13) throw blocked(OUTCOME.tooling, `${label}: --tool IO failure: ${result.stderr}`, EXIT.io);
    if (result.code !== 0 && !REFUSALS.has(result.code))
      throw blocked(OUTCOME.tooling, `${label}: --tool exit ${result.code}: ${result.stderr}`, EXIT.tooling);
    return result.code;
  };
  const readJson = (path) => JSON.parse(io.readFileSync(path, "utf8"));
  const record = (value) => {
    const path = at("loop.json"), temp = at("loop.json.tmp");
    io.writeFileSync(temp, serialise(value)); io.renameSync(temp, path);
  };
  const name = basename(source);
  const seeds = variationSeeds(task.observation.scenario.seed, task.variationSeeds);
  const maxRounds = task.maxRounds ?? MAX_ROUNDS;
  const summary = { schemaVersion: 1, brief: task.brief, source: { path: resolve(source) }, rounds: [] };

  const spec = (aloPath, seed) => ({ schemaVersion: 1, scenario: { schemaVersion: 1, ...task.observation.scenario, seed, aloPath },
    lifecycle: task.observation.lifecycle, output: task.observation.output });
  // An effect --observe cannot load (exit 3) is a missing asset; any other
  // observation failure, or a bundle of the wrong bytes, is the tooling's.
  const observeRun = async (directoryName, aloPath, expectedHash, seed) => {
    const specPath = at(`${directoryName}.observation.json`);
    io.writeFileSync(specPath, serialise(spec(aloPath, seed)));
    const directory = dirAt(directoryName);
    try { await observe(specPath, directory, { exe, roots, run, io }); }
    catch (error) {
      if (error.exitCode === 3) throw blocked(OUTCOME.asset, `${directoryName}: --observe could not load the effect: ${error.message}`, EXIT.asset);
      throw blocked(OUTCOME.tooling, `${directoryName}: ${error.message}`, EXIT.tooling);
    }
    if (readJson(join(directory, "capture.json")).source.sha256 !== expectedHash)
      throw blocked(OUTCOME.tooling, `${directoryName} observed different bytes than it was given`, EXIT.tooling);
    return directory;
  };
  const compare = async (baselineDir, candidateDir, outName) => {
    try { return await writeComparison(baselineDir, candidateDir, dirAt(outName), { budgets: task.budgets, run, io }); }
    catch (error) {
      throw blocked(OUTCOME.tooling, `${outName}: ${error.message}`, error.exitCode === 7 ? EXIT.misaligned : EXIT.tooling);
    }
  };

  try {
    current = "source";
    let sourceBytes;
    try { sourceBytes = io.readFileSync(source); }
    catch (error) { throw blocked(OUTCOME.asset, `the source is unavailable: ${error.message}`, EXIT.asset); }
    const sourceHash = sha256(sourceBytes);
    summary.source.sha256 = sourceHash;
    // The --tool verbs warn when a file's name differs from the effect's
    // internal name, so the copy and every candidate keep the source's name.
    const copy = at(`source/${name}`);
    io.writeFileSync(copy, sourceBytes);
    log(current, { sha256: sourceHash, copy: `source/${name}` });

    current = "evidence";
    if (await tool(["inspect", copy, "--out", at("evidence/inspect.json")], "inspect") !== 0)
      throw blocked(OUTCOME.asset, "inspect refused the source", EXIT.asset);
    if (readJson(at("evidence/inspect.json")).sourceDocumentHash !== sourceHash)
      throw blocked(OUTCOME.tooling, "inspect hashed different source bytes", EXIT.tooling);
    const validated = await tool(["validate", copy, "--out", at("evidence/validate.json")], "validate source");
    if (validated !== 0) throw blocked(OUTCOME.asset, `the source is invalid (validate exit ${validated}); see evidence/validate.json`, EXIT.asset);
    log(current, { inspect: "evidence/inspect.json", validate: "evidence/validate.json" });

    current = "baseline";
    const baselineDir = await observeRun("observations/baseline", copy, sourceHash, seeds[0]);
    log(current, { bundle: "observations/baseline", seed: seeds[0] });

    // One round: the patch set's requests chained over this round's input. A
    // refusal anywhere rejects the round; a tool failure blocks the run.
    const playRound = async (r, patchSet, input, inputHash) => {
      const root = `rounds/${pad(r)}`, entry = { round: r, input: { sha256: inputHash }, accepted: false, reason: null };
      const reject = (reason) => Object.assign(entry, { reason });
      io.writeFileSync(at(`${root}/patch-set.json`), patchSet?.[UNREADABLE] ? patchSet[UNREADABLE].text : serialise(patchSet));
      entry.patchSet = `${root}/patch-set.json`;
      entry.findings = Array.isArray(patchSet?.findings) ? patchSet.findings.length : 0;
      const problems = checkPatchSet(patchSet, inputHash);
      if (problems.length) return reject(problems.join("; "));
      let path = input, hash = inputHash;
      for (const [i, original] of patchSet.requests.entries()) {
        const step = `${root}/steps/${pad(i + 1)}`;
        current = `round-${pad(r)}-patch-${pad(i + 1)}`;
        const request = structuredClone(original);
        request.selector.sourceDocumentHash = hash;
        const requestPath = at(`${step}/request.json`), output = at(`${step}/${name}`), reportPath = at(`${step}/report.json`);
        io.writeFileSync(requestPath, serialise(request));
        const code = await tool(["patch", path, requestPath, "--out", output, "--report", reportPath], current);
        const report = io.existsSync(reportPath) ? readJson(reportPath) : null;
        if (code !== 0) {
          const refusal = report?.refusal ? ` (${report.refusal.category}: ${report.refusal.reason})` : "";
          return reject(`request ${i + 1} refused with --tool exit ${code}${refusal}; see ${step}/report.json`);
        }
        const bytes = io.readFileSync(output);
        if (report?.outcome !== "ok" || report.candidateHash !== sha256(bytes) || !Object.values(report.checks ?? {}).every(Boolean))
          throw blocked(OUTCOME.tooling, `${current}: the patch report does not vouch for ${step}/${name}`, EXIT.tooling);
        path = output; hash = report.candidateHash;
        log(current, { emitter: report.selector?.name ?? "?", candidateHash: hash });
      }
      current = `round-${pad(r)}-candidate`;
      const candidate = at(`${root}/candidate/${name}`);
      io.copyFileSync(path, candidate);
      entry.candidate = { path: `${root}/candidate/${name}`, sha256: hash };
      const valid = await tool(["validate", candidate, "--out", at(`${root}/candidate/validate.json`)], current);
      if (valid !== 0) return reject(`the candidate is invalid (validate exit ${valid}); see ${root}/candidate/validate.json`);
      // The next round's author reads this dump to write selectors against it.
      if (await tool(["inspect", candidate, "--out", at(`${root}/candidate/inspect.json`)], current) !== 0)
        throw blocked(OUTCOME.tooling, `${current}: inspect refused a candidate that validated`, EXIT.tooling);
      current = `round-${pad(r)}-observe`;
      const observation = await observeRun(`${root}/observation`, candidate, hash, seeds[0]);
      current = `round-${pad(r)}-compare`;
      const comparison = await compare(baselineDir, observation, `${root}/comparison`);
      Object.assign(entry, { observation: `${root}/observation`, comparison: `${root}/comparison` });
      entry.budgets = comparison.metrics.budgets;
      entry.firstRandomDivergence = comparison.schedule.firstDivergentFrame;
      entry.pairedClip = comparison.clip ? `${root}/comparison/${comparison.clip.path}` : null;
      if (!comparison.metrics.budgetsPass)
        return reject(`budget failed on the primary seed: ${comparison.metrics.budgets.filter((v) => !v.pass).map((v) => v.id).join(", ")}`);
      Object.assign(entry, { accepted: true, reason: "validated, every request applied, every budget held on the primary seed" });
      entry.paths = { candidate, observation };
      entry.result = comparison;
      return entry;
    };

    // Rounds: each builds on the last accepted candidate; two
    // consecutive rejections, maxRounds, or the author's flag stop the run.
    let input = copy, inputHash = sourceHash, last = null, rejectedInRow = 0, stop = null;
    for (const [i, patchSet] of patchSets.slice(0, maxRounds).entries()) {
      const r = i + 1;
      const result = await playRound(r, patchSet, input, inputHash);
      const { paths, result: comparison, ...kept } = result;
      summary.rounds.push(kept);
      log(`round-${pad(r)}`, { accepted: result.accepted, reason: JSON.stringify(result.reason) });
      if (result.accepted) {
        last = result; rejectedInRow = 0;
        input = paths.candidate; inputHash = result.candidate.sha256;
        if (patchSet.needsHumanJudgment) { stop = { condition: "author-flagged", detail: patchSet.needsHumanJudgment }; break; }
      } else if (++rejectedInRow === 2) { stop = { condition: "two-rejections", detail: `rounds ${r - 1} and ${r} were rejected` }; break; }
    }
    if (!stop) stop = patchSets.length > maxRounds
      ? { condition: "max-rounds", detail: `maxRounds ${maxRounds} reached with ${patchSets.length - maxRounds} round(s) unplayed` }
      : { condition: "author-finished", detail: `the author supplied ${patchSets.length} round(s)` };
    summary.stop = stop;

    let fourSeedPass = false;
    if (last) {
      current = "final";
      const final = at(`candidate/${name}`);
      io.copyFileSync(last.paths.candidate, final);
      summary.candidate = { path: `candidate/${name}`, sha256: last.candidate.sha256, round: last.round };
      log(current, { round: last.round, sha256: last.candidate.sha256 });

      current = "four-seed";
      const matrix = await fourSeedReview({ seeds,
        primary: { baseline: "observations/baseline", candidate: last.observation, comparison: last.comparison,
          metrics: last.result.metrics, schedule: last.result.schedule, summaries: last.result.summaries },
        observePair: async (seed, root) => ({
          baseline: await observeRun(`${root}/baseline`, copy, sourceHash, seed),
          candidate: await observeRun(`${root}/candidate`, final, last.candidate.sha256, seed) }),
        compare: (baselineDir, candidateDir, outName) => compare(baselineDir, candidateDir, outName),
        writeJson: (rel, text) => io.writeFileSync(at(rel), text), say });
      fourSeedPass = matrix.allPass;
      summary.fourSeed = { matrix: "four-seed/matrix.json", allPass: matrix.allPass,
        failedSeeds: matrix.seeds.filter((v) => !v.budgetsPass).map((v) => v.seed) };
      log(current, { allPass: matrix.allPass });
    }

    current = "source-check";
    for (const [label, path] of [["source", source], ["source copy", copy]])
      if (sha256(io.readFileSync(path)) !== sourceHash) throw blocked(OUTCOME.tooling, `the ${label} changed during the loop`, EXIT.tooling);
    log(current, { unchanged: true });

    summary.outcome = decideOutcome({ accepted: !!last, stop: stop.condition, fourSeedPass });
    summary.failure = null;
    summary.steps = steps;
    current = "package";
    summary.package = writePackage(out, summary, { io, at });
    record(summary);
    say(`[vfx-loop] outcome=${summary.outcome} stop=${stop.condition}`);
    return summary;
  } catch (error) {
    const exitCode = error.exitCode ?? EXIT.tooling;
    summary.outcome = error.outcome ?? OUTCOME.tooling;
    summary.failure = { step: current, exitCode, message: error.message };
    summary.steps = steps;
    try { summary.package = writePackage(out, summary, { io, at }); } catch { /* the original failure is the one to report */ }
    try { record(summary); } catch { /* as above */ }
    throw Object.assign(error, { exitCode, outcome: summary.outcome });
  }
}

export async function main(argv = process.argv.slice(2)) {
  const usage = "usage: node scripts/vfx-loop/loop.mjs --task <task.json> --source <effect.alo> --patch <round.json> [--patch <round.json> ...] --out <task-root> [--exe path] [--game-root path ...]";
  const options = {}, overrides = [], patches = [];
  for (let i = 0; i < argv.length; i += 2) {
    const flag = argv[i], value = argv[i + 1];
    if (!["--task", "--source", "--patch", "--out", "--exe", "--game-root"].includes(flag) || !value || value.startsWith("--"))
      throw fail(`bad argument ${flag}\n${usage}`, EXIT.input);
    if (flag === "--game-root") overrides.push(resolve(value));
    else if (flag === "--patch") patches.push(resolve(value));
    else options[flag.slice(2)] = resolve(value);
  }
  for (const key of ["task", "source", "out"]) if (!options[key]) throw fail(`missing --${key}\n${usage}`, EXIT.input);
  if (!patches.length) throw fail(`missing --patch\n${usage}`, EXIT.input);
  const exe = options.exe ?? join(repoRoot, "x64/Release/ParticleEditor.exe");
  if (!fs.existsSync(exe)) throw fail(`missing executable ${exe}; this script builds nothing`, EXIT.input);
  const roots = resolveReplayGameRoots({ overrides });
  if (!roots.length) throw fail("missing game data: host lookup found no usable MEG archive", EXIT.input);
  const parse = (path) => {
    try { return JSON.parse(fs.readFileSync(path, "utf8")); }
    catch (error) { throw fail(`cannot read ${path}: ${error.message}`, EXIT.input); }
  };
  const readPatchSet = (path) => {
    try { return parsePatchSet(fs.readFileSync(path, "utf8")); }
    catch (error) { throw fail(`cannot read ${path}: ${error.message}`, EXIT.input); }
  };
  const summary = await runLoop({ task: parse(options.task), source: options.source, patchSets: patches.map(readPatchSet),
    out: options.out, exe, roots });
  console.log(`[vfx-loop] completed ${options.out}: ${summary.outcome} (${summary.stop.condition}); see package/README.md`);
  return EXIT.ok;
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  try { process.exitCode = await main(); }
  catch (error) { console.error(`[vfx-loop] ${error.outcome ?? ""} ${error.message}`.trim()); process.exitCode = error.exitCode ?? EXIT.tooling; }
}
