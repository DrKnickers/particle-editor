/* Guide editor client — plain JS (served at /__editor.js) so no template-literal escaping. */
"use strict";
const $ = (s) => document.querySelector(s);
const pagesSel = $("#pages"), status = $("#status");
const preview = $("#preview"), errbar = $("#errbar"), loadbar = $("#loadbar"), fname = $("#fname"), openfull = $("#openfull");
const diffPanel = $("#diffPanel"), diffSummary = $("#diffSummary"), diffBody = $("#diffBody");
let editor = null, current = null, dirty = false, saveTimer = null, saving = false, ready = false, loadingPage = false;

function setStatus(text, cls) { status.textContent = text; status.className = "pill" + (cls ? " " + cls : ""); }
function showErr(msg) {
  if (msg) { errbar.textContent = "Build error — page not updated:\n" + msg; errbar.style.display = "block"; }
  else { errbar.style.display = "none"; }
}

// ---- media-anchor shim: <!-- Media[ (planned)]: id --> <-> protected [media[-planned]:id] chip
function importShim(md) {
  return md
    .replace(/<!--\s*Media \(planned\):\s*([\w-]+)\s*-->/g, "[media-planned:$1]")
    .replace(/<!--\s*Media:\s*([\w-]+)\s*-->/g, "[media:$1]");
}

// Toast UI over-escapes ASCII punctuation (\-, \~, \., \#, …). build-guide is a SUBSET renderer
// that does NOT process backslash escapes, so any backslash Toast UI adds renders literally.
// De-escape \<punct> back to <punct> — but only OUTSIDE code spans/blocks, where Toast UI never
// adds escapes and a real backslash (e.g. a Windows path in a code span) must be preserved.
// KNOWN HAZARD: a deliberately AUTHORED prose escape (e.g. \* for a literal asterisk) is
// indistinguishable from a Toast-added one and gets stripped too. No guide page uses one
// today, and the server's rendered-diff guard flags any resulting output change on save —
// that guard is the designed mitigation. If authored escapes ever become common, replace
// this blanket pass with tracking of Toast-introduced escapes only.
const ESCAPABLE = new Set("!\"#$%&'()*+,-./:;<=>?@[]^_`{|}~".split(""));
function deEscapePunct(md) {
  // Split so odd indices are code (fenced or inline); leave those verbatim.
  const parts = md.split(/(```[\s\S]*?```|`[^`\n]*`)/g);
  for (let i = 0; i < parts.length; i += 2) {
    let s = parts[i], out = "";
    for (let j = 0; j < s.length; j++) {
      if (s[j] === "\\" && j + 1 < s.length && ESCAPABLE.has(s[j + 1])) { out += s[j + 1]; j++; }
      else out += s[j];
    }
    parts[i] = out;
  }
  return parts.join("");
}

function exportShim(md) {
  let out = md
    // Toast UI serializes widgets as `$$widgetN <token>$$` — unwrap to the bare token.
    // Restricted to OUR media tokens so a literal `$$widget…$$` in authored prose or a
    // code example can never be mangled by the unwrap.
    .replace(/\$\$widget\d+\s+(\[media(?:-planned)?:[\w-]+\])\s*\$\$/g, "$1");
  out = deEscapePunct(out);
  return out
    .replace(/\[media-planned:([\w-]+)\]/g, "<!-- Media (planned): $1 -->")
    .replace(/\[media:([\w-]+)\]/g, "<!-- Media: $1 -->");
}

async function loadPages() {
  const list = await (await fetch("/__api/pages")).json();
  pagesSel.innerHTML = "";
  for (const p of list) {
    const o = document.createElement("option");
    o.value = p.name;
    o.textContent = p.title ? p.title + "  (" + p.name + ")" : p.name;
    pagesSel.appendChild(o);
  }
}

const previewUrl = (name) => "/guide/" + name.replace(/\.md$/, ".html");
const scrollOfPreview = () => { try { return preview.contentWindow.scrollY || 0; } catch (_) { return 0; } };
function loadPreview(url, scroll) {
  preview.onload = () => { if (scroll) { try { preview.contentWindow.scrollTo(0, scroll); } catch (_) {} } };
  preview.src = url + "?t=" + Date.now();
}

// ---- rendered-diff guard: show exactly what changed in the shipped HTML vs the last commit
function renderDiff(d) {
  if (!d || (d.added === 0 && d.removed === 0)) {
    diffSummary.textContent = "Rendered output vs last commit: no change";
    diffPanel.className = "diff-panel clean";
    diffBody.textContent = "";
    diffBody.style.display = "none";
    return;
  }
  diffSummary.textContent = "Rendered output changed vs last commit: +" + d.added + " −" + d.removed + " lines  (click to review)";
  diffPanel.className = "diff-panel changed";
  diffBody.textContent = d.text || "";
}

async function openPage(name) {
  current = name; fname.textContent = name;
  const md = await (await fetch("/__api/page?name=" + encodeURIComponent(name))).text();
  loadingPage = true;
  editor.setMarkdown(importShim(md), false);
  setTimeout(() => { loadingPage = false; }, 60);
  dirty = false; setStatus("saved", "saved"); showErr("");
  openfull.href = previewUrl(name);
  loadPreview(previewUrl(name), 0);
  // show any rendered delta this session has already introduced for the page
  fetch("/__api/diff?name=" + encodeURIComponent(name)).then((r) => r.json()).then(renderDiff).catch(() => {});
}

async function save() {
  if (saving || !current || !ready) return;
  clearTimeout(saveTimer);
  saving = true; setStatus("saving…", "saving");
  const scroll = scrollOfPreview();
  const md = exportShim(editor.getMarkdown());
  const res = await fetch("/__api/save?name=" + encodeURIComponent(current), { method: "POST", body: md });
  const out = await res.json();
  saving = false;
  if (out.ok) {
    dirty = false; setStatus("saved ✓ #" + out.buildId, "saved"); showErr("");
    loadPreview(previewUrl(current), scroll);
    renderDiff(out.diff);
  } else { setStatus("build error", "err"); showErr(out.error || "unknown build error"); }
}

function onEdit() {
  if (!ready || loadingPage) return;
  dirty = true; setStatus("unsaved…", "");
  clearTimeout(saveTimer); saveTimer = setTimeout(save, 800);
}

// diff panel expand/collapse
diffSummary.addEventListener("click", () => {
  diffBody.style.display = diffBody.style.display === "block" ? "none" : "block";
});

// draggable splitter
const splitter = $("#splitter"), left = $("#left");
let dragging = false;
splitter.addEventListener("mousedown", () => { dragging = true; splitter.classList.add("drag"); document.body.style.userSelect = "none"; });
window.addEventListener("mousemove", (e) => { if (!dragging) return; const pct = Math.min(75, Math.max(20, (e.clientX / window.innerWidth) * 100)); left.style.flex = "0 0 " + pct + "%"; });
window.addEventListener("mouseup", () => { dragging = false; splitter.classList.remove("drag"); document.body.style.userSelect = ""; });

document.addEventListener("keydown", (e) => { if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "s") { e.preventDefault(); save(); } });
window.addEventListener("beforeunload", (e) => { if (dirty) { e.preventDefault(); e.returnValue = ""; } });
pagesSel.addEventListener("change", async () => { if (dirty) await save(); openPage(pagesSel.value); });

(async function init() {
  if (!window.toastui || !window.toastui.Editor) {
    loadbar.style.display = "block";
    loadbar.textContent = "Could not load the rich editor from the CDN (offline?). Tell Claude and I'll switch to the source editor.";
    setStatus("editor unavailable", "err");
    return;
  }
  const mediaWidget = {
    rule: /\[media(?:-planned)?:[\w-]+\]/,
    toDOM(text) {
      const planned = text.indexOf("-planned") !== -1;
      const id = text.replace(/\[media(?:-planned)?:/, "").replace(/\]$/, "");
      const el = document.createElement("span");
      el.className = "media-chip" + (planned ? " planned" : "");
      el.textContent = (planned ? "🎬 planned · " : "🎬 ") + id;
      return el;
    },
  };
  editor = new toastui.Editor({
    el: $("#editorHost"),
    height: "100%",
    theme: "dark",
    initialEditType: "wysiwyg",
    previewStyle: "vertical",
    hideModeSwitch: false,
    usageStatistics: false,
    widgetRules: [mediaWidget],
    toolbarItems: [
      ["heading", "bold", "italic", "strike"],
      ["hr", "quote"],
      ["ul", "ol"],
      ["table", "link"],
      ["code", "codeblock"],
    ],
    events: { change: onEdit },
  });
  await loadPages();
  ready = true;
  const start = new URLSearchParams(location.search).get("page");
  const name = (start && [...pagesSel.options].some((o) => o.value === start)) ? start : pagesSel.options[0]?.value;
  if (name) { pagesSel.value = name; openPage(name); }
})();
