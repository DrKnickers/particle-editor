// subagent-run: a benchmark run whose agent is a Claude sub-agent, one fresh
// context per attempt. The confinement guarantee is replaced by detection:
// every attempt works in its own folder under a fixed tool set, and its
// transcript records every tool call with its arguments, so the audit can
// prove no attempt read, searched or wrote anything outside its folder.
// Two outcomes, as the prompt states them: a rule break that stays inside the
// folder (another tool on the attempt's own files) voids that attempt; an
// escape (a path outside, a relative path, a tool whose reach cannot be read
// from its arguments) fails the run's canary, and the report gives no verdict.
//
//   node scripts/vfx-benchmark/subagent-run.mjs --prepare <store> <run> --agent <id> --model <model> [--seed <n>]
//   (the session launches one sub-agent per attempt with the prompt in <run>/private/prompts.json)
//   node scripts/vfx-benchmark/subagent-run.mjs --audit <run> <transcripts-dir>
//   node scripts/vfx-benchmark/subagent-run.mjs --score <store> <run>
import fs from "node:fs";
import { randomBytes } from "node:crypto";
import { join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { checkStore, packExpected } from "./check-corpus.mjs";
import { contextFor } from "./context.mjs";
import { PACK_TIME, verifyPack } from "./pack.mjs";
import { attemptsOf, buildReport, markdown, verdictLine } from "./report.mjs";
import { checkFindings, RUBRIC } from "./schema-check.mjs";
import { scoreAttempt } from "./score.mjs";
import { prng, shuffle } from "./seed.mjs";
import { store } from "./store.mjs";

export const TOOLS = ["Read", "Glob", "Grep", "Write"];
const SHELLS = ["Bash", "PowerShell"];
const norm = (p) => resolve(p).replace(/\\/g, "/").toLowerCase();

export function attemptPrompt(dir) {
  const d = dir.replace(/\//g, "\\");
  return [
    "You are taking part in a scored benchmark. Work alone.",
    `Your folder is ${d}. Read ${d}\\pack\\prompt.md and follow it; the evidence is in ${d}\\pack.`,
    `Write your answer to ${d}\\out\\findings.json and nothing else.`,
    `Rules: use only the ${TOOLS.join(", ")} tools, and pass every one of them an absolute path inside ${d}`,
    "(Glob and Grep need an explicit path). Any other tool, or any path outside that folder, voids the attempt.",
    "When the answer file is written, reply with the single word: done.",
  ].join("\n");
}

// The scheduled attempts (attemptsOf): three per case in the primary condition;
// in the bundle-only condition, per secondary-eligible case and control, one
// (three for a draw-order case).
export function schedule(corpus) {
  const out = [];
  for (const c of corpus.cases) {
    for (let a = 1; a <= attemptsOf(corpus, c, "primary"); a++) out.push({ caseId: c.id, condition: "primary", attempt: a });
    if (c.kind === "control" ? c.shape !== "single" : !c.secondaryEligible) continue;
    for (let a = 1; a <= attemptsOf(corpus, c, "bundle-only"); a++) out.push({ caseId: c.id, condition: "bundle-only", attempt: a });
  }
  return out;
}

function copyPack(from, to, manifest, io) {
  for (const f of manifest.files) {
    const target = join(to, ...f.path.split("/"));
    io.mkdirSync(join(target, ".."), { recursive: true });
    io.copyFileSync(join(from, ...f.path.split("/")), target);
  }
  // Pack times are normalised; a copy must not carry the copy's own time.
  const walk = (dir) => {
    for (const e of io.readdirSync(dir, { withFileTypes: true })) {
      const p = join(dir, e.name);
      if (e.isDirectory()) walk(p);
      io.utimesSync(p, PACK_TIME, PACK_TIME);
    }
    io.utimesSync(dir, PACK_TIME, PACK_TIME);
  };
  walk(to);
}

export function prepare({ storeRoot, runRoot, agent, model, seed = 1, io = fs, ids = () => randomBytes(4).toString("hex") }) {
  const errors = checkStore(storeRoot, { io });
  if (errors.length) throw new Error(`check-corpus refused: ${errors.join("; ")}`);
  if (io.existsSync(runRoot)) throw new Error(`${runRoot} exists; a run starts fresh`);
  const s = store(storeRoot, io);
  const corpus = s.json("corpus.json");
  const attempts = shuffle(schedule(corpus), prng(seed)).map((a) => ({ ...a, id: ids() }));
  if (new Set(attempts.map((a) => a.id)).size !== attempts.length) throw new Error("attempt id collision; run again");
  const prompts = {};
  for (const a of attempts) {
    const folder = a.condition === "primary" ? "packs" : "packs-bundle-only";
    const manifest = s.json(folder, `${a.caseId}.manifest.json`);
    const dir = join(runRoot, "attempts", a.id);
    copyPack(s.at(folder, a.caseId), join(dir, "pack"), manifest, io);
    const problems = verifyPack(join(dir, "pack"), manifest, packExpected(join(dir, "pack"), a.condition === "primary", io), { io });
    if (problems.length) throw new Error(`${a.id}: the staged pack does not verify: ${problems.join("; ")}`);
    io.mkdirSync(join(dir, "out"), { recursive: true });
    prompts[a.id] = attemptPrompt(resolve(dir));
  }
  const run = { schemaVersion: 1, corpus: corpus.version, truth: corpus.hashes.truth, seed, backend: "claude-subagent",
    agents: [{ id: agent, model, imageCondition: "frames" }], contentRoots: "not staged", probes: {}, canary: "pending", tools: TOOLS };
  const p = store(runRoot, io);
  p.writeJson(["run.json"], run);
  p.writeJson(["private", "schedule.json"], attempts);
  p.writeJson(["private", "prompts.json"], prompts);
  return { run, attempts };
}

// Every tool call of one transcript against its attempt folder.
export function auditTranscript(lines, dir) {
  const root = `${norm(dir)}/`;
  const inside = (p) => typeof p === "string" && (norm(p) + "/").startsWith(root);
  const violations = [], escapes = [];
  let calls = 0;
  for (const line of lines) {
    const content = line.message?.content;
    if (line.type !== "assistant" || !Array.isArray(content)) continue;
    for (const use of content.filter((x) => x.type === "tool_use")) {
      calls++;
      const input = use.input ?? {};
      if (!TOOLS.includes(use.name)) {
        violations.push(`${use.name} is not an allowed tool`);
        // A shell command stays inside only if every path it names is absolute and inside, with no relative step.
        const text = SHELLS.includes(use.name) ? String(input.command ?? "") : null;
        const absolute = text === null ? [] : text.match(/[a-z]:[\\/][^"'\s;|&<>]*/gi) ?? [];
        if (text === null) escapes.push(`${use.name}: its reach cannot be read from its arguments`);
        else if (/\.\.|(^|[\s;&|])(cd|pushd|set-location)\b/i.test(text) || !absolute.length || !absolute.every(inside)) escapes.push(`${use.name} ${text.slice(0, 120)} reaches outside the attempt`);
        continue;
      }
      const paths = use.name === "Read" || use.name === "Write" ? [input.file_path] : [input.path];
      if (use.name === "Glob" && /^([a-z]:|[\\/])/i.test(input.pattern ?? "")) paths.push(input.pattern.replace(/[*?[{].*$/, ""));
      for (const p of paths) if (!inside(p)) escapes.push(`${use.name} ${p ?? "(no path: the session's folder)"} is outside the attempt`);
      if (use.name === "Write" && inside(input.file_path) && !norm(input.file_path).startsWith(`${root}out/`)) violations.push(`Write ${input.file_path} is outside out/`);
    }
  }
  return { calls, violations, escapes };
}

export function audit({ runRoot, transcriptsDir, io = fs }) {
  const p = store(runRoot, io);
  const attempts = p.json("private", "schedule.json");
  const byId = new Map();
  for (const name of io.readdirSync(transcriptsDir).filter((n) => n.endsWith(".jsonl"))) {
    const lines = io.readFileSync(join(transcriptsDir, name), "utf8").split("\n").filter(Boolean).map((l) => JSON.parse(l));
    const first = lines.find((l) => l.type === "user");
    const text = typeof first?.message?.content === "string" ? first.message.content : (first?.message?.content ?? []).map((c) => c.text ?? "").join("");
    const m = /attempts\\([0-9a-f]{8})\\pack\\prompt\.md/.exec(text);
    if (m) (byId.get(m[1]) ?? byId.set(m[1], []).get(m[1])).push({ name, lines });
  }
  const results = {};
  for (const a of attempts) {
    const found = byId.get(a.id) ?? [];
    if (found.length !== 1) { results[a.id] = { transcript: null, calls: 0, violations: [], escapes: found.length ? ["more than one transcript"] : [] }; continue; }
    results[a.id] = { transcript: found[0].name, ...auditTranscript(found[0].lines, join(runRoot, "attempts", a.id)) };
  }
  p.writeJson(["private", "audit.json"], results);
  return results;
}

export function score({ storeRoot, runRoot, io = fs }) {
  const s = store(storeRoot, io), p = store(runRoot, io);
  const corpus = s.json("corpus.json"), run = p.json("run.json");
  const attempts = p.json("private", "schedule.json");
  const auditResults = p.exists("private", "audit.json") ? p.json("private", "audit.json") : null;
  const textures = JSON.parse(s.text("packs", corpus.cases[0].id, "textures.json"));
  const scores = [], rows = [];
  for (const a of attempts) {
    const c = corpus.cases.find((x) => x.id === a.caseId);
    const file = join(runRoot, "attempts", a.id, "out", "findings.json");
    const checked = io.existsSync(file) ? checkFindings(io.readFileSync(file)) : { verdict: "missing", errors: ["no answer file"] };
    const voided = (auditResults?.[a.id]?.violations ?? []).length > 0;
    const findings = checked.verdict === "ok" && !voided ? checked.value.findings : null;
    const answers = s.json("cases", c.id, "answers.json")[run.agents[0].imageCondition].variants;
    const result = scoreAttempt({ caseInfo: c, answers, findings, context: contextFor(s, c, a.condition, textures, io), rubric: RUBRIC });
    scores.push({ agent: run.agents[0].id, condition: a.condition, caseId: c.id, attempt: a.attempt, score: findings ? result : null });
    rows.push({ id: a.id, caseId: c.id, class: c.class ?? "control", condition: a.condition, attempt: a.attempt, verdict: voided ? "voided" : checked.verdict, errors: checked.errors ?? [],
      identified: result.identified, diagnosisOnly: result.diagnosisOnly, flagged: result.flagged, fp: result.fp,
      units: result.units.map((u) => ({ label: u.label, category: u.category, direction: u.direction, errors: u.errors })), audit: auditResults?.[a.id] ?? null });
  }
  const audited = auditResults && attempts.every((a) => auditResults[a.id]?.transcript);
  const breached = auditResults && Object.values(auditResults).some((r) => r.escapes.length);
  const finalRun = { ...run, canary: !auditResults ? "missing" : breached ? "failed" : audited ? "passed" : "incomplete" };
  const report = buildReport(corpus, finalRun, scores);
  p.writeJson(["run.json"], finalRun);
  p.writeJson(["private", "scores.json"], rows);
  p.writeJson(["report.json"], report);
  p.write(["report.md"], markdown(report));
  return { report, rows, line: verdictLine(report, report.agents[0]) };
}

export function main(argv = process.argv.slice(2)) {
  const opt = (k) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : undefined; };
  try {
    if (argv[0] === "--prepare") {
      const { attempts } = prepare({ storeRoot: argv[1], runRoot: argv[2], agent: opt("--agent"), model: opt("--model"), seed: Number(opt("--seed") ?? 1) });
      console.log(`[bench] prepared ${attempts.length} attempts in ${argv[2]}`);
    } else if (argv[0] === "--audit") {
      const r = audit({ runRoot: argv[1], transcriptsDir: argv[2] });
      const bad = Object.entries(r).filter(([, v]) => v.violations.length || v.escapes.length || !v.transcript);
      const count = (f) => Object.values(r).filter(f).length;
      console.log(`[bench] audited ${Object.keys(r).length} attempts: ${count((v) => v.escapes.length)} escapes, ${count((v) => v.violations.length)} voided, ${count((v) => !v.transcript)} without a transcript`);
      for (const [id, v] of bad) console.log(`  ${id}: ${v.transcript ? [...v.escapes.map((e) => `ESCAPE ${e}`), ...v.violations].join("; ") : "no transcript"}`);
    } else if (argv[0] === "--score") {
      console.log(score({ storeRoot: argv[1], runRoot: argv[2] }).line);
    } else { console.error("usage: subagent-run.mjs --prepare <store> <run> --agent <id> --model <m> [--seed n] | --audit <run> <transcripts> | --score <store> <run>"); return 2; }
    return 0;
  } catch (error) { console.error(`[bench] refused: ${error.message}`); return 3; }
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) process.exitCode = main();
