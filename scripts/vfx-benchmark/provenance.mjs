// One frozen content provenance per draft (plan 3.4 build-corpus, 3.3).
// Pure: a bundle's capture.json against the draft's provenance and its
// draft-wide asset map. A disagreement is mixed provenance and refuses the
// draft; the remedy is a new draft, never a partial refresh.
import { canonical } from "./semantic.mjs";
import { canonicalRoot } from "./store.mjs";

// capture.render: the adapter (description, vendor and device ids, driver version), the
// display and depth formats, multisampling and the vertex-processing mode.
export const RENDER_FIELDS = ["adapter", "displayFormat", "depthStencilFormat", "multisample", "vertexProcessing"];

const assetKey = (r) => `${r.type}:${r.identity}`;

// draft: { contentRoots, render (null until the first bundle), assets: Map key -> canonical record }
export function checkBundle(capture, draft, { label, allowAssets = [] } = {}) {
  const errors = [];
  const roots = (capture.content?.gameRoots ?? []).map(canonicalRoot);
  const want = draft.contentRoots.map(canonicalRoot);
  if (canonical(roots) !== canonical(want)) errors.push(`${label}: game roots ${roots.join(", ")} are not the provenance's ${want.join(", ")}`);
  if ((capture.content?.selectedModPaths ?? []).length) errors.push(`${label}: selected mod paths must be empty (content roots are explicit)`);
  if (!capture.render) errors.push(`${label}: no render profile`);
  else if (draft.render === null) draft.render = capture.render;
  else for (const f of RENDER_FIELDS) {
    if (canonical(capture.render[f]) !== canonical(draft.render[f])) errors.push(`${label}: render ${f} differs from the draft's (mixed provenance)`);
  }
  const assets = capture.content?.loadedAssets;
  if (!Array.isArray(assets)) errors.push(`${label}: no asset ledger`);
  else for (const r of assets) {
    const key = assetKey(r), text = canonical({ ...r, requests: undefined });
    if (!draft.assets.has(key)) draft.assets.set(key, text);
    else if (draft.assets.get(key) !== text && !allowAssets.includes(key)) errors.push(`${label}: ${key} differs from the draft's asset map (mixed provenance)`);
  }
  return errors;
}

// The set of assets a bundle loaded, for the per-base dependency manifest.
export const assetSet = (capture) => (capture.content?.loadedAssets ?? []).map(assetKey).sort();

export function checkDependencies(capture, baseSet, { label, allow = [] } = {}) {
  const have = new Set(assetSet(capture)), want = new Set(baseSet);
  const errors = [];
  for (const k of want) if (!have.has(k) && !allow.includes(k)) errors.push(`${label}: ${k} is missing against its base`);
  for (const k of have) if (!want.has(k) && !allow.includes(k)) errors.push(`${label}: ${k} was not loaded by its base`);
  return errors;
}

// Distinct non-empty texture names an inspect dump requests.
export function textureNames(inspect) {
  return [...new Set(inspect.emitters.flatMap((e) => [e.colorTexture, e.normalTexture]).filter((n) => n !== ""))].sort();
}

// Texture attestation: a base or control resolves every name its inspect
// names; case 1 holds exactly its planted name unresolved; every other case
// matches its base exactly.
export function checkAttestation(capture, inspect, { label, base = null, planted = null }) {
  const errors = [];
  const resolved = capture.content?.resolvedTextures, unresolved = capture.content?.unresolvedTextures;
  if (!Array.isArray(resolved) || !Array.isArray(unresolved)) return [`${label}: no texture attestation`];
  const sorted = (a) => [...a].sort();
  if (planted !== null) {
    if (canonical(sorted(unresolved)) !== canonical([planted])) errors.push(`${label}: unresolved textures must be exactly ${planted}`);
    const expected = textureNames(inspect).filter((n) => n !== planted);
    if (canonical(sorted(resolved)) !== canonical(expected)) errors.push(`${label}: resolved textures ${sorted(resolved).join(", ")} are not ${expected.join(", ")}`);
    return errors;
  }
  if (unresolved.length) errors.push(`${label}: unresolved textures ${unresolved.join(", ")}`);
  for (const n of textureNames(inspect)) if (!resolved.includes(n)) errors.push(`${label}: ${n} is unverified (never requested in the window)`);
  if (base && canonical(sorted(resolved)) !== canonical(sorted(base.content.resolvedTextures))) errors.push(`${label}: resolved textures differ from the base's`);
  return errors;
}
