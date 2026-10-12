// blind-page (plan 3.4, 3.5): the owner's blind pass. A loopback HTTP server
// that shows each pack exactly as an agent receives it, in the corpus's
// recorded order, through both transports (the individual frames, and the
// contact sheets), and writes cases/<id>/truth.json into the DRAFT corpus. It
// refuses to start on a locked corpus, and refuses a write once the corpus
// locks. Exposed on the tailnet by the session (tailscale serve), torn down
// after the pass.
//
//   node scripts/vfx-benchmark/blind-page.mjs <store> [--port 4190]
//
// Only files a pack's manifest lists are served: a path is looked up, never
// resolved, so nothing outside the pack can be named. Every accepted answer is
// a complete variant (plan 3.8); whether it is a correct one is lock's check.
import fs from "node:fs";
import http from "node:http";
import { fileURLToPath } from "node:url";
import { CONDITIONS, variantErrors } from "./answers.mjs";
import { THREE } from "./context.mjs";
import { RUBRIC } from "./schema-check.mjs";
import { store } from "./store.mjs";

export const TRANSPORTS = ["frames", "contact-sheets"];
const BODY_LIMIT = 256 * 1024;
const TYPES = { ".png": "image/png", ".json": "application/json", ".jsonl": "text/plain; charset=utf-8", ".md": "text/plain; charset=utf-8" };

class BlindError extends Error {
  constructor(message, status = 400) { super(message); this.status = status; }
}

function draftCorpus(s) {
  const corpus = s.json("corpus.json");
  if (corpus.state !== "draft") throw new BlindError("the corpus is locked; the blind pass writes only a draft", 409);
  return corpus;
}

// What the page needs to show one pack: its bundles, their frames, its emitters.
export function packInfo(s, id) {
  const manifest = s.json("packs", `${id}.manifest.json`);
  const paths = manifest.files.map((f) => f.path);
  const three = THREE.every((b) => paths.includes(`${b}/selected.json`));
  const bundles = three ? THREE : ["bundle"];
  const prefix = (b) => (three ? `${b}/` : "");
  const summary = s.json("packs", id, ...`${prefix(bundles[0])}summary.json`.split("/"));
  return {
    id, bundles,
    files: paths,
    frames: Object.fromEntries(bundles.map((b) => [b, s.json("packs", id, ...`${prefix(b)}selected.json`.split("/")).tiles.map((t) => t.frame)])),
    prefix: Object.fromEntries(bundles.map((b) => [b, prefix(b)])),
    emitters: summary.emitters.map((e) => ({ index: e.index, name: e.name })),
  };
}

export function session(root, io = fs) {
  const s = store(root, io);
  const corpus = draftCorpus(s);
  const known = new Set(corpus.cases.map((c) => c.id));
  return {
    transports: TRANSPORTS,
    categories: RUBRIC.categories,
    packs: corpus.order.filter((id) => known.has(id)).map((id, n) => ({ n: n + 1, ...packInfo(s, id), done: s.exists("cases", id, "truth.json") })),
  };
}

// A pack file by its manifest path, or null.
export function packFile(root, id, rel, io = fs) {
  const s = store(root, io);
  const corpus = draftCorpus(s);
  if (!corpus.cases.some((c) => c.id === id)) return null;
  const manifest = s.json("packs", `${id}.manifest.json`);
  const entry = manifest.files.find((f) => f.path === rel);
  return entry ? s.read("packs", id, ...entry.path.split("/")) : null;
}

const isRef = (r, emitters) => r && Number.isInteger(r.index) && typeof r.name === "string" && emitters.some((e) => e.index === r.index && e.name === r.name);
const frameList = (list) => Array.isArray(list) && list.every((f) => Number.isInteger(f) && f >= 0);

// A complete variant: every field an answer contract needs, in the pack's terms.
export function variantIncomplete(v, pack, rubric = RUBRIC) {
  const errors = [];
  if (!v || typeof v !== "object") return ["a variant is an object"];
  if (!pack.bundles.includes(v.bundleId)) errors.push(`bundle ${v.bundleId} is not in the pack`);
  const category = rubric.categories[v.category];
  if (!category) errors.push(`category ${v.category} is not in the rubric`);
  const options = category?.directions?.[v.direction];
  if (category && !options) errors.push(`direction ${v.direction} is not one of ${v.category}'s`);
  if (v.category === "draw-order") {
    if (!isRef(v.relation?.front, pack.emitters) || !isRef(v.relation?.back, pack.emitters)) errors.push("draw-order names both emitters of its relation");
  } else if (!Array.isArray(v.emitters) || !v.emitters.length || !v.emitters.every((e) => isRef(e, pack.emitters))) {
    errors.push("the variant names its emitters");
  }
  if (!v.windows || typeof v.windows !== "object") errors.push("the variant has windows");
  else {
    for (const [b, w] of Object.entries(v.windows)) {
      if (!pack.bundles.includes(b)) errors.push(`window bundle ${b} is not in the pack`);
      if (!frameList(w?.image) || !frameList(w?.telemetry)) errors.push(`the ${b} window lists image and telemetry frames`);
    }
  }
  for (const c of CONDITIONS) if (!v.policy || !(c in v.policy)) errors.push(`the policy names its ${c} condition`);
  const c = v.correction;
  if (!c || !options?.some((o) => o.kind === c.kind && (o.field === undefined || o.field === c.field) && (o.track === undefined || o.track === c.track))) {
    errors.push("the correction is one of the direction's options");
  } else if (c.kind !== "reorder" && !isRef(c.emitter, pack.emitters)) errors.push("the correction names its emitter");
  if (!errors.length) errors.push(...variantErrors({ ...v, id: 0 }));
  return errors;
}

export function truthErrors(truth, pack, { control = false } = {}) {
  const errors = [];
  if (!truth || typeof truth !== "object" || !truth.transports) return ["truth carries transports"];
  for (const t of TRANSPORTS) {
    const set = truth.transports[t];
    if (!set) { errors.push(`no ${t} annotation`); continue; }
    if (typeof set.visible !== "boolean") errors.push(`${t}: visible is yes or no`);
    if (!Array.isArray(set.variants)) { errors.push(`${t}: variants is a list`); continue; }
    if (!set.visible && set.variants.length && !control) errors.push(`${t}: a not-visible annotation carries no answer`);
    set.variants.forEach((v, i) => { for (const e of variantIncomplete(v, pack)) errors.push(`${t} variant ${i}: ${e}`); });
  }
  for (const k of Object.keys(truth.transports)) if (!TRANSPORTS.includes(k)) errors.push(`unknown transport ${k}`);
  return errors;
}

export function writeTruth(root, id, truth, io = fs) {
  const s = store(root, io);
  const corpus = draftCorpus(s);
  const c = corpus.cases.find((x) => x.id === id);
  if (!c) throw new BlindError(`no case ${id}`, 404);
  const pack = packInfo(s, id);
  const errors = truthErrors(truth, pack, { control: c.kind === "control" });
  if (errors.length) throw new BlindError(errors.join("\n"));
  const clean = { transports: Object.fromEntries(TRANSPORTS.map((t) => [t, { visible: truth.transports[t].visible, variants: truth.transports[t].variants }])) };
  s.writeJson(["cases", id, "truth.json"], clean);
  return clean;
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    const parts = [];
    let size = 0;
    req.on("data", (chunk) => {
      size += chunk.length;
      if (size <= BODY_LIMIT) parts.push(chunk);
    });
    // Drain an oversized body and answer it, rather than reset the connection.
    req.on("end", () => (size > BODY_LIMIT ? reject(new BlindError("body too large", 413)) : resolve(Buffer.concat(parts).toString("utf8"))));
    req.on("error", reject);
  });
}

export function createBlindServer({ root, io = fs }) {
  session(root, io); // refuses a locked corpus before listening
  return http.createServer(async (req, res) => {
    const send = (status, body, type = "application/json") => {
      res.writeHead(status, { "content-type": type, "cache-control": "no-store", "x-content-type-options": "nosniff" });
      res.end(body);
    };
    try {
      const url = new URL(req.url, "http://blind.local");
      const path = decodeURIComponent(url.pathname);
      if (req.method === "GET" && path === "/") return send(200, PAGE, "text/html; charset=utf-8");
      if (req.method === "GET" && path === "/api/session") return send(200, JSON.stringify(session(root, io)));
      const file = /^\/pack\/([0-9A-Za-z_-]+)\/(.+)$/.exec(path);
      if (req.method === "GET" && file) {
        const bytes = packFile(root, file[1], file[2], io);
        if (!bytes) return send(404, JSON.stringify({ error: "not in the pack" }));
        return send(200, bytes, TYPES[file[2].slice(file[2].lastIndexOf("."))] ?? "application/octet-stream");
      }
      const truth = /^\/api\/truth\/([0-9A-Za-z_-]+)$/.exec(path);
      if (truth && req.method === "GET") {
        const s = store(root, io);
        draftCorpus(s);
        return send(200, s.exists("cases", truth[1], "truth.json") ? s.text("cases", truth[1], "truth.json") : "null");
      }
      if (truth && req.method === "POST") {
        if (!/^application\/json\b/.test(req.headers["content-type"] ?? "")) throw new BlindError("content-type must be application/json", 415);
        let body;
        try { body = JSON.parse(await readBody(req)); } catch (e) { throw e instanceof BlindError ? e : new BlindError("the body is not JSON"); }
        return send(200, JSON.stringify(writeTruth(root, truth[1], body, io)));
      }
      return send(404, JSON.stringify({ error: "not found" }));
    } catch (error) {
      return send(error.status ?? 500, JSON.stringify({ error: error.message }));
    }
  });
}

export function main(argv = process.argv.slice(2)) {
  const [root, ...rest] = argv;
  const portAt = rest.indexOf("--port");
  const port = portAt >= 0 ? Number(rest[portAt + 1]) : 4190;
  if (!root || !Number.isInteger(port)) { console.error("usage: blind-page.mjs <store> [--port 4190]"); return 2; }
  let server;
  try { server = createBlindServer({ root }); } catch (error) { console.error(`[bench] refused: ${error.message}`); return 3; }
  server.listen(port, "127.0.0.1", () => console.log(`[bench] blind pass on http://127.0.0.1:${port}/ (loopback only; expose it with tailscale serve)`));
  return 0;
}

const PAGE = `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Blind pass</title>
<style>
:root { --bg: #f6f6f4; --fg: #1d1d1b; --muted: #6b6b66; --line: #d9d9d4; --accent: #2f5fd0; --bad: #b3261e; --card: #fff; }
@media (prefers-color-scheme: dark) { :root { --bg: #161615; --fg: #ececea; --muted: #9a9a94; --line: #34342f; --accent: #8fb0ff; --bad: #ff8a80; --card: #1f1f1d; } }
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--fg); font: 15px/1.45 system-ui, sans-serif; }
header { position: sticky; top: 0; background: var(--bg); border-bottom: 1px solid var(--line); padding: 10px 16px; display: flex; gap: 12px; align-items: center; flex-wrap: wrap; z-index: 1; }
main { padding: 16px; max-width: 1200px; margin: 0 auto; }
button, select, input { font: inherit; color: inherit; background: var(--card); border: 1px solid var(--line); border-radius: 6px; padding: 4px 8px; }
button.primary { background: var(--accent); color: #fff; border-color: var(--accent); }
.tabs button[aria-pressed=true] { border-color: var(--accent); color: var(--accent); }
.card { background: var(--card); border: 1px solid var(--line); border-radius: 8px; padding: 12px; margin: 12px 0; }
.frames { display: grid; grid-template-columns: repeat(auto-fill, minmax(180px, 1fr)); gap: 8px; }
.frames figure { margin: 0; } .frames img { width: 100%; border: 1px solid var(--line); }
.sheet { overflow-x: auto; } .sheet img { display: block; max-width: none; margin-bottom: 8px; border: 1px solid var(--line); }
figcaption, .muted { color: var(--muted); font-size: 13px; }
pre { white-space: pre-wrap; margin: 0; }
.grid { display: grid; grid-template-columns: max-content 1fr; gap: 6px 10px; align-items: center; }
.error { color: var(--bad); white-space: pre-wrap; }
</style></head><body>
<header><strong>Blind pass</strong><span id="where" class="muted"></span>
<button id="prev">Previous</button><button id="next">Next</button>
<span class="tabs" id="tabs"></span></header>
<main id="main">Loading…</main>
<script>
const $ = (t, a = {}, ...kids) => { const e = document.createElement(t); for (const [k, v] of Object.entries(a)) k.startsWith("on") ? e.addEventListener(k.slice(2), v) : e.setAttribute(k, v); for (const k of kids) e.append(k); return e; };
let S, at = 0, transport = "frames", truth = null;
const frames = (text) => text.split(",").flatMap((p) => { p = p.trim(); if (!p) return []; const m = /^(\\d+)-(\\d+)$/.exec(p); if (m) { const out = []; for (let i = +m[1]; i <= +m[2]; i++) out.push(i); return out; } return [Number(p)]; });
const ref = (e) => ({ index: e.index, name: e.name });
const emptySet = () => ({ visible: false, variants: [] });
async function load() { S = await (await fetch("/api/session")).json(); await show(); }
async function show() {
  const p = S.packs[at];
  truth = (await (await fetch("/api/truth/" + p.id)).json()) ?? { transports: { frames: emptySet(), "contact-sheets": emptySet() } };
  document.getElementById("where").textContent = "Pack " + p.n + " of " + S.packs.length + (p.done ? " (annotated)" : "");
  const tabs = document.getElementById("tabs"); tabs.replaceChildren(...S.transports.map((t) => $("button", { "aria-pressed": String(t === transport), onclick: () => { transport = t; show(); } }, t)));
  const main = document.getElementById("main"); main.replaceChildren();
  const url = (rel) => "/pack/" + p.id + "/" + rel;
  if (p.files.includes("intent.md")) main.append($("div", { class: "card" }, $("pre", {}, await (await fetch(url("intent.md"))).text())));
  for (const b of p.bundles) {
    const card = $("div", { class: "card" }, $("strong", {}, b));
    if (transport === "frames") card.append($("div", { class: "frames" }, ...p.frames[b].map((f) => { const src = url(p.prefix[b] + "frames/frame-" + String(f).padStart(5, "0") + ".png"); return $("figure", {}, $("a", { href: src, target: "_blank" }, $("img", { src, alt: "frame " + f })), $("figcaption", {}, "frame " + f)); })));
    else card.append($("div", { class: "sheet" }, $("img", { src: url(p.prefix[b] + "selected.png"), alt: "selected frames" }), $("img", { src: url(p.prefix[b] + "filmstrip.png"), alt: "filmstrip" })));
    card.append($("p", { class: "muted" }, "Also in the pack: ", ...["summary.json", "frames.jsonl", "selected.json", "filmstrip.json"].map((f) => $("a", { href: url(p.prefix[b] + f), target: "_blank" }, f + " "))));
    main.append(card);
  }
  if (p.files.includes("inspect.json")) main.append($("p", {}, $("a", { href: url("inspect.json"), target: "_blank" }, "inspect.json")));
  main.append(form(p));
}
function form(p) {
  const set = truth.transports[transport];
  const box = $("div", { class: "card" }, $("h3", {}, "Your answer (" + transport + ")"));
  const vis = $("input", { type: "checkbox" }); vis.checked = set.visible; vis.onchange = () => { set.visible = vis.checked; };
  box.append($("label", {}, vis, " I see a defect"));
  set.variants.forEach((v, i) => box.append(variantForm(p, set, v, i)));
  box.append($("p", {}, $("button", { onclick: () => { set.variants.push({ bundleId: p.bundles[0], emitters: [], windows: {}, policy: { primary: "frames", "bundle-only": null } }); show2(); } }, set.variants.length ? "Add an alternative answer" : "Add the answer")));
  const err = $("div", { class: "error" });
  box.append($("p", {}, $("button", { class: "primary", onclick: async () => {
    const r = await fetch("/api/truth/" + p.id, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify(truth) });
    const j = await r.json(); err.className = r.ok ? "muted" : "error"; if (!r.ok) { err.textContent = j.error; return; } p.done = true; err.textContent = "Saved."; } }, "Save both transports"),
    " ", $("button", { onclick: () => { const other = transport === "frames" ? "contact-sheets" : "frames"; truth.transports[other] = JSON.parse(JSON.stringify(set)); err.textContent = "Copied to " + other + "; review it there."; } }, "Copy to the other transport")), err);
  return box;
}
function show2() { const p = S.packs[at]; const main = document.getElementById("main"); main.lastChild.replaceWith(form(p)); }
function select(options, value, onchange) { const s = $("select", { onchange: (e) => onchange(e.target.value) }, $("option", { value: "" }, "—"), ...options.map(([v, label]) => { const o = $("option", { value: v }, label); if (v === value) o.selected = true; return o; })); return s; }
function variantForm(p, set, v, i) {
  const em = p.emitters.map((e) => [String(e.index), e.index + " " + e.name]);
  const byIndex = (x) => ref(p.emitters.find((e) => String(e.index) === x));
  const cat = S.categories[v.category];
  const opts = cat?.directions?.[v.direction] ?? [];
  const optKey = (o) => o.kind + ":" + (o.field ?? o.track ?? "");
  const g = $("div", { class: "grid" });
  const row = (label, el) => g.append($("span", {}, label), el);
  row("Bundle", select(p.bundles.map((b) => [b, b]), v.bundleId, (x) => { v.bundleId = x; }));
  row("Category", select(Object.keys(S.categories).map((c) => [c, c]), v.category, (x) => { v.category = x; v.direction = undefined; v.correction = undefined; show2(); }));
  row("Direction", select(Object.keys(cat?.directions ?? {}).map((d) => [d, d]), v.direction, (x) => { v.direction = x; v.correction = undefined; show2(); }));
  if (v.category === "draw-order") {
    v.relation ??= {};
    row("In front", select(em, v.relation.front && String(v.relation.front.index), (x) => { v.relation.front = byIndex(x); }));
    row("Behind", select(em, v.relation.back && String(v.relation.back.index), (x) => { v.relation.back = byIndex(x); }));
  } else {
    const multi = $("select", { multiple: "", size: String(Math.min(6, em.length)), onchange: (e) => { v.emitters = [...e.target.selectedOptions].map((o) => byIndex(o.value)); } }, ...em.map(([val, label]) => { const o = $("option", { value: val }, label); if (v.emitters?.some((x) => String(x.index) === val)) o.selected = true; return o; }));
    row("Emitters", multi);
  }
  for (const b of p.bundles) {
    const w = (v.windows[b] ??= { image: [], telemetry: [] });
    row(b + " image frames", $("input", { value: w.image.join(", "), placeholder: "e.g. 30, 45", onchange: (e) => { w.image = frames(e.target.value); } }));
    row(b + " telemetry frames", $("input", { value: w.telemetry.join(", "), placeholder: "e.g. 31-40", onchange: (e) => { w.telemetry = frames(e.target.value); } }));
  }
  row("Evidence (with inspect)", select([["frames", "frames"], ["inspect", "inspect"], ["either", "either"]], v.policy.primary, (x) => { v.policy.primary = x; }));
  row("Evidence (bundle only)", select([["frames", "frames"]], v.policy["bundle-only"] ?? "", (x) => { v.policy["bundle-only"] = x || null; }));
  row("Correction", select(opts.map((o) => [optKey(o), o.kind + " " + (o.field ?? ("track " + o.track)) + (o.direction ? " (" + o.direction + ")" : "")]), v.correction && optKey(v.correction), (x) => {
    const o = opts.find((q) => optKey(q) === x); v.correction = o ? { kind: o.kind, ...(o.field !== undefined ? { field: o.field } : {}), ...(o.track !== undefined ? { track: o.track } : {}), ...(v.correction?.emitter ? { emitter: v.correction.emitter } : {}) } : undefined; show2(); }));
  if (v.correction && v.correction.kind !== "reorder") row("Correction emitter", select(em, v.correction.emitter && String(v.correction.emitter.index), (x) => { v.correction.emitter = byIndex(x); }));
  return $("div", { class: "card" }, $("strong", {}, i ? "Alternative " + i : "Answer"), " ", $("button", { onclick: () => { set.variants.splice(i, 1); show2(); } }, "Remove"), g);
}
document.getElementById("prev").onclick = () => { at = Math.max(0, at - 1); show(); };
document.getElementById("next").onclick = () => { at = Math.min(S.packs.length - 1, at + 1); show(); };
load();
</script></body></html>`;

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) process.exitCode = main();
