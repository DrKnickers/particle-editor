// Section — collapsible header for inspector tabs.
//
// Entire header row is clickable (per user spec — bigger hit target
// than chevron-only). Keyboard accessibility via role="button" +
// tabIndex={0} + onKeyDown handling Enter / Space. Space gets
// preventDefault to suppress page scroll.
//
// State is local + session-only — defaults to defaultOpen=true on
// every mount. The inspector remounts when the user selects a
// different emitter, which intentionally resets every section to
// its default state. If state persistence becomes desirable later,
// the upgrade path is a single lifted useState or a per-tab
// persistence map.

import { useState, type ReactNode } from "react";
import { ChevronDown } from "lucide-react";

type Props = {
  title: string;
  defaultOpen?: boolean;
  children: ReactNode;
};

export function Section({ title, defaultOpen = true, children }: Props) {
  const [open, setOpen] = useState(defaultOpen);
  const toggle = () => setOpen((o) => !o);
  return (
    <div className="panel-section" data-open={open}>
      <div
        className="panel-section-header"
        role="button"
        tabIndex={0}
        onClick={toggle}
        onKeyDown={(e) => {
          if (e.key === "Enter" || e.key === " ") {
            e.preventDefault();
            toggle();
          }
        }}
        aria-expanded={open}
        data-testid={`section-${title.toLowerCase().replace(/\s+/g, "-")}`}
      >
        <span>{title}</span>
        <ChevronDown className="chev size-3" />
      </div>
      {/* Body stays mounted (animated collapse via .collapse-anim); the
          grid wrapper drives the height tween off data-open. The inner
          padding-free clip div is the grid item that collapses to a true
          0 — the padded body must be its child, or the body's vertical
          padding would leave an ~8px sliver when collapsed. */}
      <div className="collapse-anim" data-open={open}>
        <div>
          <div className="panel-section-body">{children}</div>
        </div>
      </div>
    </div>
  );
}
