// AtlasPickerPanel — atlas frame grid + click-to-assign (Task 8 + 9 + 12).
//
// Displays the texture atlas for the selected emitter's colorTexture as a
// side×side cell grid.  Hover previews a cell; the selection.frame field from
// AtlasContext highlights the currently-assigned frame with the amber token
// --atlas-selected.  Clicking a cell assigns the frame to all selected index
// keys (Task 9 — with a confirm dialog for differing values).  Preview fetches
// are mod-stack-keyed (Task 12) so a mod switch invalidates stale results.
//
// Placeholder precedence (top = highest priority):
//   1. no colorTexture           → "No color texture set."
//   2. atlas too large           → "Atlas too large to display (N×N)."
//   3. textureSize < 4 (side<2)  → "Single frame — no atlas to pick from."
//   4. preview missing           → "Texture not found."
//   5. preview broken            → "Texture could not be read."
//   6. focusedTrack !== "index"  → "Select keys on the index channel…"
//   happy path                  → grid + preview box

import { useEffect, useState } from "react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { useAtlasContext } from "@/lib/atlas-context";
import { ToolPanel } from "@/components/ToolPanel";
import { AtlasConfirmModal } from "@/components/AtlasConfirmModal";
import {
  gridSide,
  frameCount,
  isAtlasTooLarge,
  resolveFrame,
  cellRect,
} from "@/lib/atlas-grid";
import { getPreviewCached } from "@/lib/atlas-preview-cache";
import { useModStack } from "@/lib/mod-stack";

// ─── types ───────────────────────────────────────────────────────────────────

type PreviewState =
  | { kind: "loading" }
  | { kind: "ok"; dataUri: string; srcW: number; srcH: number }
  | { kind: "missing" }
  | { kind: "broken" };

// ─── component ───────────────────────────────────────────────────────────────

export function AtlasPickerPanel({
  bridge,
  onClose,
  closing,
}: {
  bridge: Bridge;
  onClose: () => void;
  closing?: boolean;
}) {
  const emitterId     = useAtlasContext((c) => c.emitterId);
  const focusedTrack  = useAtlasContext((c) => c.focusedTrack);
  const interpolation = useAtlasContext((c) => c.interpolation);
  const frame         = useAtlasContext((c) => c.selection.frame);
  const keyTimes      = useAtlasContext((c) => c.selection.keyTimes);
  const stack         = useModStack();

  const [textureSize, setTextureSize]   = useState(1);
  const [colorTexture, setColorTexture] = useState("");
  const [preview, setPreview]           = useState<PreviewState>({ kind: "loading" });
  const [hover, setHover]               = useState<number | null>(null);
  const [confirmTarget, setConfirmTarget] = useState<{ frame: number; emitterId: number; keyTimes: number[] } | null>(null);

  // ── fetch emitter properties ─────────────────────────────────────────────

  useEffect(() => {
    if (emitterId === null) return;
    let live = true;
    void bridge
      .request({ kind: "emitters/get-properties", params: { id: emitterId } })
      .then((r) => {
        if (!live) return;
        setTextureSize(r.properties.textureSize);
        setColorTexture(r.properties.colorTexture);
      })
      .catch(() => {
        // leave defaults → triggers no-texture placeholder
      });
    return () => {
      live = false;
    };
  }, [bridge, emitterId]);

  // ── fetch texture preview ─────────────────────────────────────────────────

  const side     = gridSide(textureSize);
  const tooLarge = isAtlasTooLarge(textureSize);
  const eligible = side >= 2;

  useEffect(() => {
    if (!eligible || tooLarge || !colorTexture) {
      setPreview({ kind: "loading" });
      return;
    }
    let live = true;
    setPreview({ kind: "loading" });
    void getPreviewCached(
      stack,
      colorTexture,
      () => bridge.request({ kind: "textures/get-preview", params: { filename: colorTexture } }),
    )
      .then((r) => {
        if (!live) return;
        if (r.status === "ok") {
          setPreview({ kind: "ok", dataUri: r.dataUri, srcW: r.srcW, srcH: r.srcH });
        } else {
          setPreview({ kind: r.status });
        }
      })
      .catch(() => {
        if (live) setPreview({ kind: "broken" });
      });
    return () => {
      live = false;
    };
  }, [bridge, colorTexture, eligible, stack, tooLarge]);

  // ── derived display values ────────────────────────────────────────────────

  const offIndex     = focusedTrack !== "index";

  // ── click-to-assign ──────────────────────────────────────────────────────

  async function assignAll(frameF: number, targetEmitterId: number | null, targetKeyTimes: number[]) {
    if (targetEmitterId === null || targetKeyTimes.length === 0) return;
    const results = await Promise.allSettled(
      targetKeyTimes.map((t) =>
        bridge.request({
          kind: "emitters/set-track-key",
          params: { id: targetEmitterId, track: "index", oldTime: t, newTime: t, newValue: frameF },
        }),
      ),
    );
    if (results.some((r) => r.status === "rejected"))
      console.warn("[atlas] some index frames could not be set; grid reflects the committed state.");
    // Highlight follows committed data via tree/changed → CurveEditorPanel republish.
  }

  function onCellClick(k: number) {
    if (offIndex || keyTimes.length === 0) return;
    if (frame === null && keyTimes.length > 1) {
      if (emitterId !== null) setConfirmTarget({ frame: k, emitterId, keyTimes });
      return;
    }
    void assignAll(k, emitterId, keyTimes);
  }

  const totalCells   = side * side;
  const fc           = frameCount(textureSize);
  // Show "M of N" only when the atlas has unused cells (frameCount < textureSize).
  const meta         = totalCells === Math.max(1, Math.floor(Number.isFinite(textureSize) ? textureSize : 1))
    ? `${side}×${side} · ${fc}`
    : `${side}×${side} · ${fc} of ${textureSize}`;
  const highlight    = frame === null ? null : resolveFrame(frame, side);
  const previewFrame = hover ?? highlight;

  // ── body content ─────────────────────────────────────────────────────────

  let body: React.ReactNode;

  if (!colorTexture) {
    body = <Placeholder>No color texture set.</Placeholder>;
  } else if (tooLarge) {
    body = <Placeholder>Atlas too large to display ({side}×{side}).</Placeholder>;
  } else if (!eligible) {
    body = <Placeholder>Single frame — no atlas to pick from.</Placeholder>;
  } else if (preview.kind === "missing") {
    body = <Placeholder>Texture not found.</Placeholder>;
  } else if (preview.kind === "broken") {
    body = <Placeholder>Texture could not be read.</Placeholder>;
  } else if (offIndex) {
    body = (
      <Placeholder>Select keys on the index channel to assign frames.</Placeholder>
    );
  } else {
    body = (
      <>
        {/* Pinned preview box */}
        <div className="shrink-0 p-3">
          <PreviewBox
            preview={preview}
            side={side}
            frame={previewFrame}
            rawFrame={frame}
          />
        </div>
        {/* Scrollable cell grid */}
        <div className="min-h-0 flex-1 overflow-y-auto p-3 pt-0">
          <div
            className="grid gap-1"
            style={{ gridTemplateColumns: `repeat(${side}, minmax(0, 1fr))` }}
          >
            {Array.from({ length: totalCells }, (_, k) => (
              <Cell
                key={k}
                k={k}
                side={side}
                preview={preview}
                selected={k === highlight}
                onHover={setHover}
                onClick={onCellClick}
              />
            ))}
          </div>
        </div>
      </>
    );
  }

  // ── header meta visibility ────────────────────────────────────────────────

  const showMeta = eligible && !tooLarge && !!colorTexture;

  return (
    <ToolPanel title="Atlas Frames" onClose={onClose} variant="docked" closing={closing}>
      {/* Full-height flex column that negates ToolPanel's body padding so
          the pinned preview and scrollable grid can fill the available space. */}
      <div className="-m-3 flex h-full flex-col overflow-hidden">
        {/* Sub-header: atlas meta (grid dimensions + frame count) and
            interpolation badge. Shown only when the atlas is displayable. */}
        {showMeta && (
          <div className="flex shrink-0 items-center gap-2 border-b border-border px-3 py-1 text-xs text-text-3">
            <span data-testid="atlas-meta" className="min-w-0 flex-1 truncate">
              {meta}
            </span>
            {interpolation && (
              <span className="shrink-0 rounded border border-border px-1">
                {interpolation}
              </span>
            )}
          </div>
        )}
        {body}
      </div>
      <AtlasConfirmModal
        open={confirmTarget !== null}
        count={confirmTarget?.keyTimes.length ?? 0}
        frame={confirmTarget?.frame ?? 0}
        onConfirm={() => {
          const t = confirmTarget!;
          setConfirmTarget(null);
          void assignAll(t.frame, t.emitterId, t.keyTimes);
        }}
        onCancel={() => setConfirmTarget(null)}
      />
    </ToolPanel>
  );
}

// ─── sub-components ───────────────────────────────────────────────────────────

function Placeholder({ children }: { children: React.ReactNode }) {
  return (
    <div className="flex flex-1 items-center justify-center p-6 text-center text-xs text-text-3">
      {children}
    </div>
  );
}

/** Compute CSS background-* properties to crop the texture to cell k. */
function cropStyle(
  k: number,
  side: number,
  p: Extract<PreviewState, { kind: "ok" }>,
): React.CSSProperties {
  const r = cellRect(k, side, p.srcW, p.srcH);
  // backgroundSize: the full image is `side` cells wide/tall.
  // backgroundPosition: position the relevant cell into view.
  const posX =
    side > 1
      ? `${(r.left / (p.srcW - r.width)) * 100}%`
      : "0%";
  const posY =
    side > 1
      ? `${(r.top / (p.srcH - r.height)) * 100}%`
      : "0%";
  return {
    backgroundImage:    `url(${p.dataUri})`,
    backgroundRepeat:   "no-repeat",
    backgroundSize:     `${side * 100}% ${side * 100}%`,
    backgroundPosition: `${posX} ${posY}`,
  };
}

function Cell({
  k,
  side,
  preview,
  selected,
  onHover,
  onClick,
}: {
  k: number;
  side: number;
  preview: PreviewState;
  selected: boolean;
  onHover: (k: number | null) => void;
  onClick?: (k: number) => void;
}) {
  const style: React.CSSProperties =
    preview.kind === "ok" ? cropStyle(k, side, preview) : {};
  if (selected) {
    style.borderColor = "var(--atlas-selected)";
  }

  return (
    <div
      data-testid="atlas-cell"
      data-frame={k}
      data-selected={selected ? "true" : "false"}
      className={`relative aspect-square rounded-sm border ${
        selected ? "border-2" : "border-border"
      } bg-bg-2`}
      style={style}
      onMouseEnter={() => onHover(k)}
      onMouseLeave={() => onHover(null)}
      onClick={() => onClick?.(k)}
    >
      {/* Small frame index label — always white text, low opacity */}
      <span className="pointer-events-none absolute left-0 top-0 px-px text-[8px] leading-none text-white/40">
        {k}
      </span>
    </div>
  );
}

function PreviewBox({
  preview,
  side,
  frame,
  rawFrame,
}: {
  preview: PreviewState;
  side: number;
  frame: number | null;
  rawFrame: number | null;
}) {
  const style: React.CSSProperties =
    preview.kind === "ok" && frame !== null
      ? cropStyle(frame, side, preview)
      : {};

  return (
    <div
      className="flex aspect-square w-full items-center justify-center rounded border border-border bg-bg-2 text-center text-xs text-text-3"
      style={style}
    >
      {frame === null && (
        <span>
          {rawFrame === null
            ? "Hover or select a frame"
            : `Frame ${rawFrame} — outside the ${side}×${side} atlas (in-game sampling is off-grid)`}
        </span>
      )}
    </div>
  );
}
