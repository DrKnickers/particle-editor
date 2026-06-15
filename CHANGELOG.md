# Changelog

All notable user-facing changes to the Particle Editor are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). This project ships continuously, one pull
request at a time, rather than in tagged releases — so changes are grouped by **merge date** (newest
first) and each entry links its PR.

For the per-PR engineering diary (design decisions, implementation detail, and issues-and-resolutions),
see [`DEVELOPMENT_LOG.md`](DEVELOPMENT_LOG.md). For planned and in-progress work, see [`ROADMAP.md`](ROADMAP.md).

## 2026-06-15

### Added

- Undo and Redo buttons in the toolbar
- Reference-object moves, gizmo drags, Reset, and position spinners are now undoable on the same Ctrl+Z timeline as particle edits
- Reference units now mount their hardpoint weapons and turrets, and hide damaged-state and collision geometry

### Changed

- Gizmo gains screen-uniform handles, Shift-for-precision drags, grid/angle snapping, and in-drag guide lines
- Core is now a selectable, orderable submod layer instead of being silently auto-loaded
- Object catalog now prefetches eagerly and parses in parallel, so the picker rarely waits on loading
- Object picker now lists only units and structures with a search box, and builds off the UI thread to avoid freezes

### Fixed

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
