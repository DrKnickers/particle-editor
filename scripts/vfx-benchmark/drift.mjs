// The drift floor and the candidate windows (plan 3.5). Pure: the base's and
// the case's telemetry, selected frames and SSIMs in; eligibility and the
// candidate windows the owner validates in the blind pass out.
//
// Images align on the stride frames both runs captured (plus landmark frames
// that coincide); telemetry aligns at every frame of the union of both review
// windows, the shorter natural-end run zero-padded past its end. A
// permutation case maps the base's per-emitter counts through the recipe's
// position map first, so renumbering alone never counts as a difference.

// [instances, emitters, particles, perEmitter] per frame; the RNG fields excluded.
export function projection(lines, map = null) {
  const out = new Map();
  for (const l of lines) {
    let per = l.perEmitter ?? [];
    if (map) { const m = new Array(per.length).fill(0); per.forEach((v, i) => { m[map[i]] = v; }); per = m; }
    out.set(l.frame, [l.instances, l.emitters, l.particles, per]);
  }
  return out;
}

const zero = (width) => [0, 0, 0, new Array(width).fill(0)];
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

// Frames at which the projections differ, over the union review window.
export function telemetryDiff(baseLines, caseLines, { baseWindow, caseWindow, map = null }) {
  const b = projection(baseLines, map), c = projection(caseLines);
  const width = Math.max(baseLines[0]?.perEmitter?.length ?? 0, caseLines[0]?.perEmitter?.length ?? 0);
  const first = Math.min(baseWindow[0], caseWindow[0]), last = Math.max(baseWindow[1], caseWindow[1]);
  const frames = [];
  for (let f = first; f <= last; f++) if (!same(b.get(f) ?? zero(width), c.get(f) ?? zero(width))) frames.push(f);
  return frames;
}

// Image frames both runs captured: common stride frames, plus landmark frames when they coincide.
export function alignedImageFrames(baseCapture, caseCapture) {
  const frames = (c) => new Set(c.frames.map((x) => x.frame));
  const b = frames(baseCapture), c = frames(caseCapture);
  return [...b].filter((f) => c.has(f)).sort((x, y) => x - y);
}

// input: { recipe, base: { capture, lines }, case: { capture, lines }, ssim: { frame: value }, inspectDiffEmpty, subjective, target }
export function driftFloor(input) {
  const { recipe, base, ssim } = input;
  const kase = input.case;
  const map = recipe.kind === "permutation" ? recipe.map : null;
  const aligned = alignedImageFrames(base.capture, kase.capture);
  const changedImages = aligned.filter((f) => ssim[f] !== undefined && ssim[f] < 1);
  const changedTelemetry = telemetryDiff(base.lines, kase.lines, { baseWindow: base.capture.lifecycle.reviewWindow, caseWindow: kase.capture.lifecycle.reviewWindow, map });
  const visual = changedImages.length > 0, telemetry = changedTelemetry.length > 0, inspect = !input.inspectDiffEmpty;
  const refusals = [], warnings = [];
  if (!visual && !telemetry && !inspect) refusals.push("no drift: the case is not eligible");
  if (recipe.kind === "permutation" && telemetry) refusals.push(`the mapped telemetry differs from the base at frames ${changedTelemetry.slice(0, 5).join(", ")}`);
  if (input.delayTarget !== undefined) {
    const at = input.delayTarget;
    const b = base.capture.lifecycle.emitters[at], c = kase.capture.lifecycle.emitters[at];
    if (!(c.firstActive !== null && b.firstActive !== null && c.firstActive > b.firstActive)) refusals.push("the delayed emitter does not start later than in the base");
    if (!(c.peakParticles > 0)) refusals.push("the delayed emitter's emission vanished");
  }
  if (input.subjective && !visual) warnings.push("no aligned visual drift: the owner's blind pass decides");
  const caseSelected = new Set(kase.capture.frames.map((x) => x.frame));
  let candidateImage = [...new Set([...changedImages, ...changedTelemetry.filter((f) => caseSelected.has(f))])].sort((x, y) => x - y);
  let candidateTelemetry = changedTelemetry;
  if (recipe.kind === "permutation") {
    // Draw order: image frames where both roots have live particles (visible overlap); no telemetry candidate.
    const [front, back] = [recipe.relation.front.index, recipe.relation.back.index];
    const live = new Map(kase.lines.map((l) => [l.frame, l.perEmitter]));
    candidateImage = aligned.filter((f) => (live.get(f)?.[front] ?? 0) > 0 && (live.get(f)?.[back] ?? 0) > 0);
    candidateTelemetry = [];
  }
  return { visual, telemetry, inspect, eligible: refusals.length === 0, refusals, warnings, candidateImage, candidateTelemetry };
}

// A frozen window frame must lie inside its kind's candidate (objective cases).
export function windowErrors(windows, candidates) {
  const errors = [];
  for (const kind of ["image", "telemetry"]) {
    for (const f of windows[kind] ?? []) if (!candidates[kind].includes(f)) errors.push(`${kind} frame ${f} is outside its candidate window`);
  }
  return errors;
}
