#!/usr/bin/env node
import fs from "node:fs";
import path from "node:path";

function usage() {
  console.error("usage: node scripts/summarize-perf-audit-runs.mjs <run-root> [--out <file.md>]");
  process.exit(2);
}

const args = process.argv.slice(2);
if (args.length < 1) usage();
const root = path.resolve(args[0]);
let outPath = path.join(root, "group-summary.md");
for (let i = 1; i < args.length; i++) {
  if (args[i] === "--out" && i + 1 < args.length) outPath = path.resolve(args[++i]);
  else usage();
}

function readJson(file) {
  try { return JSON.parse(fs.readFileSync(file, "utf8").replace(/^\uFEFF/, "")); }
  catch { return null; }
}

function walk(dir, out = []) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) walk(full, out);
    else if (entry.isFile() && entry.name === "perf-summary.json") out.push(full);
  }
  return out;
}

function pct(values, p) {
  if (!values.length) return 0;
  const sorted = [...values].sort((a, b) => a - b);
  const idx = Math.min(sorted.length - 1, Math.max(0, Math.ceil((p / 100) * sorted.length) - 1));
  return sorted[idx];
}

function mean(values) {
  return values.length ? values.reduce((acc, v) => acc + v, 0) / values.length : 0;
}

function stddev(values) {
  if (values.length < 2) return 0;
  const m = mean(values);
  return Math.sqrt(values.reduce((acc, v) => acc + Math.pow(v - m, 2), 0) / (values.length - 1));
}

const groups = new Map();
for (const summaryPath of walk(root)) {
  const runDir = path.dirname(summaryPath);
  const summary = readJson(summaryPath);
  const metadata = readJson(path.join(runDir, "run-metadata.json"));
  const frame = readJson(path.join(runDir, "frame-summary.json"));
  if (!summary || !metadata) continue;
  const fixture = path.basename(metadata.fixture?.path ?? runDir, ".json");
  const profile = metadata.profile?.reused ? "same-profile" : "clean-profile";
  const key = `${fixture}\u0000${profile}`;
  if (!groups.has(key)) groups.set(key, { fixture, profile, samples: [] });
  groups.get(key).samples.push({ runDir, summary, frame });
}

const lines = [
  "# Perf Audit Group Summary",
  "",
  `Run root: \`${root}\``,
  `Trace groups: ${groups.size}`,
  "",
];

for (const group of [...groups.values()].sort((a, b) => a.fixture.localeCompare(b.fixture) || a.profile.localeCompare(b.profile))) {
  lines.push(`## ${group.fixture} (${group.profile})`, "");
  lines.push(`Samples: ${group.samples.length}`, "");
  const eventNames = new Set();
  for (const sample of group.samples) {
    for (const row of sample.summary.rows ?? []) eventNames.add(row.eventName);
  }
  lines.push("| Event | n | total p50 | total p95 | max | stddev total |");
  lines.push("|---|---:|---:|---:|---:|---:|");
  for (const eventName of [...eventNames].sort()) {
    const totals = [];
    const maxes = [];
    for (const sample of group.samples) {
      const row = (sample.summary.rows ?? []).find((r) => r.eventName === eventName);
      if (!row) continue;
      if (Number.isFinite(row.totalMs)) totals.push(row.totalMs);
      if (Number.isFinite(row.maxMs)) maxes.push(row.maxMs);
    }
    if (!totals.length) continue;
    lines.push(`| \`${eventName}\` | ${totals.length} | ${pct(totals, 50).toFixed(1)} | ${pct(totals, 95).toFixed(1)} | ${Math.max(...maxes).toFixed(1)} | ${stddev(totals).toFixed(1)} |`);
  }
  const frameSamples = group.samples.map((s) => s.frame).filter(Boolean);
  if (frameSamples.length) {
    const maxFrameMs = frameSamples.map((f) => f.maxFrameMs).filter(Number.isFinite);
    const over16 = frameSamples.map((f) => f.over16Ms).filter(Number.isFinite);
    const privateUsage = frameSamples.map((f) => f.maxPrivateUsageBytes).filter(Number.isFinite);
    lines.push("", "| Frame Metric | p50 | p95 | max |");
    lines.push("|---|---:|---:|---:|");
    lines.push(`| max frame ms | ${pct(maxFrameMs, 50).toFixed(1)} | ${pct(maxFrameMs, 95).toFixed(1)} | ${Math.max(...maxFrameMs).toFixed(1)} |`);
    lines.push(`| frames over 16.7 ms | ${pct(over16, 50).toFixed(0)} | ${pct(over16, 95).toFixed(0)} | ${Math.max(...over16).toFixed(0)} |`);
    lines.push(`| max private bytes | ${pct(privateUsage, 50).toFixed(0)} | ${pct(privateUsage, 95).toFixed(0)} | ${Math.max(...privateUsage).toFixed(0)} |`);
  }
  lines.push("");
}

fs.mkdirSync(path.dirname(outPath), { recursive: true });
fs.writeFileSync(outPath, lines.join("\n"));
console.log(`summary=${outPath}`);
