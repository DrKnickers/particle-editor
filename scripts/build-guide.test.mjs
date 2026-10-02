// Unit tests for the guide media embedding in build-guide.mjs. Importing the module does NOT
// run a build (the CLI is guarded behind an import.meta.url === argv[1] check), so we can
// exercise renderMedia() in isolation — the fail-loud paths especially, which the Playwright
// site lane can't reach because it only inspects already-generated pages.
import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { build, renderMedia, mediaStubText, referencedMediaIds, expectedMediaStub, MEDIA_STUB } from "./build-guide.mjs";

// build() reads the PRIVATE media manifest (tasks/wiki-media/manifest.json) when
// it exists and the published stub (site/guide-src/media.json) otherwise, so the
// full-build tests below run in both checkouts — no skip on the public mirror.
const repoRootForData = join(dirname(fileURLToPath(import.meta.url)), "..");
const hasPrivateManifest = existsSync(join(repoRootForData, "tasks", "wiki-media", "manifest.json"));

// renderMedia takes (id, context) where context = { media: Map<id, item>, sourcePage }.
function ctx(entries) {
  return { media: new Map(Object.entries(entries)), sourcePage: "test-page" };
}

test("clip → <video class=clip-video controls …> with the manifest output/poster filenames", () => {
  const html = renderMedia("c1", ctx({
    c1: { kind: "clip", output: "c1.mp4", poster: "c1-poster.jpg", purpose: "Show a thing" },
  }));
  assert.match(html, /<video class="clip-video" controls loop muted playsinline preload="none"/);
  assert.match(html, /data-clip="c1\.mp4"/);
  assert.match(html, /data-poster="c1-poster\.jpg"/);
  assert.match(html, /<figure class="guide-media" aria-label="Show a thing">/);
});

test("image → <img class=clip-img> with output as data-poster and purpose as alt", () => {
  const html = renderMedia("i1", ctx({ i1: { kind: "image", output: "i1.png", purpose: "A still" } }));
  assert.equal(html, `<figure class="guide-media"><img class="clip-img" data-poster="i1.png" alt="A still"></figure>`);
});

test("manual item → inert comment, no visible embed", () => {
  const html = renderMedia("m1", ctx({ m1: { kind: "image", manual: true, output: "m1.jpg" } }));
  assert.equal(html, `<!-- Media (manual, added post-launch): m1 -->`);
});

test("unknown id fails loud (guide cannot drift from the manifest)", () => {
  assert.throws(() => renderMedia("nope", ctx({})), /Unknown media id "nope" in test-page/);
});

test("a pathful output is rejected — must be a flat release basename", () => {
  assert.throws(
    () => renderMedia("c1", ctx({ c1: { kind: "clip", output: "wiki/tutorial-01/c1.mp4", poster: "c1-poster.jpg" } })),
    /must be a flat release filename/,
  );
});

test("a clip missing its poster fails loud", () => {
  assert.throws(
    () => renderMedia("c1", ctx({ c1: { kind: "clip", output: "c1.mp4" } })),
    /missing manifest poster/,
  );
});

test("attribute-bearing purpose text is HTML-escaped in alt/aria-label", () => {
  const html = renderMedia("c1", ctx({
    c1: { kind: "clip", output: "c1.mp4", poster: "c1-poster.jpg", purpose: `a "quoted" <tag> & amp` },
  }));
  assert.match(html, /aria-label="a &quot;quoted&quot; &lt;tag&gt; &amp; amp"/);
  assert.ok(!html.includes('aria-label="a "quoted"'), "raw quote must not break out of the attribute");
});

test("referencedMediaIds collects anchors (incl. | still) and nothing else", () => {
  const ids = referencedMediaIds([
    "intro\n<!-- Media: a-clip -->\ntext <!-- Media: inline-ignored -->",
    "  <!-- Media: b-still | still -->\r\n<!-- not media -->",
  ]);
  assert.deepEqual([...ids].sort(), ["a-clip", "b-still"]);
});

test("mediaStubText keeps only anchored ids and rendered fields, sorted by id", () => {
  const manifest = { items: [
    { id: "z", kind: "clip", output: "z.mp4", poster: "z.jpg", purpose: "Z", timeline: "tasks/x.json", notes: "internal" },
    { id: "a", kind: "image", output: "a.png", purpose: "A", acceptance: ["internal"] },
    { id: "unused", kind: "clip", output: "u.mp4", poster: "u.jpg" },
  ] };
  const stub = JSON.parse(mediaStubText(manifest, new Set(["z", "a", "not-in-manifest"])));
  assert.deepEqual(stub.items.map((it) => it.id), ["a", "z"]);
  assert.deepEqual(stub.items[1], { id: "z", kind: "clip", manual: false, purpose: "Z", output: "z.mp4", poster: "z.jpg" });
  assert.ok(!JSON.stringify(stub).includes("internal"), "pipeline-only fields must not reach the published stub");
});

test("the published media stub renders the guide (and matches the private manifest where present)", () => {
  assert.ok(existsSync(MEDIA_STUB), "site/guide-src/media.json must be committed — it is the public build input");
  const fromStub = build({ mediaSource: MEDIA_STUB });
  assert.ok(fromStub.size > 1);
  if (hasPrivateManifest) {
    // Private checkout: the stub must be current, and must render byte-identical pages,
    // drafts included — otherwise a public rebuild would silently differ from ours.
    const committed = readFileSync(MEDIA_STUB, "utf8").replace(/\r\n/g, "\n");
    assert.equal(committed, expectedMediaStub(), 'stale stub — run "node scripts/build-guide.mjs" and commit it');
    assert.deepEqual(fromStub, build());
    assert.deepEqual(build({ previewDrafts: true, mediaSource: MEDIA_STUB }), build({ previewDrafts: true }));
  }
});

test("unpublished pages are omitted by default but available to the local draft preview", () => {
  const publicOutputs = build();
  assert.equal(publicOutputs.has("02-polish-hardpoint-damage-smoke.html"), false);
  assert.equal(publicOutputs.has("04-recolor-and-orient-a-shield-impact.html"), false);

  const previewOutputs = build({ previewDrafts: true });
  assert.match(previewOutputs.get("02-polish-hardpoint-damage-smoke.html"), /Draft · Not published/);
  assert.match(previewOutputs.get("04-recolor-and-orient-a-shield-impact.html"), /Draft · Not published/);
});
