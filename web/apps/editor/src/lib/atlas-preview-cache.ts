// atlas-preview-cache.ts — mod-stack-keyed preview cache (Task 12).
//
// Caches "textures/get-preview" responses keyed by (modStack, filename).
// Only successful (status === "ok") results are cached; non-ok results
// are always re-fetched so a later mod-stack change can resolve them.
// invalidatePreviewCache() is called by initModStack whenever the active
// stack changes so stale previews are not served.

import { create } from "zustand";

type OkResult   = { status: "ok"; dataUri: string; srcW: number; srcH: number };
type FetchResult = OkResult | { status: "missing" } | { status: "broken" };

const cache = new Map<string, OkResult>();

// Texture-content epoch. A re-import / "reload textures" can change a texture's
// pixels without changing its filename, so the (modStack, filename) cache key
// alone would serve a stale preview. Consumers include this epoch in their fetch
// deps; bumpTextureEpoch() (called at the reload-textures sites) drops the cache
// and increments the epoch so previews re-fetch fresh content.
export const useTextureEpoch = create<{ epoch: number }>(() => ({ epoch: 0 }));

/** Drop cached previews and advance the epoch after a texture reload / re-import. */
export function bumpTextureEpoch(): void {
  cache.clear();
  useTextureEpoch.setState((s) => ({ epoch: s.epoch + 1 }));
}

function cacheKey(stack: string[], filename: string): string {
  return stack.join("|") + "::" + filename;
}

/**
 * Return a cached preview result if one exists for (stack, filename),
 * otherwise call fetcher(), cache the result if it is "ok", and return it.
 */
export async function getPreviewCached(
  stack: string[],
  filename: string,
  fetcher: () => Promise<FetchResult>,
): Promise<FetchResult> {
  const k = cacheKey(stack, filename);
  const hit = cache.get(k);
  if (hit) return hit;
  const res = await fetcher();
  if (res.status === "ok") cache.set(k, res);
  return res;
}

/** Clear all cached previews (call when the mod stack changes). */
export function invalidatePreviewCache(): void {
  cache.clear();
}

/** Reset for test isolation — equivalent to invalidatePreviewCache but signals intent. */
export function __resetPreviewCache(): void {
  cache.clear();
  useTextureEpoch.setState({ epoch: 0 });
}
