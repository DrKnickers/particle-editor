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
// and ordered lists (one nesting level), fenced code blocks, blockquotes, horizontal rules,
// inline code / bold / italic / links / images, and raw block-level HTML passthrough (a line
// starting with '<') for embeds. It is NOT full CommonMark — if the guide outgrows this,
// swap in a real parser. All output links are relative, so the site works at any Pages subpath.

import fs from "node:fs";
import { join, resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const SITE = resolve(HERE, "..", "site");
const SRC = join(SITE, "guide-src");
const OUT = join(SITE, "guide");

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

function renderInline(text) {
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
    (_, t, href, title) => isSafeHref(href)
      ? `<a href="${escapeHtml(href)}"${title ? ` title="${title}"` : ""}>${t}</a>`
      : t,
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
function parseList(lines, start) {
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
      const nested = parseList(lines, i);
      items[items.length - 1].children = nested.html;
      i = nested.next;
      continue;
    }
    items.push({ text: m[2], children: "" });
    i++;
  }
  const tag = ordered ? "ol" : "ul";
  const body = items
    .map((it) => `<li>${renderInline(it.text)}${it.children ? "\n" + it.children : ""}</li>`)
    .join("\n");
  return { html: `<${tag}>\n${body}\n</${tag}>`, next: i };
}

// Render Markdown to { html, toc }. `toc` collects h2/h3 for the "On this page" rail.
function renderMarkdown(md) {
  const lines = md.replace(/\r\n?/g, "\n").split("\n");
  const out = [];
  const toc = [];
  let i = 0;
  const isBreak = (l) =>
    /^\s*$/.test(l) ||
    /^(#{1,6})\s/.test(l) ||
    /^```/.test(l) ||
    /^>\s?/.test(l) ||
    /^\s*</.test(l) ||
    /^\s*(?:[-*+]|\d+\.)\s+/.test(l) ||
    /^(-{3,}|\*{3,}|_{3,})\s*$/.test(l);

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
      out.push(`<h${level} id="${id}">${renderInline(text)}</h${level}>`);
      i++;
      continue;
    }

    if (/^(-{3,}|\*{3,}|_{3,})\s*$/.test(line)) { out.push("<hr>"); i++; continue; }

    if (/^>\s?/.test(line)) {
      const buf = [];
      while (i < lines.length && /^>\s?/.test(lines[i])) { buf.push(lines[i].replace(/^>\s?/, "")); i++; }
      out.push(`<blockquote>${renderInline(buf.join(" "))}</blockquote>`);
      continue;
    }

    if (/^\s*</.test(line)) {
      const buf = [];
      while (i < lines.length && !/^\s*$/.test(lines[i])) { buf.push(lines[i]); i++; }
      out.push(buf.join("\n"));
      continue;
    }

    if (/^\s*(?:[-*+]|\d+\.)\s+/.test(line)) {
      const lst = parseList(lines, i);
      out.push(lst.html);
      i = lst.next;
      continue;
    }

    // paragraph: gather until a block break
    const buf = [];
    while (i < lines.length && !isBreak(lines[i])) { buf.push(lines[i]); i++; }
    out.push(`<p>${renderInline(buf.join(" "))}</p>`);
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

function pageHtml({ title, guideHref, sidebar, content, toc }) {
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
${content}
      </article>
    </main>
${toc}
  </div>
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
  const pages = nav.sections.flatMap((s) => s.pages);
  if (!pages.length) throw new Error("nav.json lists no pages");

  const guideHref = `./${pages[0].slug}.html`; // the "Guide" nav link targets the first page
  const outputs = new Map(); // relative path under site/guide -> html
  for (const p of pages) {
    const mdPath = join(SRC, `${p.slug}.md`);
    if (!fs.existsSync(mdPath)) throw new Error(`nav.json references ${p.slug} but ${mdPath} is missing`);
    const md = fs.readFileSync(mdPath, "utf8");
    const { html, toc } = renderMarkdown(md);
    outputs.set(`${p.slug}.html`, pageHtml({
      title: p.title,
      guideHref,
      sidebar: sidebarHtml(nav, p.slug),
      content: html,
      toc: tocHtml(toc),
    }));
  }
  outputs.set("index.html", redirectHtml(pages[0].slug));
  return outputs;
}

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
