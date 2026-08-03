import type { ReactElement } from "react";

export interface RecordCursorProps {
  x: number; // device px
  y: number; // device px
  visible: boolean;
  pressed: boolean;
}

/**
 * A synthetic pointer sprite for --record clips. Positioned in CSS px (device px
 * divided by devicePixelRatio). pointer-events:none + a high z-index so it floats
 * over chrome AND the engine viewport in the PrintWindow composite. Renders
 * nothing until visible; only the host's ui/cursor push ever shows it.
 */
export function RecordCursor({ x, y, visible, pressed }: RecordCursorProps): ReactElement | null {
  if (!visible) return null;
  const dpr = window.devicePixelRatio || 1;
  return (
    <div
      data-testid="record-cursor"
      data-pressed={pressed ? "true" : "false"}
      style={{
        position: "fixed",
        left: `${x / dpr}px`,
        top: `${y / dpr}px`,
        width: 0,
        height: 0,
        pointerEvents: "none",
        zIndex: 2147483647,
      }}
    >
      {/* arrow sprite; hotspot is the tip at SVG (2,2). The -2px margins pull that
          tip onto the div's positioned point so the cursor tip lands exactly on the
          target coordinate (without them it renders ~2px down-right). The press
          scale shares the same (2,2) origin, so the tip stays pinned while scaling. */}
      <svg
        data-testid="record-cursor-sprite"
        width="24"
        height="24"
        viewBox="0 0 24 24"
        style={{
          display: "block",
          marginLeft: "-2px",
          marginTop: "-2px",
          filter: "drop-shadow(0 1px 1px rgba(0,0,0,.6))",
          transform: pressed ? "scale(0.82)" : "scale(1)",
          transformOrigin: "2px 2px",
        }}
      >
        <path d="M2 2 L2 18 L7 13 L11 21 L14 19 L10 12 L17 12 Z" fill="#fff" stroke="#000" strokeWidth="1.5" />
      </svg>
    </div>
  );
}
