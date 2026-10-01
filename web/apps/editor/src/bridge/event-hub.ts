// EventHub — the keyed listener registry shared by every bridge (NativeBridge,
// TestHostBridge, MockBridge) and the host ui/* message hub. One subscriber
// throwing must not starve the rest, so each handler runs in its own
// try/catch: the error is logged and fan-out continues.

export class EventHub<M extends Record<string, unknown>> {
  private buckets = new Map<keyof M, Set<(value: never) => void>>();

  constructor(private readonly label: string) {}

  /** Subscribe to `key`; returns an idempotent unsubscribe. */
  on<K extends keyof M>(key: K, handler: (value: M[K]) => void): () => void {
    let bucket = this.buckets.get(key);
    if (!bucket) {
      bucket = new Set();
      this.buckets.set(key, bucket);
    }
    bucket.add(handler as (value: never) => void);
    return () => { bucket?.delete(handler as (value: never) => void); };
  }

  /** Live subscriber count for `key` (diagnostics, e.g. the profiler audit spec). */
  listenerCount(key: keyof M): number {
    return this.buckets.get(key)?.size ?? 0;
  }

  emit<K extends keyof M>(key: K, value: M[K]): void {
    this.buckets.get(key)?.forEach((h) => {
      try {
        (h as (value: M[K]) => void)(value);
      } catch (err) {
        console.error(`[${this.label}] "${String(key)}" handler threw:`, err);
      }
    });
  }
}
