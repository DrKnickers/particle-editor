// Vitest unit tests for the shared Modal component.
// Exercises: open prop renders content; Esc + overlay click fire
// onOpenChange(false).

import { describe, it, expect, vi } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { BridgeContext } from "@/lib/bridge-context";
import { Modal } from "../Modal";
import { useModalOpen } from "@/lib/modal-open";

function makeStubBridge() {
  const request = vi.fn().mockResolvedValue({});
  const on = vi.fn().mockReturnValue(() => {});
  return { request, on } as unknown as Bridge & {
    request: ReturnType<typeof vi.fn>;
  };
}

describe("Modal", () => {
  it("renders title and body when open={true}", () => {
    render(
      <Modal open onOpenChange={() => {}} title="Test Modal">
        <Modal.Body>
          <p>body-content</p>
        </Modal.Body>
        <Modal.Footer>
          <Modal.OkButton>OK</Modal.OkButton>
        </Modal.Footer>
      </Modal>
    );
    expect(screen.getByText("Test Modal")).toBeInTheDocument();
    expect(screen.getByText("body-content")).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "OK" })).toBeInTheDocument();
  });

  it("Esc key fires onOpenChange(false)", () => {
    const onOpenChange = vi.fn();
    render(
      <Modal open onOpenChange={onOpenChange} title="Test Modal">
        <Modal.Body>body</Modal.Body>
      </Modal>
    );
    // Radix Dialog listens for Escape on the document while open.
    fireEvent.keyDown(document.activeElement ?? document.body, {
      key: "Escape",
      code: "Escape",
    });
    expect(onOpenChange).toHaveBeenCalledWith(false);
  });

  // Polish regression guard. The Modal sits over the
  // layered engine viewport, where HTML effects (box-shadow extent
  // > occlusion pad, semi-transparent backgrounds, backdrop-filter)
  // produce visible compositing artifacts that don't appear over the
  // panel chrome. These assertions lock in the "opaque body + small
  // shadow" design choice so a future Tailwind tweak doesn't quietly
  // re-introduce a `shadow-2xl` / `bg-X/N` that breaks the visual
  // again. Architectural detail: see Modal.tsx's comment above the
  // useEffect for the alpha-cut sizing rationale.
  it("dialog body declares an opaque background", () => {
    render(
      <Modal open onOpenChange={() => {}} title="Test Modal">
        <Modal.Body>body</Modal.Body>
      </Modal>
    );
    // The body element is the rounded card itself — anchor via Radix's
    // implicit `role=dialog` (Dialog.Content). Class string check
    // beats getComputedStyle because jsdom doesn't compute Tailwind
    // utility classes; we assert the policy at the class layer.
    const content = screen.getByRole("dialog");
    expect(content.className).toContain("bg-bg-2");
    expect(content.className).not.toMatch(/bg-\w+\/\d+/); // no bg-X/N slash-opacity
    expect(content.className).not.toContain("backdrop-blur");
    expect(content.className).not.toContain("backdrop-filter");
  });

  it("dialog body uses a small drop-shadow (no shadow-xl or shadow-2xl)", () => {
    render(
      <Modal open onOpenChange={() => {}} title="Test Modal">
        <Modal.Body>body</Modal.Body>
      </Modal>
    );
    const content = screen.getByRole("dialog");
    // Larger Tailwind shadow tokens extend beyond the modal's alpha-cut
    // pad and produce a "shadow truncated by engine popup" artifact.
    // Only `shadow-sm` / `shadow` / `shadow-md` (≤ ~8 px extent) are
    // safe with the current 8 px pad in the Modal useEffect.
    expect(content.className).not.toContain("shadow-xl");
    expect(content.className).not.toContain("shadow-2xl");
  });

  it("uses the motion classes, not the dead animate-in utilities", () => {
    // The old `data-[state=open]:animate-in` utilities came from the
    // tailwindcss-animate plugin, which this Tailwind v4 build does NOT
    // load — they generated zero CSS (see components.css's popover
    // section note). They were replaced with real keyframe classes.
    render(
      <Modal open onOpenChange={() => {}} title="Test Modal">
        <Modal.Body>body</Modal.Body>
      </Modal>
    );
    const content = screen.getByRole("dialog");
    expect(content.className).toContain("modal-animate");
    expect(content.className).not.toContain("animate-in");
    const overlay = screen.getByTestId("modal-overlay");
    expect(overlay.className).toContain("modal-overlay-animate");
    expect(overlay.className).not.toContain("animate-in");
  });

  it("dispatches viewport/capture-snapshot on open for the frosted-glass backdrop", async () => {
    // The frosted-glass backdrop replaces the old modal-mask
    // approach (which dimmed engine pixels server-side and produced an
    // inner-shadow seam). The new flow snapshots the engine
    // into a portaled <img>; Dialog.Overlay's CSS effects then blur panels
    // + snapshot uniformly. We assert the capture fires on open. The
    // quadrant rect comes from a data-testid="quadrant-viewport" element
    // we stub so getBoundingClientRect does not collapse to zero.
    const bridge = makeStubBridge();
    render(
      <BridgeContext.Provider value={bridge}>
        <div data-testid="quadrant-viewport" style={{ width: 800, height: 600 }} />
        <Modal open onOpenChange={() => {}} title="Test Modal">
          <Modal.Body>body</Modal.Body>
        </Modal>
      </BridgeContext.Provider>,
    );
    await waitFor(() => {
      expect(bridge.request).toHaveBeenCalledWith({
        kind: "viewport/capture-snapshot",
        params: {},
      });
    });
    // Lock: the deleted modal-mask surface MUST NOT be invoked any more.
    expect(bridge.request).not.toHaveBeenCalledWith(
      expect.objectContaining({ kind: "viewport/set-modal-mask" }),
    );
  });

  it("close glyph in header fires onOpenChange(false)", () => {
    // Radix Dialog overlay-click dismissal is enforced by the
    // pointerDownOutside hook in Radix internals, which is sensitive to
    // event-construction details that jsdom doesn't perfectly emulate.
    // The user-visible contract — header X glyph also dismisses — is the
    // simpler, more stable assertion for the click-to-close path. The
    // overlay-click path is covered end-to-end by the Playwright spec
    // (dialogs.spec.ts) where a real browser fires real events.
    const onOpenChange = vi.fn();
    render(
      <Modal open onOpenChange={onOpenChange} title="Test Modal">
        <Modal.Body>body</Modal.Body>
      </Modal>
    );
    const closeBtn = screen.getByRole("button", { name: "Close" });
    fireEvent.click(closeBtn);
    expect(onOpenChange).toHaveBeenCalledWith(false);
  });

  it("tracks useModalOpen: +1 while open, -1 on close and on unmount (#12)", () => {
    useModalOpen.setState({ count: 0 });
    const view = (open: boolean) => (
      <Modal open={open} onOpenChange={() => {}} title="M">
        <Modal.Body>b</Modal.Body>
      </Modal>
    );
    const { rerender, unmount } = render(view(true));
    expect(useModalOpen.getState().count).toBe(1);   // open -> inc
    rerender(view(false));
    expect(useModalOpen.getState().count).toBe(0);   // close -> dec
    rerender(view(true));
    expect(useModalOpen.getState().count).toBe(1);   // reopen -> inc
    unmount();
    expect(useModalOpen.getState().count).toBe(0);   // unmount -> dec
  });
});
