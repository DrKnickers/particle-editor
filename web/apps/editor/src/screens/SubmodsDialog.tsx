// SubmodsDialog — pick and order the submod stack under the active mod.
//
// A mod (e.g. Mod) can keep several submod folders (Mod/GCW/Rev/TR) that each
// carry their own Data\Art. The shared Core core is one of these folders
// now too — a normal selectable/orderable layer (Mod/IR/TR include it after their
// campaign; Rev omits it). Several can be stacked in explicit precedence order — the
// user arranges them to mirror their game launch stack. This dialog is a reorderable
// checklist: check the layers to include, use ↑/↓ to set precedence (top wins), then Apply.
//
// State: loads the available submods + the current stack from `mods/list` each
// time it opens. Apply dispatches `mods/set-submods` once (one engine reload),
// rather than live-applying per click (each apply triggers a shader/texture
// reload natively). Cancel/close discards local edits.

import { useEffect, useState } from "react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { Modal } from "@/components/Modal";

type Props = { bridge: Bridge; open: boolean; onOpenChange: (open: boolean) => void };
type Row = { name: string; checked: boolean };

export function SubmodsDialog({ bridge, open, onOpenChange }: Props) {
  const [rows, setRows] = useState<Row[]>([]);

  // Re-load the available submods + current stack each time the dialog opens.
  useEffect(() => {
    if (!open) return;
    let cancelled = false;
    bridge
      .request({ kind: "mods/list", params: {} })
      .then((r) => {
        if (cancelled) return;
        const available = Array.isArray(r?.submods) ? r.submods : [];
        const active = Array.isArray(r?.activeSubmods) ? r.activeSubmods : [];
        // Selected first (in precedence order), then the rest (discovered order).
        const selected = active.filter((n) => available.includes(n));
        const rest = available.filter((n) => !selected.includes(n));
        setRows([
          ...selected.map((name) => ({ name, checked: true })),
          ...rest.map((name) => ({ name, checked: false })),
        ]);
      })
      .catch((err) => console.warn("[SubmodsDialog] mods/list failed:", err));
    return () => { cancelled = true; };
  }, [open, bridge]);

  const toggle = (i: number) =>
    setRows((rs) => rs.map((r, j) => (j === i ? { ...r, checked: !r.checked } : r)));

  const move = (i: number, dir: -1 | 1) =>
    setRows((rs) => {
      const j = i + dir;
      if (j < 0 || j >= rs.length) return rs;
      const next = rs.slice();
      [next[i], next[j]] = [next[j], next[i]];
      return next;
    });

  const apply = () => {
    const names = rows.filter((r) => r.checked).map((r) => r.name);
    void bridge.request({ kind: "mods/set-submods", params: { names } });
    onOpenChange(false);
  };

  return (
    <Modal open={open} onOpenChange={onOpenChange} title="Submods" size="sm">
      <Modal.Body>
        <div className="flex flex-col gap-2 text-sm">
          <p className="text-xs text-text-3">
            Checked submods load on top of the mod's shared core, in order — the top
            of the list wins on a shared asset name.
          </p>
          {rows.length === 0 ? (
            <p className="text-text-3">No submods found under the active mod.</p>
          ) : (
            <ul className="flex flex-col gap-1" aria-label="Submod load order">
              {rows.map((row, i) => (
                <li
                  key={row.name}
                  className="flex items-center gap-2 rounded border border-border-2 bg-bg-2 px-2 py-1"
                >
                  <input
                    type="checkbox"
                    checked={row.checked}
                    onChange={() => toggle(i)}
                    aria-label={`Include ${row.name}`}
                    className="accent-[var(--accent)]"
                  />
                  <span className={`flex-1 ${row.checked ? "text-text" : "text-text-3"}`}>
                    {row.name}
                  </span>
                  <button
                    type="button"
                    aria-label={`Move ${row.name} up`}
                    disabled={i === 0}
                    onClick={() => move(i, -1)}
                    className="px-1 text-text-2 disabled:opacity-30"
                  >
                    ↑
                  </button>
                  <button
                    type="button"
                    aria-label={`Move ${row.name} down`}
                    disabled={i === rows.length - 1}
                    onClick={() => move(i, 1)}
                    className="px-1 text-text-2 disabled:opacity-30"
                  >
                    ↓
                  </button>
                </li>
              ))}
            </ul>
          )}
        </div>
      </Modal.Body>
      <Modal.Footer>
        <Modal.CancelButton onClick={() => onOpenChange(false)}>Cancel</Modal.CancelButton>
        <Modal.OkButton onClick={apply}>Apply</Modal.OkButton>
      </Modal.Footer>
    </Modal>
  );
}
