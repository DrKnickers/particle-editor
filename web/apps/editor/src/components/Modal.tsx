// Modal — shared dialog foundation for sub-dialogs.
//
// Radix Dialog wrapper exposing a compound-component API:
//   <Modal bridge open onOpenChange title size="sm|md|lg">
//     <Modal.Body>…</Modal.Body>
//     <Modal.Footer>
//       <Modal.CancelButton>Cancel</Modal.CancelButton>
//       <Modal.OkButton onClick disabled>OK</Modal.OkButton>
//     </Modal.Footer>
//   </Modal>
//
// Dismissal: Esc + overlay click + close glyph all fire onOpenChange(false).
// Radix Dialog handles Esc + overlay-click natively; the close glyph in the
// header dispatches via the same callback.
//
// Sizes:
//   sm = 320 px (info modals like About, simple two-field forms like Rescale)
//   md = 480 px (default for property panels)
//   lg = 640 px (heavyweight forms like Lighting / Spawner)
//
// The dark theme matches the rest of the editor (neutral-900 surface,
// neutral-800 borders). Heights are auto, clamped to max-h-[80vh] with
// internal body scroll.

import * as Dialog from "@radix-ui/react-dialog";
import { X } from "lucide-react";
import { useEffect, useState, type ComponentPropsWithoutRef, type ReactNode } from "react";
import { createPortal } from "react-dom";
import type { Bridge } from "@particle-editor/bridge-schema";
import { useModalOpen } from "@/lib/modal-open";
import { cn } from "@/lib/utils";
import { Button, type ButtonVariant } from "@/primitives/Button";
import { IconButton } from "@/primitives/IconButton";

export type ModalSize = "sm" | "md" | "lg";

const QUADRANT_VIEWPORT = '[data-testid="quadrant-viewport"]';

const SIZE_CLASS: Record<ModalSize, string> = {
  sm: "w-[320px]",
  md: "w-[480px]",
  lg: "w-[640px]",
};

type ModalProps = {
  /** The live bridge, threaded down like everywhere else in the app — used
   *  for the one-shot frosted-backdrop snapshot (viewport/capture-snapshot). */
  bridge: Bridge;
  open: boolean;
  onOpenChange: (open: boolean) => void;
  title: string;
  size?: ModalSize;
  children: ReactNode;
};

export function Modal({
  bridge,
  open,
  onOpenChange,
  title,
  size = "md",
  children,
}: ModalProps) {
  // Frosted-glass modal backdrop via engine-snapshot capture.
  // The engine renders into a DComp visual UNDER the transparent
  // WebView2 — its pixels can't be reached by CSS effects
  // (backdrop-filter, opacity, blur) applied to HTML elements (the
  // structural reason ruled out the failed
  // server-side modal-mask approach this replaces). The fix lifts the
  // engine output INTO the WebView2 DOM as a frozen <img>:
  //
  //   1. Open: request a JPEG snapshot of the engine viewport and render
  //      it as an opaque <img> portaled into the viewport-quadrant DOM.
  //      The <img> covers the live DComp engine visual beneath the
  //      transparent WebView2, so the user sees the frozen snapshot.
  //   2. Dialog.Overlay's `bg-[var(--overlay-scrim)] backdrop-blur-sm` then dims +
  //      blurs everything in its DOM background uniformly (panels AND
  //      the snapshot img) -- both are now WebView2-rendered pixels.
  //   3. Close: clear the snapshot state; the viewport quadrant goes
  //      transparent again and the live DComp engine visual shows
  //      through. The engine keeps rendering through the modal lifecycle.
  //
  // `bridge` is the prop App threads down (NOT `window.bridge`, which
  // exposeBridgeForTests can swap for a TestHostBridge under WebView2).
  const [snapshot, setSnapshot] = useState<{ imageBase64: string; w: number; h: number } | null>(null);
  const [viewportEl, setViewportEl] = useState<HTMLElement | null>(null);
  // Gate Dialog open on snapshot
  // readiness so Dialog.Overlay's fade-in starts with the <img>
  // already mounted. Pre-deferral (cache-hit) the snapshot resolved in
  // ~0.1 ms — effectively synchronous with the modal-open render — so
  // Dialog.Overlay's backdrop-filter had stable content from frame
  // one. Post-deferral the snapshot resolves ~50-500 ms later
  // (on-demand GPU readback + GDI+ PNG encode + IPC + PNG decode for
  // the 3440×1369 frame at maximize), which lands the <img> mid-fade-
  // in. Chromium's backdrop-filter doesn't update reliably when its
  // source content changes mid-animation, producing a visible flash
  // of unblurred snapshot before the blur kicks in. Gating the open
  // prop here delays Dialog mount until after setSnapshot fires —
  // user-perceived modal-open latency goes up by the same ~50-500 ms,
  // but the visual flash is gone.
  //
  // Fallback timeout: open the dialog anyway after 750 ms even if the
  // snapshot hasn't arrived, so a host hang in the capture path
  // never bricks the menu. 750 ms is well above the 95th-percentile
  // observed capture cost at maximize.
  const [snapshotReady, setSnapshotReady] = useState(false);

  useEffect(() => {
    if (!open) {
      setSnapshotReady(false);
      return;
    }
    if (!document.querySelector(QUADRANT_VIEWPORT)) {
      // No viewport quadrant to freeze (a Modal mounted outside the App
      // shell — unit tests): no snapshot will be taken, so open now.
      setSnapshotReady(true);
      return;
    }
    const fallback = window.setTimeout(() => setSnapshotReady(true), 750);
    return () => window.clearTimeout(fallback);
  }, [open]);

  // Mark a blocking modal open so the viewport suppresses global keys while this
  // dialog is up (release-audit #12). Keyed on `open` — Modal stays mounted and
  // toggles `open`; the cleanup decrements on close OR unmount and is
  // StrictMode-safe (open→inc, cleanup→dec net correctly across the dev double-invoke).
  useEffect(() => {
    if (!open) return;
    useModalOpen.getState().open();
    return () => useModalOpen.getState().close();
  }, [open]);

  useEffect(() => {
    if (!open) return;

    // Look up the quadrant-viewport node lazily on open — App.tsx's
    // shell mounts it once at startup, so by the time any modal opens
    // it's already in the DOM. A miss is the test-env path (Modal mounted
    // in isolation without the App shell): there is nowhere to put a
    // snapshot, so none is requested (the effect above opened the dialog).
    const el = document.querySelector<HTMLElement>(QUADRANT_VIEWPORT);
    if (!el) return;
    setViewportEl(el);

    let cancelled = false;

    // One-shot snapshot capture on modal open. NO re-capture during the
    // modal's lifetime -- by design:
    //
    //   1. The snapshot img sits at position:absolute; inset:0 inside the
    //      quadrant, so CSS scales it to fill the current bounds as the
    //      window resizes. The content is mildly stale during a drag (the
    //      engine keeps rendering but we don't re-encode), but it sits
    //      behind Dialog.Overlay's `bg-[var(--overlay-scrim)] backdrop-blur-sm` so
    //      particle motion blurs to mush -- staleness is invisible.
    //
    //   2. Re-capturing during drag would force a ~10-30 ms JPEG encode
    //      per frame plus base64 transit, on top of the engine's
    //      already-expensive D3D9 device Reset per WM_SIZE -- visible
    //      stutter. Dropping the re-capture removes the modal's share.
    void bridge
      .request({ kind: "viewport/capture-snapshot", params: {} })
      .then((res) => {
        if (cancelled) return;
        const snap = res as { imageBase64: string; w: number; h: number };
        setSnapshot(snap);

        // Open the Dialog on the next animation frame so React has
        // mounted the portaled <img> + Chromium has had a chance to
        // start the JPEG decode. Dialog.Overlay's fade-in then starts
        // with stable backdrop content, and backdrop-filter blurs it
        // correctly from frame one.
        window.requestAnimationFrame(() => {
          if (!cancelled) setSnapshotReady(true);
        });
      })
      .catch(() => {
        // MockBridge / test env / host failure — open the dialog
        // anyway with whatever backdrop state we have (typically the
        // empty-snapshot render guard short-circuits the <img>).
        if (!cancelled) setSnapshotReady(true);
      });

    return () => {
      cancelled = true;
      setSnapshot(null);
      setViewportEl(null);
    };
  }, [open, bridge]);

  return (
    <>
      {/* Frosted-glass backdrop. Portal the snapshot <img>
          into the viewport-quadrant DOM so it sits below Dialog.Overlay
          in the same compositing tree — Dialog.Overlay's `bg-[var(--overlay-scrim)]
          backdrop-blur-sm` then blurs panels + snapshot uniformly. The
          render guard skips when the host returns an empty image
          (MockBridge, fresh engine, just-reset device). the host
          encodes the backdrop as JPEG (blurred → lossy is invisible). */}
      {open && viewportEl && snapshot && snapshot.imageBase64 ? createPortal(
        <img
          data-testid="modal-backdrop-snapshot"
          src={`data:image/jpeg;base64,${snapshot.imageBase64}`}
          alt=""
          aria-hidden
          style={{
            position: "absolute",
            inset: 0,
            width: "100%",
            height: "100%",
            pointerEvents: "none",
          }}
        />,
        viewportEl,
      ) : null}

      <Dialog.Root open={open && snapshotReady} onOpenChange={onOpenChange}>
        <Dialog.Portal>
          <Dialog.Overlay
            data-testid="modal-overlay"
            className="fixed inset-0 z-40 bg-[var(--overlay-scrim)] backdrop-blur-sm modal-overlay-animate"
          />
          <Dialog.Content
            // aria-describedby={undefined} opts out of Radix's accessibility
            // warning about a missing Dialog.Description. Sub-dialogs at the
            // small-scale dialogs (About, Rescale) have no separate body
            // copy worth distinguishing from the title; the title alone is
            // sufficient SR context.
            aria-describedby={undefined}
            className={cn(
              "fixed left-1/2 top-1/2 z-50 -translate-x-1/2 -translate-y-1/2 max-h-[80vh] overflow-hidden rounded-lg border border-border bg-bg-2 text-text shadow-[var(--shadow-soft)] outline-none modal-animate",
              SIZE_CLASS[size],
            )}
          >
            {/* Header */}
            <div className="flex h-12 shrink-0 items-center justify-between border-b border-border bg-bg-2 px-4">
              <Dialog.Title className="text-sm font-semibold text-text">
                {title}
              </Dialog.Title>
              <Dialog.Close asChild>
                <IconButton label="Close" tip={null} variant="ghost">
                  <X className="size-4" />
                </IconButton>
              </Dialog.Close>
            </div>
            {children}
          </Dialog.Content>
        </Dialog.Portal>
      </Dialog.Root>
    </>
  );
}

function ModalBody({ children }: { children: ReactNode }) {
  return (
    <div className="overflow-y-auto p-4" style={{ maxHeight: "calc(80vh - 48px - 56px)" }}>
      {children}
    </div>
  );
}

function ModalFooter({ children }: { children: ReactNode }) {
  return (
    <div className="flex h-14 shrink-0 items-center justify-end gap-2 border-t border-border bg-bg-2 px-4">
      {children}
    </div>
  );
}

type ButtonProps = ComponentPropsWithoutRef<"button">;

function ModalCancelButton({ children = "Cancel", ...buttonProps }: ButtonProps) {
  // Wrap the button in Dialog.Close so clicking it always closes the modal
  // via Radix (firing onOpenChange(false)). Callers can attach onClick for
  // any extra side-effects (e.g. resetting a draft form). asChild forwards
  // the close behaviour to our styled <button>.
  return (
    <Dialog.Close asChild>
      <Button variant="secondary" {...buttonProps}>
        {children}
      </Button>
    </Dialog.Close>
  );
}

function ModalOkButton({
  children = "OK",
  variant = "primary",
  ...buttonProps
}: ButtonProps & { variant?: ButtonVariant }) {
  // OK button does NOT auto-close. Callers fire their commit action in
  // onClick and then call onOpenChange(false) themselves. This lets a
  // caller keep the modal open on error (e.g. "rescale failed, show
  // inline error and leave dialog open").
  return (
    <Button variant={variant} data-testid="modal-ok" {...buttonProps}>
      {children}
    </Button>
  );
}

// Attach compound members so consumers can write <Modal.Body /> etc.
Modal.Body = ModalBody;
Modal.Footer = ModalFooter;
Modal.CancelButton = ModalCancelButton;
Modal.OkButton = ModalOkButton;
