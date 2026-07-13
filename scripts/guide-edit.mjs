// Side-by-side WYSIWYG editor for the Particle Editor guide.
//   - Editor UI at  http://localhost:4178/   (Toast UI rich editor | live rendered page)
//   - Serves site/ as the web root (guide pages reference ../styles.css etc.).
//   - Saving writes the .md and re-runs scripts/build-guide.mjs (the REAL renderer),
//     then the right pane reloads the true page and a rendered-diff guard shows what changed.
//   - Client logic lives in guide-editor-client.js (served at /__editor.js) — no inline JS.
// Dependency-free (Node built-ins only), matching the repo's convention.
//
//   node scripts/guide-edit.mjs
//
// Dev-only tool; not part of any build or gate.

import http from "node:http";
import fs from "node:fs";
import { spawnSync } from "node:child_process";
import { join, resolve, relative, isAbsolute, extname, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const CLIENT_JS = join(HERE, "guide-edit-client.js");
const REPO = resolve(HERE, "..");
const SITE = join(REPO, "site");
const SRC = join(SITE, "guide-src");
const BUILD = join(REPO, "scripts", "build-guide.mjs");
const PORT = 4178;

let buildId = 0;
let building = false;
let pending = false;
let lastBuildError = "";
let muteWatchUntil = 0; // suppress the watcher briefly after an API save (avoids double build)

function rebuild(reason) {
  if (building) { pending = true; return { ok: true, buildId }; }
  building = true;
  const started = process.hrtime.bigint();
  const r = spawnSync(process.execPath, [BUILD], { cwd: REPO, encoding: "utf8" });
  const ms = Number(process.hrtime.bigint() - started) / 1e6;
  if (r.status === 0) {
    buildId++;
    lastBuildError = "";
    console.log(`[build] ok (${ms.toFixed(0)}ms) #${buildId} — ${reason}`);
  } else {
    lastBuildError = (r.stderr || r.stdout || "build failed").trim();
    console.error(`[build] FAILED — ${reason}\n${lastBuildError}`);
  }
  building = false;
  const okNow = r.status === 0;
  if (pending) { pending = false; rebuild("coalesced save"); }
  return { ok: okNow, buildId, error: lastBuildError };
}

// Rendered-diff guard: what has this session changed in the SHIPPED html for one page,
// vs the last commit? --ignore-all-space drops the autocrlf/EOL phantom churn.
function renderedDiff(name) {
  const rel = "site/guide/" + name.replace(/\.md$/, ".html");
  const numstat = spawnSync("git", ["diff", "--ignore-all-space", "--numstat", "--", rel], { cwd: REPO, encoding: "utf8" });
  let added = 0, removed = 0;
  const row = (numstat.stdout || "").trim().split("\n")[0]?.split("\t");
  if (row && row.length >= 2 && row[0] !== "-") { added = parseInt(row[0], 10) || 0; removed = parseInt(row[1], 10) || 0; }
  let text = "";
  if (added || removed) {
    const d = spawnSync("git", ["diff", "--ignore-all-space", "--no-color", "--", rel], { cwd: REPO, encoding: "utf8" });
    text = (d.stdout || "").split("\n")
      .filter((l) => (l.startsWith("+") || l.startsWith("-")) && !/^(\+\+\+|---)/.test(l))
      .slice(0, 80).join("\n");
  }
  return { added, removed, text };
}

// Debounced watch: catches edits made in an external editor too.
let timer = null;
fs.watch(SRC, { persistent: true }, (_evt, file) => {
  if (Date.now() < muteWatchUntil) return;
  if (file && !file.endsWith(".md")) return;
  clearTimeout(timer);
  timer = setTimeout(() => rebuild(`saved ${file ?? "guide-src"}`), 150);
});

// ---- page listing / md read-write helpers --------------------------------------------------

const mdFiles = () => fs.readdirSync(SRC).filter((f) => f.endsWith(".md")).sort();
const titleOf = (md) => { const m = md.match(/^#\s+(.+)$/m); return m ? m[1].trim() : ""; };
const isValidName = (name) =>
  typeof name === "string" && /^[\w-]+\.md$/.test(name) && mdFiles().includes(name);

// Cap request bodies: the largest guide page is ~25KB, so 2MB is generous headroom
// while still preventing a runaway request from exhausting memory or triggering an
// expensive rebuild off a garbage payload. Oversize/aborted requests resolve null.
const MAX_BODY = 2 * 1024 * 1024;
function readBody(req) {
  return new Promise((res) => {
    let data = "";
    req.on("data", (c) => {
      data += c;
      if (data.length > MAX_BODY) { data = null; req.destroy(); res(null); }
    });
    req.on("end", () => res(data));
    req.on("error", () => res(null));
  });
}

// ---- static serving + livereload -----------------------------------------------------------

const MIME = {
  ".html": "text/html; charset=utf-8", ".css": "text/css; charset=utf-8",
  ".js": "text/javascript; charset=utf-8", ".mjs": "text/javascript; charset=utf-8",
  ".json": "application/json", ".svg": "image/svg+xml", ".woff2": "font/woff2",
  ".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".webp": "image/webp",
  ".gif": "image/gif", ".mp4": "video/mp4", ".ico": "image/x-icon", ".txt": "text/plain",
};

const LIVERELOAD = `
<script>
(function () {
  let last = null;
  async function tick() {
    try {
      const id = await (await fetch("/__buildid", { cache: "no-store" })).text();
      if (last !== null && id !== last) { location.reload(); return; }
      last = id;
    } catch (_) {}
    setTimeout(tick, 500);
  }
  tick();
})();
</script>`;

function safePath(urlPath) {
  let clean;
  try {
    clean = decodeURIComponent(urlPath.split("?")[0]);
  } catch {
    return null; // malformed percent-encoding -> 403 rather than an uncaught throw
  }
  const root = resolve(SITE);
  const full = resolve(SITE, "." + clean);
  // path.relative-based containment: a bare string-prefix check would accept a
  // sibling directory that merely starts with the same prefix (e.g. site-extra/).
  const rel = relative(root, full);
  if (rel === "" || rel === ".") return join(root, "index.html");
  if (rel.startsWith("..") || isAbsolute(rel)) return null;
  return full;
}

function serveStatic(req, res) {
  let file = safePath(req.url);
  if (!file) { res.writeHead(403); res.end("forbidden"); return; }
  if (fs.existsSync(file) && fs.statSync(file).isDirectory()) file = join(file, "index.html");
  fs.readFile(file, (err, data) => {
    if (err) { res.writeHead(404); res.end("not found: " + req.url); return; }
    const ext = extname(file).toLowerCase();
    const type = MIME[ext] || "application/octet-stream";
    if (ext === ".html") {
      let html = data.toString("utf8");
      html = html.includes("</body>") ? html.replace("</body>", LIVERELOAD + "\n</body>") : html + LIVERELOAD;
      res.writeHead(200, { "content-type": type, "cache-control": "no-store" });
      res.end(html);
    } else {
      res.writeHead(200, { "content-type": type });
      res.end(data);
    }
  });
}

const json = (res, code, obj) => {
  res.writeHead(code, { "content-type": "application/json", "cache-control": "no-store" });
  res.end(JSON.stringify(obj));
};

// ---- editor app (HTML shell; behaviour is in /__editor.js) ---------------------------------

function editorHtml() {
  const TUI = "3.2.2";
  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Guide Editor</title>
<link rel="stylesheet" href="https://uicdn.toast.com/editor/${TUI}/toastui-editor.min.css">
<link rel="stylesheet" href="https://uicdn.toast.com/editor/${TUI}/theme/toastui-editor-dark.min.css">
<style>
  :root {
    --bg: #0e1116; --panel: #161b22; --panel2: #1c2430; --line: #2b3542;
    --text: #d7dee7; --muted: #8b97a6; --accent: #6aa6ff; --ok: #4cc38a;
    --warn: #e0a458; --err: #f2606a; --mono: "IBM Plex Mono", ui-monospace, "Cascadia Code", Consolas, monospace;
  }
  * { box-sizing: border-box; }
  html, body { height: 100%; margin: 0; }
  body { background: var(--bg); color: var(--text); font: 14px/1.5 -apple-system, "Segoe UI", system-ui, sans-serif; display: flex; flex-direction: column; }
  header { display: flex; align-items: center; gap: 12px; padding: 8px 14px; background: var(--panel); border-bottom: 1px solid var(--line); flex: none; }
  header .brand { font-weight: 650; letter-spacing: .2px; margin-right: 4px; }
  select { background: var(--panel2); color: var(--text); border: 1px solid var(--line); border-radius: 7px; padding: 6px 10px; font: inherit; max-width: 40vw; }
  .pill { font: 12px/1 var(--mono); padding: 5px 9px; border-radius: 999px; border: 1px solid var(--line); color: var(--muted); white-space: nowrap; }
  .pill.saving { color: var(--warn); border-color: color-mix(in srgb, var(--warn) 40%, var(--line)); }
  .pill.saved  { color: var(--ok);   border-color: color-mix(in srgb, var(--ok) 40%, var(--line)); }
  .pill.err    { color: var(--err);  border-color: color-mix(in srgb, var(--err) 45%, var(--line)); }
  .spacer { flex: 1; }
  a.ext { color: var(--accent); text-decoration: none; font-size: 13px; }
  a.ext:hover { text-decoration: underline; }
  main { flex: 1; display: flex; min-height: 0; }
  .pane { min-width: 0; min-height: 0; display: flex; flex-direction: column; }
  #left  { flex: 0 0 50%; border-right: 1px solid var(--line); }
  #right { flex: 1; }
  .pane .cap { font: 11px/1 var(--mono); text-transform: uppercase; letter-spacing: .1em; color: var(--muted); padding: 7px 12px; background: var(--panel); border-bottom: 1px solid var(--line); flex: none; display: flex; align-items: center; gap: 8px; }
  #splitter { flex: 0 0 6px; cursor: col-resize; background: transparent; }
  #splitter:hover, #splitter.drag { background: color-mix(in srgb, var(--accent) 40%, transparent); }
  #editorHost { flex: 1; min-height: 0; }
  .toastui-editor-defaultUI { height: 100% !important; border: 0 !important; }
  iframe { flex: 1; width: 100%; border: 0; background: #fff; }
  .err-bar { display: none; padding: 8px 12px; background: color-mix(in srgb, var(--err) 16%, var(--panel)); color: #ffd7da; border-bottom: 1px solid var(--err); font: 12px/1.5 var(--mono); white-space: pre-wrap; }
  .load-bar { display: none; padding: 8px 12px; background: color-mix(in srgb, var(--warn) 16%, var(--panel)); color: #ffe6c2; border-bottom: 1px solid var(--warn); font: 12px/1.5 var(--mono); }
  .hint { color: var(--muted); font-size: 12px; }
  kbd { font: 11px/1 var(--mono); background: var(--panel2); border: 1px solid var(--line); border-bottom-width: 2px; border-radius: 4px; padding: 2px 5px; }
  .media-chip { display: inline-flex; align-items: center; gap: 6px; padding: 2px 9px; margin: 0 1px;
    border-radius: 999px; font: 12px/1.6 var(--mono); background: color-mix(in srgb, var(--accent) 18%, #0b1220);
    color: #cfe0ff; border: 1px solid color-mix(in srgb, var(--accent) 45%, var(--line)); cursor: default; user-select: none; }
  .media-chip.planned { background: color-mix(in srgb, var(--warn) 16%, #0b1220); color: #ffe6c2; border-color: color-mix(in srgb, var(--warn) 45%, var(--line)); }
  /* rendered-diff guard */
  .diff-panel { flex: none; border-bottom: 1px solid var(--line); background: var(--panel); }
  .diff-panel .sum { padding: 6px 12px; font: 12px/1.4 var(--mono); cursor: pointer; user-select: none; }
  .diff-panel.clean .sum { color: var(--ok); }
  .diff-panel.changed .sum { color: var(--warn); }
  .diff-body { display: none; max-height: 34vh; overflow: auto; padding: 8px 12px; font: 12px/1.5 var(--mono);
    white-space: pre-wrap; word-break: break-word; border-top: 1px dashed var(--line); background: #0b0f14; }
  .diff-body .a { color: var(--ok); } .diff-body .r { color: var(--err); }
</style>
</head>
<body>
<header>
  <span class="brand">Guide Editor</span>
  <select id="pages" title="Guide page"></select>
  <span id="status" class="pill">loading…</span>
  <span class="spacer"></span>
  <span class="hint">WYSIWYG · <kbd>Ctrl</kbd>+<kbd>S</kbd> save · autosaves on pause</span>
  <a id="openfull" class="ext" target="_blank" rel="noopener">Open full page ↗</a>
</header>
<div class="load-bar" id="loadbar"></div>
<div class="err-bar" id="errbar"></div>
<div class="diff-panel clean" id="diffPanel">
  <div class="sum" id="diffSummary">Rendered output vs last commit: no change</div>
  <div class="diff-body" id="diffBody"></div>
</div>
<main>
  <section class="pane" id="left">
    <div class="cap">Rich editor <span class="hint" id="fname"></span></div>
    <div id="editorHost"></div>
  </section>
  <div id="splitter" title="Drag to resize"></div>
  <section class="pane" id="right">
    <div class="cap">Live rendered page (real output)</div>
    <iframe id="preview" title="Rendered guide page"></iframe>
  </section>
</main>
<script src="https://uicdn.toast.com/editor/${TUI}/toastui-editor-all.min.js"></script>
<script src="/__editor.js"></script>
</body>
</html>`;
}

// ---- request router ------------------------------------------------------------------------

http.createServer(async (req, res) => {
  const url = req.url || "/";

  if (url === "/" || url.startsWith("/?")) {
    res.writeHead(200, { "content-type": "text/html; charset=utf-8", "cache-control": "no-store" });
    res.end(editorHtml()); return;
  }
  if (url === "/__editor.js") {
    fs.readFile(CLIENT_JS, (err, data) => {
      if (err) { res.writeHead(404); res.end("// client missing"); return; }
      res.writeHead(200, { "content-type": "text/javascript; charset=utf-8", "cache-control": "no-store" });
      res.end(data);
    });
    return;
  }
  if (url === "/__buildid") { res.writeHead(200, { "content-type": "text/plain", "cache-control": "no-store" }); res.end(String(buildId)); return; }
  if (url === "/__api/pages") {
    const list = mdFiles().map((name) => ({ name, title: titleOf(fs.readFileSync(join(SRC, name), "utf8")) }));
    json(res, 200, list); return;
  }
  if (url.startsWith("/__api/page")) {
    const name = new URL(url, "http://x").searchParams.get("name");
    if (!isValidName(name)) { json(res, 400, { error: "bad name" }); return; }
    res.writeHead(200, { "content-type": "text/plain; charset=utf-8", "cache-control": "no-store" });
    res.end(fs.readFileSync(join(SRC, name), "utf8")); return;
  }
  if (url.startsWith("/__api/diff")) {
    const name = new URL(url, "http://x").searchParams.get("name");
    if (!isValidName(name)) { json(res, 400, { error: "bad name" }); return; }
    json(res, 200, renderedDiff(name)); return;
  }
  if (url.startsWith("/__api/save") && req.method === "POST") {
    const name = new URL(url, "http://x").searchParams.get("name");
    if (!isValidName(name)) { json(res, 400, { ok: false, error: "bad name" }); return; }
    const body = await readBody(req);
    if (body === null) { json(res, 413, { ok: false, error: "body too large or aborted" }); return; }
    fs.writeFileSync(join(SRC, name), body, "utf8");
    muteWatchUntil = Date.now() + 400; // our own rebuild covers this write
    const r = rebuild(`api save ${name}`);
    if (r.ok) r.diff = renderedDiff(name);
    json(res, 200, r); return;
  }

  serveStatic(req, res);
}).listen(PORT, "127.0.0.1", () => {
  // Loopback-only: this server writes guide-src/*.md on unauthenticated POSTs,
  // so it must never be reachable from the LAN.
  rebuild("startup");
  console.log(`\n  Guide editor  →  http://localhost:${PORT}/`);
  console.log(`  WYSIWYG · saves to site/guide-src/*.md · rendered-diff guard on\n`);
});
