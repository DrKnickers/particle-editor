// Embed the built React bundle (web/apps/editor/dist) INTO ParticleEditor.exe so
// the app ships as a single exe with no separate web folder. Zero dependencies —
// Node built-ins only, mirroring scripts/build-guide.mjs.
//
//   node scripts/embed-web-dist.mjs           regenerate src/generated/EmbeddedWebAssets.{rc,h}
//   node scripts/embed-web-dist.mjs --check    exit 1 if any generated file is stale
//
// The vcxproj's GenerateEmbeddedWebAssets target runs this before ResourceCompile;
// the runtime handler (HostWindow.cpp) serves https://app.local/<path> from these
// RCDATA resources via WebResourceRequested. Content-Type and Cache-Control are
// decided HERE (per file extension) and baked into the manifest so the C++ handler
// stays dumb. Asset filenames are content-hashed by Vite and change every build,
// so the manifest MUST be generated, never hand-authored.

import fs from "node:fs";
import { join, resolve, dirname, relative } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, "..");
const DIST = join(ROOT, "web", "apps", "editor", "dist");
const GEN = join(ROOT, "src", "generated");
// Clear of src/Resources/resource.h ids (all <= 159).
const RESOURCE_ID_BASE = 3000;

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".mjs": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".json": "application/json",
  ".map": "application/json",
  ".woff2": "font/woff2",
  ".woff": "font/woff",
  ".ttf": "font/ttf",
  ".otf": "font/otf",
  ".svg": "image/svg+xml",
  ".png": "image/png",
  ".jpg": "image/jpeg",
  ".jpeg": "image/jpeg",
  ".gif": "image/gif",
  ".webp": "image/webp",
  ".avif": "image/avif",
  ".ico": "image/x-icon",
  ".wasm": "application/wasm",
  ".txt": "text/plain; charset=utf-8",
};

const extOf = (p) => {
  const i = p.lastIndexOf(".");
  return i < 0 ? "" : p.slice(i).toLowerCase();
};
export const mimeFor = (p) => MIME[extOf(p)] ?? "application/octet-stream";

// `immutable` is ONLY safe for content-addressed URLs — the filename changes
// when the bytes do. Vite content-hashes everything under /assets/, so those may
// cache for a year. Everything else keeps a STABLE filename whose bytes CAN change
// across releases: the unhashed /index.html entry document AND /fonts/* (the Inter
// font ships at a fixed path). WebView2 honors these headers on the embedded
// responses, so an immutable stable-named font would strand old bytes in a
// returning user's profile for a year — hence no-cache for anything not hashed.
export const cacheFor = (urlPath) =>
  urlPath.startsWith("/assets/") ? "public, max-age=31536000, immutable" : "no-cache";

// The generator embeds EVERY file under dist/ into the exe as RCDATA, so a stray
// artifact in dist (a sourcemap, a leftover fixture, a secret, a symlink-reached
// file) would ship INSIDE the exe and be served on app.local. This is the choke
// point where dist pollution must be caught — the release packager no longer sees
// a dist folder to allowlist. Fail loud on any path outside the known-good shape;
// a legitimately new asset kind is a deliberate one-line addition here, not a
// silent inclusion.
// The /assets/ pattern requires Vite's content-hash suffix ("-<8+ base64url>"),
// so a hand-planted /assets/secret.js or a stable /assets/foo.css (which would be
// wrongly cached immutable) is rejected — but it permits the full set of static
// asset types Vite emits under /assets/ (imported images, fonts, wasm), so
// importing e.g. an SVG does not break every native build. Anything hash-shaped
// still has to be REFERENCED by the bundle (assertReferenced), so widening the
// type list here does not widen what can actually ship.
const ALLOWED_SHAPE = [
  /^\/index\.html$/,
  /^\/assets\/[^/]+-[A-Za-z0-9_-]{8,}\.(js|css|svg|png|jpe?g|gif|webp|avif|ico|woff2?|ttf|otf|wasm)$/,
  /^\/fonts\/.+\.(woff2?|ttf|otf)$/,
];
// The bundle is unusable if any of these is absent — index.html would load but
// React would 404 its own script. The generator (which sees the REAL dist at build
// time) is the only place that can prove completeness; the packager's exe grep
// cannot. So require each here.
const REQUIRED_SHAPE = [
  { rx: /^\/assets\/[^/]+-[A-Za-z0-9_-]{8,}\.js$/, what: "a hashed JS bundle under /assets/" },
  { rx: /^\/assets\/[^/]+-[A-Za-z0-9_-]{8,}\.css$/, what: "a hashed CSS bundle under /assets/" },
  { rx: /^\/index\.html$/, what: "index.html" },
];
export function assertKnownShape(files) {
  const paths = files.map((f) => f.urlPath);
  const unexpected = paths.filter((p) => !ALLOWED_SHAPE.some((rx) => rx.test(p)));
  if (unexpected.length) {
    throw new Error(
      "[embed-web-dist] unexpected file(s) in web/apps/editor/dist not in the embed shape allowlist " +
        "(a stray artifact would ship inside the exe on app.local) -- vet it, then add a pattern to " +
        "ALLOWED_SHAPE if it belongs:\n" +
        unexpected.map((p) => "  " + p).join("\n"),
    );
  }
  const missing = REQUIRED_SHAPE.filter((r) => !paths.some((p) => r.rx.test(p)));
  if (missing.length) {
    throw new Error(
      "[embed-web-dist] the dist bundle is INCOMPLETE -- embedding it would ship an exe whose UI " +
        "cannot load. Rebuild the web bundle (cd web && pnpm --filter ./apps/editor build). Missing:\n" +
        missing.map((r) => "  " + r.what).join("\n"),
    );
  }
}

function walk(dir, out = []) {
  for (const name of fs.readdirSync(dir).sort()) {
    const full = join(dir, name);
    // lstat, NOT stat: a symlink under dist/ can point outside the bundle, so
    // following it would embed an unintended file inside the exe. Refuse it — a
    // real Vite build has none.
    const st = fs.lstatSync(full);
    if (st.isSymbolicLink()) {
      throw new Error(
        `[embed-web-dist] refusing to embed a symlink (it can reach outside dist/ and ship an ` +
          `unintended file inside the exe): ${full}`,
      );
    }
    if (st.isDirectory()) walk(full, out);
    else out.push(full);
  }
  return out;
}

// Scan the built bundle into the manifest rows the generators consume. Reads
// the filesystem; exits the process if the bundle is missing (CLI contract).
function scanDist() {
  if (!fs.existsSync(join(DIST, "index.html"))) {
    console.error(
      "[embed-web-dist] web/apps/editor/dist/index.html not found — build the editor web bundle first " +
        "(cd web && pnpm --filter ./apps/editor build).",
    );
    process.exit(1);
  }
  // walk() rejects symlinked CHILDREN, but the dist root itself could be a
  // symlink/junction pointing at an arbitrary tree — check it too.
  if (fs.lstatSync(DIST).isSymbolicLink()) {
    throw new Error(
      `[embed-web-dist] refusing to embed: web/apps/editor/dist is a symlink (${DIST}). ` +
        "It must be a real Vite build directory.",
    );
  }

  const files = walk(DIST).map((full, i) => {
    const segs = relative(DIST, full).split(/[\\/]/);
    return {
      urlPath: "/" + segs.join("/"), // e.g. /assets/index-<hash>.js
      full,
      // ABSOLUTE path with the doubled backslashes an RC quoted string needs.
      // This .rc is #included by src/ParticleEditor.rc, and rc.exe's base dir
      // for a nested-#include's data-file paths is ambiguous (top-level .rc dir
      // vs. the included file's dir, flag/version dependent). The file is a
      // gitignored per-build artifact, so a machine-specific absolute path is
      // both harmless and unambiguous.
      rcPath: full.split(/[\\/]/).join("\\\\"),
      id: RESOURCE_ID_BASE + i,
      contentType: mimeFor(full),
      cacheControl: cacheFor("/" + segs.join("/")),
    };
  });
  assertKnownShape(files); // reject dist pollution before it embeds into the exe
  assertReferenced(files); // reject hash-shaped files nothing in the bundle references
  return files;
}

// Pure core of the reference check: given index.html text and a map of
// {urlPath -> text} for the referencing files (JS + CSS), return the /assets/ and
// /fonts/ files NOT reachable from index.html. Reachability is transitive and
// INDEX-ROOTED (a BFS): index.html seeds the reachable set, then each reachable
// file's text pulls in the assets it names, and so on. Per-file indegree is not
// enough — two planted scripts that reference only each other would each have a
// referrer yet neither is reachable from the entry document. Reviewer round-5.
// (Limit: matching is by basename, so a same-basename plant in a different dir
// could ride on the real file's reachability — Vite's content hashes make that
// near-impossible, and this is a build-output sanity check, not anti-tamper.)
export function orphanAssets(files, indexHtml, refTextByPath) {
  const assets = files.filter((f) => /^\/(assets|fonts)\//.test(f.urlPath));
  const baseOf = (f) => f.urlPath.split("/").pop();
  const textByBase = new Map();
  for (const [p, text] of Object.entries(refTextByPath)) textByBase.set(p.split("/").pop(), text);

  const reachable = new Set();
  let frontier = [];
  for (const f of assets) {
    if (indexHtml.includes(baseOf(f))) { reachable.add(baseOf(f)); frontier.push(baseOf(f)); }
  }
  while (frontier.length) {
    const next = [];
    for (const base of frontier) {
      const text = textByBase.get(base); // only JS/CSS carry text that references more
      if (!text) continue;
      for (const f of assets) {
        const b = baseOf(f);
        if (!reachable.has(b) && text.includes(b)) { reachable.add(b); next.push(b); }
      }
    }
    frontier = next;
  }
  return assets.filter((f) => !reachable.has(baseOf(f)));
}

// A file whose name is merely hash-SHAPED can pass the allowlist, so also require
// that every /assets/ and /fonts/ file is actually REFERENCED elsewhere in the
// bundle: its basename must appear in index.html, a JS file (code-split chunk /
// imported-asset names), or a CSS file (url(...) references). Real Vite output is
// always referenced; a planted file is not. (Caveat: an asset referenced only via
// a runtime-built string would false-reject — Vite does not emit those, and it
// fails loud rather than shipping silently.)
function assertReferenced(files) {
  const indexHtml = fs.readFileSync(join(DIST, "index.html"), "utf8");
  const refTextByPath = {};
  for (const f of files) {
    if (/\.(js|css)$/.test(f.urlPath)) refTextByPath[f.urlPath] = fs.readFileSync(f.full, "utf8");
  }
  const orphans = orphanAssets(files, indexHtml, refTextByPath);
  if (orphans.length) {
    throw new Error(
      "[embed-web-dist] refusing to embed file(s) that nothing in the bundle references " +
        "(hash-shaped name but not real Vite output — vet before shipping inside the exe):\n" +
        orphans.map((f) => "  " + f.urlPath).join("\n"),
    );
  }
}

// Pure: manifest rows -> generated file contents. No filesystem access, so a
// unit test can pin the .rc/.h shape (ids, absolute RCDATA paths, the CRLF
// header encoding, the MIME/cache policy) against synthetic input.
export function render(files) {
  const rc =
    [
      "// AUTO-GENERATED by scripts/embed-web-dist.mjs -- do not edit.",
      "// Regenerated before ResourceCompile from web/apps/editor/dist.",
      ...files.map((f) => `${f.id} RCDATA "${f.rcPath}"`),
    ].join("\n") + "\n";

  // Header-only: the manifest is `inline` so only HostWindow.cpp needs to
  // include it -- no separate .cpp / <ClCompile> item to reference a file that
  // does not exist on a clean checkout before the generator runs.
  const rows = files.map(
    (f) =>
      `        { L"${f.urlPath}", ${f.id}, L"Content-Type: ${f.contentType}\\r\\n` +
      `Cache-Control: ${f.cacheControl}\\r\\n" },`,
  );
  const header =
    [
      "// AUTO-GENERATED by scripts/embed-web-dist.mjs -- do not edit.",
      "#pragma once",
      "#include <cstddef>",
      "",
      "namespace host {",
      "// One embedded web-bundle file: its request path, its RT_RCDATA id in this",
      "// exe, and the pre-joined response headers (Content-Type + Cache-Control).",
      "struct EmbeddedAsset {",
      "    const wchar_t* urlPath;",
      "    int            resourceId;",
      "    const wchar_t* responseHeaders;",
      "};",
      "inline const EmbeddedAsset* EmbeddedWebAssets() {",
      "    static const EmbeddedAsset kAssets[] = {",
      ...rows,
      "    };",
      "    return kAssets;",
      "}",
      `inline size_t EmbeddedWebAssetCount() { return ${files.length}; }`,
      "}  // namespace host",
    ].join("\n") + "\n";

  return { rc, header };
}

function main() {
  const files = scanDist();
  const { rc, header } = render(files);
  const targets = [
    [join(GEN, "EmbeddedWebAssets.rc"), rc],
    [join(GEN, "EmbeddedWebAssets.h"), header],
  ];

  if (process.argv.includes("--check")) {
    const stale = targets.filter(
      ([p, c]) => (fs.existsSync(p) ? fs.readFileSync(p, "utf8") : null) !== c,
    );
    if (stale.length) {
      console.error(
        "[embed-web-dist] stale generated file(s) -- run `node scripts/embed-web-dist.mjs`:\n" +
          stale.map(([p]) => "  " + p).join("\n"),
      );
      process.exit(1);
    }
    console.log(`[embed-web-dist] ${files.length} asset(s), generated files up to date`);
  } else {
    fs.mkdirSync(GEN, { recursive: true });
    let changed = 0;
    for (const [p, c] of targets) {
      const cur = fs.existsSync(p) ? fs.readFileSync(p, "utf8") : null;
      if (cur !== c) {
        fs.writeFileSync(p, c);
        changed++;
      }
    }
    console.log(`[embed-web-dist] ${files.length} asset(s) embedded; ${changed} generated file(s) updated`);
  }
}

// CLI only when run directly; importing (the unit test) exposes the pure
// render()/mimeFor()/cacheFor() without touching the filesystem.
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main();
}
