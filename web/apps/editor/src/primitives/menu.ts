// menu.ts — the ONE style source for every Radix menu surface: the menubar
// dropdowns (MenuBar, ModsMenu), the emitter-tree row context menu and its
// New Emitter dropdown, the curve-key context menu, and the Select listboxes.
// Surfaces pass only their own min-width on top (via cn()); chrome, item rows,
// highlight, disabled look and separators all come from here so they can't
// drift apart again.

/** Floating-panel chrome shared by menus and select listboxes (no motion). */
const MENU_SURFACE =
  "z-50 rounded-md border border-border-2 bg-bg-2 p-1 shadow-[var(--shadow-soft)]";

/** Menu / submenu content (Menubar, ContextMenu): entrance + exit fade. */
export const MENU_CONTENT = `${MENU_SURFACE} popover-animate`;

/** A menubar dropdown (File / Edit / … / Mods). */
export const MENUBAR_CONTENT = `${MENU_CONTENT} min-w-[200px]`;

/** Select listbox content. ENTRANCE-only: Radix Select unmounts on close, so
 *  an exit animation could never play (components.css, .popover-animate-in). */
export const SELECT_CONTENT = `${MENU_SURFACE} popover-animate-in`;

/** A menu row (Item / SubTrigger / CheckboxItem). Radix moves
 *  data-highlighted with both the pointer and the keyboard, so it is the only
 *  hover state needed. Disabled = text-3 at opacity-40 (DESIGN.md). */
export const MENU_ITEM =
  "flex cursor-pointer select-none items-center gap-2 rounded px-2 py-1 text-xs text-text outline-none data-[highlighted]:bg-panel-2 data-[disabled]:cursor-not-allowed data-[disabled]:text-text-3 data-[disabled]:opacity-40";

/** A select listbox option — accent highlight marks the value about to be
 *  chosen (a menu row's neutral highlight marks an action). */
export const SELECT_ITEM =
  "cursor-pointer select-none rounded px-2 py-0.5 text-xs text-text outline-none data-[highlighted]:bg-accent-soft data-[highlighted]:text-accent";

export const MENU_SEPARATOR = "my-1 h-px bg-panel-2";

/** Top-level menubar trigger (File / Edit / … / Mods). focus-ring: keyboard
 *  focus on a CLOSED trigger was invisible (outline-none with no replacement)
 *  — a 2.4.7 gap the PRODUCT.md conformance check caught. */
export const MENUBAR_TRIGGER =
  "px-2 py-1 text-xs font-medium text-text-2 transition-colors motion-reduce:transition-none hover:bg-bg-2 rounded data-[state=open]:bg-bg-2 data-[state=open]:text-text outline-none focus-ring select-none cursor-default";
