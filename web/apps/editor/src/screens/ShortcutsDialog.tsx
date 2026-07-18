// ShortcutsDialog — Help → Keyboard Shortcuts… (design follow-ups, F1).
//
// A curated reference for every keyboard surface in the editor. The app-wide
// accelerator section mirrors lib/use-app-accelerators.ts (`ACCEL_COMBOS` +
// its dispatch switch) and the MenuBar `Hint` strings — when an accelerator
// is added/changed THERE, update the row HERE (single static table by
// design: a runtime registry for a reference dialog isn't worth the
// plumbing; this comment is the drift tripwire). Panel-local keys (tree,
// curve plot, atlas grid, spinners) are documented at their handlers:
// EmitterTree.handleTreeKeyDown, CurveEditor.onKeyboardNav wiring,
// AtlasPickerPanel.onGridKeyDown, Spinner's scrub column.
import { Fragment } from "react";
import { Modal } from "@/components/Modal";

type Row = { keys: string[]; does: string };
type Section = { title: string; rows: Row[] };

const SECTIONS: readonly Section[] = [
  {
    title: "File",
    rows: [
      { keys: ["Ctrl+N"], does: "New document" },
      { keys: ["Ctrl+O"], does: "Open…" },
      { keys: ["Ctrl+S"], does: "Save" },
      { keys: ["Alt+F4"], does: "Exit" },
    ],
  },
  {
    title: "Edit",
    rows: [
      { keys: ["Ctrl+Z"], does: "Undo" },
      { keys: ["Ctrl+Y", "Ctrl+Shift+Z"], does: "Redo" },
      { keys: ["Ctrl+C", "Ctrl+X", "Ctrl+V"], does: "Copy / cut / paste emitters (tree focused)" },
      { keys: ["Del"], does: "Delete selected emitters (tree) or curve keys (curve editor)" },
      { keys: ["Ctrl+Del"], does: "Clear all particles" },
    ],
  },
  {
    title: "Playback & viewport",
    rows: [
      { keys: ["F8"], does: "Pause / resume" },
      { keys: ["F9"], does: "Step one frame" },
      { keys: ["F10"], does: "Step ten frames" },
      { keys: ["Shift"], does: "Hold to spawn an instance at the cursor — only while the pointer is over the viewport" },
      { keys: ["Ctrl+Space"], does: "Trigger a spawner burst" },
      { keys: ["Ctrl+Home"], does: "Reset the camera" },
    ],
  },
  {
    title: "View",
    rows: [
      { keys: ["Ctrl+G"], does: "Toggle ground" },
      { keys: ["Ctrl+H"], does: "Toggle heat debug" },
      { keys: ["Ctrl+L"], does: "Toggle reference-object lock" },
      { keys: ["F5"], does: "Reload textures" },
      { keys: ["F6"], does: "Reload shaders" },
      { keys: ["F7"], does: "Toggle the Spawner panel" },
    ],
  },
  {
    title: "Emitter tree",
    rows: [
      { keys: ["↑", "↓"], does: "Move between rows" },
      { keys: ["Enter", "Space"], does: "Select the focused row" },
      { keys: ["F2"], does: "Rename the focused emitter" },
      { keys: ["Alt+↑", "Alt+↓"], does: "Move the selected root up / down" },
    ],
  },
  {
    title: "Curve editor (plot focused)",
    rows: [
      { keys: ["←", "→"], does: "Select the previous / next key on the focused channel" },
      { keys: ["↑", "↓"], does: "Switch the focused channel" },
      { keys: ["Ctrl+←", "Ctrl+→"], does: "Nudge the selected key's time" },
      { keys: ["Ctrl+↑", "Ctrl+↓"], does: "Nudge the selected key's value" },
      { keys: ["Home", "End"], does: "Jump to the first / last key" },
    ],
  },
  {
    title: "Atlas frame picker (grid focused)",
    rows: [
      { keys: ["←", "→", "↑", "↓"], does: "Move the frame cursor" },
      { keys: ["Home", "End"], does: "First / last frame" },
      { keys: ["Enter", "Space"], does: "Assign the frame" },
    ],
  },
  {
    title: "Value fields",
    rows: [
      { keys: ["↑", "↓"], does: "Step the focused spinner" },
      { keys: ["Drag ▲▼"], does: "Scrub continuously on the stepper column" },
    ],
  },
];

function Kbd({ children }: { children: string }) {
  return (
    <kbd className="rounded border border-border-2 bg-bg-3 px-1.5 py-0.5 font-mono text-[10px] text-text-2">
      {children}
    </kbd>
  );
}

export function ShortcutsDialog({
  open,
  onOpenChange,
}: {
  open: boolean;
  onOpenChange: (open: boolean) => void;
}) {
  return (
    <Modal open={open} onOpenChange={onOpenChange} title="Keyboard Shortcuts" size="md">
      <Modal.Body>
        <div className="flex flex-col gap-4">
          {SECTIONS.map((s) => (
            <section key={s.title}>
              <div className="mb-1.5 text-[11px] font-semibold uppercase tracking-[0.04em] text-text-2">
                {s.title}
              </div>
              <div className="grid grid-cols-[minmax(120px,auto)_1fr] items-baseline gap-x-4 gap-y-1.5">
                {s.rows.map((r) => (
                  <Fragment key={r.does}>
                    <span className="flex flex-wrap items-center gap-1">
                      {r.keys.map((k) => (
                        <Kbd key={k}>{k}</Kbd>
                      ))}
                    </span>
                    <span className="text-xs text-text-2">{r.does}</span>
                  </Fragment>
                ))}
              </div>
            </section>
          ))}
        </div>
      </Modal.Body>
    </Modal>
  );
}
