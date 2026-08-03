# Design

<!-- Scope: the EDITOR APP (web/apps/editor). Captured from the live system
     (styles/tokens.css + components.css are the source of truth — this file
     describes them; when they disagree, the CSS wins and this file is stale).
     -->

## Foundations

- **Source of truth:** `apps/editor/src/styles/tokens.css` (CSS custom
  properties; dark default on `:root`, light via `[data-theme="light"]`),
  republished to Tailwind v4 utilities through `@theme inline`.
- **Themes:** dark is primary; light is first-class. `color-scheme` set per
  theme so native controls follow. Theme flips cross-fade (~150ms) via a
  transient `html.theme-transition` class — skipped under reduced-motion and
  `--record`.
- **Body/html stay transparent** (WebView2 ↔ D3D9 sibling-HWND compositing);
  the shell div paints `--bg`. Never put a background on `body`.

## Color

- **Neutral ramp (deliberately desaturated, no blue tint):** `--bg → --bg-3`,
  `--panel → --panel-3`, `--border/--border-2`, `--hover`. Panels sit one step
  lighter than the canvas; borders two steps.
- **Text ramp:** `--text` (primary), `--text-2` (secondary), `--text-3`
  (tertiary — AA verified against `--bg` ONLY; do not treat as universally AA).
- **Accent (blue) is interaction state only** — focus rings, selection,
  pressed toggles, primary buttons. Never decoration.
- **Fill vs foreground split:** `--accent-strong`, `--danger-strong` are the
  only fills allowed behind white text (AA); `--*-fg` variants are the only
  semantic TEXT colors (theme-adjusted to AA). Base `--danger/success/warning`
  are for tints/fills only.
- **Theme-independent by design:** `--overlay-scrim(-fg)` (fixed dark scrim +
  light fg over arbitrary imagery — also the modal overlay), `--atlas-selected`
  amber (domain color for atlas-frame selection ONLY), axis colors
  (`--x/y/z-axis`), the Windows-native close-button red, and scene-preview
  swatches (they depict engine state, not chrome — documented at each site).
- **Invalid state:** `[aria-invalid="true"]` paints `--danger-fg` border — the
  one shared error language for fields.

## Typography

- **Inter variable** (`font-display: block`), one family for everything.
- **12px body convention app-wide.** Tailwind's `--text-sm` is rebased to 12px
  so `text-sm` and `text-xs` agree; larger steps reserved for headings/glyphs.
  Tabular numerals for stats/readouts.

## Spacing, radii, chrome

- Radii: `--radius` 8 / `--radius-sm` 5 / `--radius-xs` 4 / `--radius-2xs` 2.
- Row heights: `--row-h` 26 / `--row-h-sm` 22.
- One elevation shadow for floating surfaces: `--shadow-soft` (two-layer,
  theme-tuned). Sole exception: the adaptive viewport-overlay scrim carries
  its own tokenized shadow (`--vp-scrim-shadow`) because it sits over
  arbitrary render output, not chrome. No other drop shadows.
- Scrollbars are themed per semantic scroll container; reserve gutters with
  `.scrollbar-stable` where collapsibles can overflow.

## Motion

- **Tokens:** fast tier `--motion-fast-in/out` (130/110ms) for tooltips,
  menus, micro-fades; slow tier `--motion-slow-in/out` (180/150ms) for
  modals/banners/chips. Entrances `ease-out`, exits `ease-in`; per-surface
  slip distances (`--slip-tooltip/banner/modal`).
- **Vocabulary:** `.popover-animate` (entrance+exit, Radix `data-state`),
  `.popover-animate-in` (Select — entrance-only; Radix Select unmounts on
  close, exits can never play), `.tip-animate`, `.modal-animate`,
  `.banner-animate`, `.fade-animate(-fast)` (usePresence chips),
  `.fade-in-fast` / `.row-fade-in` (remount/entrance fades), `.collapse-anim`
  (grid-rows tween). JS glides: FLIP reorder (EmitterTree/use-stack-reorder),
  host-synced dock slide, curve morph.
- **Two kill-switches, both mandatory for ANY new motion:**
  `@media (prefers-reduced-motion: reduce)` (or `motion-reduce:` at Tailwind
  call sites) and the `[data-recording]` root attribute (clip-frame
  determinism). Transitions enumerate properties — never `transition: all`.
- Regression fence: `tests-web/motion-coverage.spec.ts`.

## Components & interaction

- **Radix primitives** for menus/dialogs/popovers/tabs/selects/tooltips; the
  shared `Modal` owns dialog chrome + frozen-viewport backdrop; `ToolPanel`
  owns docked/overlay tool windows (incl. focus management: chrome-triggered
  opens focus the panel, close restores the opener, auto-open never steals).
- **Focus:** one canonical ring — `.focus-ring` / `.focus-ring-inset`
  (keyboard-only). Borderless inputs get wrapper `focus-within:border-accent`.
- **Collections are single Tab stops** (roving tabindex): emitter tree,
  atlas grid (aria-activedescendant listbox — the exemplar), texture palette,
  color swatch grids (`useRovingIndex` for new consumers).
- **States:** every control ships default/hover/focus/active/disabled —
  the bar for new work; one known latent gap: `.text-input` has no disabled
  styling (unreachable today, FieldText exposes no disabled prop). Disabled
  = `opacity-40` + `cursor-not-allowed`. Async feedback via
  `role="status"`/`aria-live` regions. Empty states teach the next action
  (AtlasPickerPanel's six cause-specific messages are the pattern).
- **Density:** compact (26/22px rows, 12px type) — a tool, not a marketing
  page. Uppercase section headers are the settings-panel convention.

## Layout

- App shell: title bar / menubar / toolbar / left tree+inspector / center
  viewport (transparent D3D9 slot) / right dock (Spawner ⇄ Lighting ⇄ Atlas)
  / bottom curve editor / status bar. Panels resize via react-resizable-panels
  (keyboard-operable splitters); the right dock is one slot whose content
  swaps with an entrance fade.
