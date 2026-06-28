#!/usr/bin/env node
import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const auditRoot = path.join(repoRoot, "tasks", "perf-audit-2026-06-28");
const manifestPath = path.join(auditRoot, "fixtures", "manifest.json");

const fixturePaths = [
  "tasks/perf-audit-2026-06-28/fixtures/drive/00-baseline.json",
  "tasks/perf-audit-2026-06-28/fixtures/drive/10-texture-preview.json",
  "tasks/perf-audit-2026-06-28/fixtures/drive/20-open-alo.json",
  "tasks/perf-audit-2026-06-28/fixtures/drive/30-layout-scene-rect-storm.json",
  "tasks/perf-audit-2026-06-28/fixtures/drive/40-frame-idle.json",
  "web/apps/editor/tests/fixtures/a11y-base-state.alo",
  "web/apps/editor/tests/fixtures/nt-5-singleton.alo",
];

function sha256(file) {
  return crypto.createHash("sha256").update(fs.readFileSync(file)).digest("hex");
}

function readJson(file) {
  try { return JSON.parse(fs.readFileSync(file, "utf8")); }
  catch { return null; }
}

const files = fixturePaths.map((rel) => {
  const abs = path.join(repoRoot, rel);
  const stat = fs.statSync(abs);
  const json = rel.endsWith(".json") ? readJson(abs) : null;
  return {
    path: rel.replaceAll("\\", "/"),
    bytes: stat.size,
    sha256: sha256(abs),
    requiredEvents: Array.isArray(json?.requiredEvents) ? json.requiredEvents : undefined,
  };
});

const manifest = {
  schemaVersion: 1,
  auditId: "perf-audit-2026-06-28",
  generatedBy: "scripts/write-perf-fixture-manifest.mjs",
  notes: [
    "Drive fixtures are deterministic in-repo workflows.",
    "10-texture-preview still resolves p_fire.dds through the active game/mod stack; release-grade texture-root pinning remains a proposed fix.",
  ],
  files,
};

fs.mkdirSync(path.dirname(manifestPath), { recursive: true });
fs.writeFileSync(manifestPath, JSON.stringify(manifest, null, 2) + "\n");
console.log(manifestPath);
