// ToolPanel — shared shell for the Screen 8 Batch 2 modeless tool
// windows (Lighting, Bloom Settings, Ground Texture, Background).
//
// Lives under `components/` (app-shell-style), not `primitives/`,
// because it owns layout positioning (absolute-right, full-height of
// the main row) and is opinionated about chrome — not a reusable atom
// like Spinner or ColorButton.
//
// Compound API:
//   <ToolPanel title="Lighting" onClose={() => setOpenToolPanel(null)}>
//     <ToolPanel.Section title="Sun">...</ToolPanel.Section>
//     ...
//   </ToolPanel>
//
// Chrome cues borrowed from BackgroundPicker (the pre-existing
// reference implementation): 320 px wide, dark surface, 48 px header
// with title + `×` close glyph, scrollable body. The 48 px header height
// matches the shared Modal so the eye reads them as the same family
// even though one is portalled overlay and the other slides over the
// main row.
//
// The host is non-modal: the user can interact with the viewport,
// menus, and other UI freely while a ToolPanel is open. Esc and
// click-outside intentionally do NOT dismiss — the user dismisses via
// the `×` glyph or by re-toggling the launcher (e.g. clicking the
// Background pill again, picking a different Tools-menu entry).

import { ChevronDown, X } from "lucide-react";
import { useState, type ReactNode } from "react";

type ToolPanelProps = {
  title: string;
  onClose: () => void;
  children?: ReactNode;
  /** "overlay" (default) floats over the viewport, absolute-right, 320px.
   *  "docked" fills its parent layout column (the right-dock slot, shared
   *  with the Spawner). */
  variant?: "overlay" | "docked";
  /** True while the dock is sliding shut (logically closed but still mounted
   *  for the exit animation). Stamps data-state="closing" on the dialog so it
   *  no longer matches the "open ToolPanel" selector
   *  (`[role="dialog"]:not([data-state])`) — a closing panel is not an open,
   *  closeable dialog. The PanelLayout slot also marks itself `inert` during
   *  this window. Together they stop a click that lands in the ~260ms
   *  slide-out from targeting the shrinking/detaching Close button. */
  closing?: boolean;
  /** When false, the body does NOT scroll or reserve a scrollbar gutter. For
   *  panels that own an INTERNAL scroll region (e.g. AtlasPickerPanel, whose
   *  cell grid scrolls in its own container) the body never overflows, so the
   *  default reserved `scrollbar-gutter` is pure waste — a 15px empty strip on
   *  the right that pushes the panel's content (and its centred grid) off-centre.
   *  Default true (Lighting / Spawner scroll their bodies and need the gutter). */
  bodyScroll?: boolean;
};

const HEADER_HEIGHT_PX = 48;

export function ToolPanel({
  title,
  onClose,
  children,
  variant = "overlay",
  closing = false,
  bodyScroll = true,
}: ToolPanelProps) {
  const docked = variant === "docked";
  return (
    <div
      className={
        docked
          ? "flex h-full w-full flex-col border-l border-border bg-bg text-text"
          : "absolute right-0 top-0 bottom-0 z-10 flex w-80 flex-col border-l border-border bg-bg text-text"
      }
      role="dialog"
      aria-label={title}
      data-state={closing ? "closing" : undefined}
    >
      {/* Header — mirrors Modal's header layout (48 px, title left, X right). */}
      <div
        className="flex shrink-0 items-center justify-between border-b border-border bg-bg-2 px-4"
        style={{ height: HEADER_HEIGHT_PX }}
      >
        <span className="text-sm font-semibold text-text">{title}</span>
        <button
          type="button"
          onClick={onClose}
          aria-label="Close"
          className="flex size-6 items-center justify-center rounded text-text-2 outline-none hover:bg-panel-2 hover:text-text"
        >
          <X className="size-4" />
        </button>
      </div>
      {/* Body — scrolls independently when content overflows the panel
          height. The 48 px subtraction keeps the scrollbar inside the
          body so the header stays pinned to the top. `bodyScroll={false}`
          (panels with their own internal scroll) drops the overflow + reserved
          gutter so the body doesn't waste a 15px right strip. */}
      <div
        className={`flex-1 p-3 ${
          bodyScroll ? "overflow-y-auto scrollbar-stable" : "overflow-hidden"
        }`}
        style={{ height: `calc(100% - ${HEADER_HEIGHT_PX}px)` }}
      >
        {children}
      </div>
    </div>
  );
}

type ToolPanelSectionProps = {
  title: string;
  defaultOpen?: boolean;
  /** When true, the section renders flat (no <details>/<summary>); use
   *  for sections that are always visible like Ambient / Shadow. */
  alwaysOpen?: boolean;
  /** Optional unit annotation shown muted next to the title (e.g.
   *  "units", "units/s", "units/s²"). Labels the measurement of a section's
   *  bare numeric fields once, instead of per-spinner suffixes that truncate
   *  in dense / resizable XYZ grids. */
  unit?: string;
  children?: ReactNode;
};

function ToolPanelSection({
  title,
  defaultOpen = false,
  alwaysOpen = false,
  unit,
  children,
}: ToolPanelSectionProps) {
  // B1.3.2: shared `.panel-section` class set with Section.tsx so both
  // collapsible-section consumers use one source-of-truth styling.
  // alwaysOpen branch keeps its no-chevron / no-cursor variant; the
  // `cursor: default` override on the header suppresses the shared
  // class's `cursor: pointer` so Lighting's Ambient / Shadow sections
  // don't suggest interactivity they don't have.
  if (alwaysOpen) {
    return (
      <section className="panel-section">
        <div className="panel-section-header" style={{ cursor: "default" }}>
          <SectionTitle title={title} unit={unit} />
        </div>
        <div className="panel-section-body">{children}</div>
      </section>
    );
  }
  // Controlled (was native <details>) so the body can animate — native
  // details toggles content instantly and can't tween. Header matches
  // Section.tsx (div role=button) so both controlled disclosures expose
  // the same a11y shape + chevron behaviour.
  return <CollapsibleSection title={title} defaultOpen={defaultOpen} unit={unit}>{children}</CollapsibleSection>;
}

// Shared section-header label: title + optional muted unit annotation.
// The unit overrides the header's uppercase/letter-spacing so "units/s²" stays
// readable, and is aria-hidden so the header button's accessible name stays the
// bare `title` (the unit is decorative context, mirroring the Spinner unit span).
function SectionTitle({ title, unit }: { title: string; unit?: string }) {
  return (
    <span className="flex items-baseline gap-1.5">
      <span>{title}</span>
      {unit && <span className="font-normal normal-case tracking-normal text-text-3" aria-hidden="true">{unit}</span>}
    </span>
  );
}

function CollapsibleSection({
  title,
  defaultOpen,
  unit,
  children,
}: {
  title: string;
  defaultOpen: boolean;
  unit?: string;
  children?: ReactNode;
}) {
  const [open, setOpen] = useState(defaultOpen);
  const toggle = () => setOpen((o) => !o);
  return (
    <section className="panel-section" data-open={open}>
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
      >
        <SectionTitle title={title} unit={unit} />
        <ChevronDown className="chev size-3" />
      </div>
      <div className="collapse-anim" data-open={open}>
        {/* padding-free clip div so collapse reaches a true 0 (the body's
            vertical padding would otherwise leave a sliver). */}
        <div>
          <div className="panel-section-body">{children}</div>
        </div>
      </div>
    </section>
  );
}

type ToolPanelFooterProps = { children?: ReactNode };

function ToolPanelFooter({ children }: ToolPanelFooterProps) {
  return (
    <div className="mt-3 flex flex-wrap items-center gap-2 border-t border-border pt-3">
      {children}
    </div>
  );
}

type ToolPanelRowProps = {
  label: string;
  children?: ReactNode;
};

/** Two-column row helper: label on left, control on right. */
function ToolPanelRow({ label, children }: ToolPanelRowProps) {
  return (
    <div className="grid grid-cols-[80px_1fr] items-center gap-2">
      <span className="text-[11px] text-text-2">{label}</span>
      <div className="min-w-0">{children}</div>
    </div>
  );
}

ToolPanel.Section = ToolPanelSection;
ToolPanel.Footer = ToolPanelFooter;
ToolPanel.Row = ToolPanelRow;
