// The corpus store's I/O (plan 3.1): paths, JSON files, hashes, and the three
// exe calls the corpus needs (inspect, patch, observe). Every exe call goes
// through `run(bin, args)` so tests substitute a fake, and every observation
// is bracketed by a hash of the executable (plan 3.3: a version string cannot
// tell two builds apart).
import fs from "node:fs";
import { createHash } from "node:crypto";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { observe, spawnWithDeadline } from "../observe.mjs";

export const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
export const DEFAULT_EXE = join(repoRoot, "x64", "Release", "ParticleEditor.exe");
export const sha256 = (bytes) => createHash("sha256").update(bytes).digest("hex");

export const serialise = (value) => `${JSON.stringify(value, null, 2)}\n`;

export function store(root, io = fs) {
  const at = (...parts) => join(root, ...parts);
  return {
    root, at,
    exists: (...parts) => io.existsSync(at(...parts)),
    read: (...parts) => io.readFileSync(at(...parts)),
    text: (...parts) => io.readFileSync(at(...parts), "utf8"),
    json: (...parts) => JSON.parse(io.readFileSync(at(...parts), "utf8")),
    write(parts, bytes) {
      const path = at(...[].concat(parts));
      io.mkdirSync(dirname(path), { recursive: true });
      const temp = `${path}.tmp`;
      io.writeFileSync(temp, bytes);
      io.renameSync(temp, path);
      return path;
    },
    writeJson(parts, value) { return this.write(parts, serialise(value)); },
    hash: (...parts) => sha256(io.readFileSync(at(...parts))),
    list: (...parts) => (io.existsSync(at(...parts)) ? io.readdirSync(at(...parts), { withFileTypes: true }).filter((e) => e.isDirectory()).map((e) => e.name).sort() : []),
  };
}

// The executable, hashed before and after every generation.
export function exeLedger(exe, io = fs) {
  const entries = [];
  const hash = () => sha256(io.readFileSync(exe));
  const first = hash();
  return {
    sha256: first,
    entries,
    async around(label, action) {
      const before = hash();
      if (before !== first) throw new Error(`${label}: the executable changed (${before} != ${first})`);
      const result = await action();
      const after = hash();
      if (after !== first) throw new Error(`${label}: the executable changed during the run`);
      entries.push({ label, before, after });
      return result;
    },
  };
}

function toolError(verb, result) {
  const error = new Error(`--tool ${verb} exit ${result.code}: ${result.stderr}`);
  error.exitCode = result.code;
  return error;
}

export function tools({ exe = DEFAULT_EXE, run = spawnWithDeadline, roots = [], io = fs } = {}) {
  return {
    async inspect(alo, out) {
      const result = await run(exe, ["--tool", "inspect", alo, "--out", out], { timeoutMs: 120000 });
      if (result.code !== 0) throw toolError("inspect", result);
      return JSON.parse(io.readFileSync(out, "utf8"));
    },
    async patch(alo, requestPath, out, reportPath) {
      const result = await run(exe, ["--tool", "patch", alo, requestPath, "--out", out, "--report", reportPath], { timeoutMs: 120000 });
      if (result.code !== 0) throw toolError("patch", result);
      return JSON.parse(io.readFileSync(reportPath, "utf8"));
    },
    async observe(specPath, directory) {
      await observe(specPath, directory, { repeat: true, exe, roots, run, io });
      return JSON.parse(io.readFileSync(join(directory, "capture.json"), "utf8"));
    },
  };
}

// The engine's game roots come back as forward-slash paths with one trailing
// separator; provenance lists them as given. One canonical form for both.
export function canonicalRoot(p) {
  return `${p.replace(/\\/g, "/").replace(/\/+$/, "")}/`;
}

export function readLines(path, io = fs) {
  return io.readFileSync(path, "utf8").split("\n").filter((l) => l.trim()).map((l) => JSON.parse(l));
}

// Every pinned file of a corpus store: bases and cases, each bundle's
// manifest and each pack's manifest. Pack and bundle contents are pinned
// through those manifests and re-verified file by file by check-corpus.
export function pinFiles(root, io = fs) {
  const pins = {};
  const walk = (rel, filter) => {
    const abs = join(root, rel);
    if (!io.existsSync(abs)) return;
    for (const entry of io.readdirSync(abs, { withFileTypes: true })) {
      const child = `${rel}/${entry.name}`;
      if (entry.isDirectory()) walk(child, filter);
      else if (filter(child)) pins[child] = sha256(io.readFileSync(join(root, child)));
    }
  };
  walk("bases", (p) => !p.endsWith(".tmp") && !/\/effect\.(first|second)\./.test(p));
  walk("cases", (p) => !p.endsWith(".tmp"));
  walk("bundles", (p) => /^bundles\/[^/]+(\/bundle-[abc])?\/manifest\.json$/.test(p) || /^bundles\/seed-search\/\d+\/manifest\.json$/.test(p));
  walk("packs", (p) => /^packs\/[^/]+\.manifest\.json$/.test(p));
  walk("packs-bundle-only", (p) => /^packs-bundle-only\/[^/]+\.manifest\.json$/.test(p));
  for (const name of ["assets.json", "provenance.json", "seed-search.json"]) if (io.existsSync(join(root, name))) pins[name] = sha256(io.readFileSync(join(root, name)));
  return Object.fromEntries(Object.entries(pins).sort(([a], [b]) => (a < b ? -1 : 1)));
}
