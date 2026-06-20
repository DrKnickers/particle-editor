// atlas-preview-cache.ts — mod-stack-keyed preview cache (Task 12).
//
// Caches "textures/get-preview" responses keyed by (modStack, filename).
// Only successful (status === "ok") results are cached; non-ok results
// are always re-fetched so a later mod-stack change can resolve them.
// invalidatePreviewCache() is called by initModStack whenever the active
// stack changes so stale previews are not served.

type OkResult   = { status: "ok"; dataUri: string; srcW: number; srcH: number };
type FetchResult = OkResult | { status: "missing" } | { status: "broken" };

const cache = new Map<string, OkResult>();

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
}
