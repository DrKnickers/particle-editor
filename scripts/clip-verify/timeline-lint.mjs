#!/usr/bin/env node
import { readFileSync, readdirSync, statSync, existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { join, dirname } from "node:path";

const USAGE = "usage: node timeline-lint.mjs [--no-ref-check] <timeline.json> [more.json ...]";

// Static preflight for --record timelines: turns the durable authoring traps
// (the clip-author skill's reference notes and its hard rules) into
// mechanical checks so a mis-authored timeline fails HERE, not after a render.
// Errors = the render would wedge/misbehave; warnings = probably wrong, but a
// legitimate timeline could trip them. This complements (never replaces) the
// exe's own preflight + the post-render verify gates.

const CUMULATIVE_KINDS = new Set([
  "spawner/trigger",
  "emitters/add-root",
  "emitters/add-lifetime-child",
  "emitters/add-death-child",
]);
const ADD_EMITTER_KINDS = new Set([
  "emitters/add-root",
  "emitters/add-lifetime-child",
  "emitters/add-death-child",
]);
const ALIASED_CHANNELS = new Set(["green", "blue", "alpha"]);

// The ui/* record allowlist (ClipTimeline.h IsAllowedRecordKind — kept in sync by
// a test that greps the header). Any OTHER kind, ui/ or not, fails the parse with
// exit 2 (ClipTimeline.h enforces IsAllowedRecordKind on every at-event), so a
// typo'd ui kind is NOT a silent no-op — but catching it here is still cheaper
// than a render round-trip.
export const KNOWN_UI_KINDS = new Set([
  "ui/show-panel",
  "ui/open-picker",
  "ui/set-theme",
  "ui/focus-channel",
  "ui/reveal-curve-channel",
  "ui/select-key",
  "ui/pose-drag",
  "ui/set-picker-search",
  "ui/picker-collapse",
]);

function refOf(key) {
  return key?.target?.kind === "element" ? key.target.ref : null;
}

// Scan the web source for every testid the app can render, so a timeline's
// `testid:` refs can be checked before a render. Testids appear as
// data-testid= / testid= / testId= with a string literal or a template literal
// (whose static prefix legitimizes dynamic ids like `emitter-row:${id}`).
// Test files are EXCLUDED — a test-only id is not clickable in the real app.
const TESTID_RE = /(?:data-)?test[iI]d=(?:"([^"]+)"|\{`([^`]+)`\})/g;
// Multiline/conditional forms too: data-testid={ testId ? `${testId}-option-${v}` : undefined }
// — any template literal inside a braced testid attribute value (dotall, bounded
// at the first backtick pair so it can't swallow unrelated code).
const TESTID_TPL_RE = /(?:data-)?test[iI]d=\{[^{}`]*`([^`]+)`/gs;

function addTemplate(tpl, exact, prefixes, infixes) {
  const cut = tpl.indexOf("${");
  if (cut < 0) { exact.add(tpl); return; }
  if (cut > 0) { prefixes.add(tpl.slice(0, cut)); return; }
  // leading placeholder (`${testId}-option-...`): no usable prefix — keep the
  // static segments between placeholders as infixes (min length guards noise)
  for (const seg of tpl.split(/\$\{[^}]*\}/)) {
    if (seg.length >= 4) infixes.add(seg);
  }
}

export function collectTestids(srcDir) {
  const exact = new Set();
  const prefixes = new Set();
  const infixes = new Set();
  const walk = (dir) => {
    for (const name of readdirSync(dir)) {
      if (name === "node_modules" || name === "__tests__") continue;
      const p = join(dir, name);
      if (statSync(p).isDirectory()) { walk(p); continue; }
      if (!/\.(ts|tsx)$/.test(name) || /\.test\./.test(name)) continue;
      const text = readFileSync(p, "utf8");
      for (const m of text.matchAll(TESTID_RE)) {
        if (m[1] != null) exact.add(m[1]);
        else addTemplate(m[2], exact, prefixes, infixes);
      }
      for (const m of text.matchAll(TESTID_TPL_RE)) addTemplate(m[1], exact, prefixes, infixes);
    }
  };
  walk(srcDir);
  return { exact, prefixes: [...prefixes], infixes: [...infixes] };
}

function testidKnown(id, known) {
  if (!known) return true;
  if (known.exact.has(id)) return true;
  if (known.prefixes.some((p) => id.startsWith(p))) return true;
  return (known.infixes ?? []).some((s) => id.includes(s));
}

export function lintTimeline(timeline, opts = {}) {
  const { knownTestids = null } = opts;
  const errors = [];
  const warnings = [];
  const err = (rule, msg) => errors.push({ rule, msg });
  const warn = (rule, msg) => warnings.push({ rule, msg });

  const tracks = Array.isArray(timeline?.tracks) ? timeline.tracks : [];
  const atEvents = tracks
    .filter((t) => typeof t?.at === "number" && typeof t?.kind === "string")
    .slice()
    .sort((a, b) => a.at - b.at);
  const cursorTracks = tracks.filter((t) => Array.isArray(t?.cursor));

  if (typeof timeline?.fps === "number" && 60 % timeline.fps !== 0) {
    err("fps", `fps ${timeline.fps} must divide 60`);
  }

  // --- ui/* kinds: anything off the allowlist fails the parse (exit 2) ---
  for (const ev of atEvents) {
    if (ev.kind.startsWith("ui/") && !KNOWN_UI_KINDS.has(ev.kind))
      err("ui-kind", `unknown ui kind '${ev.kind}' at ${ev.at} — not on the --record allowlist ` +
        `(ClipTimeline.h); the RENDER would reject it at parse (its exit 2 — this linter exits 1). ` +
        `Known: ${[...KNOWN_UI_KINDS].join(", ")}`);
  }

  // --- file/save confinement (mirrors the exe preflight, but pre-render) ---
  for (const ev of atEvents) {
    if (ev.kind !== "file/save") continue;
    if (typeof timeline.saveRoot !== "string" || timeline.saveRoot.length === 0)
      err("save-root", `file/save at ${ev.at} but the timeline has no top-level saveRoot`);
    if (typeof ev.params?.path !== "string" || ev.params.path.length === 0)
      err("save-root", `file/save at ${ev.at} has no params.path (dialog fallback is not record-safe)`);
  }

  // --- new-emitter G/B/A track alias (renders white without an unlock) ---
  const firstAdd = atEvents.find((e) => ADD_EMITTER_KINDS.has(e.kind));
  if (firstAdd) {
    for (const ev of atEvents) {
      if (ev.at <= firstAdd.at) continue;
      if (ev.kind !== "emitters/set-track-key" && ev.kind !== "emitters/add-track-key") continue;
      const track = ev.params?.track;
      if (!ALIASED_CHANNELS.has(track)) continue;
      const unlocked = atEvents.some(
        (u) => u.kind === "emitters/set-track-lock" && u.params?.channel === track &&
               u.at > firstAdd.at && u.at <= ev.at,
      );
      if (!unlocked)
        err("track-lock", `${ev.kind} on '${track}' at ${ev.at} after ${firstAdd.kind} at ${firstAdd.at} with no ` +
          `emitters/set-track-lock {channel:"${track}"} between them — a NEW emitter aliases G/B/A onto Red ` +
          `(every per-channel write funnels into one track; renders white). Heuristic: ids aren't tracked, ` +
          `so if the write targets a pre-existing emitter, unlock anyway or reorder before the add.`);
    }
  }

  // --- lifetime vs clip end (dim-tail loop seam) ---
  // An instance is BORN at each spawner/trigger (manual mode: spawner/start only
  // commits config — no birth) or at a non-manual start. Only the LAST birth's
  // expiry matters for the tail: earlier instances dying mid-clip is the normal
  // repeated-spawn pattern.
  if (typeof timeline?.durationMs === "number") {
    const starts = atEvents.filter((e) => e.kind === "spawner/start");
    const configAt = (t) => {
      let cfg = null;
      for (const s of starts) { if (s.at <= t) cfg = s; else break; }
      return cfg;
    };
    const births = [];
    for (const trig of atEvents.filter((e) => e.kind === "spawner/trigger")) {
      const cfg = configAt(trig.at);
      const life = cfg?.params?.maxLifetimeSec;
      if (typeof life === "number") births.push({ at: trig.at, life });
    }
    for (const s of starts) {
      if (s.params?.mode === "manual") continue; // config only; trigger births it
      const life = s.params?.maxLifetimeSec;
      if (typeof life === "number") births.push({ at: s.at, life });
    }
    if (births.length > 0) {
      const lastExpiry = Math.max(...births.map((b) => b.at + b.life * 1000));
      if (lastExpiry < timeline.durationMs)
        warn("lifetime", `the LAST spawned instance expires at ~${lastExpiry}ms, before the clip ends ` +
          `(${timeline.durationMs}ms) — the tail goes dim, which breaks a hard-cut loop seam. Fine only if intended.`);
    }
  }

  // --- cursor track checks ---
  if (cursorTracks.length > 1) err("cursor-track", `${cursorTracks.length} cursor tracks; a timeline allows at most one`);
  const keys = cursorTracks.length === 1 ? cursorTracks[0].cursor : [];

  let sawTarget = false, sawLiteral = false;
  let prevT = -Infinity;
  for (const k of keys) {
    if (k?.target) sawTarget = true; else sawLiteral = true;
    if (typeof k?.t === "number") {
      if (k.t < prevT) err("cursor-order", `cursor key at t=${k.t} is out of order (previous t=${prevT})`);
      prevT = k.t;
    }
  }
  if (sawTarget && sawLiteral) err("cursor-mixed", "cursor track mixes target keys and literal x/y keys (the parser rejects mixing)");

  // Focused-channel state over time (panel boots red-focused).
  const focusEvents = atEvents.filter((e) => e.kind === "ui/focus-channel" && typeof e.params?.channel === "string");
  // STRICTLY before: the runner posts the cursor tick BEFORE same-frame at-events
  // (ClipRunner), so a focus event at exactly the cursor key's t applies one frame
  // too late for that key's resolution.
  const focusedAt = (t) => {
    let ch = "red";
    for (const f of focusEvents) { if (f.at < t) ch = f.params.channel; else break; }
    return ch;
  };
  // Atlas dock open state over time.
  const panelEvents = atEvents.filter((e) => e.kind === "ui/show-panel");
  const atlasOpenSince = (t) => {
    let since = null;
    for (const p of panelEvents) {
      if (p.at > t) break;
      since = p.params?.panel === "atlas" ? p.at : null;
    }
    return since;
  };
  // Strictly before, same reasoning as focusedAt: a same-timestamp select
  // applies after the cursor tick for that frame.
  const hasIndexSelectBefore = (t) =>
    atEvents.some((e) => e.kind === "ui/select-key" && e.params?.track === "index" && e.at < t);

  for (let i = 0; i < keys.length; i++) {
    const k = keys[i];
    const ref = refOf(k);
    const press = k?.press === true;
    const activate = k?.activate === true;

    // Press TRANSITION check: press steps from the UPCOMING key while position
    // eases from the PREVIOUS one, so on a false->true edge the pointerdown fires
    // while the cursor is still at (or just leaving) the previous key's target.
    const prev = i > 0 ? keys[i - 1] : null;
    if (press && (!prev || prev.press !== true)) {
      const prevRef = prev ? refOf(prev) : null;
      if (prevRef && prevRef.startsWith("curve-key:"))
        err("curve-key-press", `press engages at t=${k.t} while the cursor departs '${prevRef}' — the pointerdown ` +
          `lands on the curve key it is leaving (press steps from the upcoming key; position from the previous). ` +
          `Insert a non-press travel key off the curve key first.`);
      if (prevRef && prevRef.startsWith("testid:spinner-"))
        err("spinner-press", `press engages at t=${k.t} while the cursor departs '${prevRef}' — the pointerdown ` +
          `lands on the spinner it is leaving. Insert a non-press travel key first.`);
      const sameTarget = prev != null && (
        (prevRef != null && prevRef === ref) ||
        (prev.target?.kind === "point" && k.target?.kind === "point" &&
         prev.target.x === k.target.x && prev.target.y === k.target.y));
      if (prev != null && !sameTarget)
        warn("press-transition", `press engages at t=${k.t} mid-travel from '${prevRef ?? "point"}' toward ` +
          `'${ref ?? "point"}' — the pointerdown lands near the departure target. Add a non-press approach key ` +
          `on the press target first (the committed pattern).`);
    }

    if (ref) {
      // Unknown testid: a hover would SILENTLY park the cursor (only presses fail
      // loud at render). Warning, not error — the scan can't see ids passed
      // through variable props.
      if (ref.startsWith("testid:") && !testidKnown(ref.slice(7), knownTestids))
        warn("testid-unknown", `cursor key t=${k.t} targets '${ref}' but no such testid is in the web source — ` +
          `typo (a hover silently parks; a press exits 3), or an id passed via a variable prop the scan can't see`);

      // The two never-press rules (#517: a press dispatches REAL pointer events).
      if (ref.startsWith("curve-key:") && (press || activate))
        err("curve-key-press", `cursor key t=${k.t} ${press ? "presses" : "activates"} '${ref}' — NEVER press/activate a ` +
          `curve key (arms CurveEditor's pointer-capture drag, which never gets its pointerup and wedges the focus ` +
          `layer). Hover the ref; change values with ui/select-key + set-track-key/tween.`);
      if (ref.startsWith("testid:spinner-") && (press || activate))
        err("spinner-press", `cursor key t=${k.t} presses '${ref}' — NEVER press a spinner (real hold-repeat races ` +
          `scripted steps). Hover beside it while scripted set-properties/set-track-key steps run.`);

      // Channel focus precondition: curve-key/channel-row only resolve when focused.
      const m = ref.match(/^(?:curve-key|channel-row):([a-z]+)/);
      if (m && focusedAt(k.t) !== m[1]) {
        const msg = `cursor key t=${k.t} targets '${ref}' but the focused channel at that time is ` +
          `'${focusedAt(k.t)}' — the element won't resolve. Fire ui/focus-channel {channel:"${m[1]}"} first.`;
        if (press) err("focus-order", msg); else warn("focus-order", msg + " (hover-only: benign transit, but likely a mistake)");
      }

      // Atlas tile presses need the dock open + settled, and a selected index key.
      if (ref.startsWith("atlas-tile:") && press) {
        const since = atlasOpenSince(k.t);
        if (since == null)
          err("atlas-order", `cursor key t=${k.t} presses '${ref}' but the atlas dock isn't open — ` +
            `ui/show-panel {panel:"atlas"} must fire (and settle ~1s) first`);
        else if (k.t - since < 800)
          warn("atlas-order", `cursor key t=${k.t} presses '${ref}' only ${k.t - since}ms after the atlas dock opened — ` +
            `leave ~1s for the grid to mount + the slide to finish`);
        if (activate && !hasIndexSelectBefore(k.t))
          warn("atlas-select", `activate click on '${ref}' at t=${k.t} with no prior ui/select-key {track:"index"} — ` +
            `the tile click applies to the SELECTED index key, so the select is load-bearing`);
      }
    }

    // Activation pairing: the click fires on release, and only if the release
    // lands back on the armed element — the release key should stay on-target.
    if (press && activate) {
      const rel = keys.slice(i + 1).find((n) => n.press !== true);
      if (!rel)
        warn("activate-pair", `activate press at t=${k.t} never releases — no click will fire`);
      else if (refOf(rel) !== ref || rel.activate !== true)
        warn("activate-pair", `activate press at t=${k.t} on '${ref ?? "point"}' releases at t=${rel.t} on ` +
          `'${refOf(rel) ?? "point"}'${rel.activate === true ? "" : " without activate:true"} — a release off the ` +
          `armed element cancels the click (keep down+release on the same target, both with activate)`);
    }

    // Double-fire: a real click near a scripted cumulative action.
    if (press && activate) {
      for (const ev of atEvents) {
        if (!CUMULATIVE_KINDS.has(ev.kind)) continue;
        if (Math.abs(ev.at - k.t) <= 800)
          warn("double-fire", `activate press at t=${k.t} within 800ms of scripted ${ev.kind} at ${ev.at} — if the ` +
            `click performs the same action it fires twice. Cumulative actions get ONE mode per beat.`);
      }
    }
  }

  return { errors, warnings };
}

function main(argv) {
  const noRefCheck = argv.includes("--no-ref-check");
  const paths = argv.filter((a) => a !== "--no-ref-check");
  if (paths.length === 0 || paths.includes("--help")) {
    console.log(USAGE);
    process.exit(paths.length === 0 ? 2 : 0);
  }
  let knownTestids = null;
  if (!noRefCheck) {
    const webSrc = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "web", "apps", "editor", "src");
    if (existsSync(webSrc)) knownTestids = collectTestids(webSrc);
  }
  let failed = false;
  for (const path of paths) {
    let timeline;
    try {
      timeline = JSON.parse(readFileSync(path, "utf8"));
    } catch (e) {
      console.error(`${path}: ${e.message}`);
      failed = true;
      continue;
    }
    const { errors, warnings } = lintTimeline(timeline, { knownTestids });
    for (const w of warnings) console.warn(`${path}: WARN [${w.rule}] ${w.msg}`);
    for (const e of errors) console.error(`${path}: ERROR [${e.rule}] ${e.msg}`);
    if (errors.length > 0) failed = true;
    else console.log(`${path}: OK (${warnings.length} warning${warnings.length === 1 ? "" : "s"})`);
  }
  process.exit(failed ? 1 : 0);
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2));
}
