// Modal — shared dialog foundation for Screen 8 sub-dialogs.
//
// Radix Dialog wrapper exposing a compound-component API:
//   <Modal open onOpenChange title size="sm|md|lg">
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
import { useEffect, useState, type ReactNode, type MouseEventHandler } from "react";
import { createPortal } from "react-dom";
import { useBridge } from "@/lib/bridge-context";

export type ModalSize = "sm" | "md" | "lg";

const SIZE_CLASS: Record<ModalSize, string> = {
  sm: "w-[320px]",
  md: "w-[480px]",
  lg: "w-[640px]",
};

type ModalProps = {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  title: string;
  size?: ModalSize;
  children: ReactNode;
};

export function Modal({
  open,
  onOpenChange,
  title,
  size = "md",
  children,
}: ModalProps) {
  // B1.3.1.1: frosted-glass modal backdrop via engine-snapshot capture.
  // The engine renders into a DComp visual UNDER the transparent
  // WebView2 — its pixels can't be reached by CSS effects
  // (backdrop-filter, opacity, blur) applied to HTML elements (see
  // tasks/lessons.md for the structural reason and the failed
  // server-side modal-mask approach this replaces). The fix lifts the
  // engine output INTO the WebView2 DOM as a frozen <img>:
  //
  //   1. Open: request a JPEG snapshot of the engine viewport and render
  //      it as an opaque <img> portaled into the viewport-quadrant DOM.
  //      The <img> covers the live DComp engine visual beneath the
  //      transparent WebView2, so the user sees the frozen snapshot.
  //   2. Dialog.Overlay's `bg-black/60 backdrop-blur-sm` then dims +
  //      blurs everything in its DOM background uniformly (panels AND
  //      the snapshot img) -- both are now WebView2-rendered pixels.
  //   3. Close: clear the snapshot state; the viewport quadrant goes
  //      transparent again and the live DComp engine visual shows
  //      through. The engine keeps rendering through the modal lifecycle.
  //
  // Bridge comes from BridgeContext (NOT `window.bridge` — see).
  const bridge = useBridge();
  const [snapshot, setSnapshot] = useState<{ imageBase64: string; w: number; h: number } | null>(null);
  const [viewportEl, setViewportEl] = useState<HTMLElement | null>(null);
  // Phase 3 Stage 1 follow-up: gate Dialog open on snapshot
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
    if (!bridge) {
      // Test env / no-context path — open the dialog immediately so
      // unit tests rendering <Modal open> without a BridgeContext
      // see Dialog.Content mount. The snapshot/<img> render guard
      // already short-circuits the portaled <img> in this case.
      setSnapshotReady(true);
      return;
    }
    const fallback = window.setTimeout(() => setSnapshotReady(true), 750);
    return () => window.clearTimeout(fallback);
  }, [open, bridge]);

  useEffect(() => {
    if (!open || !bridge) return;

    // Look up the quadrant-viewport node lazily on open — App.tsx's
    // shell mounts it once at startup, so by the time any modal opens
    // it's already in the DOM. The querySelector miss is the test-env
    // path (Modal mounted in isolation without the App shell); in that
    // case viewportEl stays null and the createPortal render guards
    // skip the img output.
    const el = document.querySelector<HTMLElement>('[data-testid="quadrant-viewport"]');
    setViewportEl(el);

    let cancelled = false;

    // One-shot snapshot capture on modal open. NO re-capture during the
    // modal's lifetime -- by design:
    //
    //   1. The snapshot img sits at position:absolute; inset:0 inside the
    //      quadrant, so CSS scales it to fill the current bounds as the
    //      window resizes. The content is mildly stale during a drag (the
    //      engine keeps rendering but we don't re-encode), but it sits
    //      behind Dialog.Overlay's `bg-black/60 backdrop-blur-sm` so
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
      {/* B1.3.1.1 frosted-glass backdrop. Portal the snapshot <img>
          into the viewport-quadrant DOM so it sits below Dialog.Overlay
          in the same compositing tree — Dialog.Overlay's `bg-black/60
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
            className="fixed inset-0 z-40 bg-black/60 backdrop-blur-sm modal-overlay-animate"
          />
          <Dialog.Content
            // aria-describedby={undefined} opts out of Radix's accessibility
            // warning about a missing Dialog.Description. Sub-dialogs at the
            // Screen 8 batch 1 scale (About, Rescale) have no separate body
            // copy worth distinguishing from the title; the title alone is
            // sufficient SR context.
            aria-describedby={undefined}
            className={`fixed left-1/2 top-1/2 z-50 -translate-x-1/2 -translate-y-1/2 ${SIZE_CLASS[size]} max-h-[80vh] overflow-hidden rounded-lg border border-border bg-bg-2 text-text shadow-md outline-none modal-animate`}
          >
            {/* Header */}
            <div className="flex h-12 shrink-0 items-center justify-between border-b border-border bg-bg-2 px-4">
              <Dialog.Title className="text-sm font-semibold text-text">
                {title}
              </Dialog.Title>
              <Dialog.Close
                aria-label="Close"
                className="flex size-6 items-center justify-center rounded text-text-2 hover:bg-panel-2 hover:text-text focus-ring"
              >
                <X className="size-4" />
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

type ButtonProps = {
  children?: ReactNode;
  onClick?: MouseEventHandler<HTMLButtonElement>;
  disabled?: boolean;
};

function ModalCancelButton({ children = "Cancel", onClick, disabled }: ButtonProps) {
  // Wrap the button in Dialog.Close so clicking it always closes the modal
  // via Radix (firing onOpenChange(false)). Callers can attach onClick for
  // any extra side-effects (e.g. resetting a draft form). asChild forwards
  // the close behaviour to our styled <button>.
  return (
    <Dialog.Close asChild>
      <button
        type="button"
        onClick={onClick}
        disabled={disabled}
        className="rounded border border-border-2 bg-panel-2 px-3 py-1 text-xs text-text hover:bg-panel-3 focus-ring disabled:cursor-not-allowed disabled:opacity-50"
      >
        {children}
      </button>
    </Dialog.Close>
  );
}

function ModalOkButton({ children = "OK", onClick, disabled }: ButtonProps) {
  // OK button does NOT auto-close. Callers fire their commit action in
  // onClick and then call onOpenChange(false) themselves. This lets a
  // caller keep the modal open on error (e.g. "rescale failed, show
  // inline error and leave dialog open").
  return (
    <button
      type="button"
      onClick={onClick}
      disabled={disabled}
      className="rounded bg-accent px-3 py-1 text-xs font-medium text-white hover:bg-accent focus-ring disabled:cursor-not-allowed disabled:opacity-50"
    >
      {children}
    </button>
  );
}

// Attach compound members so consumers can write <Modal.Body /> etc.
Modal.Body = ModalBody;
Modal.Footer = ModalFooter;
Modal.CancelButton = ModalCancelButton;
Modal.OkButton = ModalOkButton;
