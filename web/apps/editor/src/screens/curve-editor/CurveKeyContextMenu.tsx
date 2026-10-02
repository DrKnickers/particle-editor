// CurveKeyContextMenu — the per-key right-click menu on the curve canvas, a
// Radix ContextMenu like the emitter tree's row menu: focus moves into the
// menu, arrow keys / Home / End / typeahead walk the items, Escape or an
// outside click dismisses, focus returns to where it was, and the popup flips
// / shifts to stay inside the viewport.
//
// The right-click lands on an SVG key marker deep inside CurveEditor, which
// reports it through `onKeyContextMenu(time, isBorder, x, y)` (and consumes
// the native event). The panel then calls `openAt(key, x, y)`, which replays
// the right-click as a `contextmenu` event on this menu's zero-size trigger —
// Radix ContextMenu opens at the event's pointer coordinates, wherever its
// trigger sits. Non-modal, so a click elsewhere on the canvas both dismisses
// the menu and lands (the old hand-built menu's click-through).

import { useImperativeHandle, useRef, useState, type Ref } from "react";
import * as ContextMenu from "@radix-ui/react-context-menu";
import { cn } from "@/lib/utils";
import { MENU_CONTENT, MENU_ITEM } from "@/primitives/menu";
import { Tip } from "@/primitives/Tip";

export type CurveKeyMenuTarget = { time: number; isBorder: boolean };

export type CurveKeyContextMenuHandle = {
  /** Open the menu for `key` at viewport coordinates (the right-click's
   *  clientX / clientY). */
  openAt: (key: CurveKeyMenuTarget, x: number, y: number) => void;
};

type Props = {
  ref?: Ref<CurveKeyContextMenuHandle>;
  onDelete: (time: number) => void;
};

export function CurveKeyContextMenu({ ref, onDelete }: Props) {
  const anchor = useRef<HTMLSpanElement>(null);
  // The key the menu acts on. Kept after close so the exit fade still renders
  // the item it faded from; replaced on the next open.
  const [target, setTarget] = useState<CurveKeyMenuTarget | null>(null);
  useImperativeHandle(ref, () => ({
    openAt(key, x, y) {
      setTarget(key);
      anchor.current?.dispatchEvent(
        new MouseEvent("contextmenu", { bubbles: true, cancelable: true, button: 2, clientX: x, clientY: y }),
      );
    },
  }), []);

  return (
    <ContextMenu.Root modal={false}>
      <ContextMenu.Trigger asChild>
        <span ref={anchor} aria-hidden="true" className="pointer-events-none fixed left-0 top-0 size-0" />
      </ContextMenu.Trigger>
      <ContextMenu.Portal>
        <ContextMenu.Content
          data-testid="ce-key-context-menu"
          aria-label="Curve key actions"
          className={cn(MENU_CONTENT, "min-w-[140px]")}
        >
          {/* Border keys can't be deleted (the host filters them out); the
              tooltip says why. A disabled Radix item still takes pointer
              events, so the Tip can sit on it directly. */}
          <Tip content={target?.isBorder ? "Border keys cannot be deleted" : undefined}>
            <ContextMenu.Item
              data-testid="ce-key-context-menu-delete"
              disabled={target === null || target.isBorder}
              onSelect={() => { if (target !== null) onDelete(target.time); }}
              className={MENU_ITEM}
            >
              Delete
            </ContextMenu.Item>
          </Tip>
        </ContextMenu.Content>
      </ContextMenu.Portal>
    </ContextMenu.Root>
  );
}
