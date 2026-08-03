// Unit tests for scripts/embed-web-dist.mjs — the generator that embeds the built
// React bundle into ParticleEditor.exe as RCDATA. Importing the module does NOT run a
// build or touch the filesystem (the CLI is guarded behind import.meta.url === argv[1]),
// so we exercise the pure policy (mimeFor/cacheFor) and the .rc/.h shape (render) in
// isolation. The live build + standalone-exe run prove the paths resolve; these pin the
// MIME/cache POLICY and header ENCODING that a wrong edit could silently regress
// (a bad JS Content-Type blocks React from executing; a cached index.html masks a
// rebuilt bundle).
import { test } from "node:test";
import assert from "node:assert/strict";

import { render, mimeFor, cacheFor, assertKnownShape, orphanAssets } from "./embed-web-dist.mjs";

test("mimeFor maps the extensions the bundle actually emits", () => {
  assert.equal(mimeFor("index-abc.js"), "text/javascript; charset=utf-8");
  assert.equal(mimeFor("x.mjs"), "text/javascript; charset=utf-8");
  assert.equal(mimeFor("index-abc.css"), "text/css; charset=utf-8");
  assert.equal(mimeFor("InterVariable.woff2"), "font/woff2");
  assert.equal(mimeFor("index.html"), "text/html; charset=utf-8");
  assert.equal(mimeFor("logo.svg"), "image/svg+xml");
  assert.equal(mimeFor("mod.wasm"), "application/wasm");
});

test("mimeFor is case-insensitive and falls back to octet-stream", () => {
  assert.equal(mimeFor("STYLE.CSS"), "text/css; charset=utf-8");
  assert.equal(mimeFor("blob.bin"), "application/octet-stream");
  assert.equal(mimeFor("noextension"), "application/octet-stream");
});

test("cacheFor: immutable ONLY for content-hashed /assets/; stable-named paths are no-cache", () => {
  assert.equal(cacheFor("/assets/index-BOf_GXwN.js"), "public, max-age=31536000, immutable");
  assert.equal(cacheFor("/assets/index-CSiyNn7N.css"), "public, max-age=31536000, immutable");
  // index.html and the fixed-path Inter font keep STABLE filenames whose bytes can
  // change across releases — immutable would strand old bytes in a WebView2 profile.
  assert.equal(cacheFor("/index.html"), "no-cache");
  assert.equal(cacheFor("/fonts/inter/InterVariable.woff2"), "no-cache");
});

test("assertKnownShape accepts the real bundle shape", () => {
  assert.doesNotThrow(() =>
    assertKnownShape([
      { urlPath: "/index.html" },
      { urlPath: "/assets/index-BOf_GXwN.css" },
      { urlPath: "/assets/index-CSiyNn7N.js" },
      { urlPath: "/fonts/inter/InterVariable.woff2" },
    ]),
  );
});

// A complete, valid base set to layer pollution onto (so the completeness check
// doesn't fire first and mask the shape assertion under test).
const COMPLETE = [
  { urlPath: "/index.html" },
  { urlPath: "/assets/index-CSiyNn7N.js" },
  { urlPath: "/assets/index-BOf_GXwN.css" },
];

test("assertKnownShape rejects dist pollution before it embeds into the exe", () => {
  // A sourcemap riding along in dist/ — the exact artifact the removed package
  // allowlist used to catch, now caught at the embed choke point instead.
  assert.throws(
    () => assertKnownShape([...COMPLETE, { urlPath: "/assets/index-BOf_GXwN.js.map" }]),
    /unexpected file\(s\) in web\/apps\/editor\/dist.*index-BOf_GXwN\.js\.map/s,
  );
  // A stray secret / fixture at the root, or an unexpected nested dir.
  assert.throws(() => assertKnownShape([...COMPLETE, { urlPath: "/.env" }]), /allowlist/);
  assert.throws(() => assertKnownShape([...COMPLETE, { urlPath: "/assets/data/leak.json" }]), /allowlist/);
});

test("assertKnownShape requires the Vite content-hash on /assets/ (no unhashed passthrough)", () => {
  // A hand-planted, un-hashed script/style under /assets/ must NOT slip through
  // (and must not get immutable caching). Reviewer round-2 finding.
  assert.throws(() => assertKnownShape([...COMPLETE, { urlPath: "/assets/secret.js" }]), /allowlist/);
  assert.throws(() => assertKnownShape([...COMPLETE, { urlPath: "/assets/stable.css" }]), /allowlist/);
});

test("assertKnownShape permits a legitimately HASHED static asset type (no false-reject)", () => {
  // Importing an image emits e.g. /assets/logo-AbCdEf12.svg. That must pass the
  // shape check (reviewer round-3: a JS/CSS-only allowlist breaks image imports).
  // The stronger gate against non-Vite files is assertReferenced, exercised by the
  // real build, not this pure shape test.
  assert.doesNotThrow(() => assertKnownShape([...COMPLETE, { urlPath: "/assets/logo-AbCdEf12.svg" }]));
  assert.doesNotThrow(() => assertKnownShape([...COMPLETE, { urlPath: "/assets/hero-Z9y8X7w6.png" }]));
  // ...but still hashed: an unhashed image is rejected.
  assert.throws(() => assertKnownShape([...COMPLETE, { urlPath: "/assets/logo.svg" }]), /allowlist/);
});

test("assertKnownShape rejects an INCOMPLETE bundle (missing JS or CSS)", () => {
  // Deleting dist/assets after a build leaves index.html but no script — the app
  // shell loads and then 404s React. Fail at embed time, not on the user's machine.
  assert.throws(() => assertKnownShape([{ urlPath: "/index.html" }]), /INCOMPLETE/);
  assert.throws(
    () => assertKnownShape([{ urlPath: "/index.html" }, { urlPath: "/assets/index-CSiyNn7N.js" }]),
    /INCOMPLETE.*CSS/s,
  );
});

// Synthetic manifest rows (render is pure — it interpolates these verbatim, so the
// doubled-backslash rcPath produced by scanDist reaches the .rc unchanged).
const files = [
  {
    urlPath: "/assets/app.js",
    rcPath: "ABS\\\\assets\\\\app.js",
    id: 3000,
    contentType: "text/javascript; charset=utf-8",
    cacheControl: "public, max-age=31536000, immutable",
  },
  {
    urlPath: "/index.html",
    rcPath: "ABS\\\\index.html",
    id: 3001,
    contentType: "text/html; charset=utf-8",
    cacheControl: "no-cache",
  },
];

test("render → .rc: one RCDATA line per file, id + rcPath passed through verbatim", () => {
  const { rc } = render(files);
  assert.match(rc, /^\/\/ AUTO-GENERATED by scripts\/embed-web-dist\.mjs/);
  assert.match(rc, /3000 RCDATA "ABS\\\\assets\\\\app\.js"/);
  assert.match(rc, /3001 RCDATA "ABS\\\\index\.html"/);
});

test("render → .h: inline table, CRLF-joined headers, exact count", () => {
  const { header } = render(files);
  assert.match(header, /#pragma once/);
  assert.match(header, /struct EmbeddedAsset/);
  assert.match(header, /inline const EmbeddedAsset\* EmbeddedWebAssets\(\)/);
  // The response-header block must be CRLF-delimited (what CreateWebResourceResponse
  // parses) — Content-Type then Cache-Control, each terminated by \r\n.
  assert.ok(
    header.includes(
      '{ L"/assets/app.js", 3000, L"Content-Type: text/javascript; charset=utf-8\\r\\n' +
        'Cache-Control: public, max-age=31536000, immutable\\r\\n" }',
    ),
    "app.js row has the exact CRLF-joined header string",
  );
  assert.ok(
    header.includes(
      '{ L"/index.html", 3001, L"Content-Type: text/html; charset=utf-8\\r\\n' +
        'Cache-Control: no-cache\\r\\n" }',
    ),
    "index.html row is no-cache",
  );
  assert.match(header, /inline size_t EmbeddedWebAssetCount\(\) \{ return 2; \}/);
});

// --- orphanAssets: the reference check that backs assertReferenced ---
const orphanPaths = (...args) => orphanAssets(...args).map((f) => f.urlPath);

test("orphanAssets: files referenced by index.html / another JS / CSS url() are NOT orphans", () => {
  const files = [
    { urlPath: "/assets/entry-AAAAAAAA.js" },
    { urlPath: "/assets/chunk-BBBBBBBB.js" },
    { urlPath: "/assets/main-CCCCCCCC.css" },
    { urlPath: "/assets/bg-DDDDDDDD.png" },
    { urlPath: "/fonts/inter/InterVariable.woff2" },
  ];
  const indexHtml =
    '<script src="./assets/entry-AAAAAAAA.js"></script>' +
    '<link href="./assets/main-CCCCCCCC.css">' +
    '<link rel="preload" href="./fonts/inter/InterVariable.woff2">';
  const refText = {
    "/assets/entry-AAAAAAAA.js": 'import("./chunk-BBBBBBBB.js")', // dynamic chunk
    "/assets/chunk-BBBBBBBB.js": "console.log(1)",
    "/assets/main-CCCCCCCC.css": "body{background:url(./bg-DDDDDDDD.png)}", // CSS url()
  };
  assert.deepEqual(orphanAssets(files, indexHtml, refText), []);
});

test("orphanAssets: a self-referencing planted script is STILL an orphan", () => {
  // The bypass reviewer round-4 found: a planted file naming itself must not pass.
  const files = [{ urlPath: "/assets/secret-AAAAAAAA.js" }];
  const refText = { "/assets/secret-AAAAAAAA.js": 'fetch("secret-AAAAAAAA.js")' };
  assert.deepEqual(orphanPaths(files, "<div id=root></div>", refText), ["/assets/secret-AAAAAAAA.js"]);
});

test("orphanAssets: a truly unreferenced file is an orphan", () => {
  assert.deepEqual(
    orphanPaths([{ urlPath: "/assets/orphan-AAAAAAAA.png" }], "<html></html>", {}),
    ["/assets/orphan-AAAAAAAA.png"],
  );
});

test("orphanAssets: two plants referencing ONLY each other are both orphans (index-rooted)", () => {
  // Per-file indegree would clear both; index-rooted BFS does not, because neither
  // is reachable from index.html. Reviewer round-5 bypass.
  const files = [{ urlPath: "/assets/evil1-AAAAAAAA.js" }, { urlPath: "/assets/evil2-BBBBBBBB.js" }];
  const refText = {
    "/assets/evil1-AAAAAAAA.js": 'import("./evil2-BBBBBBBB.js")',
    "/assets/evil2-BBBBBBBB.js": 'import("./evil1-AAAAAAAA.js")',
  };
  assert.deepEqual(orphanPaths(files, "<div id=root></div>", refText).sort(), [
    "/assets/evil1-AAAAAAAA.js",
    "/assets/evil2-BBBBBBBB.js",
  ]);
});

test("render(empty) still emits a well-formed, zero-asset table", () => {
  const { rc, header } = render([]);
  assert.match(rc, /^\/\/ AUTO-GENERATED/);
  assert.match(header, /inline size_t EmbeddedWebAssetCount\(\) \{ return 0; \}/);
});
