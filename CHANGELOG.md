# Changelog

All notable user-facing changes to the Particle Editor are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Changes are grouped by **SemVer release
version** (newest first).
See the [tags](https://github.com/DrKnickers/particle-editor/tags) ·
[Releases](https://github.com/DrKnickers/particle-editor/releases).

## [Unreleased]

### Fixed

- Dragging a reference object with the gizmo now tracks the cursor 1:1 instead of drifting behind it
- The coordinate readout shown while dragging a reference object now updates smoothly instead of stuttering
- Resizing the main editor panels now persists between sessions even when the right-hand dock (Spawner / Lighting / Atlas) is closed — previously a resize made with the dock closed was discarded on restart

## [0.3.0] - 2026-08-02

### Added

- A "What's New" page — reached from the landing topbar and pointed to from the guide home page — walks returning modders through what this editor does that Mike.NL's GlyphX Particle Editor v1.5 couldn't (scene context, mod-stack editing, the spawner, emitter draw-order, the Atlas Frame Picker), plus a table of deliberate behavioral departures from the legacy editor
- Help → Keyboard Shortcuts… opens a dialog listing every keyboard shortcut in the editor, grouped into 8 sections
- Deleting every emitter now leaves a hint in the viewport — "No emitters — press + to add one, or Ctrl+Z to undo" — instead of an empty canvas with no explanation
- The status bar now confirms add, duplicate, paste, cut, and delete actions with a brief message (e.g. "Deleted 3 emitters — Ctrl+Z to undo"), announced to screen readers
- Deleting an emitter now fades its tree row out in place instead of removing it instantly, while the remaining rows glide smoothly into the gap
- The curve editor's key plot is now keyboard-operable — Tab into it, use the arrow keys to select a key and switch channels, and Ctrl+arrow to nudge the selected key's time or value (announced by a screen-reader status line); the emitter tree, the texture palette, and the color-swatch grids gained matching arrow-key navigation, and three inputs that previously had no visible focus ring now show one
- The curve editor now has a faint sub-grid and a snap-to-grid toggle — a magnet button in the curve toolbar (off by default, remembered between sessions); turn it on and dragging or inserting keys snaps them to the finer grid on both axes
- The guide has been revised for readers new to authoring particle effects — 28 factual corrections across blend modes, motion & physics, generation types, the glossary, the primer, troubleshooting, stacking, and the tutorials, each checked directly against the editor's source; Setup now walks all the way through finding your game install, extracting the tutorial mod step by step, and opening it via the real Mods menu (Add mod… / Mod Load Order / Refresh); two new pages — a concept bridge before the from-scratch tutorials and a value-free capstone brief for building your own effect; first-use definitions for emitter / hardpoint / root-vs-child / additive / quad; the home page now opens with a "What you are building" intro and the primer gained an emitter→pixel diagram; the Atlas Picker is now called by its real UI name, "Atlas Frame Picker," and terminology (Index track, Continuous stream, Weather particle) is consistent across every page; and the tutorials' parameter recipes are grounded in the real shipped `.alo` files, render as tables instead of code blocks, and call out key gotchas in styled callout boxes
- "Duplicate with Index Increment" now has a Repeat count (1–999, default 1) alongside the increment delta — set it above 1 to chain that many duplicates, each stepping the index by the delta from the last copy, in a single undo step
- The in-app title bar's corner icon now matches the Windows taskbar/exe icon instead of the fainter tile-less brand mark
- The guide's Basic Controls, Blend Modes, Generation Types, and Stacking Emitters & Children pages are no longer placeholders — each now covers its topic in full (viewport controls, all 10 blend modes, the Bursts/Continuous/Weather generation modes, and sibling vs. child-emitter stacking); the standalone Weather Particles stub is gone (folded into Generation Types, since Weather is a generation mode, not a separate topic), the "Coming Soon" nav section is dissolved now that its pages are written, and the Particle Authoring Primer gained a new Render Order section
- The guide's tutorial pages now embed their walkthrough media inline — each step's recorded editor clip (with pause/replay controls) plays as you scroll to it, alongside the before/after screenshots — instead of empty placeholders; the media ships from the shared release at the (gated) site rollout
- The site guide now carries the full tutorial course — Setup, a Particle Authoring Primer, four hands-on tutorials, and four quick references — with per-page wayfinding ("Tutorials · 3 of 4"), previous/next links at the bottom of every page, and readable link tables
- The landing site now has a user guide — Markdown-authored pages rendered in the site's look with a sidebar and per-page contents rail, linked from the top bar; navigating between the landing page and the guide fades smoothly instead of hard-cutting
- Public landing page — a static GitHub Pages showcase for the editor in `site/`, with looping UI clips and a download CTA; built now, deploys to the public fork at the (gated) rollout
- The Import Emitters list is now fully keyboard-navigable — arrow keys move between emitters, ←/→ collapse/expand, Home/End jump, and Space/Enter selects
- The editor now tells you when an `.alo` has no particle emitters (a model file, not a particle effect) — both when importing emitters and when opening a file, instead of showing an empty list / empty editor with no explanation
- Every interactive control now shows a keyboard-focus indicator, so the editor is fully keyboard-navigable
- Animations now respect the operating system's "reduce motion" accessibility setting
- Stack multiple mod layers in any load order — compose the stack from the Mods menu (add layers via **Add mod…**, drag rows to reorder, **Reset** to unmodded) or the full **Load Order** dialog (**Expand to full editor**); the top layer wins. Replaces picking a single mod + submods
- Drag-to-reorder the mod stack with a live glide preview — a make-room gap opens at the drop point while a floating chip glides to it — in both the Mods menu's active stack and the Load Order dialog (matching the Emitter tree's feel)
- Pick atlas frames visually — a right-dock **Atlas Frame Picker** shows the selected emitter’s texture as its frame grid; the active `index` key’s frame is highlighted, hovering previews a frame, and clicking a cell assigns it (with a confirm before overwriting keys that hold different frames). It opens automatically when you select a key on the index channel, or via the toolbar ⊞ Atlas button
- Model shadows can now be soft-edged, matching the game's blurred look; a "Soft shadows" toggle (Preferences → Rendering, on by default) switches between the soft and hard-edged shadow
- Reference objects (imported game units/structures) now cast stencil shadows onto the ground and self-shadow, using the game's own shadow technique; shadow darkness is driven by the existing "Sun Shadow Color" swatch in the Lighting panel, and the effect can be disabled via Preferences → Rendering → "Model shadows"
- A "Smooth skydome seams" toggle (Preferences → Rendering, on by default) hides the seam baked into stock skydome backgrounds; turn it off to show the dome exactly as the game renders it
- Imported reference objects now render at their true in-game size — the game's per-object scale factor is applied, so a particle effect can be sized against a faithful reference instead of one that was undersized
- A live readout appears next to the gizmo while you drag a reference object, showing its position (units) or rotation (degrees)
- A "Show grid" toggle for the unit grid is now on the toolbar (next to "Show ground") and in the View menu, alongside the existing Ground dropdown checkbox
- Antialiasing for the 3D viewport, with a quality setting in Preferences → Rendering (Off / 2× / 4× / 8×, limited to what your GPU supports) — applied instantly and remembered
- Lock the reference object — a "Lock object" checkbox in the object popup freezes its placement; while locked it can't be selected, dragged, or nudged, and the lock persists across object swaps and editor restarts
- Ground-plane handle on the reference-object gizmo — drag the object across the floor (X/Y) at a fixed height; respects grid snap and Shift-precision, and is undoable
- Undo and Redo buttons in the toolbar
- Reference-object moves, gizmo drags, Reset, and position spinners are now undoable on the same Ctrl+Z timeline as particle edits
- Reference units now mount their hardpoint weapons and turrets, and hide damaged-state and collision geometry
- Stack multiple submods in explicit precedence order via Mods > Submods
- Reference-object gizmo gains world-axis rotation rings alongside the translate arrows
- Drop a real game or mod object into the preview as a scale reference, with a unit grid and draggable axis handles
- Groundwork to render the game's real skydome behind the preview
- Window titlebar now shows the open file name with an unsaved-changes dot, plus a "Particle Editor" rebrand
- Preview overload guard now refuses oversized spawns preemptively and clears the preview with a banner
- Curves morph smoothly when keys are added, deleted, pasted, or interpolation changes
- Preferences toggle and tunable cap for the live-preview particle ceiling
- Advisory warning on emitter-tree rows when a chain may spawn too many particles
- Drag a multi-selection of emitters as one block with a make-room gap and cursor chip
- Confirm prompt before deleting emitters with children or in bulk, plus error modals for failed Save/Open
- Paste As ▸ Lifetime Child / Death Child in the emitter-tree context menu
- Clear button in the Import Emitters dialog to deselect all emitters at once
- Status bar shows a Shift-to-spawn hint, a PAUSED indicator, and a 2-decimal cursor readout
- Emitter-tree reorder drag autoscrolls at list edges and cancels with Esc or right-click
- Crash-recovery autosave with recent and stable snapshots and a restore prompt on relaunch
- Saved lighting restores at startup, Force Align syncs with the legacy editor, and a Lighting toolbar toggle is added
- Frequently-used texture palette with pinned and recent thumbnails for emitter color/bump textures
- Browse button to pick emitter color and bump textures from a native file dialog
- Headless frame-capture mode for rendering-fidelity checks via --capture
- Undo and Redo (Ctrl+Z / Ctrl+Shift+Z) for particle-system edits in the new UI
- Resizable panel splitters with persisted sizes and a View menu Reset panel layout option
- New design system with Inter typography and a Sun/Moon dark/light theme toggle that persists
- Curve editing restored with a focus-channel model: emphasized active channel, edit toolbar, and full key editing
- Mods menu now lists installed EaW/FoC mods and hot-swaps the active mod on selection
- File Exit, Reset Camera, Reset View Settings, and Force Align Fill Lights menu items now work
- EmitterTree panel toolbar and a live 3D cursor position readout in the status bar

### Changed
- The Windows download is now a self-contained `ParticleEditor.exe` — the React interface is built into the executable, so there is no separate `web` folder to keep beside it and you can run the `.exe` from anywhere; only `d3dx9_43.dll` still ships alongside. On the few PCs without the Microsoft Edge WebView2 runtime, the editor opens Microsoft's download page for it
- The What's New page is retired — even slimmed it stayed redundant with the landing page. Returning v1.5 users now get a dedicated Start Here guide that compares old and current workflows across authoring, mod assets, scene context, preview tools, and safer editing; the frozen five-row departures table moves with it, the guide home points there, and the topbar on every page drops the What's New item
- The guide sidebar's "Videos pending" badge on tutorials 2 and 4 is now bracketed — "(Videos pending)" — so it reads as an aside instead of blending into the tutorial title
- Guide article links now carry a rest-state underline instead of being distinguished by colour alone, fixing a WCAG 1.4.1 (link-in-text-block) failure across every guide page
- The What's New page is now a slim reference for returning modders — the five capability sections duplicated the landing page's feature tour clip-for-clip and were cut; the page keeps the lineage claim and the verified departures table, and links to the landing for the tour

- Tutorials 2 and 4 in the site guide no longer embed walkthrough clips — their sidebar entries now show a "Videos pending" badge but stay ordinary clickable links, and each page keeps its opening still
- Hover/press feedback, opening a menu or a context menu or a dropdown, switching an inspector tab, swapping the right-dock panel, adding an emitter row, the PAUSED indicator, and the light/dark theme switch all animate now instead of cutting instantly (still inert with reduce-motion on, or while a clip is recording); the empty emitter tree, the atlas picker's missing-texture message, and the curve panel's empty state now tell you what to do next instead of just naming the gap, and an invalid hex color entry is now visibly flagged instead of silently reverting
- Tutorial 5's section 4 gained a build-from-default clip ahead of the Option A result — the walkthrough starts from a plain default-looking Fire emitter and scrubs every field reachable from the UI to its exact tutorial value, raises its Scale curve, and picks its two flipbook frames from the Atlas Frame Picker, so the whole assembly is watchable step by step instead of only the finished burst
- Tutorial 5's explosion clips now sit the effect centred in the preview viewport instead of small and off in the corner of the editor — a still shot, no camera movement or zoom. The interaction clips zoom only onto their UI steps — the emitter tree for the drag, the curve editor for the Index staircase, the Atlas Frame Picker as it highlights Frame 0 and Frame 15, the Set Link Group dialog — and no longer push in on the explosion itself; that zoom is only there to keep the UI details legible
- Tutorial 5's section 4 now shows **both** ways to build the fireball side by side — the flipbook fire under Option A and the layered additive fire (Fire + Fire Details, from the plain example) under Option B — so the choice the page describes is one you can watch, not just read
- Tutorial 5's walkthrough clips now show the real explosion the page is written around — the two downloadable example effects, with their textured flipbook fire, smoke, debris, flash and shockwave — instead of an unrelated placeholder built from plain untextured particles; the step clips now demonstrate the page's actual lessons on that effect: the fire's render order flipping when it is reordered against the smoke, the Index track stepping the flipbook frames in the Atlas Frame Picker, and four debris copies being multi-selected into a link group
- Link-group badges in the emitter tree now show the bare group number ("1") instead of "G1"
- The editor no longer writes a `bloom-diagnostic.log` next to the exe (or prints the bloom parameter dump) on every run — that introspection is now opt-in via the `ALO_SHADER_DIAG` environment variable
- The hardpoint-damage, smoke-polish, and shield-impact tutorials' walkthrough clips now show the actual clicks each edit takes — focusing a colour channel by clicking its row, stepping key values on the Value spinner, flashing the active mod stack from the Mods menu, and ticking Link-particles-to-instance after collapsing the sections above it — and zoom in on the panel being edited so the controls are readable at embed size, matching the laser-shot tutorial's style
- The laser-shot tutorial's walkthrough clips now show the actual clicks and typing each step takes — renaming an emitter by typing into the name box, picking an atlas frame in the picker, dropping keys onto the colour curves, switching Bursts generation, dialling the Spawner's velocity, and adjusting Parent-speed-inherit in the Physics tab — and zoom in on the relevant panel so the controls are readable at embed size
- Landing page reorganized into a numbered walkthrough — Scene context, Mod stack, Spawner, Emitter order, Atlas picker, then a grouped Toolset grid (Authoring · Preview & viewport · Files & reliability) — with a new Spawner clip showing a projectile effect in flight and copy that calls out mod-shader resolution and live reload
- The app icon now stays legible at taskbar and title-bar sizes — small renders simplify to a bolder spline + particle on a brighter tile, and the icon ships exact 20/24/40px variants so Windows shows a crisp icon instead of a resampled one
- Landing page: the editor clips now display about a third larger — media breaks out of the text column — and the hero editor window is visible above the fold instead of a toolbar sliver
- The Atlas Frame Picker now previews each frame the way the emitter's blend mode actually renders it, and only dims genuinely-empty frames on blend modes where an empty frame renders as nothing — so the preview matches the viewport automatically, per emitter. The manual "Alpha" toggle is gone; the blend mode drives it
- New app icon — an azure spline-and-particle mark on a dark Windows-style tile — now appears in the taskbar, window title bar, and Alt-Tab, and is used as the landing page's favicon (replacing the placeholders)
- Switching mod layers no longer leaves a broken reference-object selection: if the selected object isn't in the new stack the selection clears to None, and it's restored automatically when you switch back to a stack that has it (your saved selection is kept)
- A locked reference object no longer blocks camera navigation — clicking and dragging on a frozen object now pans/zooms the view straight through it, so you can frame an effect against the object without unlocking it first
- The Grass, Sand, and Snow ground textures now load from your Empire at War / Forces of Corruption game install at runtime instead of being bundled with the editor; when no game install is configured they appear greyed-out in the ground picker (Dirt and Solid Colour always work)
- Ground, grid, and bloom toggles now live in a compact overlay in the bottom-left of the viewport (moved off the toolbar), alongside a new control to **lock the reference object** so you can pan and orbit without accidentally nudging it — toggle the lock from the overlay, the View menu, or Ctrl+L
- The viewport display-options pill now adapts its scrim to the scene — a light chip over dark backdrops, dark otherwise — and rests more quietly until you hover or focus it
- Atlas Picker frames now lift on hover, making the frame under your cursor easy to see against busy textures
- The Atlas Picker hover now reads clearly over busy thumbnails in both light and dark mode — a stronger tint and slightly larger lift
- Skydomes now render their authored textures faithfully, exactly as the game does; the "Smooth skydome seams" preference has been removed (it distorted the starfield and could not cleanly remove the asset's own seam)
- Redesign the Import Emitters dialog — emitters appear as a collapsible branch-select tree with a live "N of M selected" count and Select all / Clear beside it; ticking a parent now selects its whole branch (the separate Auto-include children toggle is gone)
- The editor now starts with the default emitter selected — the Inspector and curve editor are populated on launch and on New, matching the classic editor
- Keyboard focus is drawn one consistent way across the editor, and value fields gently highlight when activated
- Disabled controls now share one consistent dimmed appearance
- Atlas Frame Picker: every thumbnail now shows its frame index at all times (previously only on hover), and the grid's scrollbar matches the rest of the editor
- Atlas Frame Picker: the thumbnail grid reflows to fill the panel width, the preview shows the selected frame's number, hovering a thumbnail highlights it, and you can navigate frames with the arrow keys and assign with Enter/Space
- Redesigned the **Preferences** and **Load Order** dialogs — grouped section cards, clearer dependency nesting (dependent controls dim/disable under their parent), tree-style rows, a search box, and a top-wins precedence rail
- The Mods menu is now the primary mod-stack editor (compose and reorder in place), with the Load Order dialog as the **Expand to full editor** fallback
- Reference models and the ground now render with the game's ambient lighting, matching their in-game brightness instead of appearing darker
- Distance, velocity, and acceleration fields now show their units (units, units/s, units/s²)
- Object picker now remembers your faction filter, expanded/collapsed groups, and scroll position when you reopen it
- The toolbar's "Show ground" button now uses a floor icon instead of a grid icon (the grid icon moved to the new "Show grid" toggle)
- Object picker's faction chips now scroll within a bounded row instead of overflowing the popup when a mod has many factions
- Reference-object gizmo restyled for readability — thicker outlined handles that read on any background, camera-faded rotation rings that sit back until hovered, a corner-bracket selection box, and a filled sweep wedge while rotating
- The reference object and its gizmo now glide smoothly to their target, including snapped grid positions and angles (the saved values stay exact)
- Object picker gains faction filter chips (All · Empire · Rebel · …) above the tree to narrow the list by allegiance
- Object picker tree is now keyboard-navigable — arrow keys move between rows, ←/→ collapse/expand groups, Home/End jump, Enter selects — and a restored selection inside a collapsed group auto-expands so it's visible
- Object picker now groups objects into a collapsible Heroes / Ground / Space tree with sub-categories (Infantry, Vehicles, Fighters, Capitals, …) for easier browsing
- Object picker now lists only the units and structures a player can actually build or field (heroes always shown), instead of thousands of entries
- Gizmo drag guide lines are now dimmed during a translate or plane drag, for less visual noise
- About dialog now shows the editor's own version (0.3.0) instead of the upstream 1.5, which is kept as a fork credit line
- Gizmo gains screen-uniform handles, Shift-for-precision drags, grid/angle snapping, and in-drag guide lines
- A mod's shared core folder is now a selectable, orderable submod layer instead of being silently auto-loaded
- Object catalog now prefetches eagerly and parses in parallel, so the picker rarely waits on loading
- Object picker now lists only units and structures with a search box, and builds off the UI thread to avoid freezes
- Picker now distinguishes a missing model file from one that fails to decode
- Background popover shows which mode is actually rendering and flags skydomes that fail to load; unused custom-texture slots removed
- Ground plane now responds to the Lighting panel with bump-mapped terrain shading
- Spawner jitter reworked into per-instance path shaping with arc acceleration and squiggle controls
- Link-group emitters now marked by a colored spine, row tint, and group badge at fixed positions
- Overload warnings now track the configurable cap, with a predictive system-load chip and cleaner banner exit
- Selected curve key now uses a crisp inverted-core dot instead of a blurred drop shadow
- Overload guard default cap lowered to 10,000 particles for a more responsive preview
- Styled animated tooltips app-wide with a shared motion family for modals and banners
- Lighter splitter-drag: dedupes scene-rect sends and trims per-message logging
- Render loop paced to display refresh, cutting idle CPU from a full core to ~20%
- Window resizing is smooth and reveals more scene at the edges instead of rescaling
- Emitter rows glide to their new positions on reorder instead of snapping
- Delete, duplicate, and reorder now act on the whole multi-emitter selection, preserving order
- UI polish: consistent padding, unclipped fields, softer curve keys, denser emitter list, new Preferences dialog, and mod-aware Open
- New WebView2/React UI is now the default; pass --legacy for the classic Win32 chrome
- Curve-key spinner edits now record a single undo step per gesture instead of one per tick
- Collapsible sections now expand and collapse with a smooth height animation
- Shorter texture labels, exclusive Rotation curve channel, and clicking a link-group bracket selects all members
- Curve-editor marquee selection can now begin from the axis-label gutters
- Reset Camera menu item and Ctrl+Home shortcut now share one definition so they can't drift
- Rapid scroll-wheel and spinner edits to one field now collapse into a single undo step
- Decimal numeric fields now display a consistent 2 decimal places
- Lighting is now a docked side pane sharing the Spawner slot, with Bloom settings folded in
- Link-group brackets show per-member stubs, hug the names, and keep dedicated lanes
- Emitter rows show the visibility eye on the left and child spawn-role glyph on the right
- Refined number fields, curve editor, emitter toolbar, and link-group brackets to match legacy
- Denser inspector layout with flat sections, indent hierarchy, and aligned checkboxes
- Inspector labels use the primary text colour; dimming now signals disabled params
- Emitter list text now matches the 12px body size used across side panels
- Themed emitter-list scrollbar and a native title bar that follows the app theme
- Removed the light-grey hairline border framing the viewport for a seamless edge
- Sphere and Cylinder emitter distribution fields now match the legacy editor, with a "Constrain to surface" checkbox
- Spawner panel now renders as a single clean card matching its neighbours
- Dark theme panels are now neutral grey instead of reading as navy/purple
- New UI now boots DXGI composition rendering by default, with a legacy opt-out env var
- Engine viewport now composites via DXGI for smoother, higher-FPS rendering
- Chrome panels now render over the viewport via DirectComposition hosting
- Inspector tabs and tool panels share unified collapsible section headers, with polished field layouts and alignment
- Modal dialogs now sit over a frosted-glass backdrop that blurs panels and the viewport seamlessly
- Refined left-pane layout: 25/75 tree-to-tabs split, working File toolbar buttons, and pinned tree toolbar
- Property tab strip always visible even with no file loaded, sharing the left column on a 25/75 split
- Basic, Appearance, and Physics tabs reorganized to match the legacy editor's section structure
- Left pane gains collapsible sections, wider Name input, Duplicate button, and Show/Hide All icon buttons
- Left pane realigned: bottom tree toolbar, per-row visibility eye, and multi-lane link-group brackets
- Workspace restructured into the 2026 layout with grouped toolbar, popovers, permanent Spawner column, and viewport toggles
- Viewport chrome edges now blend with soft, feathered alpha instead of a hard pixel seam

### Fixed

- The very first launch on a fresh PC no longer opens with a black, unlit viewport. The default scene lighting was only applied when the editor found settings from a previous run, so the one launch every new user starts with rendered the ground plane unlit; the defaults now apply on the first run too
- The status bar now warns you when an autosave fails to write. Previously a failing autosave was silent, so you could keep working for an hour believing a crash would cost you nothing while the newest recoverable copy fell further and further behind
- Opening a particle file whose emitters are chained thousands deep no longer closes the editor without warning. Such a file now loads with the over-deep part of the chain reattached at the top level, and nothing in it is discarded
- Switching mod layers repeatedly while browsing textures no longer piles up memory. Preview work for the old mod stack is now dropped when you switch, rather than being finished and thrown away
- The undo history now has a memory ceiling as well as a 100-step limit, so an unusually large effect can no longer grow it without bound
- Switching emitters while a curve edit is still saving no longer carries the old emitter's key selection onto the new one. Previously the spinner could show the previous emitter's value against a key that held a different one, so the next nudge silently wrote that wrong value
- One Ctrl+Z now undoes a whole multi-emitter delete. Deleting three emitters at once used to take three presses of Ctrl+Z to reverse, restoring them one at a time
- One Ctrl+Z now undoes a whole multi-key curve paste. Pasting several curve keys used to take one press per key to reverse, removing them one at a time
- Saving now fails loudly instead of silently writing a truncated file. If the disk fills mid-save the editor reports the failure and leaves your original file untouched, where before it could report success and replace your work with a corrupt copy
- Screen readers now correctly announce emitter tree rows as tree items, including which one is selected, when moving through the tree with the keyboard
- Pausing the preview now pauses the spawner too — an Auto spawner on an interval no longer banks up its countdown while the scene is stopped and releases a burst of instances all at once when you unpause; frame-stepping a paused scene advances the spawner by exactly the frames you stepped
- Shift no longer spawns a particle instance when your pointer is over a panel, dialog, or menu — only Shift over the 3D viewport spawns (Shift+click inside the viewport is unchanged)
- Dragging a multi-key selection in the curve editor no longer lets a key slide on top of an unselected key (which left a stuck duplicate) — the group now stops just short of any key it would collide with
- Dragging a multi-key selection up in the curve editor no longer jumps on release for auto-ranging channels (Scale, Index) — the keys commit exactly where the curve showed them mid-drag
- Dragging the Time spinner's arrow column now keeps moving the selected key for the whole scrub, instead of the key stalling after the first tick while the number ran ahead
- The "Show ground" viewport toggle now stays how you left it across restarts, and toggling it no longer marks the file as unsaved
- Adjusting a curve key with the Value or Time spinner now updates the curve immediately and stays locked to the number, instead of lagging behind while you scrub
- Child emitters now render behind the parent emitter they were spawned from, matching the game — a spawned child no longer draws on top of its parent
- Dragging a key in the curve editor now keeps the curve line locked to the cursor instead of lagging behind it on busy effects
- Heavy particle systems that repeatedly refused to render in the preview — even after raising the overload cap to its maximum — now render correctly; the preview's overload estimate no longer wildly over-projects particle counts for chains where a child emitter spawns on its parent's death
- The Atlas Frame Picker now correctly highlights the wrapped frame when an index-curve value runs past the last atlas cell (e.g. index 4 on a 4-frame atlas now shows frame 0, matching the game), instead of losing the selection highlight
- Child emitters (spawned during a parent's life or on its death) now preview drawn behind or in front of their siblings according to their authored rank, instead of always drawing on top regardless of rank
- The Atlas Frame Picker no longer freezes the editor for several seconds when opened on a large atlas texture (e.g. a 256-frame particle atlas) — the frame grid now draws in a single canvas paint instead of one DOM element per frame, so opening, scrolling, and closing the picker stay smooth regardless of atlas size
- Face-camera (screen-oriented) particles no longer freeze their orientation while the preview is paused — they keep re-orienting to the camera as you orbit the view
- The Blend Mode dropdown no longer wraps or overflows for long labels (e.g. "Diffuse transparent") — it now truncates to one line
- A failing Ctrl+S (and a parked Ctrl+O/Ctrl+N behind the unsaved-changes prompt) no longer trips an unhandled internal error — the failure is now caught and handled the same way other save/open failures are
- A corrupted or malformed `.alo` file with an invalid chunk header now fails to load with a clear error instead of risking a crash on load
- Mode-11 bump particles no longer preview at ~50% opacity under a mod's modified bump shader — the editor now sets the shader's distance-fade parameter to a full-alpha value instead of leaving it unset
- Bump-mapped particles (normal/depth-textured emitters — e.g. explosion debris rocks) no longer render as dark squares at small sizes; they keep their alpha-cutout rock shapes, matching the in-game look
- A particle effect with custom link-group "exempt" settings now saves and reloads correctly instead of producing a file the editor couldn't reopen; malformed or oversized `.alo` files also no longer hang or exhaust memory when loading
- Reference objects no longer show their muzzle flashes stuck on — the editor now hides muzzle-flash geometry to match the in-game look, where a unit's muzzle flash appears only while it fires
- A reference object you positioned with the gizmo no longer slides back to the origin when you close the reference-object picker (or deselect it) — the placement is kept, and the picker's position fields now track the gizmo live instead of showing a stale value
- Reference objects (imported game units) now remember their own position and rotation — switching to a different unit no longer leaves it floating above the ground with the previous object's placement, and switching back to a unit restores where you put it
- Browsing for a file (Import Emitters' **Browse**, and the skydome/ground texture pickers) no longer replaces or discards your open particle system — it only returns the chosen path
- Changing the mod load order now shows an error if it couldn't be applied (for example, a layer's shaders failed to reload) instead of silently appearing to take effect
- A failed save now reports the error instead of silently appearing to succeed
- Deleting multiple emitters now removes exactly the ones you selected even if the tree changed after you selected them, and asks once before deleting emitters that still have children
- When crash-recovery can't load an autosave, the recovery file is now kept so you can retry, instead of being deleted
- Typing in a dialog no longer leaks keystrokes through to the 3D viewport behind it
- Switching focus away from the 3D viewport in the middle of a drag or spawn action now cleanly ends the action, instead of leaving a stray spawn or a half-applied move
- The vanilla space skydomes (Stars Low/Medium/High) no longer show a faint white vertical line — the sun glow now renders as a round billboard facing the camera, the way the game draws it, instead of an edge-on sliver
- Tooltips now appear cleanly — the little pointer arrow fades in with the tooltip instead of popping in a moment later
- Saving an `.alo` can no longer corrupt your existing file if the save fails partway through (disk full, a removable/network drive disconnected, permission denied) — your original file is left untouched until the new one is fully written
- Closing the editor window with unsaved changes now prompts you to Save / Don't Save / Cancel, instead of silently discarding your work and deleting its crash-recovery autosave
- The editor now shows a recovery screen with a Reload button if the interface ever hits an unexpected error, instead of going blank
- Import Emitters now reports the result accurately — a partial or failed import keeps the dialog open with a message instead of closing as if it had fully succeeded, and double-clicking Import no longer imports twice
- Primary buttons, selection badges, checkboxes, and hint text now meet WCAG AA colour contrast — primary buttons use a slightly deeper blue so their white labels stay legible in both themes
- The Delete confirmation button now meets WCAG AA contrast — a deeper red so its white label stays legible
- The Open dialog (File→Open and Import Emitters' Browse) now opens in the active mod's models folder instead of an unrelated directory
- Light theme: status messages (error / success / warning) now meet readable contrast, and the UI uses a single consistent accent blue
- Light theme: menus, dialogs, and popovers cast a soft shadow instead of a hard-edged one, and the atlas selection highlight now matches the theme
- Light theme: curve-editor key markers and lines no longer disappear
- Atlas Frame Picker: cell badges and the hover cue now read correctly on the light theme (the previous hover highlight was invisible there), and the picker's styling matches the rest of the editor
- Atlas Frame Picker: a failed frame assignment is now announced to screen readers instead of silently doing nothing
- Atlas Frame Picker: the thumbnail grid no longer reflows or jumps on the first open after launch and now sits centered in the panel; the curve editor no longer jumps during the panel slide; and the first Alpha toggle is instant
- Enabling a reference model's cast shadows no longer makes additive and transparent particles vanish or turn black — particles render correctly with reference-model shadows on
- Model shadows now stay anchored to the object as the camera zooms out, instead of sliding out of position at a distance
- The Lighting panel's Reset now restores the same lighting the scene loads with, instead of unexpectedly brightening the render
- Curve-editor keys now glide into place when you switch the selected emitter, instead of popping/blinking in (the curve lines already animated; the keys now match)
- Switching mods or submods is snappier — the editor no longer re-scans the skydome list multiple times per switch
- A scrollbar no longer flashes when dragging a reference object to the right edge of the viewport
- Mod skydomes now appear in the background picker — domes a mod registers under non-standard filenames are listed and selectable, not just the base-game ones
- Object picker no longer leaks planet and skydome backdrops (e.g. low-orbit planet models) into the unit list
- Lighting panel changes now persist to the registry and survive reopen and restart
- Skinned reference units render in bind pose, nested submod content loads, gizmo gains Reset, and unit-grid toggle moves to the Ground popup
- Imported game objects render correctly: hidden meshes stay hidden, collision and transparency are faithful, normals fixed, with an amber selection box
- Space skydomes now render the secondary nebula over the starfield (was invisible)
- Game and mod skydomes now render correctly on packed installs (were empty or black)
- Background picker selections now persist across restarts in the new UI
- Transparent particles in the preview now match in-game opacity instead of looking washed out
- Morphing follower curves no longer paint over the focus channel's keys
- Spinner arrow buttons stay within the field border and now show a press state
- Time/Value spinners live-update while dragging a multi-key curve selection
- Locked curve channels are now truly read-only and render as a dashed mirror
- Extreme spawn values no longer crash the live preview; spawning pauses over budget
- Closed right dock no longer opens empty on drag or mounts open at startup
- Emitter-tree drag-reorder fixes including a child-slot swap that corrupted saved files
- Multi-select move and duplicate now correctly mark the document dirty in browser mode
- Modal frosted-glass backdrop now paints near-instantly when the editor is maximized
- Viewport edge now glides smoothly with the right dock during its open/close slide
- Editing a shared property on a linked emitter with live particles no longer crashes
- Right dock now slides open and closed without flickering the left pane
- Stable scrollbar gutter, wider texture field, easier-to-click curve keys, and tighter splitter cursor
- Undo no longer swallows a step after a redo
- Left inspector section chevrons now animate on collapse and expand
- Ground, background, and skydome view settings now restore from your saved preferences at startup
- Emitter-tree drag-to-reorder works again in the new UI
- Enabling bloom produces visible glow again from saved settings
- Removed the thin black line along the Spawner panel's viewport edge
- Child emitter spawn-role glyph sits next to the name, and seconds fields now accept three decimals
- Viewport no longer goes dead when a resize reallocation fails under memory pressure
- Web UI bridge no longer leaks pending requests on send failure or page teardown
- XML parser no longer hangs at full CPU on elements carrying attributes
- Import Emitters now actually imports the selected emitters in the new UI
- Solid-colour ground now opens a picker, recolours the plane, and the ground-height field returns
- A failed save now keeps the document, dirty marker, and autosave instead of losing your work
- Linked emitter groups now share parameters and propagate edits across members
- Viewport rendering no longer slows down when the window is maximized or large
- Panel corners and gaps over the viewport now blend with the theme instead of showing dark wedges
- Splitter gutters next to the viewport no longer show black seams
- Particles render correctly over a background skydome (no more white blowout or tinting)
- Shift-spawn now places particles exactly under the cursor, with correct status-bar world coordinates
- Recover the FPS drop when maximizing the editor window in the new UI
- Undoing back to the last saved state now clears the unsaved-changes prompt
- Link groups left with a single member now auto-demote so the Inspector reads correctly
- Resizing panes or the window now cleanly reveals more scene instead of distorting content
- Reclaim per-frame overhead at high resolutions for smoother maximized playback
- Chrome dropdowns over the viewport no longer show an alpha-cutout artifact
- Curve editor now fully usable: working lock-to, correct axis labels, theme-aware grid, and robust spinners
- Skydome and ground custom-slot pickers now open with texture filters and the ground slot actually applies

### Removed

- Removed the legacy classic (Win32) editor interface and its `--legacy` / `--legacy-ui` opt-out — the WebView2/React UI is now the only interface (x64-only)

### Security

- A mod-supplied texture name inside an `.alo` can no longer reach files outside the mod folder. The traversal guard rejected `..` but not `.. ` (with a trailing space), which Windows resolves to the same parent directory — the whole dots-and-spaces family is now rejected, in both the asset-name and capture-path guards
- Close the last mod-asset allocation gap from the #413 audit: whole-file reads of textures, models, and shaders now reject an oversized file before allocating its buffer, so a safe-named but very large mod asset can no longer force a huge memory allocation
- Extend the malformed/malicious mod-file hardening to every place the editor reads mod assets — skydome and ground textures, reference models, palette previews, and the game-object catalog — so a path in any of them stays inside the mod folder, with hard size limits that reject oversized or malformed data
- Harden the editor against malformed or maliciously crafted mod files: untrusted `.alo`, `.meg`, and `.xml` files are now rejected cleanly instead of risking a crash, and a texture/shader name embedded in an `.alo` can no longer reach outside the mod folder or trigger an outbound network request when the file is opened
- Web UI host enforces an origin allow-list, blocking off-origin navigation, popups, and permission requests

## [0.2.0] - 2026-05-16

### Added

- Import selected emitters from another .alo file via File menu, as a single undo step
- Selectable skydome backgrounds from a 12-slot picker opened via the Background button
- Lighting dialog to adjust the preview's sun, fill lights, ambient, and shadow colours
- Frequently-used textures palette with recent and pinned thumbnails, per mod
- Selectable ground texture with bundled presets, solid color, and custom slots
- Per-group settings dialog to choose which fields are shared across a link group
- Coloured brackets in the emitter tree show link-group membership at a glance
- Multi-select emitters with Ctrl-click, Shift-click, and marquee drag for bulk linking
- Link emitters into groups so shared parameters stay in lock-step across members
- Duplicate emitter with atlas index increment, by one or a chosen amount
- Pause and frame-step the preview with F8, F9, and F10 or toolbar buttons
- Bloom in the preview via View → Bloom dialog (Ctrl+B), toolbar toggle, and persisted Strength/Cutoff/Size knobs
- Adjustable ground-plane height spinner in the preview, persisted across sessions
- Two-tier autosave writes recovery snapshots and offers to restore after a crash
- Drag-and-drop reparenting in the emitter tree, moving the whole subtree as a block
- Drag-and-drop reordering of root emitters in the tree, with ghost image and insertion mark
- Undo and Redo for editor edits (Ctrl+Z / Ctrl+Y) via menu, toolbar, and shortcuts

### Changed

- Ground Height now resets to 0 on every launch instead of persisting
- Skydome slots load real base-game and mod-overlay textures, refreshing live on mod switch

### Fixed

- Curve-editor RGB color tracks now tint bump-mapped particles like every other blend mode

## [0.1.0] - 2026-05-10

### Added

- Programmable particle Spawner dialog (F7) with manual and auto burst test modes
- Active mod's engine shaders now load from the mod folder, falling back to the base game
- View settings (background color, ground toggle, custom colors) persist across sessions, plus Reset View Settings
- Move Up / Move Down buttons, context menu, and Alt+Up/Down shortcuts to reorder root emitters
- Right-click Duplicate Emitter places a copy directly below the original
- Mouse wheel adjusts spinner values, with Shift for 10x and Ctrl for finer steps
- Reload Textures (F5) and Reload Shaders (F6) refresh assets without restarting
- Right-click a mod in the Mods menu to set a custom nickname
- Mods menu lists installed mods and hot-swaps them without restarting

### Changed

- Duplicate and paste now auto-rename with a collision-free numeric suffix instead of "(copy)"
- Editor now runs as a native 64-bit build with registry-backed game-data path lookup

### Fixed

- Tailed particles ignore the rotation track in preview, matching in-game behavior
- Garbled accented characters and symbols on dialog labels now display correctly
- Malformed .alo files with out-of-range spawn indices now load with a warning instead of crashing
- Deleting an emitter with live particles no longer crashes the editor
- Overlapping emitters now stack in the same order as the game
