import { describe, it, expect, beforeEach } from "vitest";
import {
  useRecording,
  markRecording,
  latchRecordModeFromMessage,
  __resetRecordModeForTests,
} from "../record-mode";
import { renderHook, act } from "@testing-library/react";

// Valid host-message fixtures (shapes per record-cursor-track.ts / record-cursor-bridge.ts).
const CURSOR_TRACK = {
  type: "ui/cursor-track",
  keys: [{ t: 0, vis: false, press: false, target: { kind: "point", x: 0, y: 0 } }],
};
const LEGACY_CURSOR = { type: "ui/cursor", x: 0, y: 0, visible: false, pressed: false };
const CURSOR_TICK = { type: "ui/cursor-tick", t: 0, frame: 0 };

describe("record-mode", () => {
  beforeEach(() => __resetRecordModeForTests());

  it("defaults to not recording", () => {
    const { result } = renderHook(() => useRecording());
    expect(result.current).toBe(false);
  });

  it("latches recording on when marked", () => {
    const { result } = renderHook(() => useRecording());
    act(() => markRecording());
    expect(result.current).toBe(true);
  });

  it("stays latched — a second mark is idempotent", () => {
    const { result } = renderHook(() => useRecording());
    act(() => markRecording());
    act(() => markRecording());
    expect(result.current).toBe(true);
  });

  describe("latchRecordModeFromMessage (App.tsx wiring guard)", () => {
    it("latches on a ui/cursor-track message (the target-clip path)", () => {
      const { result } = renderHook(() => useRecording());
      act(() => latchRecordModeFromMessage(CURSOR_TRACK));
      expect(result.current).toBe(true);
    });

    it("latches on a legacy ui/cursor message (hero/faith path)", () => {
      const { result } = renderHook(() => useRecording());
      act(() => latchRecordModeFromMessage(LEGACY_CURSOR));
      expect(result.current).toBe(true);
    });

    it("does NOT latch on a per-frame ui/cursor-tick", () => {
      const { result } = renderHook(() => useRecording());
      act(() => latchRecordModeFromMessage(CURSOR_TICK));
      expect(result.current).toBe(false);
    });

    it("does NOT latch on an unrelated message (interactive user stays un-recorded)", () => {
      const { result } = renderHook(() => useRecording());
      act(() => latchRecordModeFromMessage({ type: "engine/state/changed" }));
      act(() => latchRecordModeFromMessage("not even an object"));
      expect(result.current).toBe(false);
    });
  });
});
