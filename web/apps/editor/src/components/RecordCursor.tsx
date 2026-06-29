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
      {/* arrow sprite; hotspot at (0,0) = the tip */}
      <svg
        width="24"
        height="24"
        viewBox="0 0 24 24"
        style={{ display: "block", filter: "drop-shadow(0 1px 1px rgba(0,0,0,.6))" }}
      >
        <path d="M2 2 L2 18 L7 13 L11 21 L14 19 L10 12 L17 12 Z" fill="#fff" stroke="#000" strokeWidth="1.5" />
      </svg>
      {pressed && (
        <span
          style={{
            position: "absolute",
            left: -10,
            top: -10,
            width: 20,
            height: 20,
            border: "2px solid rgba(120,180,255,.9)",
            borderRadius: "50%",
          }}
        />
      )}
    </div>
  );
}
