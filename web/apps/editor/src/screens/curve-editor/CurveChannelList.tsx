// CurveChannelList — the curve editor's left column (presentational): one row
// per channel with a visibility checkbox, the channel swatch, and the channel
// name, which is the "edit this curve" control. CurveEditorPanel owns
// visibility + focus state.
//
// Each row holds two SEPARATE controls — a native checkbox (show/hide the
// curve) and a button (make it the edit focus; aria-pressed while focused) —
// rather than a role="button" row wrapped around a checkbox: an interactive
// control nested in a button role is invalid ARIA, and the row's own
// Space/Enter handler used to swallow Space before it reached the checkbox.
// A pointer click anywhere else on the row still focuses the channel (the
// button's click bubbles to the same row handler, so it fires once).

import type { ChannelDef } from "@/screens/curve-editor/CurveEditor";
import { cn } from "@/lib/utils";
import { Checkbox } from "@/primitives/Checkbox";

type Props = {
  channels: readonly ChannelDef[];
  visible: Record<string, boolean>;
  focusChannel: string;
  onFocusChannel: (id: string) => void;
  onToggleVisible: (id: string, on: boolean) => void;
  /** Render the group divider before this channel (the colour → transform
   *  split). */
  dividerBefore: string;
};

export function CurveChannelList({
  channels,
  visible,
  focusChannel,
  onFocusChannel,
  onToggleVisible,
  dividerBefore,
}: Props) {
  return (
    <div
      className="curve-list"
      role="group"
      aria-label="Curve channels"
      data-testid="curve-channel-list"
    >
      {channels.flatMap((c) => {
        const isOn = visible[c.id] ?? c.defaultOn;
        const isFocus = c.id === focusChannel;
        const row = (
          <div
            key={c.id}
            className={cn("curve-row", isFocus && "!bg-accent-soft")}
            data-testid={`curve-channel-row-${c.id}`}
            data-on={isOn ? "true" : "false"}
            data-focus={isFocus ? "true" : "false"}
            onClick={() => onFocusChannel(c.id)}
          >
            <Checkbox
              checked={isOn}
              onChange={(e) => onToggleVisible(c.id, e.target.checked)}
              // The checkbox toggles visibility only; the rest of the row
              // handles focus.
              onClick={(e) => e.stopPropagation()}
              aria-label={`Toggle ${c.label} curve`}
              data-testid={`curve-channel-checkbox-${c.id}`}
            />
            <span className="swatch" style={{ background: c.color }} aria-hidden="true" />
            <button
              type="button"
              aria-pressed={isFocus}
              aria-label={`Edit ${c.label} curve`}
              data-testid={`curve-channel-focus-${c.id}`}
              className="min-w-0 flex-1 truncate rounded-[var(--radius-2xs)] text-left focus-ring-inset"
            >
              {c.label}
            </button>
          </div>
        );
        // A horizontal divider before the first transform-y channel (Scale)
        // separates the colour group from the transform group.
        if (c.id === dividerBefore) {
          return [
            <div
              key="curve-channel-group-divider"
              className="section-divider"
              data-testid="curve-channel-group-divider"
              role="separator"
              aria-hidden="true"
            />,
            row,
          ];
        }
        return [row];
      })}
    </div>
  );
}
