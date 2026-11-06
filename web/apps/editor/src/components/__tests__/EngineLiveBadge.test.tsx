// Live engine view: the badge mirrors host events; explicit start delegates launch to the host.
// Streaming and drawing remain native and are not simulated here.
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { act, render, screen } from "@testing-library/react";
import { ENGINE_LIVE_STATE_CHANGED_EVENT, type EngineLiveStatsDto } from "@particle-editor/bridge-schema";
import { EngineLiveBadge } from "../EngineLiveBadge";
import { startEngineLive, useEngineLive } from "@/lib/engine-live";
import { useEnginePreview } from "@/lib/engine-preview";
import { makeBridgeStub, type BridgeStub } from "@/test/bridge-stub";

const FALLBACK_MESSAGE = "The engine host closed; showing the editor's own view.";
const MISSED_UPDATE_MESSAGE = "The engine view missed an update: timeout";
const LIVE_FPS = 59.6;
const STATS_POLL_MS = 1_000;
const GROUND_NOTE = "Engine view keeps its ground visible at height 0.";
const MESSAGE_VISIBLE_MS = 6_000;

beforeEach((): void => {
  useEngineLive.setState(useEngineLive.getInitialState(), true);
  useEnginePreview.setState(useEnginePreview.getInitialState(), true);
});

function makeLiveBridge(): BridgeStub {
  return makeBridgeStub({
    responses: {
      "engineLive/start": { state: "starting" },
      "engineLive/stats": { state: "live", fps: LIVE_FPS, cameraLatencyMs: null, groundNote: "" },
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
  it("keeps the ground note after transient messages expire and clears it when defaults return", async (): Promise<void> => {
    let groundNote: string = GROUND_NOTE;
    const bridge: BridgeStub = makeBridgeStub({
      responses: {
        "engineLive/stats": (): EngineLiveStatsDto => ({
          state: "live",
          fps: LIVE_FPS,
          cameraLatencyMs: null,
          groundNote,
        }),
      },
    });
    render(<EngineLiveBadge bridge={bridge} />);
    bridge.emit(ENGINE_LIVE_STATE_CHANGED_EVENT, { state: "live", message: MISSED_UPDATE_MESSAGE });
    await act(async (): Promise<void> => {
      await vi.advanceTimersByTimeAsync(MESSAGE_VISIBLE_MS);
    });
    expect(screen.getByRole("status")).toHaveTextContent(GROUND_NOTE);
    expect(screen.getByRole("status")).not.toHaveTextContent(MISSED_UPDATE_MESSAGE);
    groundNote = "";
    await act(async (): Promise<void> => {
      await vi.advanceTimersByTimeAsync(STATS_POLL_MS);
    });
    expect(screen.getByRole("status")).not.toHaveTextContent(GROUND_NOTE);
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
  it("asks the host to start even when passive status is absent", async (): Promise<void> => {
    const bridge: BridgeStub = makeLiveBridge();
    useEnginePreview.setState({ status: { state: "absent", requestId: 1 } });
    await startEngineLive(bridge);
    expect(bridge.request).toHaveBeenCalledWith({ kind: "engineLive/start", params: {} });
    expect(bridge.request).toHaveBeenCalledTimes(1);
    expect(useEngineLive.getState().state).toBe("starting");
  });
  it("reports the explicit start refusal", async (): Promise<void> => {
    const message = "Set the map editor executable in Preferences > Engine preview.";
    const bridge: BridgeStub = makeBridgeStub({
      responses: {
        "engineLive/start": (): never => {
          throw new Error(message);
        },
      },
    });
    await startEngineLive(bridge);
    expect(bridge.request).toHaveBeenCalledWith({ kind: "engineLive/start", params: {} });
    expect(bridge.request).toHaveBeenCalledTimes(1);
    expect(useEngineLive.getState().state).toBe("off");
    expect(useEngineLive.getState().message).toBe(message);
  });
});
