#!/usr/bin/env node
import fs from "node:fs";
import path from "node:path";

function usage() {
  console.error("usage: node scripts/analyze-perf-trace.mjs <trace.ndjson> [--out <dir>]");
  process.exit(2);
}

const args = process.argv.slice(2);
if (args.length < 1) usage();
const tracePath = args[0];
let outDir = path.dirname(path.resolve(tracePath));
const requiredEvents = [];
for (let i = 1; i < args.length; i++) {
  if (args[i] === "--out" && i + 1 < args.length) {
    outDir = args[++i];
  } else if (args[i] === "--require-event" && i + 1 < args.length) {
    const raw = args[++i];
    const [eventName, minDurationsRaw] = raw.split(":");
    const minDurations = minDurationsRaw === undefined ? 0 : Number(minDurationsRaw);
    if (!eventName || !Number.isFinite(minDurations) || minDurations < 0) usage();
    requiredEvents.push({ eventName, minDurations });
  } else {
    usage();
  }
}

const raw = fs.readFileSync(tracePath, "utf8");
const events = [];
const errors = [];
raw.split(/\r?\n/).forEach((line, index) => {
  if (index === 0 && line.charCodeAt(0) === 0xfeff) line = line.slice(1);
  if (!line.trim()) return;
  try {
    const event = JSON.parse(line);
    events.push({ event, lineNumber: index + 1 });
    if (event.schemaVersion !== 1) errors.push(`line ${index + 1}: schemaVersion != 1`);
    if (!event.eventName) errors.push(`line ${index + 1}: missing eventName`);
    if (!event.eventType) errors.push(`line ${index + 1}: missing eventType`);
    if (event.source !== "codex-perf-audit") errors.push(`line ${index + 1}: source != codex-perf-audit`);
  } catch (err) {
    errors.push(`line ${index + 1}: ${err.message}`);
  }
});
if (events.length === 0) {
  errors.push("trace contains no events");
}

const spanStarts = new Map();
const spanEnds = new Map();
function spanKey(event) {
  return `${event.eventName}\u0000${event.spanId}`;
}
function hasString(value) {
  return typeof value === "string" && value.length > 0;
}

events.forEach(({ event, lineNumber }) => {
  const line = lineNumber;
  if (event.eventType === "span_start" || event.eventType === "span_end") {
    if (!hasString(event.spanId)) {
      errors.push(`line ${line}: ${event.eventType} missing spanId`);
    } else {
      const key = spanKey(event);
      const map = event.eventType === "span_start" ? spanStarts : spanEnds;
      if (map.has(key)) errors.push(`line ${line}: duplicate ${event.eventType} for ${event.eventName}/${event.spanId}`);
      map.set(key, line);
    }
  }
  if (event.eventType === "span_end") {
    if (typeof event.durationMs !== "number" || !Number.isFinite(event.durationMs)) {
      errors.push(`line ${line}: span_end missing numeric durationMs`);
    } else if (event.durationMs < 0) {
      errors.push(`line ${line}: negative durationMs`);
    }
    if (typeof event.startQpc === "number" && typeof event.endQpc === "number" && event.endQpc < event.startQpc) {
      errors.push(`line ${line}: endQpc before startQpc`);
    }
  }
  if ((event.eventName === "bridge.dispatch" || event.eventName === "renderer.bridge.request") &&
      !hasString(event.requestId)) {
    errors.push(`line ${line}: ${event.eventName} missing requestId`);
  }
});

for (const [key, line] of spanStarts.entries()) {
  if (!spanEnds.has(key)) errors.push(`line ${line}: unmatched span_start`);
}
for (const [key, line] of spanEnds.entries()) {
  if (!spanStarts.has(key)) errors.push(`line ${line}: unmatched span_end`);
}

const byEvent = new Map();
for (const { event } of events) {
  const name = String(event.eventName ?? "(missing)");
  let row = byEvent.get(name);
  if (!row) {
    row = { count: 0, durations: [], spanEndDurations: [], statuses: new Map() };
    byEvent.set(name, row);
  }
  row.count++;
  if (typeof event.durationMs === "number" && Number.isFinite(event.durationMs)) {
    row.durations.push(event.durationMs);
    if (event.eventType === "span_end") row.spanEndDurations.push(event.durationMs);
  }
  const status = String(event.status ?? event.eventType ?? "unknown");
  row.statuses.set(status, (row.statuses.get(status) ?? 0) + 1);
}

function pct(values, p) {
  if (!values.length) return 0;
  const sorted = [...values].sort((a, b) => a - b);
  const idx = Math.min(sorted.length - 1, Math.max(0, Math.ceil((p / 100) * sorted.length) - 1));
  return sorted[idx];
}

function sum(values) {
  return values.reduce((acc, v) => acc + v, 0);
}

const rows = [...byEvent.entries()].map(([name, row]) => {
  const totalMs = sum(row.durations);
  return {
    eventName: name,
    count: row.count,
    durationCount: row.durations.length,
    totalMs,
    avgMs: row.durations.length ? totalMs / row.durations.length : 0,
    p50Ms: pct(row.durations, 50),
    p95Ms: pct(row.durations, 95),
    maxMs: row.durations.length ? Math.max(...row.durations) : 0,
    statuses: [...row.statuses.entries()].map(([k, v]) => `${k}:${v}`).join(" "),
  };
}).sort((a, b) => b.totalMs - a.totalMs || b.maxMs - a.maxMs || b.count - a.count);

for (const req of requiredEvents) {
  const row = byEvent.get(req.eventName);
  if (!row) {
    errors.push(`required event missing: ${req.eventName}`);
    continue;
  }
  if (row.spanEndDurations.length < req.minDurations) {
    errors.push(`required event ${req.eventName} has ${row.spanEndDurations.length} span_end duration events; expected at least ${req.minDurations}`);
  }
}

fs.mkdirSync(outDir, { recursive: true });
const csvPath = path.join(outDir, "perf-summary.csv");
const summaryJsonPath = path.join(outDir, "perf-summary.json");
const mdPath = path.join(outDir, "perf-summary.md");
const frameJsonPath = path.join(outDir, "frame-summary.json");
const frameMdPath = path.join(outDir, "frame-summary.md");

const csvHeader = "eventName,count,durationCount,totalMs,avgMs,p50Ms,p95Ms,maxMs,statuses\n";
const csv = csvHeader + rows.map((r) => [
  r.eventName,
  r.count,
  r.durationCount,
  r.totalMs.toFixed(3),
  r.avgMs.toFixed(3),
  r.p50Ms.toFixed(3),
  r.p95Ms.toFixed(3),
  r.maxMs.toFixed(3),
  r.statuses,
].map((v) => `"${String(v).replace(/"/g, '""')}"`).join(",")).join("\n") + "\n";
fs.writeFileSync(csvPath, csv);
fs.writeFileSync(summaryJsonPath, JSON.stringify({ trace: path.resolve(tracePath), eventCount: events.length, rows }, null, 2) + "\n");

const frameEvents = events
  .map(({ event }) => event)
  .filter((event) => event.eventName === "engine.frame_summary");

function nums(field) {
  return frameEvents
    .map((event) => event[field])
    .filter((value) => typeof value === "number" && Number.isFinite(value));
}

if (frameEvents.length) {
  const avgFrameMs = nums("avgFrameMs");
  const maxFrameMs = nums("maxFrameMs");
  const workingSetBytes = nums("workingSetBytes");
  const privateUsageBytes = nums("privateUsageBytes");
  const frameSummary = {
    windows: frameEvents.length,
    frameCount: sum(nums("frameCount")),
    over16Ms: sum(nums("over16Ms")),
    over33Ms: sum(nums("over33Ms")),
    over50Ms: sum(nums("over50Ms")),
    avgFrameMs: avgFrameMs.length ? sum(avgFrameMs) / avgFrameMs.length : 0,
    p95AvgFrameMs: pct(avgFrameMs, 95),
    maxFrameMs: maxFrameMs.length ? Math.max(...maxFrameMs) : 0,
    maxGpuWaitMs: nums("maxGpuWaitMs").length ? Math.max(...nums("maxGpuWaitMs")) : 0,
    maxRenderMs: nums("maxRenderMs").length ? Math.max(...nums("maxRenderMs")) : 0,
    maxCompositeMs: nums("maxCompositeMs").length ? Math.max(...nums("maxCompositeMs")) : 0,
    maxWorkingSetBytes: workingSetBytes.length ? Math.max(...workingSetBytes) : 0,
    maxPrivateUsageBytes: privateUsageBytes.length ? Math.max(...privateUsageBytes) : 0,
  };
  fs.writeFileSync(frameJsonPath, JSON.stringify(frameSummary, null, 2) + "\n");
  fs.writeFileSync(frameMdPath, [
    "# Frame Summary",
    "",
    `Windows: ${frameSummary.windows}`,
    `Frames: ${frameSummary.frameCount}`,
    `Avg frame ms: ${frameSummary.avgFrameMs.toFixed(3)}`,
    `P95 avg-window frame ms: ${frameSummary.p95AvgFrameMs.toFixed(3)}`,
    `Max frame ms: ${frameSummary.maxFrameMs.toFixed(3)}`,
    `Frames over 16.7 ms: ${frameSummary.over16Ms}`,
    `Frames over 33.3 ms: ${frameSummary.over33Ms}`,
    `Frames over 50 ms: ${frameSummary.over50Ms}`,
    `Max GPU wait ms: ${frameSummary.maxGpuWaitMs.toFixed(3)}`,
    `Max render ms: ${frameSummary.maxRenderMs.toFixed(3)}`,
    `Max composite ms: ${frameSummary.maxCompositeMs.toFixed(3)}`,
    `Max working set bytes: ${frameSummary.maxWorkingSetBytes}`,
    `Max private usage bytes: ${frameSummary.maxPrivateUsageBytes}`,
    "",
  ].join("\n"));
}

const md = [
  "# Perf Trace Summary",
  "",
  `Trace: \`${path.resolve(tracePath)}\``,
  `Events: ${events.length}`,
  `Validation errors: ${errors.length}`,
  "",
  "| Event | Count | Durations | Total ms | Avg ms | P50 | P95 | Max | Statuses |",
  "|---|---:|---:|---:|---:|---:|---:|---:|---|",
  ...rows.slice(0, 80).map((r) =>
    `| \`${r.eventName}\` | ${r.count} | ${r.durationCount} | ${r.totalMs.toFixed(1)} | ${r.avgMs.toFixed(1)} | ${r.p50Ms.toFixed(1)} | ${r.p95Ms.toFixed(1)} | ${r.maxMs.toFixed(1)} | ${r.statuses || ""} |`
  ),
  "",
  ...(errors.length ? ["## Validation Errors", "", ...errors.slice(0, 200).map((e) => `- ${e}`), ""] : []),
].join("\n");
fs.writeFileSync(mdPath, md);

console.log(`events=${events.length}`);
console.log(`summary=${mdPath}`);
console.log(`csv=${csvPath}`);
console.log(`json=${summaryJsonPath}`);
if (frameEvents.length) console.log(`frameSummary=${frameMdPath}`);
if (errors.length) {
  console.error(`validationErrors=${errors.length}`);
  process.exit(1);
}
