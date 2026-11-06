// Live engine view: the badge mirrors the host's state events, and one start request checks
// the engine host first when needed. Streaming and drawing are native and live-checked.
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { act, render, screen } from "@testing-library/react";
import { ENGINE_LIVE_STATE_CHANGED_EVENT, ENGINE_PREVIEW_STATUS_CHECKED_EVENT } from "@particle-editor/bridge-schema";
import { EngineLiveBadge } from "../EngineLiveBadge";
import { startEngineLive, useEngineLive } from "@/lib/engine-live";
import { useEnginePreview } from "@/lib/engine-preview";
import { makeBridgeStub, type BridgeStub } from "@/test/bridge-stub";

const STATUS_REQUEST_ID = 1;
const FALLBACK_MESSAGE = "The engine host closed; showing the editor's own view.";
const MISSED_UPDATE_MESSAGE = "The engine view missed an update: timeout";
const LIVE_FPS = 59.6;
const STATS_POLL_MS = 1_000;

beforeEach((): void => {
  useEngineLive.setState(useEngineLive.getInitialState(), true);
  useEnginePreview.setState(useEnginePreview.getInitialState(), true);
});

function makeLiveBridge(): BridgeStub {
  return makeBridgeStub({
    responses: {
      "enginePreview/status": { status: "pending", requestId: STATUS_REQUEST_ID },
      "engineLive/start": { state: "starting" },
      "engineLive/stats": { state: "live", fps: LIVE_FPS, cameraLatencyMs: null },
    },
  });
}

describe("EngineLiveBadge", (): void => {
  it("renders nothing while the live view is off", (): void => {
    render(<EngineLiveBadge bridge={makeLiveBridge()} />);
    expect(screen.queryByRole("status")).not.toBeInTheDocument();
  });
  it("follows the host's state and shows why it fell back", (): void => {
    const bridge: BridgeStub = makeLiveBridge();
    render(<EngineLiveBadge bridge={bridge} />);
    bridge.emit(ENGINE_LIVE_STATE_CHANGED_EVENT, { state: "starting" });
    expect(screen.getByRole("status")).toHaveTextContent("Starting engine view…");
    bridge.emit(ENGINE_LIVE_STATE_CHANGED_EVENT, { state: "live" });
    expect(screen.getByRole("status")).toHaveTextContent("Engine view");
    bridge.emit(ENGINE_LIVE_STATE_CHANGED_EVENT, { state: "off", message: FALLBACK_MESSAGE });
    expect(screen.getByRole("status")).toHaveTextContent(FALLBACK_MESSAGE);
  });
});

describe("EngineLiveBadge while live", (): void => {
  beforeEach((): void => {
    vi.useFakeTimers();
  });
  afterEach((): void => {
    vi.useRealTimers();
  });
  it("shows the stream rate once the host reports it", async (): Promise<void> => {
    const bridge: BridgeStub = makeLiveBridge();
    render(<EngineLiveBadge bridge={bridge} />);
    bridge.emit(ENGINE_LIVE_STATE_CHANGED_EVENT, { state: "live" });
    await act(async (): Promise<void> => {
      await vi.advanceTimersByTimeAsync(STATS_POLL_MS);
    });
    expect(bridge.request).toHaveBeenCalledWith({ kind: "engineLive/stats", params: {} });
    expect(screen.getByRole("status")).toHaveTextContent("Engine view · 60 fps");
  });
  it("shows an update the host missed without leaving the live view", (): void => {
    const bridge: BridgeStub = makeLiveBridge();
    render(<EngineLiveBadge bridge={bridge} />);
    bridge.emit(ENGINE_LIVE_STATE_CHANGED_EVENT, { state: "live", message: MISSED_UPDATE_MESSAGE });
    expect(screen.getByRole("status")).toHaveTextContent(`Engine view: ${MISSED_UPDATE_MESSAGE}`);
  });
});

describe("startEngineLive", (): void => {
  it("checks the engine host first, then asks it to start", async (): Promise<void> => {
    const bridge: BridgeStub = makeLiveBridge();
    render(<EngineLiveBadge bridge={bridge} />);
    let started: Promise<void> = Promise.resolve();
    await act(async (): Promise<void> => {
      started = startEngineLive(bridge);
      await Promise.resolve();
    });
    bridge.emit(ENGINE_PREVIEW_STATUS_CHECKED_EVENT, { state: "ready", requestId: STATUS_REQUEST_ID });
    await act(async (): Promise<void> => {
      await started;
    });
    expect(bridge.request).toHaveBeenCalledWith({ kind: "engineLive/start", params: {} });
    expect(useEngineLive.getState().state).toBe("starting");
  });
  it("reports why it cannot start when the host is not ready", async (): Promise<void> => {
    const bridge: BridgeStub = makeLiveBridge();
    let started: Promise<void> = Promise.resolve();
    await act(async (): Promise<void> => {
      started = startEngineLive(bridge);
      await Promise.resolve();
    });
    bridge.emit(ENGINE_PREVIEW_STATUS_CHECKED_EVENT, {
      state: "absent",
      requestId: STATUS_REQUEST_ID,
      error: "Start the engine preview host, then retry the status check.",
    });
    await act(async (): Promise<void> => {
      await started;
    });
    expect(bridge.request).not.toHaveBeenCalledWith({ kind: "engineLive/start", params: {} });
    expect(useEngineLive.getState().message).toContain("Start the engine preview host");
  });
});
