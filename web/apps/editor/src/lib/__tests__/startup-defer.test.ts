// Tranche E / perf-audit P1a: non-paint-critical startup bridge requests are
// DEFERRED to the first idle slot after first paint; paint-critical ones stay
// eager. Uses a controllable fake requestIdleCallback (queue + flush) that
// OVERRIDES the synchronous stub in test-setup, so we can observe the
// before-flush vs after-flush state — the one numeric "fan-out reduced" evidence.
import { describe, it, expect, beforeEach, afterEach } from "vitest";
import { renderHook } from "@testing-library/react";
import { runWhenIdle } from "@/lib/run-after-paint";
import { initModStack } from "@/lib/mod-stack";
import { useSeedFileState } from "@/lib/file-state";
import type { Bridge } from "@particle-editor/bridge-schema";

// --- controllable fake idle ---
let idleQueue: Array<() => void>;
let origRIC: typeof globalThis.requestIdleCallback;
let origCIC: typeof globalThis.cancelIdleCallback;
function flushIdle() {
  const q = idleQueue;
  idleQueue = [];
  q.forEach((fn) => fn());
}
beforeEach(() => {
  idleQueue = [];
  origRIC = globalThis.requestIdleCallback;
  origCIC = globalThis.cancelIdleCallback;
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  (globalThis as any).requestIdleCallback = (cb: () => void) => {
    idleQueue.push(cb);
    return idleQueue.length; // 1-based id
  };
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  (globalThis as any).cancelIdleCallback = (id: number) => { idleQueue[id - 1] = () => {}; };
});
afterEach(() => {
  globalThis.requestIdleCallback = origRIC;
  globalThis.cancelIdleCallback = origCIC;
});

function recordingBridge(): Bridge & { kinds: string[] } {
  const kinds: string[] = [];
  const bridge = {
    request: (req: { kind: string }) => { kinds.push(req.kind); return Promise.resolve({ stack: [], paths: [], currentFilePath: "", dirty: false }); },
    on: () => () => {},
  } as unknown as Bridge & { kinds: string[] };
  bridge.kinds = kinds;
  return bridge;
}

describe("runWhenIdle", () => {
  it("defers fn to the idle queue and runs it on flush; cancel prevents it", () => {
    const ran: string[] = [];
    runWhenIdle(() => ran.push("a"));
    const cancel = runWhenIdle(() => ran.push("b"));
    expect(ran).toEqual([]);        // nothing ran synchronously
    cancel();                        // cancel "b"
    flushIdle();
    expect(ran).toEqual(["a"]);     // only "a" ran
  });
});

describe("startup fan-out deferral (P1a)", () => {
  it("mod stack: mods/list is NOT requested before first paint, only after idle", () => {
    const bridge = recordingBridge();
    const stop = initModStack(bridge);
    expect(bridge.kinds).not.toContain("mods/list");   // deferred
    flushIdle();
    expect(bridge.kinds).toContain("mods/list");        // ran after idle
    stop();
  });

  it("file state: engine/state/snapshot is EAGER (title), file/recent/list DEFERRED", () => {
    const bridge = recordingBridge();
    const { unmount } = renderHook(() => useSeedFileState(bridge));
    // snapshot drives currentFilePath/dirty -> document.title: must be eager.
    expect(bridge.kinds).toContain("engine/state/snapshot");
    // the recent list is non-paint-critical: deferred until idle.
    expect(bridge.kinds).not.toContain("file/recent/list");
    flushIdle();
    expect(bridge.kinds).toContain("file/recent/list");
    unmount();
  });
});
