// run-after-paint.ts — defer non-paint-critical startup work to the first idle
// slot after first interactive paint, instead of competing with it.
//
// The startup bridge fan-out (perf-audit P1) fires several requests before the
// app's first interactive paint; the non-critical ones (recent-file list, mod
// list) are deferred through here so they run once the browser is idle. The
// `timeout` forces the callback to run within that budget even in a backgrounded
// tab (where idle callbacks can otherwise be starved indefinitely).

export function runWhenIdle(fn: () => void, timeout = 1000): () => void {
  if (typeof requestIdleCallback === "function") {
    const id = requestIdleCallback(() => fn(), { timeout });
    return () => cancelIdleCallback(id);
  }
  // jsdom / older runtimes without the idle API: a macrotask is close enough.
  const id = setTimeout(fn, 0);
  return () => clearTimeout(id);
}
