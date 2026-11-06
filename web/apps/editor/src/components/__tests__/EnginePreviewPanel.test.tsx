// Panel behavior against a typed bridge: availability, timeline defaults and the
// async pending/result states. Rendering and host file writes are native tests.
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import * as Tooltip from "@radix-ui/react-tooltip";
import {
  DOCUMENT_EDITED_EVENT,
  ENGINE_PREVIEW_STILL_RENDERED_EVENT,
  ENGINE_PREVIEW_STATUS_CHECKED_EVENT,
  type Request,
} from "@particle-editor/bridge-schema";
import { EnginePreviewPanel } from "../EnginePreviewPanel";
import { useEnginePreview } from "@/lib/engine-preview";
import { CLOSED_TIMELINE, useTimelineStore } from "@/lib/timeline";
import { makeBridgeStub, type BridgeStub } from "@/test/bridge-stub";
import { MOCK_PREVIEW_PNG } from "@/bridge/mock-dispatch/dispatch-engine-preview";

const TIMELINE_TIME_SECONDS = 4.5;
const ENGINE_SECONDS = 2.6;
const STILL_REQUEST_ID = 2;
// The panel's DEFAULT_TIME_SECONDS, used when the timeline is closed.
const PANEL_DEFAULT_TIME_SECONDS = 2;
// Longer than the panel's AUTO_REFRESH_DEBOUNCE_MS (250 ms).
const PAST_DEBOUNCE_MS = 400;

beforeEach((): void => {
  useEnginePreview.setState(useEnginePreview.getInitialState(), true);
  useEnginePreview.setState({ isOpen: true });
  useTimelineStore.getState().apply(CLOSED_TIMELINE);
});
afterEach((): void => {
  vi.restoreAllMocks();
});

async function renderPanel(available: boolean = true): Promise<BridgeStub> {
  const bridge: BridgeStub = makeBridgeStub({
    responses: {
      "engine/state/snapshot": { enginePreviewAvailable: available },
      "enginePreview/status": { status: "pending", requestId: 1 },
      "enginePreview/still": { status: "pending", requestId: STILL_REQUEST_ID },
    },
  });
  render(
    <Tooltip.Provider>
      <EnginePreviewPanel bridge={bridge} />
    </Tooltip.Provider>,
  );
  await act(async (): Promise<void> => {
    await Promise.resolve();
  });
  if (available) {
    bridge.emit(ENGINE_PREVIEW_STATUS_CHECKED_EVENT, { state: "ready", requestId: 1 });
    await waitFor((): void => {
      expect(screen.getByRole("button", { name: "Preview" })).toBeEnabled();
    });
  }
  return bridge;
}

describe("EnginePreviewPanel", (): void => {
  it("is hidden when the snapshot says unavailable", async (): Promise<void> => {
    await renderPanel(false);
    expect(screen.queryByRole("region", { name: "Engine preview" })).not.toBeInTheDocument();
  });
  it("defaults to two seconds and camera matching off", async (): Promise<void> => {
    await renderPanel();
    expect(screen.getByRole("spinbutton")).toHaveValue(PANEL_DEFAULT_TIME_SECONDS);
    expect(screen.getByRole("checkbox", { name: "Match editor camera" })).not.toBeChecked();
  });
  it("defaults to the timeline's current time when open", async (): Promise<void> => {
    useTimelineStore.getState().apply({ ...CLOSED_TIMELINE, active: true, timeSeconds: TIMELINE_TIME_SECONDS });
    await renderPanel();
    expect(screen.getByRole("spinbutton")).toHaveValue(TIMELINE_TIME_SECONDS);
  });
  it("stays busy until its own event, then shows the image and round trip", async (): Promise<void> => {
    const bridge: BridgeStub = await renderPanel();
    fireEvent.click(screen.getByRole("button", { name: "Preview" }));
    await act(async (): Promise<void> => {
      await Promise.resolve();
    });
    expect(screen.getByRole("button", { name: "Preview" })).toBeDisabled();
    expect(screen.getByRole("button", { name: "Cancel" })).toBeEnabled();
    bridge.emit(ENGINE_PREVIEW_STILL_RENDERED_EVENT, { ok: false, requestId: STILL_REQUEST_ID - 1, error: "stale" });
    expect(screen.getByRole("button", { name: "Preview" })).toBeDisabled();
    bridge.emit(ENGINE_PREVIEW_STILL_RENDERED_EVENT, {
      ok: true,
      requestId: STILL_REQUEST_ID,
      dataUri: MOCK_PREVIEW_PNG,
      seconds: ENGINE_SECONDS,
      width: 1,
      height: 1,
    });
    await waitFor((): void => {
      expect(screen.getByRole("img", { name: "Engine preview still" })).toHaveAttribute("src", MOCK_PREVIEW_PNG);
    });
    // The engine share comes from the reply; the full round trip is measured in the panel.
    expect(screen.getByText(/Round trip \d+\.\d\d s \(engine\s+2\.60 s\)/)).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Preview" })).toBeEnabled();
  });
  it("shows the host error and sends cancellation", async (): Promise<void> => {
    const bridge: BridgeStub = await renderPanel();
    fireEvent.click(screen.getByRole("button", { name: "Preview" }));
    await act(async (): Promise<void> => {
      await Promise.resolve();
    });
    fireEvent.click(screen.getByRole("button", { name: "Cancel" }));
    expect(bridge.request).toHaveBeenCalledWith({ kind: "enginePreview/cancel", params: {} });
    bridge.emit(ENGINE_PREVIEW_STILL_RENDERED_EVENT, {
      ok: false,
      requestId: STILL_REQUEST_ID,
      error: "capture timed out",
    });
    await waitFor((): void => {
      expect(screen.getByRole("alert")).toHaveTextContent("capture timed out");
    });
  });
});

describe("EnginePreviewPanel auto-refresh", (): void => {
  function stillRequests(bridge: BridgeStub): Request[] {
    const requests: Request[] = bridge.request.mock.calls.map((call: [Request]): Request => call[0]);
    return requests.filter((request: Request): boolean => request.kind === "enginePreview/still");
  }
  async function waitPastDebounce(): Promise<void> {
    await act(async (): Promise<void> => {
      await new Promise((resolve: (value: unknown) => void): void => {
        setTimeout(resolve, PAST_DEBOUNCE_MS);
      });
    });
  }
  it("ignores edits while it is off", async (): Promise<void> => {
    const bridge: BridgeStub = await renderPanel();
    expect(bridge.listenerCount(DOCUMENT_EDITED_EVENT)).toBe(0);
    await waitPastDebounce();
    expect(stillRequests(bridge)).toHaveLength(0);
  });
  it("renders one still for a burst of edits once they pause", async (): Promise<void> => {
    const bridge: BridgeStub = await renderPanel();
    fireEvent.click(screen.getByRole("checkbox", { name: "Auto-refresh after edits" }));
    for (const revision of [1, 2, 3]) bridge.emit(DOCUMENT_EDITED_EVENT, { revision });
    expect(stillRequests(bridge)).toHaveLength(0);
    await waitPastDebounce();
    expect(stillRequests(bridge)).toEqual([
      { kind: "enginePreview/still", params: { timeSeconds: PANEL_DEFAULT_TIME_SECONDS, matchCamera: false } },
    ]);
  });
  it("renders the newest edit after the still in flight finishes", async (): Promise<void> => {
    const bridge: BridgeStub = await renderPanel();
    fireEvent.click(screen.getByRole("checkbox", { name: "Auto-refresh after edits" }));
    bridge.emit(DOCUMENT_EDITED_EVENT, { revision: 1 });
    await waitPastDebounce();
    bridge.emit(DOCUMENT_EDITED_EVENT, { revision: 2 });
    await waitPastDebounce();
    expect(stillRequests(bridge)).toHaveLength(1);
    bridge.emit(ENGINE_PREVIEW_STILL_RENDERED_EVENT, {
      ok: true,
      requestId: STILL_REQUEST_ID,
      dataUri: MOCK_PREVIEW_PNG,
      seconds: ENGINE_SECONDS,
      width: 1,
      height: 1,
    });
    await waitPastDebounce();
    expect(stillRequests(bridge)).toHaveLength(2);
  });
});
