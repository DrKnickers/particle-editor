// Guide media resolver — the guide's counterpart to the landing page's main.js clip loader,
// minus the motion toggle (guide pages have no Pause control). Tutorial pages embed
//   <video class="clip-video" data-clip="<id>.mp4" data-poster="<id>-poster.jpg">
//   <img   class="clip-img"   data-poster="<id>.png">
// placeholders (emitted by scripts/build-guide.mjs from tasks/wiki-media/manifest.json).
// This joins them to the Pages-served media mirror and plays clips as they scroll into view
// (muted loop), honoring prefers-reduced-motion. The MEDIA_BASE precedence matches main.js:
// window.__MEDIA_BASE__ (tests) → ?media= query param (local preview) → the site's media/
// directory, which the Pages workflow populates from the site-media release at deploy time.
// NEVER default to the release download URL itself: GitHub serves release assets with
// Content-Disposition: attachment, which browsers refuse to play as <video> sources
// (loadstart → stalled, no error — clips simply never start).
const params = new URLSearchParams(location.search);
const override = window.__MEDIA_BASE__ || (params.get("media")?.trim() || null);
let MEDIA_BASE = override ?? "../media/";
if (!MEDIA_BASE.endsWith("/")) MEDIA_BASE += "/";

const reduce = window.matchMedia("(prefers-reduced-motion: reduce)");
const videos = Array.from(document.querySelectorAll(".clip-video"));
const images = Array.from(document.querySelectorAll(".clip-img"));

// The returning-user comparison is meaningful before this script runs: its two labeled
// figures are an ordinary responsive grid. Enhancement overlays the matched images and keeps
// one native range as the only split state, so pointer, touch, and keyboard stay in sync.
for (const comparison of document.querySelectorAll("[data-ui-compare]")) {
  const range = comparison.querySelector(".ui-compare-range");
  if (!(range instanceof HTMLInputElement)) continue;

  const applySplit = () => {
    const split = Math.min(100, Math.max(0, Number(range.value)));
    comparison.style.setProperty("--compare-split", `${split}%`);
    range.setAttribute(
      "aria-valuetext",
      `${split}% GlyphX Particle Editor v1.5, ${100 - split}% current interface`,
    );
  };

  range.addEventListener("input", applySplit);
  for (const button of comparison.querySelectorAll("[data-compare-value]")) {
    button.addEventListener("click", () => {
      range.value = button.dataset.compareValue;
      applySplit();
      range.focus();
    });
  }
  for (const image of comparison.querySelectorAll("[data-compare-layer]")) {
    const errorClass = `has-${image.dataset.compareLayer}-error`;
    const markError = () => comparison.classList.add(errorClass);
    image.addEventListener("error", markError);
    image.addEventListener("load", () => comparison.classList.remove(errorClass));
    if (image.dataset.poster) image.src = MEDIA_BASE + image.dataset.poster;
    if (image.complete && image.naturalWidth === 0) markError();
  }

  applySplit();
  comparison.classList.add("is-enhanced");
}

// Posters first (cheap, avoids a blank frame); stills get their src the same way.
for (const v of videos) if (v.dataset.poster) v.poster = MEDIA_BASE + v.dataset.poster;
for (const img of images)
  if (img.dataset.poster && !img.matches("[data-compare-layer]"))
    img.src = MEDIA_BASE + img.dataset.poster;

// Start/resume one clip when it's in view and motion is allowed. Loads src lazily on first
// start so below-fold clips never fetch until needed (preload="none" + this gate).
function startVideo(v) {
  if (reduce.matches) return; // reduced-motion → the poster stands in
  if (!v.dataset.started) {
    v.dataset.started = "1";
    v.src = MEDIA_BASE + v.dataset.clip;
    v.load();
  }
  v.play().catch((e) => { if (e && e.name !== "NotAllowedError" && e.name !== "AbortError") console.warn("clip failed:", v.dataset.clip, e); });
}

if ("IntersectionObserver" in window) {
  const io = new IntersectionObserver((entries, obs) => {
    for (const e of entries) if (e.isIntersecting) { startVideo(e.target); obs.unobserve(e.target); }
  }, { rootMargin: "200px" });
  videos.forEach((v) => io.observe(v));
} else {
  videos.forEach(startVideo); // no IO → just start them
}
