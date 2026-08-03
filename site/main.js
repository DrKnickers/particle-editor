// Particle Editor landing — page-owned motion. See the spec §5 + the plan's motion model.
const params = new URLSearchParams(location.search);
const override = window.__MEDIA_BASE__ || (params.get("media")?.trim() || null);
let MEDIA_BASE = override ?? "https://github.com/DrKnickers/particle-editor/releases/download/site-media/";
if (!MEDIA_BASE.endsWith("/")) MEDIA_BASE += "/";
const mediaVersion = params.get("v")?.trim() || null;
const mediaUrl = (filename) => {
  const url = MEDIA_BASE + filename;
  return mediaVersion ? `${url}${url.includes("?") ? "&" : "?"}v=${encodeURIComponent(mediaVersion)}` : url;
};

const themeComparisons = Array.from(
  document.querySelectorAll("[data-theme-compare]"),
);

function loadThemeComparison(comparison) {
  if (comparison.dataset.loaded) return;
  comparison.dataset.loaded = "1";
  for (const image of comparison.querySelectorAll("[data-theme-layer]")) {
    const failureClass = `has-${image.dataset.themeLayer}-error`;
    image.addEventListener(
      "error",
      () => comparison.classList.add(failureClass),
      { once: true },
    );
    if (image.dataset.poster) image.src = mediaUrl(image.dataset.poster);
  }
}

for (const comparison of themeComparisons) {
  const range = comparison.querySelector(".theme-compare-range");
  if (!(range instanceof HTMLInputElement)) continue;

  const applySplit = () => {
    const dark = Math.min(100, Math.max(0, Number(range.value)));
    comparison.style.setProperty("--theme-split", `${dark}%`);
    range.setAttribute(
      "aria-valuetext",
      `${dark}% dark interface, ${100 - dark}% light interface`,
    );
  };

  range.addEventListener("input", applySplit);
  applySplit();
  comparison.classList.add("is-enhanced");
}

if ("IntersectionObserver" in window) {
  const comparisonObserver = new IntersectionObserver((entries, observer) => {
    for (const entry of entries) {
      if (!entry.isIntersecting) continue;
      loadThemeComparison(entry.target);
      observer.unobserve(entry.target);
    }
  }, { rootMargin: "200px" });
  themeComparisons.forEach((comparison) => comparisonObserver.observe(comparison));
} else {
  themeComparisons.forEach(loadThemeComparison);
}

const mq = window.matchMedia("(prefers-reduced-motion: reduce)");
let userChoice = null; // null = follow OS preference; boolean = explicit user choice
let motionOn = userChoice ?? !mq.matches;
const eligible = new Set();            // eager or scrolled-into-view → may play when motionOn

const videos = Array.from(document.querySelectorAll(".clip-video"));
const images = Array.from(document.querySelectorAll(".clip-img"));
const toggle = document.getElementById("motion-toggle");

// Posters first (cheap, avoids a blank frame); images get their src too.
for (const v of videos) if (v.dataset.poster) v.poster = mediaUrl(v.dataset.poster);
for (const img of images) if (img.dataset.poster) img.src = mediaUrl(img.dataset.poster);

// Start/resume one video — ONLY when motion is on. Loads src lazily on first start.
function startVideo(v) {
  if (!motionOn) return;               // reduced-motion / paused → poster stands in
  if (!v.dataset.started) {
    v.dataset.started = "1";
    v.src = mediaUrl(v.dataset.clip);
    v.load();
  }
  v.play().catch((e) => { if (e && e.name !== "NotAllowedError" && e.name !== "AbortError") console.warn("clip failed:", v.dataset.clip, e); });
}

// Mark eligible (eager or scrolled in) and start if motion is on.
function makeEligible(v) { eligible.add(v); startVideo(v); }

// Hero eager; below-fold lazy via IntersectionObserver. IO records eligibility even when
// motion is off, so a later "Play motion" starts the in-view clips without loading the
// ones still below the fold.
const eager = videos.filter(v => v.dataset.eager === "true");
const lazy  = videos.filter(v => v.dataset.eager !== "true");
eager.forEach(makeEligible);
if ("IntersectionObserver" in window) {
  const io = new IntersectionObserver((entries, obs) => {
    for (const e of entries) if (e.isIntersecting) { makeEligible(e.target); obs.unobserve(e.target); }
  }, { rootMargin: "200px" });
  lazy.forEach(v => io.observe(v));
} else {
  lazy.forEach(makeEligible);          // no IO → treat all as eligible
}

// Reflect motion state in the DOM + the control. Visible label stays compact;
// aria-label/title carry the full action.
function applyMotion() {
  toggle.textContent = motionOn ? "Pause" : "Play";
  const motionLabel = motionOn ? "Pause motion" : "Play motion";
  toggle.setAttribute("aria-label", motionLabel);
  toggle.title = motionLabel;
  if (motionOn) { for (const v of eligible) startVideo(v); }
  else { for (const v of videos) v.pause(); }
}
toggle.addEventListener("click", () => { userChoice = !motionOn; motionOn = userChoice; applyMotion(); });
mq.addEventListener("change", (e) => { if (userChoice === null) { motionOn = !e.matches; applyMotion(); } });
applyMotion();                         // reflect initial state (esp. "Play motion" under reduce)

// Entrance motion is pure CSS now (styles.css "Motion" block) — this module previously
// tagged .reveal + flipped body.loaded, but a network-fetched script mutating every
// element's opacity AFTER first paint caused appear→vanish→fade on arrival and forced a
// whole-page style invalidation mid view-transition (intermittent white frame).
