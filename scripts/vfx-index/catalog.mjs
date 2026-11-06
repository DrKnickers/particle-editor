// [vfx-index] The asset catalog: `--tool index` records folded into
// one asset per stack-aware identity, with the usage evidence a search ranks by.
//
// Identity is the triple (canonical name, content hash, content-stack hash).
// The native tool resolves and hashes every texture through the
// editor's own chain; nothing here resolves a path. One catalog covers one
// content stack, so the stack hash is the same for every asset in it.
//
// Usage only, no vision: observed uses are counts of what the
// effects do with an asset; suggested uses are read off that distribution.
// Atlas interpretations are counted, never collapsed to one.

// ParticleSystem::BLEND_* in order (src/effect/ParticleSystem.h).
export const BLEND_NAMES = ["none", "additive", "transparent", "inverse", "depth-additive", "depth-transparent",
  "depth-inverse", "diffuse-transparent", "stencil-darken", "stencil-darken-blur", "heat", "bump", "decal-bump",
  "scanlines"];

export const blendName = (mode) => BLEND_NAMES[mode] ?? `blend-${mode}`;

// The editor's atlas grid: floor(sqrt(textureSize)) cells a side (AtlasGridSide).
export const gridSide = (textureSize) => Math.floor(Math.sqrt(Math.max(textureSize, 1)));

// Words in effect and emitter names: "p_explosion_med01.alo" → explosion, med.
const STOP = new Set(["alo", "data", "art", "models", "emitter", "the", "and", "of", "copy", "new"]);
export function words(text) {
  return String(text).toLowerCase().split(/[^a-z]+/).filter((w) => w.length >= 3 && !STOP.has(w));
}

const count = (map, key, by = 1) => map.set(key, (map.get(key) ?? 0) + by);
const ranked = (map, name) => [...map].map(([k, n]) => ({ [name]: k, count: n }))
  .sort((a, b) => b.count - a.count || String(a[name]).localeCompare(String(b[name])));
const basename = (file) => file.split(/[\\/]/).pop();

// The identity triple, shortened: name, content and stack.
export function assetId(canonicalName, sha256, stack) {
  return `${canonicalName}#${sha256.slice(0, 12)}@${stack.slice(0, 8)}`;
}

export function checkRecords(doc) {
  if (doc?.schema !== 1 || doc.verb !== "index" || !Array.isArray(doc.records) || typeof doc.content?.sha256 !== "string")
    throw Object.assign(new Error("not a --tool index records document (schema 1)"), { exitCode: 2 });
  if (doc.summary?.unverifiable)
    throw Object.assign(new Error(`the index could not vouch for a read: ${doc.summary.unverifiable}`), { exitCode: 3 });
}

// What the distribution suggests, each with the share that supports it.
function suggest(asset, emitters) {
  const out = [];
  const share = (n) => `${n} of ${asset.uses} uses`;
  const normal = asset.slots.normal ?? 0;
  if (normal > asset.uses / 2) out.push(`normal map (${share(normal)})`);
  const [blend] = asset.blendModes;
  if (blend && blend.count > asset.uses / 2) out.push(`${blend.name} blending (${share(blend.count)})`);
  // The most common layout, counted over its own uses only.
  const [grid, ...others] = asset.atlas.interpretations;
  if (grid && grid.side > 1) {
    const animated = emitters.filter((e) => gridSide(e.textureSize) === grid.side && e.indexValues.length > 1).length;
    const also = others.length ? `; also used as ${others.map((o) => `${o.side}x${o.side}`).join(", ")}` : "";
    out.push(animated ? `flipbook on a ${grid.side}x${grid.side} atlas (${animated} of ${grid.count} uses animate the Index track${also})`
      : `${grid.side}x${grid.side} atlas, one cell per emitter (${grid.count} uses${also})`);
  }
  if (asset.orientation.world > asset.uses / 2) out.push(`world-oriented quads (${share(asset.orientation.world)})`);
  for (const t of asset.useTerms.slice(0, 3)) if (t.count > 1) out.push(`${t.term} (${t.count} emitter or effect names)`);
  return out;
}

export function buildCatalog(doc) {
  checkRecords(doc);
  const stack = doc.content.sha256;
  const assets = new Map(), unresolved = new Map();
  const effects = new Map((doc.systems ?? []).map((s) => [s.file, { file: s.file, internalName: s.internalName,
    sha256: s.sha256, source: s.source, emitters: s.emitters, emitterNames: new Set(), textures: new Set() }]));
  for (const r of doc.records) {
    const t = r.texture;
    effects.get(r.effect)?.emitterNames.add(r.emitter.name);
    if (t.outcome !== "resolved") {
      const u = unresolved.get(t.canonicalName) ?? { canonicalName: t.canonicalName, requests: new Set(), uses: 0, effects: new Set() };
      u.requests.add(t.request); u.uses++; u.effects.add(r.effect);
      unresolved.set(t.canonicalName, u);
      continue;
    }
    const id = assetId(t.canonicalName, t.sha256, stack);
    let a = assets.get(id);
    if (!a) assets.set(id, a = { id, canonicalName: t.canonicalName, sha256: t.sha256, contentStackHash: stack,
      source: t.source, requests: new Set(), uses: 0, slots: new Map(), loaderDefaultUses: 0, effects: new Map(),
      emitters: [], blends: new Map(), world: 0, grids: new Map(), terms: new Map() });
    a.requests.add(t.request); a.uses++;
    count(a.slots, r.slot);
    if (t.loaderDefault) a.loaderDefaultUses++;
    count(a.effects, r.effect);
    a.emitters.push({ effect: r.effect, emitter: r.emitter.name, index: r.emitter.index, slot: r.slot,
      blendMode: r.blendMode, world: r.isWorldOriented, textureSize: r.textureSize, indexValues: r.indexValues });
    count(a.blends, r.blendMode);
    if (r.isWorldOriented) a.world++;
    const side = gridSide(r.textureSize);
    const g = a.grids.get(side) ?? { side, textureSizes: new Set(), count: 0, cells: new Set() };
    g.count++; g.textureSizes.add(r.textureSize);
    for (const v of r.indexValues) g.cells.add(Math.floor(v));
    a.grids.set(side, g);
    for (const w of new Set([...words(r.emitter.name), ...words(basename(r.effect))])) count(a.terms, w);
    effects.get(r.effect)?.textures.add(id);
  }

  const catalog = [...assets.values()].map((a) => {
    const interpretations = [...a.grids.values()].sort((x, y) => y.count - x.count || x.side - y.side).map((g) => ({
      side: g.side, cells: g.side * g.side, textureSizes: [...g.textureSizes].sort((x, y) => x - y), count: g.count,
      observedCells: [...g.cells].sort((x, y) => x - y),
      outOfGrid: [...g.cells].filter((c) => c < 0 || c >= g.side * g.side).sort((x, y) => x - y) }));
    const asset = { id: a.id, identity: { canonicalName: a.canonicalName, sha256: a.sha256, contentStackHash: a.contentStackHash },
      canonicalName: a.canonicalName, source: a.source, requests: [...a.requests].sort(), uses: a.uses,
      slots: Object.fromEntries([...a.slots].sort()),
      loaderDefault: a.loaderDefaultUses === a.uses,
      effects: ranked(a.effects, "file"),
      emitters: a.emitters,
      blendModes: ranked(a.blends, "mode").map((b) => ({ mode: b.mode, name: blendName(b.mode), count: b.count })),
      orientation: { world: a.world, camera: a.uses - a.world },
      atlas: { interpretations, conflict: interpretations.length > 1 ? interpretations.map((i) => `${i.side}x${i.side}`) : null },
      useTerms: ranked(a.terms, "term") };
    asset.suggestedUses = suggest(asset, a.emitters);
    return asset;
  }).sort((x, y) => y.uses - x.uses || x.id.localeCompare(y.id));

  return { schema: 1, kind: "vfx-asset-catalog",
    content: doc.content, tool: doc.tool ?? null,
    summary: { assets: catalog.length, unresolved: unresolved.size, effects: effects.size, records: doc.records.length,
      atlasConflicts: catalog.filter((a) => a.atlas.conflict).length },
    assets: catalog,
    unresolved: [...unresolved.values()].map((u) => ({ canonicalName: u.canonicalName, requests: [...u.requests].sort(),
      uses: u.uses, effects: [...u.effects].sort() })).sort((x, y) => y.uses - x.uses || x.canonicalName.localeCompare(y.canonicalName)),
    // emitterNames: every emitter naming a texture, resolved or not.
    effects: [...effects.values()].map((e) => ({ ...e, emitterNames: [...e.emitterNames].sort(), textures: [...e.textures].sort() }))
      .sort((x, y) => x.file.localeCompare(y.file)),
    failures: doc.failures ?? [] };
}
