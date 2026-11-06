// [mcp-adapter] The tools. Each checks its arguments, confines its paths to the
// task root, runs one stable command, and returns that command's JSON
// unchanged. There is no format, render or safety-policy logic
// here: the exe and the repo scripts own all of it.
import * as fs from "node:fs";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { confine } from "./confine.mjs";
import { runCommand } from "./run.mjs";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
export const OBSERVE_SCRIPT = join(repoRoot, "scripts", "observe.mjs");
export const SEARCH_SCRIPT = join(repoRoot, "scripts", "vfx-index", "search.mjs");
const MAX_RESULT = 32 << 20;
const MINUTE = 60_000;

// What each exit code means.
const TOOL_EXITS = { 2: "usage, an alias, or a malformed request", 3: "the source did not load exactly as written",
  4: "the selector did not match (a stale hash, no match, an expected mismatch)", 5: "a rule refused an edit",
  6: "a verification check failed", 13: "a file could not be written", 14: "internal error" };
const SEARCH_EXITS = { 1: "the catalog or recipes could not be read", 2: "a bad query" };

const path = (description) => ({ type: "string", description: `${description}, relative to the task root` });
const text = (description) => ({ type: "string", description });

export const DEFINITIONS = [
  { name: "particle_inspect", title: "Inspect an .alo",
    description: "Every value a particle effect (.alo) saves, as JSON, with sourceDocumentHash (the SHA-256 of the file). Runs `ParticleEditor.exe --tool inspect`.",
    inputSchema: { type: "object", properties: { source: path("The .alo to read") }, required: ["source"], additionalProperties: false } },
  { name: "particle_validate", title: "Validate an .alo",
    description: "Whether an .alo loads exactly as written, with every repair the loader would make, `canonical`, and advisories. Runs `ParticleEditor.exe --tool validate`; an invalid file is a normal result (exit 3), not a tool error.",
    inputSchema: { type: "object", properties: { source: path("The .alo to check") }, required: ["source"], additionalProperties: false } },
  { name: "particle_patch", title: "Patch one emitter",
    description: "Applies a patch request's edits to one emitter and writes a verified candidate, never touching the source. The request is strict JSON: { schema: 1, selector: { sourceDocumentHash, rolePath, expected }, mutations: [...] }. Returns the patch report. Runs `ParticleEditor.exe --tool patch`.",
    inputSchema: { type: "object", properties: { source: path("The .alo to patch"),
      request: { type: "object", description: "The patch request" },
      candidate: path("Where to write the candidate .alo (a new or plain file, never the source)") },
    required: ["source", "request", "candidate"], additionalProperties: false } },
  { name: "particle_observe", title: "Observe an effect",
    description: "Replays one instance of an .alo under the fixed golden profile and writes an observation bundle: stride frames, a filmstrip, optional playback, capture.json and manifest.json. Returns manifest.json. Needs the game data the server was started with. Takes minutes; calls run one at a time.",
    inputSchema: { type: "object", properties: { source: path("The .alo to observe"),
      spec: { type: "object", description: "The observation spec (schema 1) without scenario.aloPath, which is set from `source`" },
      outDir: path("A new or empty folder for the bundle") },
    required: ["source", "spec", "outDir"], additionalProperties: false } },
  { name: "particle_asset_search", title: "Search existing textures and recipes",
    description: "Ranks the textures a catalog's effects use, and registered recipes, by filename, emitter, effect, blend mode and observed use. At least one facet is required. Runs `scripts/vfx-index/search.mjs --json`.",
    inputSchema: { type: "object", properties: { catalog: path("A catalog.json from scripts/vfx-index/build.mjs"),
      recipes: path("A recipe registry (optional)"), filename: text("File name: exact stem, prefix, then substring"),
      emitter: text("Emitter name contains"), effect: text("Effect file name contains"),
      blend: text("Blend mode by name (additive, transparent, ...) or number 0-13"),
      use: text("Words the uses carry: emitter and effect names, blend, atlas, flipbook, world-oriented"),
      slot: { type: "string", enum: ["color", "normal", "any"], description: "Texture slot (default color)" },
      limit: { type: "integer", minimum: 1, maximum: 100, description: "Most results (default 10)" } },
    required: ["catalog"], additionalProperties: false } },
];

// The subset of JSON Schema the definitions above use.
export function checkArguments(schema, args) {
  if (args === null || typeof args !== "object" || Array.isArray(args)) return ["arguments must be an object"];
  const errors = [];
  for (const key of schema.required ?? []) if (!(key in args)) errors.push(`${key}: required`);
  for (const [key, value] of Object.entries(args)) {
    const want = schema.properties[key];
    if (!want) { errors.push(`${key}: not an argument of this tool`); continue; }
    const ok = want.type === "object" ? value !== null && typeof value === "object" && !Array.isArray(value)
      : want.type === "integer" ? Number.isSafeInteger(value) && value >= (want.minimum ?? -Infinity) && value <= (want.maximum ?? Infinity)
      : typeof value === "string" && value !== "" && (!want.enum || want.enum.includes(value));
    if (!ok) errors.push(`${key}: expected ${want.enum ? `one of ${want.enum.join(", ")}` : want.type}`);
  }
  return errors;
}

const result = (parts, isError = false) => ({ content: parts.filter(Boolean).map((t) => ({ type: "text", text: t })), isError });
const failure = (message) => result([message], true);

// `config`: { root (resolved), exe (resolved), gameRoots }. `run` and `io` are
// injectable for tests.
export function createTools(config, { run = runCommand, io = fs, log = () => {} } = {}) {
  const read = (file) => {
    if (!io.existsSync(file)) return null;
    if (io.statSync(file).size > MAX_RESULT) throw Object.assign(new Error(`result larger than ${MAX_RESULT} bytes: ${file}`), { refused: true });
    return io.readFileSync(file, "utf8");
  };
  const exec = async (name, file, args, timeoutMs, signal) => {
    const r = await run(file, args, { cwd: config.root, timeoutMs, signal });
    log(`${name} argv=${JSON.stringify([file, ...args])} exit=${r.code}${r.stopped ? ` stopped=${r.stopped}` : ""}`);
    return r;
  };
  const stoppedMessage = (r, timeoutMs) => r.stopped === "timeout" ? `stopped after ${timeoutMs / 1000} s`
    : r.stopped === "output" ? "stopped: it printed more than the output cap" : "cancelled";
  const exitNote = (r, meanings) => `exit ${r.code}${meanings[r.code] ? `: ${meanings[r.code]}` : ""}${r.stderr.trim() ? `\n${r.stderr.trim()}` : ""}`;
  const withTemp = async (body) => {
    const dir = mkdtempSync(join(tmpdir(), "pe-mcp-"));
    try { return await body(dir); } finally { rmSync(dir, { recursive: true, force: true }); }
  };

  // inspect and validate: one input, the JSON the exe writes to --out.
  const report = (verb, okCodes) => (args, signal) => withTemp(async (dir) => {
    const source = confine(config.root, args.source, "file", io), out = join(dir, `${verb}.json`);
    const r = await exec(`particle_${verb}`, config.exe, ["--tool", verb, source, "--out", out], 2 * MINUTE, signal);
    if (r.stopped) return failure(stoppedMessage(r, 2 * MINUTE));
    const json = read(out);
    return json !== null && okCodes.includes(r.code) ? result([json, r.code ? exitNote(r, TOOL_EXITS) : null])
      : failure(exitNote(r, TOOL_EXITS));
  });

  const handlers = {
    particle_inspect: report("inspect", [0]),
    particle_validate: report("validate", [0, 3]),
    particle_patch: (args, signal) => withTemp(async (dir) => {
      const source = confine(config.root, args.source, "file", io);
      const candidate = confine(config.root, args.candidate, "output", io);
      const request = join(dir, "request.json"), out = join(dir, "report.json");
      io.writeFileSync(request, JSON.stringify(args.request));
      const r = await exec("particle_patch", config.exe, ["--tool", "patch", source, request, "--out", candidate, "--report", out], 2 * MINUTE, signal);
      if (r.stopped) return failure(stoppedMessage(r, 2 * MINUTE));
      const json = read(out);
      return result([json, r.code ? exitNote(r, TOOL_EXITS) : `candidate written: ${args.candidate}`], r.code !== 0);
    }),
    particle_observe: (args, signal) => withTemp(async (dir) => {
      if (args.spec.scenario === null || typeof args.spec.scenario !== "object" || Array.isArray(args.spec.scenario))
        return failure("spec.scenario: required (an object)");
      if ("aloPath" in args.spec.scenario) return failure("spec.scenario.aloPath: leave it out; it is set from `source`");
      const source = confine(config.root, args.source, "file", io);
      const outDir = confine(config.root, args.outDir, "folder", io);
      const spec = join(dir, "observation.json");
      io.writeFileSync(spec, JSON.stringify({ ...args.spec, scenario: { ...args.spec.scenario, aloPath: source } }));
      const argv = [OBSERVE_SCRIPT, spec, outDir, "--exe", config.exe, ...config.gameRoots.flatMap((g) => ["--game-root", g])];
      const r = await exec("particle_observe", process.execPath, argv, 30 * MINUTE, signal);
      if (r.stopped) return failure(stoppedMessage(r, 30 * MINUTE));
      const manifest = r.code === 0 ? read(join(outDir, "manifest.json")) : null;
      return manifest !== null ? result([manifest, `bundle written: ${args.outDir}`])
        : failure(`exit ${r.code}${r.stderr.trim() ? `\n${r.stderr.trim()}` : ""}`);
    }),
    particle_asset_search: async (args, signal) => {
      const argv = [SEARCH_SCRIPT, "--catalog", confine(config.root, args.catalog, "file", io), "--json"];
      if (args.recipes !== undefined) argv.push("--recipes", confine(config.root, args.recipes, "file", io));
      for (const facet of ["filename", "emitter", "effect", "blend", "use", "slot"])
        if (args[facet] !== undefined) argv.push(`--${facet}`, args[facet]);
      if (args.limit !== undefined) argv.push("--limit", String(args.limit));
      const r = await exec("particle_asset_search", process.execPath, argv, 2 * MINUTE, signal);
      if (r.stopped) return failure(stoppedMessage(r, 2 * MINUTE));
      return r.code === 0 ? result([r.stdout]) : failure(exitNote(r, SEARCH_EXITS));
    },
  };

  return {
    definitions: DEFINITIONS,
    async call(name, args, signal) {
      const definition = DEFINITIONS.find((d) => d.name === name);
      if (!definition) return null;
      const errors = checkArguments(definition.inputSchema, args ?? {});
      if (errors.length) return failure(`invalid arguments: ${errors.join("; ")}`);
      try {
        return await handlers[name](args ?? {}, signal);
      } catch (error) {
        if (error.refused) return failure(error.message);
        throw error;
      }
    },
  };
}
