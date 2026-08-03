import type { Bridge, Request, ResponseFor } from "@particle-editor/bridge-schema";

const pendingByBridge = new WeakMap<Bridge, Map<string, Promise<unknown>>>();

function stableKey(value: unknown): string {
  if (value === undefined) return "undefined";
  if (value === null || typeof value !== "object") return JSON.stringify(value) ?? "undefined";
  if (Array.isArray(value)) return `[${value.map(stableKey).join(",")}]`;
  const record = value as Record<string, unknown>;
  return `{${Object.keys(record)
    .sort()
    .map((key) => `${JSON.stringify(key)}:${stableKey(record[key])}`)
    .join(",")}}`;
}

function requestKey(req: Request): string {
  return `${req.kind}:${stableKey(req.params)}`;
}

export function requestTreeRefetch<R extends Request>(bridge: Bridge, req: R): Promise<ResponseFor<R>> {
  let pending = pendingByBridge.get(bridge);
  if (!pending) {
    pending = new Map();
    pendingByBridge.set(bridge, pending);
  }

  const key = requestKey(req);
  const existing = pending.get(key);
  if (existing) return existing as Promise<ResponseFor<R>>;

  const promise = bridge.request(req);
  pending.set(key, promise);
  queueMicrotask(() => {
    const current = pendingByBridge.get(bridge);
    if (current?.get(key) !== promise) return;
    current.delete(key);
    if (current.size === 0) pendingByBridge.delete(bridge);
  });
  return promise;
}
