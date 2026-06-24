# Changelog

All notable user-facing changes to the Particle Editor are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Day to day the project merges one pull
request at a time, so changes are grouped by **merge date** (newest first) and each entry links its
PR. Periodically those merges are rolled up into a tagged [SemVer](https://semver.org) release
([tags](https://github.com/DrKnickers/particle-editor/tags) ·
[Releases](https://github.com/DrKnickers/particle-editor/releases)); the versioning policy lives
in [`VERSIONING.md`](VERSIONING.md).

For the per-PR engineering diary (design decisions, implementation detail, and issues-and-resolutions),
see [`DEVELOPMENT_LOG.md`](DEVELOPMENT_LOG.md). For planned and in-progress work, see [`ROADMAP.md`](ROADMAP.md).

> **Unreleased** — heading toward `0.3.0`. Everything from here down to the
> **v0.2.0** marker (at 2026-05-16) is merged but not yet in a tagged release.
> The inline release markers below show where each tagged [SemVer](https://semver.org)
> release falls; see [`VERSIONING.md`](VERSIONING.md) for the policy.

## 2026-06-24

### Changed

- Ground, grid, and bloom toggles now live in a compact overlay in the bottom-left of the viewport (moved off the toolbar), alongside a new control to **lock the reference object** so you can pan and orbit without accidentally nudging it — toggle the lock from the overlay, the View menu, or Ctrl+L

### Fixed

- Saving an `.alo` can no longer corrupt your existing file if the save fails partway through (disk full, a removable/network drive disconnected, permission denied) — your original file is left untouched until the new one is fully written
- Closing the editor window with unsaved changes now prompts you to Save / Don't Save / Cancel, instead of silently discarding your work and deleting its crash-recovery autosave
- The editor now shows a recovery screen with a Reload button if the interface ever hits an unexpected error, instead of going blank

### Security

- Harden the editor against malformed or maliciously crafted mod files: untrusted `.alo`, `.meg`, and `.xml` files are now rejected cleanly instead of risking a crash, and a texture/shader name embedded in an `.alo` can no longer reach outside the mod folder or trigger an outbound network request when the file is opened

## 2026-06-23

### Added

- The Import Emitters list is now fully keyboard-navigable — arrow keys move between emitters, ←/→ collapse/expand, Home/End jump, and Space/Enter selects
- The editor now tells you when an `.alo` has no particle emitters (a model file, not a particle effect) — both when importing emitters and when opening a file, instead of showing an empty list / empty editor with no explanation

### Changed

- Redesign the Import Emitters dialog — emitters appear as a collapsible branch-select tree with a live "N of M selected" count and Select all / Clear beside it; ticking a parent now selects its whole branch (the separate Auto-include children toggle is gone)

### Fixed

- Import Emitters now reports the result accurately — a partial or failed import keeps the dialog open with a message instead of closing as if it had fully succeeded, and double-clicking Import no longer imports twice
- Primary buttons, selection badges, checkboxes, and hint text now meet WCAG AA colour contrast — primary buttons use a slightly deeper blue so their white labels stay legible in both themes
- The Delete confirmation button now meets WCAG AA contrast — a deeper red so its white label stays legible
- The Open dialog (File→Open and Import Emitters' Browse) now opens in the active mod's models folder instead of an unrelated directory

## 2026-06-22

### Added

- Every interactive control now shows a keyboard-focus indicator, so the editor is fully keyboard-navigable
- Animations now respect the operating system's "reduce motion" accessibility setting

### Changed

- The editor now starts with the default emitter selected — the Inspector and curve editor are populated on launch and on New, matching the classic editor
- Keyboard focus is drawn one consistent way across the editor, and value fields gently highlight when activated
- Disabled controls now share one consistent dimmed appearance

### Fixed

- Light theme: status messages (error / success / warning) now meet readable contrast, and the UI uses a single consistent accent blue
- Light theme: menus, dialogs, and popovers cast a soft shadow instead of a hard-edged one, and the atlas selection highlight now matches the theme
- Light theme: curve-editor key markers and lines no longer disappear
- Atlas Frame Picker: cell badges and the hover cue now read correctly on the light theme (the previous hover highlight was invisible there), and the picker's styling matches the rest of the editor

## 2026-06-21

### Changed

- Atlas Frame Picker: every thumbnail now shows its frame index at all times (previously only on hover), and the grid's scrollbar matches the rest of the editor

### Fixed

- Atlas Frame Picker: a failed frame assignment is now announced to screen readers instead of silently doing nothing
- Atlas Frame Picker: the thumbnail grid no longer reflows or jumps on the first open after launch and now sits centered in the panel; the curve editor no longer jumps during the panel slide; and the first Alpha toggle is instant
- Enabling a reference model's cast shadows no longer makes additive and transparent particles vanish or turn black — particles render correctly with reference-model shadows on

## 2026-06-20

### Changed

- Atlas Frame Picker: the thumbnail grid reflows to fill the panel width, the preview shows the selected frame's number, hovering a thumbnail highlights it, and you can navigate frames with the arrow keys and assign with Enter/Space

### Removed

- Removed the legacy classic (Win32) editor interface and its `--legacy` / `--legacy-ui` opt-out — the WebView2/React UI is now the only interface (x64-only)

## 2026-06-19

### Added

- Stack multiple mod layers in any load order — compose the stack from the Mods menu (add layers via **Add mod…**, drag rows to reorder, **Reset** to unmodded) or the full **Load Order** dialog (**Expand to full editor**); the top layer wins. Replaces picking a single mod + submods
- Drag-to-reorder the mod stack with a live glide preview — a make-room gap opens at the drop point while a floating chip glides to it — in both the Mods menu's active stack and the Load Order dialog (matching the Emitter tree's feel)
- Pick atlas frames visually — a right-dock **Atlas Frame Picker** shows the selected emitter’s texture as its frame grid; the active `index` key’s frame is highlighted, hovering previews a frame, and clicking a cell assigns it (with a confirm before overwriting keys that hold different frames). It opens automatically when you select a key on the index channel, or via the toolbar ⊞ Atlas button

### Changed

- Redesigned the **Preferences** and **Load Order** dialogs — grouped section cards, clearer dependency nesting (dependent controls dim/disable under their parent), tree-style rows, a search box, and a top-wins precedence rail
- The Mods menu is now the primary mod-stack editor (compose and reorder in place), with the Load Order dialog as the **Expand to full editor** fallback

## 2026-06-18

### Added

- Model shadows can now be soft-edged, matching the game's blurred look; a "Soft shadows" toggle (Preferences → Rendering, on by default) switches between the soft and hard-edged shadow

### Fixed

- Model shadows now stay anchored to the object as the camera zooms out, instead of sliding out of position at a distance

## 2026-06-17

### Added

- Reference objects (imported game units/structures) now cast stencil shadows onto the ground and self-shadow, using the game's own shadow technique; shadow darkness is driven by the existing "Sun Shadow Color" swatch in the Lighting panel, and the effect can be disabled via Preferences → Rendering → "Model shadows"
- A "Smooth skydome seams" toggle (Preferences → Rendering, on by default) hides the seam baked into stock skydome backgrounds; turn it off to show the dome exactly as the game renders it
- Imported reference objects now render at their true in-game size — the game's per-object scale factor is applied, so a particle effect can be sized against a faithful reference instead of one that was undersized

### Changed

- Reference models and the ground now render with the game's ambient lighting, matching their in-game brightness instead of appearing darker
- Distance, velocity, and acceleration fields now show their units (units, units/s, units/s²)

### Fixed

- The Lighting panel's Reset now restores the same lighting the scene loads with, instead of unexpectedly brightening the render
- Curve-editor keys now glide into place when you switch the selected emitter, instead of popping/blinking in (the curve lines already animated; the keys now match)

## 2026-06-16

### Added

- A live readout appears next to the gizmo while you drag a reference object, showing its position (units) or rotation (degrees)
- A "Show grid" toggle for the unit grid is now on the toolbar (next to "Show ground") and in the View menu, alongside the existing Ground dropdown checkbox
- Antialiasing for the 3D viewport, with a quality setting in Preferences → Rendering (Off / 2× / 4× / 8×, limited to what your GPU supports) — applied instantly and remembered

### Changed

- Object picker now remembers your faction filter, expanded/collapsed groups, and scroll position when you reopen it
- The toolbar's "Show ground" button now uses a floor icon instead of a grid icon (the grid icon moved to the new "Show grid" toggle)
- Object picker's faction chips now scroll within a bounded row instead of overflowing the popup when a mod has many factions
- Reference-object gizmo restyled for readability — thicker outlined handles that read on any background, camera-faded rotation rings that sit back until hovered, a corner-bracket selection box, and a filled sweep wedge while rotating
- The reference object and its gizmo now glide smoothly to their target, including snapped grid positions and angles (the saved values stay exact)

### Fixed

- Switching mods or submods is snappier — the editor no longer re-scans the skydome list multiple times per switch
- A scrollbar no longer flashes when dragging a reference object to the right edge of the viewport

## 2026-06-15

### Added

- Lock the reference object — a "Lock object" checkbox in the object popup freezes its placement; while locked it can't be selected, dragged, or nudged, and the lock persists across object swaps and editor restarts
- Ground-plane handle on the reference-object gizmo — drag the object across the floor (X/Y) at a fixed height; respects grid snap and Shift-precision, and is undoable
- Undo and Redo buttons in the toolbar
- Reference-object moves, gizmo drags, Reset, and position spinners are now undoable on the same Ctrl+Z timeline as particle edits
- Reference units now mount their hardpoint weapons and turrets, and hide damaged-state and collision geometry

### Changed

- Object picker gains faction filter chips (All · Empire · Rebel · …) above the tree to narrow the list by allegiance
- Object picker tree is now keyboard-navigable — arrow keys move between rows, ←/→ collapse/expand groups, Home/End jump, Enter selects — and a restored selection inside a collapsed group auto-expands so it's visible
- Object picker now groups objects into a collapsible Heroes / Ground / Space tree with sub-categories (Infantry, Vehicles, Fighters, Capitals, …) for easier browsing
- Object picker now lists only the units and structures a player can actually build or field (heroes always shown), instead of thousands of entries
- Gizmo drag guide lines are now dimmed during a translate or plane drag, for less visual noise
- About dialog now shows the editor's own version (0.3.0) instead of the upstream 1.5, which is kept as a fork credit line
- Gizmo gains screen-uniform handles, Shift-for-precision drags, grid/angle snapping, and in-drag guide lines
- Core is now a selectable, orderable submod layer instead of being silently auto-loaded
- Object catalog now prefetches eagerly and parses in parallel, so the picker rarely waits on loading
- Object picker now lists only units and structures with a search box, and builds off the UI thread to avoid freezes

### Fixed

- Mod skydomes now appear in the background picker — domes a mod registers under non-standard filenames (e.g. Mod) are listed and selectable, not just the base-game ones
- Object picker no longer leaks planet and skydome backdrops (e.g. low-orbit planet models) into the unit list
- Lighting panel changes now persist to the registry and survive reopen and restart

## 2026-06-14

### Added

- Stack multiple Mod submods in explicit precedence order via Mods > Submods
- Reference-object gizmo gains world-axis rotation rings alongside the translate arrows
- Drop a real game or mod object into the preview as a scale reference, with a unit grid and draggable axis handles

### Changed

- Picker now distinguishes a missing model file from one that fails to decode
- Background popover shows which mode is actually rendering and flags skydomes that fail to load; unused custom-texture slots removed

### Fixed

- Skinned reference units render in bind pose, mod Core content loads, gizmo gains Reset, and unit-grid toggle moves to the Ground popup
- Imported game objects render correctly: hidden meshes stay hidden, collision and transparency are faithful, normals fixed, with an amber selection box
- Space skydomes now render the secondary nebula over the starfield (was invisible)

## 2026-06-13

### Added

- Groundwork to render the game's real skydome behind the preview

### Changed

- Ground plane now responds to the Lighting panel with bump-mapped terrain shading
- Spawner jitter reworked into per-instance path shaping with arc acceleration and squiggle controls

### Fixed

- Game and mod skydomes now render correctly on packed installs (were empty or black)
- Background picker selections now persist across restarts in the new UI
- Transparent particles in the preview now match in-game opacity instead of looking washed out

## 2026-06-12

### Added

- Window titlebar now shows the open file name with an unsaved-changes dot, plus a "Particle Editor" rebrand

### Changed

- Link-group emitters now marked by a colored spine, row tint, and group badge at fixed positions

## 2026-06-11

### Added

- Preview overload guard now refuses oversized spawns preemptively and clears the preview with a banner
- Curves morph smoothly when keys are added, deleted, pasted, or interpolation changes

### Changed

- Overload warnings now track the configurable cap, with a predictive system-load chip and cleaner banner exit
- Selected curve key now uses a crisp inverted-core dot instead of a blurred drop shadow
- Overload guard default cap lowered to 10,000 particles for a more responsive preview

### Fixed

- Morphing follower curves no longer paint over the focus channel's keys
- Spinner arrow buttons stay within the field border and now show a press state
- Time/Value spinners live-update while dragging a multi-key curve selection
- Locked curve channels are now truly read-only and render as a dashed mirror

## 2026-06-10

### Added

- Preferences toggle and tunable cap for the live-preview particle ceiling
- Advisory warning on emitter-tree rows when a chain may spawn too many particles

### Changed

- Styled animated tooltips app-wide with a shared motion family for modals and banners
- Lighter splitter-drag: dedupes scene-rect sends and trims per-message logging
- Render loop paced to display refresh, cutting idle CPU from a full core to ~20%
- Window resizing is smooth and reveals more scene at the edges instead of rescaling

### Fixed

- Extreme spawn values no longer crash the live preview; spawning pauses over budget
- Closed right dock no longer opens empty on drag or mounts open at startup

## 2026-06-09

### Added

- Drag a multi-selection of emitters as one block with a make-room gap and cursor chip
- Confirm prompt before deleting emitters with children or in bulk, plus error modals for failed Save/Open

### Changed

- Emitter rows glide to their new positions on reorder instead of snapping
- Delete, duplicate, and reorder now act on the whole multi-emitter selection, preserving order

### Fixed

- Emitter-tree drag-reorder fixes including a child-slot swap that corrupted saved files
- Multi-select move and duplicate now correctly mark the document dirty in browser mode
- Modal frosted-glass backdrop now paints near-instantly when the editor is maximized

## 2026-06-08

### Changed

- UI polish: consistent padding, unclipped fields, softer curve keys, denser emitter list, new Preferences dialog, and mod-aware Open
- New WebView2/React UI is now the default; pass --legacy for the classic Win32 chrome
- Curve-key spinner edits now record a single undo step per gesture instead of one per tick

### Fixed

- Viewport edge now glides smoothly with the right dock during its open/close slide
- Editing a shared property on a linked emitter with live particles no longer crashes

## 2026-06-07

### Added

- Paste As ▸ Lifetime Child / Death Child in the emitter-tree context menu
- Clear button in the Import Emitters dialog to deselect all emitters at once

### Changed

- Collapsible sections now expand and collapse with a smooth height animation
- Shorter texture labels, exclusive Rotation curve channel, and clicking a link-group bracket selects all members

### Fixed

- Right dock now slides open and closed without flickering the left pane
- Stable scrollbar gutter, wider texture field, easier-to-click curve keys, and tighter splitter cursor

## 2026-06-06

### Added

- Status bar shows a Shift-to-spawn hint, a PAUSED indicator, and a 2-decimal cursor readout

## 2026-06-05

### Added

- Emitter-tree reorder drag autoscrolls at list edges and cancels with Esc or right-click
- Crash-recovery autosave with recent and stable snapshots and a restore prompt on relaunch

### Changed

- Curve-editor marquee selection can now begin from the axis-label gutters
- Reset Camera menu item and Ctrl+Home shortcut now share one definition so they can't drift
- Rapid scroll-wheel and spinner edits to one field now collapse into a single undo step

### Fixed

- Undo no longer swallows a step after a redo

## 2026-06-03

### Added

- Saved lighting restores at startup, Force Align syncs with the legacy editor, and a Lighting toolbar toggle is added

### Changed

- Decimal numeric fields now display a consistent 2 decimal places

### Fixed

- Left inspector section chevrons now animate on collapse and expand

## 2026-06-02

### Changed

- Lighting is now a docked side pane sharing the Spawner slot, with Bloom settings folded in
- Link-group brackets show per-member stubs, hug the names, and keep dedicated lanes

### Fixed

- Ground, background, and skydome view settings now restore from your saved preferences at startup
- Emitter-tree drag-to-reorder works again in the new UI
- Enabling bloom produces visible glow again from saved settings
- Removed the thin black line along the Spawner panel's viewport edge
- Child emitter spawn-role glyph sits next to the name, and seconds fields now accept three decimals

## 2026-06-01

### Changed

- Emitter rows show the visibility eye on the left and child spawn-role glyph on the right
- Refined number fields, curve editor, emitter toolbar, and link-group brackets to match legacy
- Denser inspector layout with flat sections, indent hierarchy, and aligned checkboxes

### Fixed

- Viewport no longer goes dead when a resize reallocation fails under memory pressure
- Web UI bridge no longer leaks pending requests on send failure or page teardown
- XML parser no longer hangs at full CPU on elements carrying attributes
- Import Emitters now actually imports the selected emitters in the new UI
- Solid-colour ground now opens a picker, recolours the plane, and the ground-height field returns
- A failed save now keeps the document, dirty marker, and autosave instead of losing your work
- Linked emitter groups now share parameters and propagate edits across members

### Security

- Web UI host enforces an origin allow-list, blocking off-origin navigation, popups, and permission requests

## 2026-05-31

### Changed

- Inspector labels use the primary text colour; dimming now signals disabled params
- Emitter list text now matches the 12px body size used across side panels
- Themed emitter-list scrollbar and a native title bar that follows the app theme
- Removed the light-grey hairline border framing the viewport for a seamless edge
- Sphere and Cylinder emitter distribution fields now match the legacy editor, with a "Constrain to surface" checkbox
- Spawner panel now renders as a single clean card matching its neighbours

### Fixed

- Viewport rendering no longer slows down when the window is maximized or large

## 2026-05-30

### Changed

- Dark theme panels are now neutral grey instead of reading as navy/purple

### Fixed

- Panel corners and gaps over the viewport now blend with the theme instead of showing dark wedges
- Splitter gutters next to the viewport no longer show black seams
- Particles render correctly over a background skydome (no more white blowout or tinting)

## 2026-05-29

### Added

- Frequently-used texture palette with pinned and recent thumbnails for emitter color/bump textures
- Browse button to pick emitter color and bump textures from a native file dialog
- Headless frame-capture mode for rendering-fidelity checks via --capture

## 2026-05-26

### Changed

- New UI now boots DXGI composition rendering by default, with a legacy opt-out env var

### Fixed

- Shift-spawn now places particles exactly under the cursor, with correct status-bar world coordinates
- Recover the FPS drop when maximizing the editor window in the new UI

## 2026-05-25

### Added

- Undo and Redo (Ctrl+Z / Ctrl+Shift+Z) for particle-system edits in the new UI

### Changed

- Engine viewport now composites via DXGI for smoother, higher-FPS rendering

### Fixed

- Undoing back to the last saved state now clears the unsaved-changes prompt
- Link groups left with a single member now auto-demote so the Inspector reads correctly
- Resizing panes or the window now cleanly reveals more scene instead of distorting content

## 2026-05-22

### Added

- Resizable panel splitters with persisted sizes and a View menu Reset panel layout option

### Changed

- Chrome panels now render over the viewport via DirectComposition hosting
- Inspector tabs and tool panels share unified collapsible section headers, with polished field layouts and alignment

### Fixed

- Reclaim per-frame overhead at high resolutions for smoother maximized playback
- Chrome dropdowns over the viewport no longer show an alpha-cutout artifact

## 2026-05-21

### Changed

- Modal dialogs now sit over a frosted-glass backdrop that blurs panels and the viewport seamlessly
- Refined left-pane layout: 25/75 tree-to-tabs split, working File toolbar buttons, and pinned tree toolbar
- Property tab strip always visible even with no file loaded, sharing the left column on a 25/75 split
- Basic, Appearance, and Physics tabs reorganized to match the legacy editor's section structure

## 2026-05-20

### Changed

- Left pane gains collapsible sections, wider Name input, Duplicate button, and Show/Hide All icon buttons
- Left pane realigned: bottom tree toolbar, per-row visibility eye, and multi-lane link-group brackets

### Fixed

- Curve editor now fully usable: working lock-to, correct axis labels, theme-aware grid, and robust spinners

## 2026-05-19

### Added

- New design system with Inter typography and a Sun/Moon dark/light theme toggle that persists
- Curve editing restored with a focus-channel model: emphasized active channel, edit toolbar, and full key editing
- Mods menu now lists installed EaW/FoC mods and hot-swaps the active mod on selection
- File Exit, Reset Camera, Reset View Settings, and Force Align Fill Lights menu items now work
- EmitterTree panel toolbar and a live 3D cursor position readout in the status bar

### Changed

- Workspace restructured into the 2026 layout with grouped toolbar, popovers, permanent Spawner column, and viewport toggles

### Fixed

- Skydome and ground custom-slot pickers now open with texture filters and the ground slot actually applies

## 2026-05-18

### Changed

- Viewport chrome edges now blend with soft, feathered alpha instead of a hard pixel seam

> ### ▾ Released as v0.2.0 — [2026-05-16](https://github.com/DrKnickers/particle-editor/releases/tag/v0.2.0)
> Everything below this line shipped in **v0.2.0** or earlier.

## 2026-05-16

### Added

- Import selected emitters from another .alo file via File menu, as a single undo step
- Selectable skydome backgrounds from a 12-slot picker opened via the Background button

### Changed

- Ground Height now resets to 0 on every launch instead of persisting
- Skydome slots load real base-game and mod-overlay textures, refreshing live on mod switch

## 2026-05-15

### Added

- Lighting dialog to adjust the preview's sun, fill lights, ambient, and shadow colours
- Frequently-used textures palette with recent and pinned thumbnails, per mod

## 2026-05-14

### Added

- Selectable ground texture with bundled presets, solid color, and custom slots
- Per-group settings dialog to choose which fields are shared across a link group
- Coloured brackets in the emitter tree show link-group membership at a glance

## 2026-05-12

### Added

- Multi-select emitters with Ctrl-click, Shift-click, and marquee drag for bulk linking
- Link emitters into groups so shared parameters stay in lock-step across members
- Duplicate emitter with atlas index increment, by one or a chosen amount

## 2026-05-11

### Added

- Pause and frame-step the preview with F8, F9, and F10 or toolbar buttons
- Bloom in the preview via View → Bloom dialog (Ctrl+B), toolbar toggle, and persisted Strength/Cutoff/Size knobs

## 2026-05-10

### Added

- Adjustable ground-plane height spinner in the preview, persisted across sessions
- Two-tier autosave writes recovery snapshots and offers to restore after a crash
- Drag-and-drop reparenting in the emitter tree, moving the whole subtree as a block
- Drag-and-drop reordering of root emitters in the tree, with ghost image and insertion mark
- Undo and Redo for editor edits (Ctrl+Z / Ctrl+Y) via menu, toolbar, and shortcuts
- Programmable particle Spawner dialog (F7) with manual and auto burst test modes

### Fixed

- Curve-editor RGB color tracks now tint bump-mapped particles like every other blend mode

> ### ▾ Released as v0.1.0 — [2026-05-10](https://github.com/DrKnickers/particle-editor/releases/tag/v0.1.0) · first release
> The 2026-05-10 section above is the split day: the Spawner dialog was the v0.1.0 release; the rest of that day landed later and shipped in v0.2.0. Everything below this line shipped in **v0.1.0**.

## 2026-05-09

### Added

- Active mod's engine shaders now load from the mod folder, falling back to the base game
- View settings (background color, ground toggle, custom colors) persist across sessions, plus Reset View Settings
- Move Up / Move Down buttons, context menu, and Alt+Up/Down shortcuts to reorder root emitters

### Changed

- Duplicate and paste now auto-rename with a collision-free numeric suffix instead of "(copy)"

### Fixed

- Tailed particles ignore the rotation track in preview, matching in-game behavior

## 2026-05-08

### Added

- Right-click Duplicate Emitter places a copy directly below the original
- Mouse wheel adjusts spinner values, with Shift for 10x and Ctrl for finer steps

### Fixed

- Garbled accented characters and symbols on dialog labels now display correctly

## 2026-05-07

### Added

- Reload Textures (F5) and Reload Shaders (F6) refresh assets without restarting
- Right-click a mod in the Mods menu to set a custom nickname
- Mods menu lists installed mods and hot-swaps them without restarting

### Changed

- Editor now runs as a native 64-bit build with registry-backed game-data path lookup

### Fixed

- Malformed .alo files with out-of-range spawn indices now load with a warning instead of crashing
- Deleting an emitter with live particles no longer crashes the editor
- Overlapping emitters now stack in the same order as the game
