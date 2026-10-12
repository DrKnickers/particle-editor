// rules.md: the public admissibility rules (plan decision 11), byte-identical
// in every pack of every condition. Generated, never hand-edited: the worked
// examples are produced by the scorer's own endpoint procedure, so the text
// and the scorer cannot disagree.
import { float32Text, interveneContract, persistedText, restoreContract, seedContract } from "./persisted.mjs";
import { RUBRIC } from "./schema-check.mjs";

const f = Math.fround;

function range(field, contract) {
  const [lo, hi] = contract.interval;
  return `from ${persistedText(field, lo)} to ${persistedText(field, hi)}`;
}

// The worked examples, as accepted proposal values.
export function workedExamples() {
  return [
    { text: "A lifetime of 2 whose stated target is 4 (restoration)", field: "lifetime", contract: restoreContract("lifetime", 4, 2) },
    { text: "A count of 80 whose stated target is 10 (restoration)", field: "nParticlesPerBurst", contract: restoreContract("nParticlesPerBurst", 10, 80) },
    { text: "A delay whose stated target is 0.2, decreased (intervention)", field: "initialDelay", contract: interveneContract("initialDelay", f(0.2), f(0.2), "decrease") },
    { text: "A delay whose stated target is 0, increased (intervention)", field: "initialDelay", contract: interveneContract("initialDelay", 0, 0, "increase") },
    { text: "A random fraction of 0.6 decreased (seed rule)", field: "randomLifetimePerc", contract: seedContract("randomLifetimePerc", f(0.6), "decrease") },
    { text: "A random fraction of 0.1 decreased (seed rule)", field: "randomLifetimePerc", contract: seedContract("randomLifetimePerc", f(0.1), "decrease") },
    { text: "A lifetime of 4 decreased (seed rule)", field: "lifetime", contract: seedContract("lifetime", 4, "decrease") },
  ];
}

const BLEND = { 1: "additive", 2: "transparent", 4: "depth additive", 5: "depth transparent", 7: "diffuse transparent" };

export function rulesMarkdown() {
  const lines = [
    "# Rules",
    "",
    "These rules are the same in every pack. They say what a finding must contain to count, and which corrections are accepted.",
    "",
    "## Your answer",
    "",
    "- Write `findings.json` as described in `findings.schema.json`. An empty `findings` array means: no defect. That is a valid answer, and the right one when you find none.",
    "- Each finding names one defect: a category, a direction, the emitters it concerns (index and name), the evidence, one correction, a severity and a confidence.",
    "- Severity is reported and never changes the score. Every finding counts, whatever its severity: a finding on an effect without a defect is a false alarm, and so is any finding that does not match the defect.",
    "- Several findings that state the same defect with different corrections or evidence are separate findings; at most one of them can be correct.",
    "",
    "## Categories, directions and corrections",
    "",
  ];
  for (const [category, spec] of Object.entries(RUBRIC.categories)) {
    for (const [direction, options] of Object.entries(spec.directions)) {
      const corrections = options.map((o) => {
        if (o.kind === "scalar") return `\`${o.field}\` ${o.direction}`;
        if (o.kind === "enum") return `\`${o.field}\` set to one of ${o.acceptable.map((m) => `${m} (${BLEND[m]})`).join(", ")}`;
        if (o.kind === "texture") return `\`${o.field}\` set to a name in \`textures.json\``;
        if (o.kind === "track") return `track ${o.track} (index) replaced`;
        return "the two emitters' draw order swapped";
      });
      lines.push(`- \`${category}\` / \`${direction}\`: ${corrections.join("; ")}.`);
    }
  }
  lines.push(
    "",
    "For draw order, also give `relation`: the emitter that should be in front and the one that should be behind.",
    "",
    "## Evidence",
    "",
    "- Cite frames as `{ \"kind\": \"image\", \"frame\": n }` (a frame in `filmstrip.json` or `selected.json`) or `{ \"kind\": \"telemetry\", \"frame\": n }` (a frame in `frames.jsonl`). In a pack with `bundle-a`, `bundle-b` and `bundle-c`, give the finding's `bundleId` and cite only frames of that bundle.",
    "- Frame evidence counts when more than half of the distinct frames you cite show the defect on the emitters you name. An image and a telemetry citation of the same frame are two citations.",
    "- When `inspect.json` is in the pack you may also cite a value: `{ \"kind\": \"inspect\", \"emitter\": { \"index\", \"name\" }, \"field\": \"lifetime\" }`. Value citations do not count toward the frame majority; some defects can be shown by the value alone.",
    "",
    "## Corrections",
    "",
    "A correction names the emitter it edits. Values are compared after the save the editor would make: decimals as 32-bit floats, counts as whole numbers.",
    "",
    "- **A field whose value differs from its stated target in `intent.md` *Targets* (restoration).** Move it toward the target by between a quarter of the way and the whole way: accepted values lie between `current + 0.25 x (target - current)` and `target`, inclusive.",
    "- **A field at its stated target (intervention).** Move it in the diagnosed direction by between 10 % and 50 % of the target (for a target of zero, by 0.1 to 0.5 seconds).",
    "- **When the file is unchanged and one bundle misbehaves (seed rule, packs with three bundles only; restoration does not apply there).** Move the named field in the diagnosed direction by a change `c` with `0.25 x B_eff <= c <= B_eff`, where `B = 0.5` for a fraction in [0, 1] and `B = 0.5 x the current value` for a lifetime, and `B_eff = min(B, the distance from the current value to the edge of the field's range in that direction)`.",
    "- For every rule: both ends of the range and your value are rounded as the save would round them, the range is cut to the field's valid values, and your value must lie inside it and strictly past the current value in the diagnosed direction.",
    "- Valid values: fractions 0 to 1; lifetimes above 0; delays 0 or more; counts 1 to 4294967295.",
    "- A texture correction must name a texture exactly as listed in `textures.json`.",
    "- A track correction gives the whole index track as `[time, value]` keys: the first at time 0, the last at 100, times strictly increasing, every value inside the atlas cells (0 up to, not including, the cell count). It is accepted when it plays the same sprite as the intended track at every age and after the last key.",
    "",
    "### Worked examples (accepted values, generated by the scorer)",
    "",
  );
  for (const e of workedExamples()) lines.push(`- ${e.text}: ${range(e.field, e.contract)}.`);
  lines.push("");
  return lines.join("\n");
}

export { float32Text };
