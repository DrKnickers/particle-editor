// Guide media resolver — the guide's counterpart to the landing page's main.js clip loader,
// minus the motion toggle (guide pages have no Pause control). Tutorial pages embed
//   <video class="clip-video" data-clip="<id>.mp4" data-poster="<id>-poster.jpg">
//   <img   class="clip-img"   data-poster="<id>.png">
// placeholders (emitted by scripts/build-guide.mjs from tasks/wiki-media/manifest.json).
// This joins them to the media release and plays clips as they scroll into view (muted loop),
// honoring prefers-reduced-motion. The MEDIA_BASE precedence matches main.js exactly:
// window.__MEDIA_BASE__ (tests) → ?media= query param (local/VPS preview) → the release URL.
const params = new URLSearchParams(location.search);
const override = window.__MEDIA_BASE__ || (params.get("media")?.trim() || null);
let MEDIA_BASE = override ?? "https://github.com/DrKnickers/particle-editor/releases/download/site-media/";
if (!MEDIA_BASE.endsWith("/")) MEDIA_BASE += "/";

const reduce = window.matchMedia("(prefers-reduced-motion: reduce)");
const videos = Array.from(document.querySelectorAll(".clip-video"));
const images = Array.from(document.querySelectorAll(".clip-img"));

// Posters first (cheap, avoids a blank frame); stills get their src the same way.
for (const v of videos) if (v.dataset.poster) v.poster = MEDIA_BASE + v.dataset.poster;
for (const img of images) if (img.dataset.poster) img.src = MEDIA_BASE + img.dataset.poster;

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
