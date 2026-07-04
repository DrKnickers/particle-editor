import { describe, it, expect, vi, beforeEach, afterEach } from "vitest";
import { parseCursorMessage, postFrameAcked, isRecordHeadlessMessage, commitAndAck } from "../record-cursor-bridge";

describe("record-cursor-bridge", () => {
  it("parses a ui/cursor message (object form)", () => {
    expect(parseCursorMessage({ type: "ui/cursor", x: 10, y: 20, visible: true, pressed: false }))
      .toEqual({ x: 10, y: 20, visible: true, pressed: false });
  });
  it("parses a ui/cursor message (string form)", () => {
    expect(parseCursorMessage(JSON.stringify({ type: "ui/cursor", x: 1, y: 2, visible: false, pressed: true })))
      .toEqual({ x: 1, y: 2, visible: false, pressed: true });
  });
  it("ignores non-cursor messages", () => {
    expect(parseCursorMessage({ type: "engine/state/changed" })).toBeNull();
    expect(parseCursorMessage(null)).toBeNull();
    expect(parseCursorMessage("not json")).toBeNull();
  });
  it("posts a frame-acked message (JSON string) with the frame id", () => {
    const postMessage = vi.fn();
    (window as unknown as { chrome: { webview: { postMessage: typeof postMessage } } }).chrome = {
      webview: { postMessage },
    };
    postFrameAcked(7);
    expect(postMessage).toHaveBeenCalledWith(JSON.stringify({ type: "ui/frame-acked", frame: 7 }));
  });

  describe("commitAndAck", () => {
    // Controllable rAF: callbacks queue instead of firing, so a test can prove
    // the ack posts BEFORE (headless) or only AFTER (legacy) rAF drains.
    let rafQueue: FrameRequestCallback[] = [];
    beforeEach(() => {
      rafQueue = [];
      vi.stubGlobal("requestAnimationFrame", (cb: FrameRequestCallback) => {
        rafQueue.push(cb);
        return rafQueue.length;
      });
    });
    afterEach(() => vi.unstubAllGlobals());
    const drainRaf = () => { const q = rafQueue; rafQueue = []; q.forEach((cb) => cb(0)); };
    const passthroughFlush = (fn: () => void) => fn();

    it("headless: flushSyncs the frame and posts the ack SYNCHRONOUSLY (no rAF)", () => {
      const order: string[] = [];
      const applyFrame = () => order.push("apply");
      const post = () => order.push("post");
      commitAndAck({ headless: true, applyFrame, post, flushSync: (fn) => { order.push("flush"); fn(); } });
      // Ack is posted immediately, before any rAF has been given a chance to run.
      expect(order).toEqual(["flush", "apply", "post"]);
      expect(rafQueue.length).toBe(0);
    });

    it("headless: withholds the ack (fail loud) when flushSync THROWS", () => {
      const post = vi.fn();
      const err = vi.spyOn(console, "error").mockImplementation(() => {});
      commitAndAck({ headless: true, applyFrame: () => {}, post, flushSync: () => { throw new Error("commit failed"); } });
      expect(post).not.toHaveBeenCalled();   // no false-good frame
      expect(err).toHaveBeenCalled();
      err.mockRestore();
    });

    it("legacy: applies immediately but posts the ack only after a DOUBLE rAF", () => {
      const post = vi.fn();
      const applyFrame = vi.fn();
      commitAndAck({ headless: false, applyFrame, post, flushSync: passthroughFlush });
      expect(applyFrame).toHaveBeenCalledTimes(1);
      expect(post).not.toHaveBeenCalled();   // not yet — waiting on rAF
      drainRaf();                            // first rAF -> schedules the second
      expect(post).not.toHaveBeenCalled();
      drainRaf();                            // second rAF -> posts
      expect(post).toHaveBeenCalledTimes(1);
    });

    it("legacy: does NOT call flushSync (preserves the golden-diff baseline path)", () => {
      const flushSync = vi.fn();
      commitAndAck({ headless: false, applyFrame: () => {}, post: () => {}, flushSync });
      expect(flushSync).not.toHaveBeenCalled();
    });
  });

  describe("isRecordHeadlessMessage", () => {
    it("detects the ui/record-headless push (object + string form)", () => {
      expect(isRecordHeadlessMessage({ type: "ui/record-headless" })).toBe(true);
      expect(isRecordHeadlessMessage(JSON.stringify({ type: "ui/record-headless" }))).toBe(true);
    });
    it("rejects other messages and malformed input", () => {
      expect(isRecordHeadlessMessage({ type: "ui/cursor" })).toBe(false);
      expect(isRecordHeadlessMessage({ type: "ui/frame-acked", frame: 1 })).toBe(false);
      expect(isRecordHeadlessMessage("not json")).toBe(false);
      expect(isRecordHeadlessMessage(null)).toBe(false);
      expect(isRecordHeadlessMessage(undefined)).toBe(false);
    });
  });
});
