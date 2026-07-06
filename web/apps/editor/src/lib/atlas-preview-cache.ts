// atlas-preview-cache.ts — mod-stack-keyed preview cache (Task 12).
//
// Caches "textures/get-preview" responses keyed by (modStack, filename).
// Only successful (status === "ok") results are cached; non-ok results
// are always re-fetched so a later mod-stack change can resolve them.
// invalidatePreviewCache() is called by initModStack whenever the active
// stack changes so stale previews are not served.

import type { Bridge } from "@particle-editor/bridge-schema";
import { create } from "zustand";

type OkResult = { status: "ok"; dataUri: string; srcW: number; srcH: number };
type FinalFetchResult = OkResult | { status: "missing" } | { status: "broken" };
type FetchResult = FinalFetchResult | { status: "pending" };

type PendingPreviewOptions = {
  bridge: Pick<Bridge, "on">;
  filename: string;
  flattenAlpha: boolean;
  timeoutMs?: number;
};

const DEFAULT_PENDING_TIMEOUT_MS = 10_000;
const cache = new Map<string, OkResult>();
const pendingWaits = new Map<string, Promise<FetchResult>>();

// Texture-content epoch. A re-import / "reload textures" can change a texture's
// pixels without changing its filename, so the (modStack, filename) cache key
// alone would serve a stale preview. Consumers include this epoch in their fetch
// deps; bumpTextureEpoch() (called at the reload-textures sites) drops the cache
// and increments the epoch so previews re-fetch fresh content.
export const useTextureEpoch = create<{ epoch: number }>(() => ({ epoch: 0 }));

/** Drop cached previews and advance the epoch after a texture reload / re-import. */
export function bumpTextureEpoch(): void {
  cache.clear();
  // The host also drops its LRU + in-flight encodes on reload (PreviewCacheClear),
  // so a wait outstanding across the reload would never see its preview-ready.
  // Clear it here too; the epoch bump reruns consumers' fetch effects, which
  // start a fresh wait against the reloaded pixels.
  pendingWaits.clear();
  useTextureEpoch.setState((s) => ({ epoch: s.epoch + 1 }));
}

function cacheKey(stack: string[], filename: string): string {
  return stack.join("|") + "::" + filename;
}

function cacheOk(k: string, res: FetchResult): void {
  if (res.status === "ok") cache.set(k, res);
}

function waitForPreviewReady(
  k: string,
  fetcher: () => Promise<FetchResult>,
  pending: PendingPreviewOptions,
): Promise<FetchResult> {
  const existing = pendingWaits.get(k);
  if (existing) return existing;

  let unsubscribe: (() => void) | undefined;
  let timer: ReturnType<typeof setTimeout> | undefined;
  let armed = false;
  const timeoutMs = pending.timeoutMs ?? DEFAULT_PENDING_TIMEOUT_MS;

  const wait = new Promise<FetchResult>((resolve, reject) => {
    // (Re)subscribe + (re)arm the timeout. A wait resolves ONLY on a TERMINAL
    // status (ok/missing/broken); a refetch that STILL returns pending means
    // the encode is legitimately in flight (slow, or a preview-ready we
    // missed), so we re-arm and keep listening rather than surfacing a
    // permanent "loading". A truly dead worker would re-arm every timeoutMs —
    // acceptable for a should-never-happen case; mod-switch / reload clear
    // pendingWaits so this can't outlive a stack/pixel change.
    const arm = () => {
      if (armed) return;
      armed = true;
      unsubscribe = pending.bridge.on("textures/preview-ready", (event) => {
        if (
          event.payload.filename === pending.filename &&
          event.payload.flattenAlpha === pending.flattenAlpha
        ) {
          settleOrRetry();
        }
      });
      timer = setTimeout(settleOrRetry, timeoutMs);
    };
    const disarm = () => {
      armed = false;
      if (timer !== undefined) clearTimeout(timer);
      unsubscribe?.();
    };
    const settleOrRetry = () => {
      if (!armed) return;
      disarm();
      fetcher().then(
        (res) => {
          cacheOk(k, res);
          if (res.status === "pending") arm();   // still encoding — keep waiting
          else resolve(res);
        },
        reject,
      );
    };
    arm();
  }).finally(() => {
    if (pendingWaits.get(k) === wait) pendingWaits.delete(k);
  });

  pendingWaits.set(k, wait);
  return wait;
}

/**
 * Return a cached preview result if one exists for (stack, filename),
 * otherwise call fetcher(), cache the result if it is "ok", and return it.
 */
export async function getPreviewCached(
  stack: string[],
  filename: string,
  fetcher: () => Promise<FetchResult>,
  pending?: PendingPreviewOptions,
): Promise<FetchResult> {
  const k = cacheKey(stack, filename);
  const hit = cache.get(k);
  if (hit) return hit;
  const wait = pendingWaits.get(k);
  if (wait) return wait;

  const res = await fetcher();
  cacheOk(k, res);
  if (res.status === "pending" && pending) {
    return waitForPreviewReady(k, fetcher, pending);
  }
  return res;
}

/** Clear all cached previews (call when the mod stack changes). */
export function invalidatePreviewCache(): void {
  cache.clear();
  // Drop in-flight waits too: on a mod switch the host bumps its preview
  // epoch and discards the pending encode, so the matching preview-ready
  // never fires — a surviving wait would just time out and refetch against
  // the NEW stack (keys differ so it's never served; no point spending it).
  pendingWaits.clear();
}

/** Reset for test isolation — equivalent to invalidatePreviewCache but signals intent. */
export function __resetPreviewCache(): void {
  cache.clear();
  pendingWaits.clear();
  useTextureEpoch.setState({ epoch: 0 });
}
