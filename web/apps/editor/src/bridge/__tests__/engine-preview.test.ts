// Verifies preview request/response/event contracts in the mock bridge. The mock
// supplies images and states only; it does not stand in for native pipe tests.
import { afterEach, beforeEach, describe, expect, it, vi, type Mock } from "vitest";
import {
  ENGINE_PREVIEW_STILL_RENDERED_EVENT,
  ENGINE_PREVIEW_STATUS_CHECKED_EVENT,
  type EnginePreviewStatusDto,
  type EnginePreviewPendingDto,
  type EngineStateDto,
} from "@particle-editor/bridge-schema";
import { MockBridge } from "../mock";
import { useMockEngineState } from "../mock-state";

const DEFAULT_TIME_SECONDS = 2;
const OUT_OF_RANGE_TIME_SECONDS = 601;

beforeEach((): void => {
  vi.useFakeTimers();
});
afterEach((): void => {
  vi.clearAllTimers();
  vi.useRealTimers();
});

describe("engine preview bridge", (): void => {
  const states: EnginePreviewStatusDto["state"][] = ["absent", "unsupported-build", "not-configured", "ready"];
  it.each(states)(
    "reports %s and its snapshot capability",
    async (state: EnginePreviewStatusDto["state"]): Promise<void> => {
      const bridge: MockBridge = new MockBridge();
      bridge.enginePreview.state = state;
      const received: Mock = vi.fn();
      bridge.on(ENGINE_PREVIEW_STATUS_CHECKED_EVENT, received);
      const pending: EnginePreviewPendingDto = await bridge.request({ kind: "enginePreview/status", params: {} });
      expect(pending.status).toBe("pending");
      await vi.runAllTimersAsync();
      expect(received).toHaveBeenCalledWith(
        expect.objectContaining({ payload: expect.objectContaining({ state, requestId: pending.requestId }) }),
      );
      expect(useMockEngineState.getState().enginePreviewAvailable).toBe(state === "ready");
    },
  );

  it("keeps status absent with a saved executable until a still is requested", async (): Promise<void> => {
    const bridge: MockBridge = new MockBridge();
    await bridge.request({ kind: "engineHost/settings/set", params: { exePath: "C:\\Preview\\editor.exe" } });
    const checked: Mock = vi.fn();
    const rendered: Mock = vi.fn();
    bridge.on(ENGINE_PREVIEW_STATUS_CHECKED_EVENT, checked);
    bridge.on(ENGINE_PREVIEW_STILL_RENDERED_EVENT, rendered);
    await bridge.request({ kind: "enginePreview/status", params: {} });
    await vi.runAllTimersAsync();
    expect(checked).toHaveBeenCalledWith(expect.objectContaining({ payload: { state: "absent", requestId: 1 } }));
    expect(bridge.enginePreview.state).toBe("absent");
    expect(bridge.enginePreview.live).toBe("off");
    expect(useMockEngineState.getState().enginePreviewAvailable).toBe(true);
    await bridge.request({
      kind: "enginePreview/still",
      params: { timeSeconds: DEFAULT_TIME_SECONDS, matchCamera: false },
    });
    await vi.runAllTimersAsync();
    expect(rendered).toHaveBeenCalledWith(expect.objectContaining({ payload: expect.objectContaining({ ok: true }) }));
    expect(bridge.enginePreview.state).toBe("ready");
  });

  it("delivers a canned still after pending without changing the document", async (): Promise<void> => {
    const bridge: MockBridge = new MockBridge();
    bridge.enginePreview.state = "ready";
    const before: EngineStateDto = await bridge.request({ kind: "engine/state/snapshot", params: {} });
    const received: Mock = vi.fn();
    bridge.on(ENGINE_PREVIEW_STILL_RENDERED_EVENT, received);
    const pending: EnginePreviewPendingDto = await bridge.request({
      kind: "enginePreview/still",
      params: { timeSeconds: DEFAULT_TIME_SECONDS, matchCamera: false },
    });
    expect(received).not.toHaveBeenCalled();
    await vi.runAllTimersAsync();
    expect(received).toHaveBeenCalledWith(
      expect.objectContaining({
        payload: expect.objectContaining({
          ok: true,
          requestId: pending.requestId,
          dataUri: expect.stringMatching(/^data:image\/png;base64,/),
        }),
      }),
    );
    const after: EngineStateDto = await bridge.request({ kind: "engine/state/snapshot", params: {} });
    expect(after.currentFilePath).toBe(before.currentFilePath);
    expect(after.dirty).toBe(before.dirty);
  });

  it("delivers host errors and cancellation as failed results", async (): Promise<void> => {
    const bridge: MockBridge = new MockBridge();
    bridge.enginePreview.state = "ready";
    bridge.enginePreview.error = "Open-map timed out";
    const received: Mock = vi.fn();
    bridge.on(ENGINE_PREVIEW_STILL_RENDERED_EVENT, received);
    await bridge.request({
      kind: "enginePreview/still",
      params: { timeSeconds: DEFAULT_TIME_SECONDS, matchCamera: false },
    });
    await vi.runAllTimersAsync();
    expect(received).toHaveBeenLastCalledWith(
      expect.objectContaining({ payload: expect.objectContaining({ ok: false, error: "Open-map timed out" }) }),
    );
    bridge.enginePreview.error = null;
    const pending: EnginePreviewPendingDto = await bridge.request({
      kind: "enginePreview/still",
      params: { timeSeconds: DEFAULT_TIME_SECONDS, matchCamera: false },
    });
    await bridge.request({ kind: "enginePreview/cancel", params: {} });
    await vi.runAllTimersAsync();
    expect(received).toHaveBeenLastCalledWith(
      expect.objectContaining({
        payload: { ok: false, requestId: pending.requestId, error: "Engine preview cancelled." },
      }),
    );
  });

  it("reports unavailable stills and rejects invalid times", async (): Promise<void> => {
    const bridge: MockBridge = new MockBridge();
    const received: Mock = vi.fn();
    bridge.on(ENGINE_PREVIEW_STILL_RENDERED_EVENT, received);
    await bridge.request({
      kind: "enginePreview/still",
      params: { timeSeconds: DEFAULT_TIME_SECONDS, matchCamera: false },
    });
    await vi.runAllTimersAsync();
    expect(received).toHaveBeenCalledWith(
      expect.objectContaining({
        payload: expect.objectContaining({ ok: false, error: expect.stringMatching(/Preferences/) }),
      }),
    );
    bridge.enginePreview.state = "ready";
    await expect(
      bridge.request({
        kind: "enginePreview/still",
        params: { timeSeconds: OUT_OF_RANGE_TIME_SECONDS, matchCamera: false },
      }),
    ).rejects.toThrow(/600/);
  });
});
