// [vfx-loop] The four-seed review.
//
// The source and the final candidate are observed under each of four variation
// seeds and compared seed by seed, so a budget such as "do not increase peak
// particle count" has to hold on every seed, not on one lucky draw. The first
// seed is the task's own: its pair is the round's existing observations.
//
//   four-seed/
//     matrix.json                     per seed: bundles, comparison, budgets
//     seed-K/{baseline,candidate}/    observation bundles (seeds 2-4)
//     seed-K/comparison/              writeComparison output (seeds 2-4)
const serialise = (value) => JSON.stringify(value, null, 2) + "\n";

// Seeds 2-4 step from the task's seed by the 32-bit golden-ratio constant: a
// fixed rule, so two runs of one task observe the same variations.
export function variationSeeds(seed, listed) {
  if (listed) return [...listed];
  return [0, 1, 2, 3].map((k) => (seed + k * 0x9e3779b9) >>> 0);
}

export function checkVariationSeeds(listed, seed) {
  if (listed === undefined) return [];
  const uint = (v) => Number.isSafeInteger(v) && v >= 0 && v <= 0xffffffff;
  if (!Array.isArray(listed) || listed.length !== 4 || !listed.every(uint) || new Set(listed).size !== 4)
    return ["task: variationSeeds must be four distinct 32-bit unsigned integers"];
  if (listed[0] !== seed) return ["task: variationSeeds[0] must equal observation.scenario.seed"];
  return [];
}

// `primary` is the round's existing pair under seeds[0]: { baseline, candidate,
// comparison, metrics, schedule, summaries } with task-root-relative paths. `observePair`
// observes both effects under one seed and returns their directories;
// `compare(baselineDir, candidateDir, name)` writes one comparison.
export async function fourSeedReview({ seeds, primary, observePair, compare, writeJson, say = () => {} }) {
  const rows = [{ seed: seeds[0], primary: true, baseline: primary.baseline, candidate: primary.candidate,
    comparison: primary.comparison, metrics: primary.metrics, schedule: primary.schedule, summaries: primary.summaries }];
  for (const [i, seed] of seeds.slice(1).entries()) {
    const name = `four-seed/seed-${i + 2}`;
    const { baseline, candidate } = await observePair(seed, name);
    const { metrics, schedule, summaries } = await compare(baseline, candidate, `${name}/comparison`);
    rows.push({ seed, primary: false, baseline: `${name}/baseline`, candidate: `${name}/candidate`,
      comparison: `${name}/comparison`, metrics, schedule, summaries });
    say(`[vfx-loop] step=four-seed seed=${seed} budgetsPass=${metrics.budgetsPass}`);
  }
  const matrix = { schemaVersion: 1,
    seeds: rows.map((row) => ({ seed: row.seed, primary: row.primary, baseline: row.baseline, candidate: row.candidate,
      comparison: row.comparison, filmstrip: `${row.comparison}/paired-filmstrip.png`, budgetsPass: row.metrics.budgetsPass,
      failed: row.metrics.budgets.filter((v) => !v.pass).map((v) => v.id),
      peakParticles: [row.summaries.baseline.peakParticles, row.summaries.candidate.peakParticles],
      firstRandomDivergence: row.schedule.firstDivergentFrame })) };
  matrix.allPass = matrix.seeds.every((v) => v.budgetsPass);
  writeJson("four-seed/matrix.json", serialise(matrix));
  return matrix;
}
