// Prerender the Markdown guide (site/guide-src/*.md) into static HTML (site/guide/*.html).
//
// Zero dependencies — Node built-ins only, matching the rest of scripts/ and the site's
// dependency-free serve.mjs. Run locally and COMMIT the generated HTML; nothing builds this
// in CI (by design). Usage:
//
//   node scripts/build-guide.mjs            build + write site/guide/*.html
//   node scripts/build-guide.mjs --check    build in memory, diff vs. committed output;
//                                           exit 1 if stale (guards against forgetting to rebuild)
//
// The Markdown renderer covers the subset a guide needs: ATX headings, paragraphs, unordered
// and ordered lists (one nesting level), pipe tables, fenced code blocks, blockquotes, horizontal
// rules, inline code / bold / italic / links / images, and raw block-level HTML passthrough
// (a line starting with '<') for embeds. It is NOT full CommonMark — if the guide outgrows this,
// swap in a real parser. Internal page links are emitted relative (./slug.html) so the site works
// at any Pages subpath; external http(s)/mailto and #anchor links pass through unchanged.

import fs from "node:fs";
import { join, resolve, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const SITE = resolve(HERE, "..", "site");
const SRC = join(SITE, "guide-src");
const OUT = join(SITE, "guide");
// The wiki-media manifest is the source of truth for each tutorial clip/still: it maps a
// media id to its kind (clip/image) and manual flag. `<!-- Media: <id> -->` anchors in the
// Markdown resolve against it (renderMedia below). The id IS the flat media basename, so
// there is no separate mapping table.
const MANIFEST = resolve(HERE, "..", "tasks", "wiki-media", "manifest.json");

// ---- Markdown → HTML (dependency-free subset renderer) ------------------------------------

const escapeHtml = (s) =>
  s.replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");

const slugify = (s) =>
  s.toLowerCase().replace(/[^\w]+/g, "-").replace(/^-+|-+$/g, "");

// Inline-code sentinel, built at runtime — a literal NUL byte in this source previously made
// git classify the whole script as binary (no reviewable diffs). NUL cannot occur in
// Markdown text, so the sentinel never collides with real content.
const NUL = String.fromCharCode(0);

// Only these href shapes survive into output; anything else (javascript:, data:, …) renders
// as plain text. Guide content is repo-authored, but the renderer shouldn't be what trusts it.
const isSafeHref = (h) =>
  /^(https?:|mailto:)/.test(h) || /^(#|\.{0,2}\/)/.test(h) || /^[\w][\w\-./]*(#[\w-]*)?$/.test(h);

function resolveGuideHref(href, { knownSlugs, pageAnchors, sourcePage }) {
  if (/^(https?:|mailto:)/.test(href) || /^(#|\.{1,2}\/)/.test(href)) return href;

  if (/^[a-z0-9-]+$/.test(href) && knownSlugs.has(href)) return `./${href}.html`;

  const anchoredSlug = href.match(/^([a-z0-9-]+)#([\w-]*)$/);
  if (anchoredSlug && knownSlugs.has(anchoredSlug[1])) {
    // A cross-page anchor must exist as a heading id on the TARGET page — otherwise the
    // link would 200 but silently land at the top (the guide.spec hash-strip blind spot).
    if (!pageAnchors.get(anchoredSlug[1])?.has(anchoredSlug[2])) {
      throw new Error(`Invalid guide link in ${sourcePage}: ${href} — no heading id "${anchoredSlug[2]}" on page "${anchoredSlug[1]}"`);
    }
    return `./${anchoredSlug[1]}.html#${anchoredSlug[2]}`;
  }

  throw new Error(`Invalid guide link in ${sourcePage}: ${href}`);
}

// Heading ids per page, pre-scanned so cross-page anchors validate at build time.
// Mirrors the heading branch in renderMarkdown() exactly (trailing-# strip + slugify);
// fenced code blocks are skipped so a commented "# heading" in code can't count.
function headingIdsFor(md) {
  const ids = new Set();
  let inFence = false;
  for (const line of md.replace(/\r\n?/g, "\n").split("\n")) {
    if (/^```/.test(line)) { inFence = !inFence; continue; }
    if (inFence) continue;
    const h = line.match(/^(#{1,6})\s+(.*)$/);
    if (h) ids.add(slugify(h[2].trim().replace(/\s+#+\s*$/, "")));
  }
  return ids;
}

// Resolve a `<!-- Media: <id> -->` anchor into the site's lazy embed markup, driven by the
// wiki-media manifest. The embed filenames are the manifest's own `output`/`poster` values —
// exactly what the render runner produces and uploads — so the guide and the pipeline cannot
// silently disagree on a filename. site/guide-media.js (guide) and site/main.js (landing) are
// two separate resolvers that share ONLY this data-clip/data-poster filename convention (their
// behaviour differs: the landing has a global motion toggle, the guide does not); each joins
// the value to the media release at runtime, so the HTML stays binary-free. An unknown id — or
// a filename that isn't a flat release basename — fails the build.
function renderMedia(id, { media, sourcePage }) {
  const item = media.get(id);
  if (!item) {
    throw new Error(`Unknown media id "${id}" in ${sourcePage} — add it to tasks/wiki-media/manifest.json`);
  }
  // The single manual shot (in-game proof) is a real game screenshot captured post-launch;
  // keep an inert anchor so the slot stays documented and nothing broken renders until then.
  if (item.manual) return `<!-- Media (manual, added post-launch): ${escapeHtml(id)} -->`;
  const label = item.purpose ? escapeHtml(item.purpose) : "";
  // The resolver does MEDIA_BASE + value, so each name must be a flat release basename; a path
  // (a stale wiki/tutorial-XX/ prefix) would resolve to a nonexistent release subpath.
  const flat = (name, field) => {
    if (!name) throw new Error(`Media "${id}" in ${sourcePage}: missing manifest ${field}`);
    if (/[\\/]/.test(name)) throw new Error(`Media "${id}" in ${sourcePage}: ${field} "${name}" must be a flat release filename (no path)`);
    return escapeHtml(name);
  };
  if (item.kind === "image") {
    // A still is its own image; the resolver sets img.src from data-poster (= its output file).
    return `<figure class="guide-media"><img class="clip-img" data-poster="${flat(item.output, "output")}" alt="${label}"></figure>`;
  }
  // Default: a clip. `controls` gives the pause/replay mechanism a looping tutorial clip needs
  // (WCAG 2.2.2) — apt for teaching, unlike the landing's ambient clips. The 4:3 dims reserve
  // layout to avoid a shift.
  return `<figure class="guide-media"${label ? ` aria-label="${label}"` : ""}>` +
    `<video class="clip-video" controls loop muted playsinline preload="none" width="1280" height="960" ` +
    `data-clip="${flat(item.output, "output")}" data-poster="${flat(item.poster, "poster")}"></video></figure>`;
}

function renderInline(text, context) {
  // Protect inline code spans first so emphasis/link regexes never touch their contents.
  const codes = [];
  text = text.replace(/`([^`]+)`/g, (_, c) => {
    codes.push(`<code>${escapeHtml(c)}</code>`);
    return `${NUL}${codes.length - 1}${NUL}`;
  });
  text = escapeHtml(text);
  // images before links (both use the [..](..) tail)
  text = text.replace(
    /!\[([^\]]*)\]\(([^)\s]+)(?:\s+&quot;([^&]*)&quot;)?\)/g,
    (_, alt, src, title) => isSafeHref(src)
      ? `<img src="${escapeHtml(src)}" alt="${alt}"${title ? ` title="${title}"` : ""}>`
      : alt,
  );
  text = text.replace(
    /\[([^\]]+)\]\(([^)\s]+)(?:\s+&quot;([^&]*)&quot;)?\)/g,
    (_, t, href, title) => {
      const resolvedHref = resolveGuideHref(href, context);
      return isSafeHref(resolvedHref)
        ? `<a href="${escapeHtml(resolvedHref)}"${title ? ` title="${title}"` : ""}>${t}</a>`
        : t;
    },
  );
  text = text.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");
  text = text.replace(/__([^_]+)__/g, "<strong>$1</strong>");
  text = text.replace(/(^|[^*])\*([^*\s][^*]*)\*/g, "$1<em>$2</em>");
  text = text.replace(/(^|[^\w])_([^_]+)_(?=[^\w]|$)/g, "$1<em>$2</em>");
  // Restore code spans.
  text = text.replace(new RegExp(`${NUL}(\\d+)${NUL}`, "g"), (_, i) => codes[+i]);
  return text;
}

// Parse one list starting at `start`; returns { html, next }. Supports one nesting level
// via indentation (a line indented >= baseIndent + 2 belongs to the previous item).
function parseList(lines, start, context) {
  const baseIndent = lines[start].match(/^(\s*)/)[1].length;
  const ordered = /^\s*\d+\.\s+/.test(lines[start]);
  const items = [];
  let i = start;
  while (i < lines.length) {
    const l = lines[i];
    if (/^\s*$/.test(l)) break;
    const m = l.match(/^(\s*)(?:[-*+]|\d+\.)\s+(.*)$/);
    if (!m) break;
    const indent = m[1].length;
    if (indent < baseIndent) break;
    if (indent >= baseIndent + 2 && items.length) {
      const nested = parseList(lines, i, context);
      items[items.length - 1].children = nested.html;
      i = nested.next;
      continue;
    }
    items.push({ text: m[2], children: "" });
    i++;
  }
  const tag = ordered ? "ol" : "ul";
  const body = items
    .map((it) => `<li>${renderInline(it.text, context)}${it.children ? "\n" + it.children : ""}</li>`)
    .join("\n");
  return { html: `<${tag}>\n${body}\n</${tag}>`, next: i };
}

function parsePipeCells(line) {
  const cells = line.trim().split("|").map((cell) => cell.trim());
  if (cells[0] === "") cells.shift();
  if (cells[cells.length - 1] === "") cells.pop();
  return cells;
}

function isPipeTableStart(lines, start) {
  if (start + 1 >= lines.length || !/^\s*\|/.test(lines[start])) return false;
  const headers = parsePipeCells(lines[start]);
  const separators = parsePipeCells(lines[start + 1]);
  return headers.length > 0 &&
    headers.length === separators.length &&
    /^\s*\|[\s|:-]*$/.test(lines[start + 1]) &&
    separators.every((cell) => /^:?-{3,}:?$/.test(cell.replace(/\s+/g, "")));
}

function parsePipeTable(lines, start, context) {
  const headers = parsePipeCells(lines[start]);
  const rows = [];
  let i = start + 2;
  while (i < lines.length && /^\s*\|/.test(lines[i])) {
    rows.push(parsePipeCells(lines[i]));
    i++;
  }

  const head = headers.map((cell) => `<th>${renderInline(cell, context)}</th>`).join("");
  const body = rows
    .map((row) => `<tr>${row.map((cell) => `<td>${renderInline(cell, context)}</td>`).join("")}</tr>`)
    .join("\n");
  return {
    html: `<table>\n<thead>\n<tr>${head}</tr>\n</thead>\n<tbody>\n${body ? `${body}\n` : ""}</tbody>\n</table>`,
    next: i,
  };
}

// Render Markdown to { html, toc }. `toc` collects h2/h3 for the "On this page" rail.
function renderMarkdown(md, context) {
  const lines = md.replace(/\r\n?/g, "\n").split("\n");
  const out = [];
  const toc = [];
  let i = 0;
  const isBreak = (idx) => {
    const l = lines[idx];
    return (
      /^\s*$/.test(l) ||
      /^(#{1,6})\s/.test(l) ||
      /^```/.test(l) ||
      /^>\s?/.test(l) ||
      /^\s*</.test(l) ||
      /^\s*(?:[-*+]|\d+\.)\s+/.test(l) ||
      /^(-{3,}|\*{3,}|_{3,})\s*$/.test(l) ||
      isPipeTableStart(lines, idx)
    );
  };

  while (i < lines.length) {
    const line = lines[i];
    if (/^\s*$/.test(line)) { i++; continue; }

    const fence = line.match(/^```\s*(\S+)?\s*$/);
    if (fence) {
      i++;
      const buf = [];
      while (i < lines.length && !/^```\s*$/.test(lines[i])) { buf.push(lines[i]); i++; }
      i++; // consume closing fence
      const cls = fence[1] ? ` class="language-${fence[1]}"` : "";
      out.push(`<pre><code${cls}>${escapeHtml(buf.join("\n"))}\n</code></pre>`);
      continue;
    }

    const h = line.match(/^(#{1,6})\s+(.*)$/);
    if (h) {
      const level = h[1].length;
      const text = h[2].trim().replace(/\s+#+\s*$/, "");
      const id = slugify(text);
      if (level === 2 || level === 3) toc.push({ id, text, level });
      out.push(`<h${level} id="${id}">${renderInline(text, context)}</h${level}>`);
      i++;
      continue;
    }

    if (/^(-{3,}|\*{3,}|_{3,})\s*$/.test(line)) { out.push("<hr>"); i++; continue; }

    if (/^>\s?/.test(line)) {
      const buf = [];
      while (i < lines.length && /^>\s?/.test(lines[i])) { buf.push(lines[i].replace(/^>\s?/, "")); i++; }
      out.push(`<blockquote>${renderInline(buf.join(" "), context)}</blockquote>`);
      continue;
    }

    // A media anchor is a comment, so it would otherwise fall into the raw-HTML passthrough
    // below and emit inertly — intercept it FIRST and expand to the manifest-driven embed.
    const mediaComment = line.match(/^\s*<!--\s*Media:\s*([\w-]+)\s*-->\s*$/);
    if (mediaComment) {
      out.push(renderMedia(mediaComment[1], context));
      i++;
      continue;
    }

    if (/^\s*</.test(line)) {
      const buf = [];
      while (i < lines.length && !/^\s*$/.test(lines[i])) { buf.push(lines[i]); i++; }
      out.push(buf.join("\n"));
      continue;
    }

    if (/^\s*(?:[-*+]|\d+\.)\s+/.test(line)) {
      const lst = parseList(lines, i, context);
      out.push(lst.html);
      i = lst.next;
      continue;
    }

    if (isPipeTableStart(lines, i)) {
      const table = parsePipeTable(lines, i, context);
      out.push(table.html);
      i = table.next;
      continue;
    }

    // paragraph: gather until a block break
    const buf = [];
    while (i < lines.length && !isBreak(i)) { buf.push(lines[i]); i++; }
    out.push(`<p>${renderInline(buf.join(" "), context)}</p>`);
  }
  return { html: out.join("\n"), toc };
}

// ---- Page template --------------------------------------------------------------------------

function sidebarHtml(nav, activeSlug) {
  return nav.sections
    .map((sec) => {
      const items = sec.pages
        .map((p) => {
          const active = p.slug === activeSlug;
          return `      <li><a href="./${encodeURIComponent(p.slug)}.html"${active ? ' class="active" aria-current="page"' : ""}>${escapeHtml(p.title)}</a></li>`;
        })
        .join("\n");
      return `    <div class="side-group">\n      <p class="side-label">${escapeHtml(sec.label)}</p>\n      <ul>\n${items}\n      </ul>\n    </div>`;
    })
    .join("\n");
}

function tocHtml(toc) {
  if (!toc.length) return "";
  const items = toc
    .map((t) => `      <li class="lvl-${t.level}"><a href="#${t.id}">${t.text}</a></li>`)
    .join("\n");
  return `    <nav class="toc" aria-label="On this page">\n      <p class="toc-label">On this page</p>\n      <ul>\n${items}\n      </ul>\n    </nav>`;
}

// Wayfinding kicker above the h1 — "TUTORIALS · 3 OF 4" in the landing's section-label
// voice (styling in guide.css; uppercase is CSS-applied).
function kickerHtml(page) {
  const count = page.sectionCount > 1
    ? ` <span class="kicker-count">· ${page.sectionIndex} of ${page.sectionCount}</span>`
    : "";
  return `<p class="guide-kicker">${escapeHtml(page.section)}${count}</p>`;
}

// Prev/next reading path at the end of each page, from flattened nav order.
function pagerHtml(prev, next) {
  if (!prev && !next) return "";
  const side = (page, cls, eyebrow) => page
    ? `        <a class="${cls}" href="./${encodeURIComponent(page.slug)}.html"><span class="pager-eyebrow">${eyebrow}</span><span class="pager-title">${escapeHtml(page.title)}</span></a>`
    : "";
  return `      <nav class="guide-pager" aria-label="Guide pages">
${[side(prev, "pager-prev", "← Previous"), side(next, "pager-next", "Next →")].filter(Boolean).join("\n")}
      </nav>`;
}

function pageHtml({ title, guideHref, sidebar, kicker, content, pager, toc }) {
  return `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>${escapeHtml(title)} — Particle Editor guide</title>
  <!-- Dark before any CSS: color-scheme makes the browser's default canvas dark, and the
       inline rules cover the frames before styles.css arrives — kills the white flash on
       navigation / view transitions. The @view-transition opt-in and the dark backstop across
       the WHOLE view-transition pseudo chain are inlined (not just in styles.css) so a slow
       styles.css can't skip the transition or leave its overlay unstyled (white gap). The
       timed new-page fade-in refinement stays in styles.css. -->
  <meta name="color-scheme" content="dark">
  <style>
    html{background-color:#0b0d12}
    @view-transition{navigation:auto}
    ::view-transition,
    ::view-transition-group(root),
    ::view-transition-image-pair(root),
    ::view-transition-new(root){background-color:#0b0d12}
    ::view-transition-old(root){display:none}
    .topbar{view-transition-name:topbar}
    ::view-transition-old(topbar){display:none}
    ::view-transition-new(topbar){animation:none}
    .motion-toggle{view-transition-name:motion-toggle}
  </style>
  <!-- Inline (timing-safe): when a cross-document view transition runs, mark the root so CSS
       suppresses the entrance fade-up — the transition's new-page fade-in IS the entrance, and re-hiding content
       mid-transition caused the appear→vanish→fade glitch + compositor churn. -->
  <script>addEventListener("pagereveal",(e)=>{if(e.viewTransition)document.documentElement.dataset.vt="1"});</script>
  <!-- Hold this page's first paint (and the incoming view-transition snapshot of it) until the
       main content is parsed, so the transition never captures a partially-rendered frame. -->
  <link rel="expect" href="#guide-main" blocking="render">
  <link rel="icon" href="../favicon.svg" type="image/svg+xml">
  <!-- Fonts: self-hosted (site/fonts/) and preloaded so text paints in the real faces —
       no third-party render-blocking request, no fallback-font swap shifting the nav.
       Font preloads always need the crossorigin attribute, even same-origin. -->
  <link rel="preload" href="../fonts/schibsted-grotesk-latin.woff2" as="font" type="font/woff2" crossorigin>
  <link rel="preload" href="../fonts/ibm-plex-mono-latin-400.woff2" as="font" type="font/woff2" crossorigin>
  <link rel="stylesheet" href="../styles.css">
  <link rel="stylesheet" href="../guide.css">
</head>
<body class="guide">
  <header class="topbar">
    <a class="wordmark" href="../index.html"><img class="wordmark-icon" src="../favicon.svg" alt="" aria-hidden="true">Particle Editor</a>
    <nav class="topnav" aria-label="Primary">
      <a class="btn-primary nav-download" href="https://github.com/DrKnickers/particle-editor/releases/latest" aria-label="Download for Windows">
        <span class="nav-download-icon" aria-hidden="true">↓</span><span class="nav-download-label">Download</span>
      </a>
      <a class="nav-source" href="${guideHref}">Guide</a>
      <a class="nav-source" href="https://github.com/DrKnickers/particle-editor">Source</a>
      <!-- Invisible placeholder matching the landing page's Pause toggle: keeps the nav
           geometry identical across pages so buttons don't shift during the cross-page
           transition. visibility:hidden preserves layout but removes it from tab order
           and assistive tech. -->
      <button type="button" class="motion-toggle" style="visibility:hidden" tabindex="-1" aria-hidden="true">Pause</button>
    </nav>
  </header>
  <div class="guide-layout">
    <aside class="guide-sidebar" aria-label="Guide navigation">
      <p class="side-title">Guide</p>
${sidebar}
    </aside>
    <main class="guide-main" id="guide-main">
      <article class="guide-article">
${kicker}
${content}
      </article>
${pager}
    </main>
${toc}
  </div>
  <!-- Resolve tutorial clips/stills against the media release (data-clip/data-poster →
       MEDIA_BASE); plays clips when scrolled into view, honoring reduced motion. Pages with
       no media are a no-op. Mirrors the landing's main.js minus its motion toggle. -->
  <script type="module" src="../guide-media.js"></script>
</body>
</html>
`;
}

function redirectHtml(firstSlug) {
  return `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="color-scheme" content="dark">
  <style>html{background-color:#0b0d12;color:#9aa1ad;font-family:system-ui}</style>
  <meta http-equiv="refresh" content="0; url=${firstSlug}.html">
  <link rel="canonical" href="${firstSlug}.html">
  <title>Guide — Particle Editor</title>
</head>
<body>
  <p><a href="${firstSlug}.html">Continue to the guide →</a></p>
</body>
</html>
`;
}

// ---- Build ----------------------------------------------------------------------------------

function build() {
  const nav = JSON.parse(fs.readFileSync(join(SRC, "nav.json"), "utf8"));
  // Index the wiki-media manifest so `<!-- Media: id -->` anchors resolve to the right
  // embed kind (and fail loudly on an id the manifest doesn't know).
  const manifest = JSON.parse(fs.readFileSync(MANIFEST, "utf8"));
  const media = new Map(
    (manifest.items ?? []).map((it) => [it.id, {
      kind: it.kind || "clip", manual: !!it.manual, purpose: it.purpose,
      output: it.output, poster: it.poster,
    }]),
  );
  // Flatten with section metadata so each page knows its kicker (section · N of M)
  // and its prev/next neighbours in reading order.
  const pages = nav.sections.flatMap((s) =>
    s.pages.map((p, i) => ({ ...p, section: s.label, sectionIndex: i + 1, sectionCount: s.pages.length })));
  if (!pages.length) throw new Error("nav.json lists no pages");
  const knownSlugs = new Set(pages.map((p) => p.slug));

  const guideHref = `./${pages[0].slug}.html`; // the "Guide" nav link targets the first page

  // Pre-read every page and index its heading ids, so links can validate cross-page
  // anchors during the render pass below.
  const sources = new Map(); // slug -> markdown
  const pageAnchors = new Map(); // slug -> Set<heading id>
  for (const p of pages) {
    const mdPath = join(SRC, `${p.slug}.md`);
    if (!fs.existsSync(mdPath)) throw new Error(`nav.json references ${p.slug} but ${mdPath} is missing`);
    const md = fs.readFileSync(mdPath, "utf8");
    sources.set(p.slug, md);
    pageAnchors.set(p.slug, headingIdsFor(md));
  }

  const outputs = new Map(); // relative path under site/guide -> html
  for (const [idx, p] of pages.entries()) {
    const md = sources.get(p.slug);
    const { html, toc } = renderMarkdown(md, { knownSlugs, pageAnchors, sourcePage: p.slug, media });
    outputs.set(`${p.slug}.html`, pageHtml({
      title: p.title,
      guideHref,
      sidebar: sidebarHtml(nav, p.slug),
      kicker: kickerHtml(p),
      content: html,
      pager: pagerHtml(pages[idx - 1], pages[idx + 1]),
      toc: tocHtml(toc),
    }));
  }
  outputs.set("index.html", redirectHtml(pages[0].slug));
  return outputs;
}

function runCli() {
  const check = process.argv.includes("--check");
  const outputs = build();

  if (check) {
    const drift = [];
    for (const [rel, html] of outputs) {
      const path = join(OUT, rel);
      const current = fs.existsSync(path) ? fs.readFileSync(path, "utf8") : null;
      if (current !== html) drift.push(rel);
    }
    // Orphans: generated pages whose nav.json entry was removed/renamed would otherwise
    // linger deployed forever — flag any .html in site/guide/ the build no longer produces.
    if (fs.existsSync(OUT)) {
      for (const f of fs.readdirSync(OUT)) {
        if (f.endsWith(".html") && !outputs.has(f)) drift.push(`${f} (orphaned — no longer generated; delete it)`);
      }
    }
    if (drift.length) {
      console.error(`guide output is stale — run "node scripts/build-guide.mjs" and commit:\n  ${drift.join("\n  ")}`);
      process.exit(1);
    }
    console.log(`guide up to date (${outputs.size} files)`);
  } else {
    fs.mkdirSync(OUT, { recursive: true });
    for (const [rel, html] of outputs) fs.writeFileSync(join(OUT, rel), html);
    console.log(`wrote ${outputs.size} files to site/guide/`);
  }
}

// Run the build only when invoked as a CLI, so unit tests can import the pure helpers
// (renderMedia/renderMarkdown) without triggering a filesystem build at import time.
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  runCli();
}

export { renderMedia, renderMarkdown, build };
