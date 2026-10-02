import { describe, it, expect, vi, afterEach } from "vitest";
import { render as rtlRender, screen, cleanup, fireEvent, act } from "@testing-library/react";
import * as Tooltip from "@radix-ui/react-tooltip";
import type { ReactElement, ReactNode } from "react";
import { TitleBar } from "../TitleBar";
import { makeBridgeStub } from "@/test/bridge-stub";
import { markHeadless, __resetRecordModeForTests } from "@/lib/record-mode";

// The window controls mount Tips (Radix Tooltip.Root), which require the
// Tooltip.Provider App.tsx supplies in production — this wrapper stands in
// for it (precedent: renderWithTooltips in EmitterTree.test.tsx).
const TipProvider = ({ children }: { children: ReactNode }) => (
  <Tooltip.Provider delayDuration={0} skipDelayDuration={0}>{children}</Tooltip.Provider>
);
const render = (ui: ReactElement) => rtlRender(ui, { wrapper: TipProvider });

afterEach(() => {
  cleanup();
  __resetRecordModeForTests();
});

function makeMockBridge() {
  const bridge = makeBridgeStub({ fallback: { ok: true, data: {} } });
  return { bridge, request: bridge.request, emit: bridge.emit };
}

describe("TitleBar", () => {
  it("shows the app name, filename, and the three window controls", () => {
    const { bridge } = makeMockBridge();
    render(<TitleBar bridge={bridge} currentFilePath="D:/mods/P_HP_IMPERIAL_DAMAGE.ALO" dirty={false} />);
    const bar = screen.getByTestId("title-bar");
    expect(bar.textContent).toContain("Particle Editor");
    expect(bar.textContent).toContain("P_HP_IMPERIAL_DAMAGE.ALO");
    expect(bar.textContent).not.toContain("D:/mods"); // basename only
    expect(screen.getByTestId("window-controls")).not.toBeNull();
    expect(screen.getByLabelText("Minimize")).not.toBeNull();
    expect(screen.getByLabelText("Maximize")).not.toBeNull();
    expect(screen.getByLabelText("Close")).not.toBeNull();
  });

  it("dispatches the matching window/* command for each button", () => {
    const { bridge, request } = makeMockBridge();
    render(<TitleBar bridge={bridge} currentFilePath={null} dirty={false} />);
    fireEvent.click(screen.getByLabelText("Minimize"));
    fireEvent.click(screen.getByLabelText("Maximize"));
    fireEvent.click(screen.getByLabelText("Close"));
    expect(request).toHaveBeenCalledWith({ kind: "window/minimize", params: {} });
    expect(request).toHaveBeenCalledWith({ kind: "window/maximize", params: {} });
    expect(request).toHaveBeenCalledWith({ kind: "window/close", params: {} });
  });

  it("swaps Maximize→Restore when the host reports maximized", () => {
    const { bridge, emit } = makeMockBridge();
    render(<TitleBar bridge={bridge} currentFilePath={null} dirty={false} />);
    expect(screen.getByLabelText("Maximize")).not.toBeNull();
    emit("window/state", { maximized: true });
    expect(screen.getByLabelText("Restore")).not.toBeNull();
    expect(screen.queryByLabelText("Maximize")).toBeNull();
  });

  it("HIDES the window controls in headless record (clean clips), keeping the branding", () => {
    markHeadless();
    const { bridge } = makeMockBridge();
    render(<TitleBar bridge={bridge} currentFilePath="D:/x/foo.alo" dirty={true} />);
    expect(screen.queryByTestId("window-controls")).toBeNull();
    const bar = screen.getByTestId("title-bar");
    expect(bar.textContent).toContain("Particle Editor");
    expect(bar.textContent).toContain("foo.alo");
  });

  it("shows the ● dirty marker and Untitled.alo for a null path", () => {
    const { bridge } = makeMockBridge();
    render(<TitleBar bridge={bridge} currentFilePath={null} dirty={true} />);
    const bar = screen.getByTestId("title-bar");
    expect(bar.textContent).toContain("●");
    expect(bar.textContent).toContain("Untitled.alo");
  });

  it("swaps Restore→Maximize when the window is restored (glyph swaps BOTH ways)", () => {
    const { bridge, emit } = makeMockBridge();
    render(<TitleBar bridge={bridge} currentFilePath={null} dirty={false} />);
    emit("window/state", { maximized: true });
    expect(screen.getByLabelText("Restore")).not.toBeNull();
    emit("window/state", { maximized: false });
    expect(screen.getByLabelText("Maximize")).not.toBeNull();
    expect(screen.queryByLabelText("Restore")).toBeNull();
  });

  it("keeps the window controls OUT of the tab order (tabIndex=-1)", () => {
    const { bridge } = makeMockBridge();
    render(<TitleBar bridge={bridge} currentFilePath={null} dirty={false} />);
    for (const label of ["Minimize", "Maximize", "Close"]) {
      expect(screen.getByLabelText(label).getAttribute("tabindex")).toBe("-1");
    }
  });

  it("hides the controls when headless latches AFTER the initial render (production order)", () => {
    const { bridge } = makeMockBridge();
    render(<TitleBar bridge={bridge} currentFilePath={null} dirty={false} />);
    expect(screen.getByTestId("window-controls")).not.toBeNull();
    act(() => markHeadless());
    expect(screen.queryByTestId("window-controls")).toBeNull();
  });

  // NOTE: the `app-region: drag`/`no-drag` split (bar draggable, controls not) is
  // a vendor-prefixed WebView2 non-client property jsdom doesn't model, so it's
  // verified in the real browser / WebView2, not here.

  it("swallows a rejected window command (a real-host failure is not an unhandled rejection)", async () => {
    const request = vi.fn().mockRejectedValue(new Error("host said no"));
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const bridge = { request, on: vi.fn(() => () => {}) } as any;
    render(<TitleBar bridge={bridge} currentFilePath={null} dirty={false} />);
    fireEvent.click(screen.getByLabelText("Minimize"));
    await Promise.resolve(); // flush the .catch — must not throw
    expect(request).toHaveBeenCalledWith({ kind: "window/minimize", params: {} });
  });

  it("unsubscribes from window/state on unmount", () => {
    const off = vi.fn();
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const bridge = { request: vi.fn().mockResolvedValue({ ok: true }), on: vi.fn(() => off) } as any;
    const { unmount } = render(<TitleBar bridge={bridge} currentFilePath={null} dirty={false} />);
    expect(bridge.on).toHaveBeenCalledWith("window/state", expect.any(Function));
    unmount();
    expect(off).toHaveBeenCalled();
  });
});
