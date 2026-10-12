// [vfx-index] Search the catalog by filename, emitter, effect, blend and observed
// use (design §20, §25 acceptance). Every given facet must match; matches rank
// by how much of an asset's usage supports them, so a texture most smoke
// emitters use outranks one a single emitter happens to share.
//
// Usage counts one texture slot: colour by default, since that is what an
// emitter looks like. --slot normal searches normal maps; --slot any, both. An
// asset with no use in the slot is not a candidate. A normal map only ever
// present as the loader's default ranks a quarter as high.
//
//   node scripts/vfx-index/search.mjs --catalog <catalog.json> [--recipes <recipes.json>]
//        [--filename <text>] [--emitter <text>] [--effect <text>] [--blend <name|mode>] [--use <text>]
//        [--slot color|normal|any] [--limit <n>] [--json]
import * as fs from "node:fs";
import { pathToFileURL } from "node:url";
import { BLEND_NAMES, gridSide, words } from "./catalog.mjs";

export const FACETS = ["filename", "emitter", "effect", "blend", "use"];
export const SLOTS = ["color", "normal", "any"];

const lower = (s) => String(s).toLowerCase();
const stem = (name) => lower(name).split(/[\\/]/).pop().replace(/\.[a-z0-9]+$/, "");
// The share of uses that support a facet, plus half a log of their count: a
// texture used 300 times of 1000 can outrank one used 3 times of 3.
const weight = (matches, uses) => matches / uses + Math.log10(1 + matches) / 2;
// A query word finds a name word it begins, or one it extends by a plural
// ending: "explo" finds "explosion", "sparks" finds "spark", "medallion" never "med".
const same = (term, word) => term.startsWith(word) || (word.startsWith(term) && word.length - term.length <= 2);
// The words one use carries: its emitter's and effect's names, its blend mode,
// and what its layout shows. Counted per use, so a match is never borrowed from
// another slot or from a suggestion's wording.
const useWords = (e) => [...words(e.emitter), ...words(stem(e.effect)), ...(BLEND_NAMES[e.blendMode]?.split("-") ?? []),
  ...(gridSide(e.textureSize) > 1 ? ["atlas", ...(e.indexValues.length > 1 ? ["flipbook"] : [])] : []),
  ...(e.world ? ["world", "oriented"] : [])];

function blendMode(text) {
  const t = lower(text).trim();
  const mode = /^\d+$/.test(t) ? Number(t) : BLEND_NAMES.indexOf(t);
  if (mode < 0 || mode >= BLEND_NAMES.length) throw Object.assign(new Error(`unknown blend "${text}"; one of ${BLEND_NAMES.join(", ")} or 0-13`), { exitCode: 2 });
  return mode;
}

// Each facet scorer gets the asset's uses in the slot and returns { score, why },
// or null when the asset does not match.
const ASSET_FACETS = {
  filename(a, q) {
    const want = stem(q), names = [a.canonicalName, ...a.requests].map(stem);
    const score = names.some((n) => n === want) ? 3 : names.some((n) => n.startsWith(want)) ? 2
      : names.some((n) => n.includes(want)) ? 1 : 0;
    return score ? { score, why: `filename matches "${q}"` } : null;
  },
  emitter(a, q, uses) {
    const n = uses.filter((e) => lower(e.emitter).includes(lower(q))).length;
    return n ? { score: weight(n, uses.length), why: `${n} of ${uses.length} uses by emitters named like "${q}"` } : null;
  },
  effect(a, q, uses) {
    const n = uses.filter((e) => lower(stem(e.effect)).includes(lower(q))).length;
    return n ? { score: weight(n, uses.length), why: `${n} of ${uses.length} uses in effects named like "${q}"` } : null;
  },
  blend(a, q, uses) {
    const mode = blendMode(q), n = uses.filter((e) => e.blendMode === mode).length;
    return n ? { score: weight(n, uses.length), why: `${BLEND_NAMES[mode] ?? mode} blending in ${n} of ${uses.length} uses` } : null;
  },
  // Every word must be carried by some use; the word the fewest uses carry scores.
  use(a, q, uses) {
    let least = Infinity;
    for (const w of words(q)) {
      const n = uses.filter((e) => useWords(e).some((t) => same(t, w))).length;
      if (!n) return null;
      least = Math.min(least, n);
    }
    return { score: weight(least, uses.length), why: `observed use "${q}" (${least} of ${uses.length} uses so named or drawn)` };
  },
};

// A recipe matches through its own fields, and blend through its textures' catalog usage.
function recipeFacets(catalog) {
  const byName = new Map();
  for (const a of catalog.assets) byName.set(`${a.canonicalName}#${a.identity.sha256}`, a);
  // A recipe scores 1 a facet, about what an asset whose one use matches scores.
  const texturesOf = (r) => r.expectedTextures.map((t) => byName.get(`${t.canonicalName}#${t.sha256}`)).filter(Boolean);
  const has = (list, q) => list.some((x) => lower(x).includes(lower(q)));
  return {
    filename: (r, q) => has(r.expectedTextures.map((t) => stem(t.canonicalName)), stem(q)) ? { score: 1, why: `expects a texture named like "${q}"` } : null,
    emitter: (r, q) => has(r.emitters ?? [], q) ? { score: 1, why: `includes an emitter named like "${q}"` } : null,
    effect: (r, q) => lower(stem(r.source.effect)).includes(lower(q)) ? { score: 1, why: `from an effect named like "${q}"` } : null,
    blend: (r, q) => {
      const mode = blendMode(q), hit = texturesOf(r).some((a) => a.blendModes.some((b) => b.mode === mode));
      return hit ? { score: 1, why: `its textures are used with ${BLEND_NAMES[mode] ?? mode} blending` } : null;
    },
    use: (r, q) => {
      const terms = words([r.title, r.description ?? "", ...(r.tags ?? [])].join(" "));
      return words(q).every((w) => terms.some((t) => same(t, w))) ? { score: 1, why: `described as "${q}"` } : null;
    },
  };
}

function score(item, facets, query, uses) {
  let total = 0;
  const why = [];
  for (const facet of FACETS) {
    if (query[facet] == null || query[facet] === "") continue;
    const hit = facets[facet](item, query[facet], uses);
    if (!hit) return null;
    total += hit.score;
    why.push(hit.why);
  }
  return { total, why };
}

export function search(catalog, query, { recipes = [], limit = 10 } = {}) {
  if (!FACETS.some((f) => query[f] != null && query[f] !== ""))
    throw Object.assign(new Error(`give at least one facet: ${FACETS.map((f) => `--${f}`).join(", ")}`), { exitCode: 2 });
  if (query.use != null && query.use !== "" && !words(query.use).length)
    throw Object.assign(new Error(`--use needs a word of three or more letters that is not a stop word`), { exitCode: 2 });
  const slot = query.slot ?? "color";
  if (!SLOTS.includes(slot)) throw Object.assign(new Error(`--slot is one of ${SLOTS.join(", ")}`), { exitCode: 2 });
  const results = [];
  for (const a of catalog.assets) {
    const uses = slot === "any" ? a.emitters : a.emitters.filter((e) => e.slot === slot);
    if (!uses.length) continue;
    const s = score(a, ASSET_FACETS, query, uses);
    if (!s) continue;
    const total = (s.total + Math.log10(1 + uses.length) / 10) * (a.loaderDefault ? 0.25 : 1);
    results.push({ kind: "asset", id: a.id, canonicalName: a.canonicalName, score: Number(total.toFixed(4)), why: s.why,
      uses: uses.length, slot, suggestedUses: a.suggestedUses, source: a.source });
  }
  const rf = recipeFacets(catalog);
  for (const r of recipes) {
    const s = score(r, rf, query);
    if (s) results.push({ kind: "recipe", id: r.id, title: r.title, type: r.type, score: Number(s.total.toFixed(4)), why: s.why });
  }
  return results.sort((x, y) => y.score - x.score || x.id.localeCompare(y.id)).slice(0, limit);
}

function parseArgs(argv) {
  const opts = { query: {}, limit: 10, json: false };
  for (let i = 0; i < argv.length; i++) {
    const flag = argv[i], value = argv[i + 1];
    const need = () => { if (value == null) throw Object.assign(new Error(`${flag} needs a value`), { exitCode: 2 }); i++; return value; };
    if (flag === "--catalog") opts.catalog = need();
    else if (flag === "--slot") opts.query.slot = need();
    else if (flag === "--recipes") opts.recipes = need();
    else if (flag === "--limit") opts.limit = Number(need());
    else if (flag === "--json") opts.json = true;
    else if (FACETS.includes(flag.slice(2)) && flag.startsWith("--")) opts.query[flag.slice(2)] = need();
    else throw Object.assign(new Error(`unknown argument ${flag}`), { exitCode: 2 });
  }
  if (!opts.catalog) throw Object.assign(new Error("--catalog is required"), { exitCode: 2 });
  if (!Number.isInteger(opts.limit) || opts.limit < 1) throw Object.assign(new Error("--limit must be a positive integer"), { exitCode: 2 });
  return opts;
}

export function main(argv = process.argv.slice(2), { io = fs, log = console.log } = {}) {
  const opts = parseArgs(argv);
  const catalog = JSON.parse(io.readFileSync(opts.catalog, "utf8"));
  const recipes = opts.recipes ? JSON.parse(io.readFileSync(opts.recipes, "utf8")).recipes : [];
  const results = search(catalog, opts.query, { recipes, limit: opts.limit });
  if (opts.json) log(JSON.stringify(results, null, 2));
  else results.forEach((r, i) => log(`${i + 1}. ${r.kind} ${r.kind === "asset" ? r.id : `${r.id} (${r.type}): ${r.title}`}  score ${r.score}\n   ${r.why.join("; ")}`
    + (r.suggestedUses?.length ? `\n   suggested: ${r.suggestedUses.join("; ")}` : "")));
  if (!results.length) log("no matches");
  return results;
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? "").href) {
  try { main(); } catch (error) { console.error(`[vfx-index] ${error.message}`); process.exit(error.exitCode ?? 1); }
}
