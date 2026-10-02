import { afterEach, describe, expect, it, vi } from "vitest";
import { act, render, screen } from "@testing-library/react";
import { ViewportUnavailableNotice } from "../ViewportUnavailableNotice";
import { makeBridgeStub } from "@/test/bridge-stub";
import { MockBridge } from "@/bridge/mock";
import { NativeBridge } from "@/bridge/native";

afterEach(() => {
  vi.useRealTimers();
  delete (window as unknown as { chrome?: unknown }).chrome;
});

describe("ViewportUnavailableNotice", () => {
  it("renders nothing when preview attachment succeeds", () => {
    render(<ViewportUnavailableNotice bridge={makeBridgeStub()} />);
    expect(screen.queryByRole("alert")).not.toBeInTheDocument();
  });

  it("shows a persistent alert with the reason, editing reassurance and recovery hint", () => {
    vi.useFakeTimers();
    const bridge = makeBridgeStub();
    render(<ViewportUnavailableNotice bridge={bridge} />);
    bridge.emit("viewport/unavailable", { reason: "The graphics frame could not be attached." });
    const alert = screen.getByRole("alert");
    expect(alert).toHaveTextContent("3D preview unavailable");
    expect(alert).toHaveTextContent("The graphics frame could not be attached.");
    expect(alert).toHaveTextContent("You can still edit and save.");
    expect(alert).toHaveTextContent("updating your graphics driver");
    expect(alert).toHaveTextContent("Details are in the log.");
    expect(screen.queryByRole("button")).not.toBeInTheDocument();
    expect(alert.className).toContain("bg-panel");
    expect(alert.className).toContain("text-text");
    expect(alert.className).toContain("border-danger-fg");

    bridge.emit("stats/tick", { overload: false });
    bridge.emit("engine/state/changed", {});
    act(() => vi.advanceTimersByTime(60 * 60 * 1000));
    expect(screen.getByRole("alert")).toBe(alert);
  });

  it("unsubscribes on unmount and clears the condition when switching host sessions", () => {
    const first = makeBridgeStub();
    const second = makeBridgeStub();
    const view = render(<ViewportUnavailableNotice bridge={first} />);
    first.emit("viewport/unavailable", { reason: "Surface initialization failed." });
    expect(first.listenerCount("viewport/unavailable")).toBe(1);
    view.rerender(<ViewportUnavailableNotice bridge={second} />);
    expect(first.listenerCount("viewport/unavailable")).toBe(0);
    expect(screen.queryByRole("alert")).not.toBeInTheDocument();
    view.unmount();
    expect(second.listenerCount("viewport/unavailable")).toBe(0);
  });

  it("can be triggered in the mock app and survives layout remount and document edits", async () => {
    const bridge = new MockBridge();
    const view = render(<ViewportUnavailableNotice bridge={bridge} />);
    act(() => bridge.setViewportUnavailable("No shared frame surface."));
    expect(screen.getByRole("alert")).toHaveTextContent("No shared frame surface.");
    view.unmount();
    await bridge.request({ kind: "file/new", params: {} });
    render(<ViewportUnavailableNotice bridge={bridge} />);
    expect(screen.getByRole("alert")).toHaveTextContent("No shared frame surface.");
  });

  it("receives a native event before subscribing, on remount, and after a page reload replay", () => {
    let receive: (e: { data: unknown }) => void = () => {};
    (window as unknown as { chrome: unknown }).chrome = {
      webview: {
        postMessage: vi.fn(),
        addEventListener: (_ev: string, h: typeof receive) => { receive = h; },
      },
    };
    const event = {
      type: "evt", kind: "viewport/unavailable",
      payload: { reason: "The graphics frame could not be attached." },
    };
    const bridge = new NativeBridge();
    receive({ data: event });
    const first = render(<ViewportUnavailableNotice bridge={bridge} />);
    expect(screen.getByRole("alert")).toHaveTextContent(event.payload.reason);
    first.unmount();
    const remount = render(<ViewportUnavailableNotice bridge={bridge} />);
    expect(screen.getByRole("alert")).toHaveTextContent(event.payload.reason);
    remount.unmount();
    bridge.dispose();

    // A new page has a fresh bridge and learns the condition from the host's
    // app/ready replay, not from the previous page's memory.
    const reloaded = new NativeBridge();
    render(<ViewportUnavailableNotice bridge={reloaded} />);
    expect(screen.queryByRole("alert")).not.toBeInTheDocument();
    act(() => receive({ data: JSON.stringify(event) }));
    expect(screen.getByRole("alert")).toHaveTextContent(event.payload.reason);
    reloaded.dispose();
  });
});
