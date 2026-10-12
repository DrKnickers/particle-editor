// [vfx-loop] The human promotion package (design §25, plan §7.1 H): one folder
// holding the facts a person needs to approve or reject the candidate.
//
//   package/package.json   the facts, machine-readable
//   package/README.md      the same facts for a person, linking into the task root
//
// It is a folder, not an action. Nothing promotes; the decision starts empty.
import * as fs from "node:fs";
import { join } from "node:path";

const serialise = (value) => JSON.stringify(value, null, 2) + "\n";

const ADVISORY = "Evidence for a human decision. No metric ranks the two effects (design §13.3); the loop checked only " +
  "explicit budgets and technical gates. Nothing has been promoted: the source is untouched.";
const LIMITATION = "Budgets are whole-effect: per-emitter shifts are reported in the telemetry delta but never budgeted, " +
  "so a change one emitter's timing hides from the whole effect (a shortened tail another emitter outlives) passes every budget.";

export function buildPackage(summary, matrix = null) {
  const last = summary.rounds?.find((v) => v.round === summary.candidate?.round) ?? null;
  const evidence = last && { pairedFilmstrip: `${last.comparison}/paired-filmstrip.png`, framePairs: `${last.comparison}/selected-frame-pairs`,
    metrics: `${last.comparison}/metrics.json`, telemetryDelta: `${last.comparison}/telemetry-delta.json`,
    randomSchedule: `${last.comparison}/random-schedule-note.json`, review: `${last.comparison}/review.json` };
  return { schemaVersion: 1, advisory: ADVISORY, brief: summary.brief, outcome: summary.outcome, stop: summary.stop ?? null,
    failure: summary.failure ?? null, source: summary.source, candidate: summary.candidate ?? null,
    rounds: (summary.rounds ?? []).map((v) => ({ round: v.round, accepted: v.accepted, reason: v.reason, patchSet: v.patchSet,
      findings: v.findings, candidate: v.candidate ?? null, comparison: v.comparison ?? null })),
    evidence, budgets: last?.budgets ?? null,
    fourSeed: matrix && { matrix: summary.fourSeed.matrix, allPass: matrix.allPass,
      seeds: matrix.seeds.map((v) => ({ seed: v.seed, primary: v.primary, budgetsPass: v.budgetsPass, failed: v.failed,
        peakParticles: v.peakParticles, firstRandomDivergence: v.firstRandomDivergence, filmstrip: v.filmstrip })) },
    limitations: [LIMITATION],
    decision: { choice: null, by: null, at: null, note: null } };
}

// Paths in package.json are task-root-relative; README links climb out of package/.
const link = (path) => `[${path}](../${path})`;

export function renderReadme(pkg) {
  const lines = ["# Promotion package", "", `> ${pkg.advisory}`, "", "## Outcome", "",
    `**${pkg.outcome}**${pkg.stop ? ` — stopped: ${pkg.stop.condition} (${pkg.stop.detail})` : ""}`];
  if (pkg.failure) lines.push("", `The run was blocked at \`${pkg.failure.step}\` (exit ${pkg.failure.exitCode}): ${pkg.failure.message}`);
  lines.push("", "## Brief", "", pkg.brief, "", "## Source and candidate", "",
    `- Source: \`${pkg.source.sha256 ?? "unread"}\` (${pkg.source.path})`,
    pkg.candidate ? `- Candidate: \`${pkg.candidate.sha256}\`, from round ${pkg.candidate.round}: ${link(pkg.candidate.path)}`
      : "- Candidate: none (no round was accepted)");
  if (pkg.rounds.length) {
    lines.push("", "## Rounds", "", "| Round | Accepted | Reason | Patch set | Findings |", "|---|---|---|---|---|");
    for (const v of pkg.rounds)
      lines.push(`| ${v.round} | ${v.accepted ? "yes" : "no"} | ${String(v.reason).replaceAll("|", "\\|")} | ${link(v.patchSet)} | ${v.findings} |`);
  }
  if (pkg.evidence) {
    lines.push("", "## Before and after (primary seed)", "", `- Paired filmstrip (baseline above candidate): ${link(pkg.evidence.pairedFilmstrip)}`,
      `- Frame pairs: ${link(pkg.evidence.framePairs)}`, `- Budgets and drift flags: ${link(pkg.evidence.metrics)}`,
      `- Telemetry delta: ${link(pkg.evidence.telemetryDelta)}`, `- Random schedule: ${link(pkg.evidence.randomSchedule)}`);
  }
  if (pkg.budgets) {
    lines.push("", "## Budgets (primary seed)", "", "| Budget | Baseline | Candidate | Delta | Max | Pass |", "|---|---|---|---|---|---|");
    for (const v of pkg.budgets) lines.push(`| ${v.id} | ${v.baseline} | ${v.candidate} | ${v.delta} | ${v.maxDelta} | ${v.pass ? "yes" : "**no**"} |`);
  }
  if (pkg.fourSeed) {
    lines.push("", "## Four-seed review", "", `Every budget on every seed: **${pkg.fourSeed.allPass ? "held" : "broken"}** (${link(pkg.fourSeed.matrix)}).`, "",
      "| Seed | Peak particles (before → after) | Budgets | First random divergence | Filmstrip |", "|---|---|---|---|---|");
    for (const v of pkg.fourSeed.seeds)
      lines.push(`| ${v.seed}${v.primary ? " (task)" : ""} | ${v.peakParticles[0]} → ${v.peakParticles[1]} | ${v.budgetsPass ? "held" : `**${v.failed.join(", ")}**`} | ${v.firstRandomDivergence ?? "none"} | ${link(v.filmstrip)} |`);
  }
  lines.push("", "## Limitations", "", ...pkg.limitations.map((v) => `- ${v}`), "", "## Decision", "",
    "Not decided. Record the choice (approve or reject), who made it, when, and why in `decision` in package.json.",
    "To promote, copy the candidate yourself; the loop never does.", "");
  return lines.join("\n");
}

export function writePackage(root, summary, { io = fs, at = (name) => join(root, name) } = {}) {
  let matrix = null;
  if (summary.fourSeed?.matrix) matrix = JSON.parse(io.readFileSync(at(summary.fourSeed.matrix), "utf8"));
  const pkg = buildPackage(summary, matrix);
  io.writeFileSync(at("package/package.json"), serialise(pkg));
  io.writeFileSync(at("package/README.md"), renderReadme(pkg));
  return { json: "package/package.json", readme: "package/README.md" };
}
