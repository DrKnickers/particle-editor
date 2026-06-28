import { afterEach, describe, expect, it, vi } from "vitest";
import {
  emitClockCalibration,
  emitPerfTrace,
  makePerfSpanId,
  traceBridgeRequestEnd,
  traceBridgeRequestStart,
} from "../perf-trace";

function installLocation(search: string) {
  window.history.replaceState(null, "", `/${search}`);
}

function installPostMessage() {
  const postMessage = vi.fn();
  (window as any).chrome = { webview: { postMessage } };
  return postMessage;
}

describe("perf-trace", () => {
  afterEach(() => {
    delete (window as any).chrome;
    installLocation("");
  });

  it("does not post when perfTrace query flag is absent", () => {
    const postMessage = installPostMessage();
    emitPerfTrace({ eventName: "x", eventType: "instant" });
    expect(postMessage).not.toHaveBeenCalled();
  });

  it("posts renderer trace events when enabled", () => {
    installLocation("?perfTrace=1");
    const postMessage = installPostMessage();
    emitPerfTrace({ eventName: "renderer.test", eventType: "instant", value: 42 });
    expect(postMessage).toHaveBeenCalledTimes(1);
    const msg = JSON.parse(postMessage.mock.calls[0][0]);
    expect(msg.kind).toBe("perf/trace");
    expect(msg.event.eventName).toBe("renderer.test");
    expect(msg.event.eventType).toBe("instant");
    expect(msg.event.value).toBe(42);
    expect(typeof msg.event.rendererTimestampMs).toBe("number");
    expect(typeof msg.event.rendererTimeOriginMs).toBe("number");
  });

  it("posts clock calibration when enabled", () => {
    installLocation("?perfTrace=1");
    const postMessage = installPostMessage();
    emitClockCalibration("unit");
    const msg = JSON.parse(postMessage.mock.calls[0][0]);
    expect(msg.kind).toBe("perf/clock-calibration");
    expect(msg.sampleId).toBe("unit");
    expect(typeof msg.rendererNowMs).toBe("number");
    expect(typeof msg.rendererTimeOriginMs).toBe("number");
  });

  it("does not measure bridge requests when disabled", () => {
    const postMessage = installPostMessage();
    const startMs = traceBridgeRequestStart("emitters/list", "7", "async");
    traceBridgeRequestEnd("emitters/list", "7", "async", startMs, "ok");
    expect(startMs).toBe(-1);
    expect(postMessage).not.toHaveBeenCalled();
  });

  it("uses requestId as the renderer bridge spanId", () => {
    installLocation("?perfTrace=1");
    const postMessage = installPostMessage();
    const startMs = traceBridgeRequestStart("emitters/list", "42", "sync");
    traceBridgeRequestEnd("emitters/list", "42", "sync", startMs, "ok");
    expect(postMessage).toHaveBeenCalledTimes(2);
    const start = JSON.parse(postMessage.mock.calls[0][0]).event;
    const end = JSON.parse(postMessage.mock.calls[1][0]).event;
    expect(start.eventType).toBe("span_start");
    expect(end.eventType).toBe("span_end");
    expect(start.spanId).toBe("42");
    expect(end.spanId).toBe("42");
    expect(end.durationMs).toBeGreaterThanOrEqual(0);
  });

  it("creates non-empty sanitized renderer span ids", () => {
    const id = makePerfSpanId("atlas.fetch_props", "fire.dds", true, "a/b");
    expect(id).toMatch(/^atlas\.fetch_props:fire\.dds:true:a_b:/);
    expect(id).not.toContain("/");
  });
});
