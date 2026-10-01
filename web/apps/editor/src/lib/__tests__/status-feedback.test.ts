// Unit tests for the status-feedback store + announceWhenOk (F4) + fireAndReport.
import { describe, it, expect, beforeEach, vi } from "vitest";
import { isRefusal, type Bridge } from "@particle-editor/bridge-schema";
import {
  announceWhenOk,
  fireAndReport,
  useStatusFeedback,
  __resetStatusFeedbackForTests,
} from "../status-feedback";

beforeEach(() => {
  __resetStatusFeedbackForTests();
});

const flush = () => new Promise((r) => setTimeout(r, 0));

describe("status-feedback", () => {
  it("announce sets the message and bumps the epoch (latest wins)", () => {
    useStatusFeedback.getState().announce("one");
    useStatusFeedback.getState().announce("two");
    expect(useStatusFeedback.getState().message).toBe("two");
    expect(useStatusFeedback.getState().epoch).toBe(2);
  });

  it("clear() is epoch-guarded: a stale timer cannot clear a newer message", () => {
    useStatusFeedback.getState().announce("one"); // epoch 1
    useStatusFeedback.getState().announce("two"); // epoch 2
    useStatusFeedback.getState().clear(1); // stale
    expect(useStatusFeedback.getState().message).toBe("two");
    useStatusFeedback.getState().clear(2); // current
    expect(useStatusFeedback.getState().message).toBeNull();
  });

  it("announceWhenOk announces after a plain resolution", async () => {
    announceWhenOk(Promise.resolve({}), "did it");
    await flush();
    expect(useStatusFeedback.getState().message).toBe("did it");
  });

  it("announceWhenOk stays SILENT on ok:false (refused mutation)", async () => {
    announceWhenOk(Promise.resolve({ ok: false }), "nope");
    await flush();
    expect(useStatusFeedback.getState().message).toBeNull();
  });

  it("announceWhenOk stays SILENT on rejection", async () => {
    announceWhenOk(Promise.reject(new Error("boom")), "nope");
    await flush();
    expect(useStatusFeedback.getState().message).toBeNull();
  });
});

describe("fireAndReport", () => {
  const bridgeReturning = (impl: () => unknown) =>
    ({ request: vi.fn(impl), on: vi.fn() }) as unknown as Bridge & { request: ReturnType<typeof vi.fn> };

  it("sends the request and stays silent on success", async () => {
    const b = bridgeReturning(() => Promise.resolve({ applied: true }));
    fireAndReport(b, { kind: "undo/perform", params: { direction: "undo" } }, "Undo");
    expect(b.request).toHaveBeenCalledWith({ kind: "undo/perform", params: { direction: "undo" } });
    await flush();
    expect(useStatusFeedback.getState().message).toBeNull();
  });

  it("reports a rejected request (channel 1) in status feedback instead of an unhandled rejection", async () => {
    const warn = vi.spyOn(console, "warn").mockImplementation(() => {});
    const b = bridgeReturning(() => Promise.reject(new Error("host gone")));
    fireAndReport(b, { kind: "engine/set/paused", params: { paused: true } }, "Pause");
    await flush();
    expect(useStatusFeedback.getState().message).toBe("Pause failed: host gone");
    warn.mockRestore();
  });

  it("reports an in-band ok:false refusal (channel 2) with the host's reason", async () => {
    const warn = vi.spyOn(console, "warn").mockImplementation(() => {});
    const b = bridgeReturning(() => Promise.resolve({ ok: false, error: "slot full" }));
    fireAndReport(b, { kind: "emitters/drop", params: { mode: "reorder", id: 0, rootIndex: 0 } }, "Move");
    await flush();
    expect(useStatusFeedback.getState().message).toBe("Move failed: slot full");
    warn.mockRestore();
  });

  it("resolves with the response on success and undefined on failure (never rejects)", async () => {
    vi.spyOn(console, "warn").mockImplementation(() => {});
    const ok = bridgeReturning(() => Promise.resolve({ applied: true }));
    await expect(fireAndReport(ok, { kind: "undo/perform", params: { direction: "redo" } }, "Redo")).resolves.toEqual({
      applied: true,
    });
    const bad = bridgeReturning(() => { throw new Error("sync throw"); });
    await expect(fireAndReport(bad, { kind: "undo/perform", params: { direction: "redo" } }, "Redo")).resolves.toBeUndefined();
    expect(useStatusFeedback.getState().message).toBe("Redo failed: sync throw");
    vi.restoreAllMocks();
  });
});

describe("isRefusal", () => {
  it("matches only an explicit ok:false", () => {
    expect(isRefusal({ ok: false, error: "x" })).toBe(true);
    expect(isRefusal({ ok: false, reason: "pins-full" })).toBe(true);
    expect(isRefusal({ ok: true })).toBe(false);
    expect(isRefusal({})).toBe(false);
    expect(isRefusal(null)).toBe(false);
    expect(isRefusal(false)).toBe(false);
    expect(isRefusal({ ok: 0 })).toBe(false);
  });
});
