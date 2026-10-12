// The seed search (plan 3.5, case 12), pre-registered in corpus.json before it
// runs. Pure: one observation summary per seed in, the outlier, its trios and
// every exclusion out.

export const SEED_THRESHOLD = 2.5;
export const MIN_ELIGIBLE = 10;

// A small recorded PRNG (mulberry32): the draw is reproducible from its seed.
export function prng(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

export function shuffle(items, random) {
  const out = [...items];
  for (let i = out.length - 1; i > 0; i--) {
    const j = Math.floor(random() * (i + 1));
    [out[i], out[j]] = [out[j], out[i]];
  }
  return out;
}

// Population z-scores; a metric with zero variance contributes 0.
function zScores(values) {
  const n = values.length;
  const mean = values.reduce((s, v) => s + v, 0) / n;
  const sd = Math.sqrt(values.reduce((s, v) => s + (v - mean) ** 2, 0) / n);
  return values.map((v) => (sd === 0 ? 0 : (v - mean) / sd));
}

// runs: [{ seed, status, refusals: { runtimeGuard, hardGuard, indexCap }, peakParticles, lastActive }]
export function seedSearch(runs, { drawSeed, threshold = SEED_THRESHOLD, minEligible = MIN_ELIGIBLE } = {}) {
  const excluded = [], eligible = [];
  for (const r of [...runs].sort((a, b) => a.seed - b.seed)) {
    const refused = r.refusals.runtimeGuard + r.refusals.hardGuard + r.refusals.indexCap;
    if (r.status !== "natural-end") excluded.push({ seed: r.seed, reason: `status ${r.status}` });
    else if (refused !== 0) excluded.push({ seed: r.seed, reason: `${refused} refusals` });
    else if (r.peakParticles === null || r.lastActive === null) excluded.push({ seed: r.seed, reason: "a null landmark" });
    else eligible.push(r);
  }
  if (eligible.length < minEligible) return { eligible: eligible.map((r) => r.seed), excluded, scores: [], outlier: null, absence: "a" };
  const peak = zScores(eligible.map((r) => r.peakParticles)), last = zScores(eligible.map((r) => r.lastActive));
  const scores = eligible.map((r, i) => ({ seed: r.seed, score: Math.max(Math.abs(peak[i]), Math.abs(last[i])) }));
  let best = null;
  for (const s of scores) if (s.score >= threshold && (!best || s.score > best.score)) best = s;   // ties keep the lower seed
  if (!best) return { eligible: eligible.map((r) => r.seed), excluded, scores, outlier: null, absence: "b" };
  const others = shuffle(eligible.filter((r) => r.seed !== best.seed).map((r) => r.seed), prng(drawSeed));
  return {
    eligible: eligible.map((r) => r.seed), excluded, scores, outlier: best.seed, absence: null, drawSeed,
    defectTrio: [best.seed, others[0], others[1]], controlTrio: [others[2], others[3], others[4]],
  };
}

// One recorded permutation of the three bundle labels over a trio.
export function labelTrio(trio, random) {
  const labels = shuffle(["bundle-a", "bundle-b", "bundle-c"], random);
  return Object.fromEntries(trio.map((seed, i) => [labels[i], seed]));
}
