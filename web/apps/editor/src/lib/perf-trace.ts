type TraceEvent = Record<string, unknown> & {
  eventName: string;
  eventType: string;
};

function perfEnabled(): boolean {
  if (typeof window === "undefined") return false;
  return new URLSearchParams(window.location.search).get("perfTrace") === "1";
}

export function isPerfTraceEnabled(): boolean {
  return perfEnabled();
}

function postPerfMessage(payload: unknown): void {
  if (!perfEnabled()) return;
  const postMessage = window.chrome?.webview?.postMessage;
  if (!postMessage) return;
  try {
    postMessage(JSON.stringify(payload));
  } catch {
    // Perf tracing must never perturb editor behavior.
  }
}

export function emitPerfTrace(event: TraceEvent): void {
  if (!isPerfTraceEnabled()) return;
  postPerfMessage({
    kind: "perf/trace",
    event: {
      rendererTimestampMs: performance.now(),
      rendererTimeOriginMs: performance.timeOrigin,
      ...event,
    },
  });
}

export function emitClockCalibration(sampleId = "startup"): void {
  if (!isPerfTraceEnabled()) return;
  postPerfMessage({
    kind: "perf/clock-calibration",
    sampleId,
    rendererNowMs: performance.now(),
    rendererTimeOriginMs: performance.timeOrigin,
  });
}

export function makePerfSpanId(prefix: string, ...parts: Array<string | number | boolean | null | undefined>): string {
  const safePrefix = prefix.replace(/[^a-zA-Z0-9_.-]+/g, "_");
  const safeParts = parts.map((part) =>
    String(part ?? "none").replace(/[^a-zA-Z0-9_.-]+/g, "_").slice(0, 80),
  );
  return [safePrefix, ...safeParts, performance.now().toFixed(3)].join(":");
}

export function traceBridgeRequestStart(
  bridgeKind: string,
  requestId: string,
  bridgeMode: "async" | "sync"
): number {
  if (!isPerfTraceEnabled()) return -1;
  const startMs = performance.now();
  emitPerfTrace({
    eventName: "renderer.bridge.request",
    eventType: "span_start",
    bridgeKind,
    requestId,
    spanId: requestId,
    bridgeMode,
    rendererStartMs: startMs,
  });
  return startMs;
}

export function traceBridgeRequestEnd(
  bridgeKind: string,
  requestId: string,
  bridgeMode: "async" | "sync",
  startMs: number,
  status: "ok" | "error",
  error?: string
): void {
  if (startMs < 0 || !isPerfTraceEnabled()) return;
  const endMs = performance.now();
  emitPerfTrace({
    eventName: "renderer.bridge.request",
    eventType: "span_end",
    bridgeKind,
    requestId,
    spanId: requestId,
    bridgeMode,
    rendererStartMs: startMs,
    rendererEndMs: endMs,
    durationMs: Math.max(0, endMs - startMs),
    status,
    ...(error ? { error } : {}),
  });
}
