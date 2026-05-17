# Changelog

Public-facing release history for the DrKnickers fork of the GlyphX particle editor. Reverse chronological — newest release at top. For per-PR engineering detail, see [`DEVELOPMENT_LOG.md`](DEVELOPMENT_LOG.md). For planned work, see [`ROADMAP.md`](ROADMAP.md).

---

## v0.2.0 — 2026-05-16

*Tag [`v0.2.0`](https://github.com/DrKnickers/particle-editor/releases/tag/v0.2.0) · Merge [`e5e90cc`](https://github.com/DrKnickers/particle-editor/commit/e5e90cc) · PR #82*

Quality-of-life features that speed up the particle workflow and preview work to bring the editor's render closer to what you see in-game.

### New features
- **Undo / Redo.** `Ctrl+Z` / `Ctrl+Y` for every edit that survives a `.alo` save/load — property fields, track keys, structural emitter ops, the `Leave Particles` toggle. Stack capped at 100 entries; edits within ~1.5 s on the same emitter coalesce.
- **Autosave with crash recovery.** Two-tier snapshots to `%TEMP%\AloParticleEditor\` — recent every 30 s, stable every 5 min. Recovery prompt on next launch when an orphan autosave is found.
- **Bloom in the preview.** Loads the game's canonical `Engine\SceneBloom.fx` through the same resolution chain as particle shaders. *View → Bloom… / Ctrl+B* dialog exposes Strength / Cutoff / Size; sunburst toolbar button toggles on/off.
- **Selectable backgrounds.** Unified Background toolbar button opens a 4×3 picker — slot 0 is solid colour, slots 1–8 are bundled skydomes (real base-game textures loaded via FileManager with mod overlay support), slots 9–11 user-customisable.
- **Selectable ground texture.** Ground Texture toolbar button opens a 4×3 picker; bundled and custom slots both supported.
- **Adjustable environment lighting.** *View → Lighting…* dialog with separate Ambient and Sun colour pickers plus a sun-direction triplet. Live preview updates.
- **Frequently-used textures palette.** *Palette* button on the Appearance tab's Textures group opens a 12-slot grid of pinned textures. Persists in `palette.ini` next to the exe.
- **Import emitters from another `.alo`.** *File → Import Emitters from File…* opens a `.alo` picker then a checkbox tree of source emitters. Selected emitters land as new roots with collision-free names, remapped spawn-child cross-references, and preserved link groups. Whole import is one undo step.

### Emitter management
- **Drag-and-drop reorder.** Click-and-drag a root emitter past sibling roots. Whole subtree moves as a block. Auto-scrolls at top/bottom of the tree.
- **Drag-and-drop reparent.** Drop emitter S onto emitter T (mid-row hover) to make S a child of T. Slot picker popup when both `spawnDuringLife` and `spawnOnDeath` are free.
- **Multi-select.** `Ctrl+click` toggles, `Shift+click` ranges. Right-click context-menu ops act on the whole selection.
- **Linked emitters.** Link emitters into groups that share parameter edits in lock-step. Configurable per-group exempt set for fields that should stay per-emitter. Visual bracket in the emitter-list icons marks each group with its colour.
- **Duplicate with index increment.** *Duplicate (increment index)* shifts the atlas index track by +1; *Duplicate (increment index…)* prompts for N (1–999).

### Viewport & preview
- **Pause / frame-step.** `F8` freezes the simulation; `F9` steps one frame; `F10` steps ten frames. Toolbar buttons next to the Bloom toggle.
- **Adjustable ground height.** Spinner in the header strip, range −100 to +100, 0.1-unit step. Session-only — launches always start at z = 0.

### Input
- Spawner manual-fire shortcut moved from `Shift+Space` to `Ctrl+Space`. `F7` open shortcut unchanged.

### Bug fixes
- Bump-mapped particles (`BLEND_BUMP`, `BLEND_DECAL_BUMP`) now inherit the curve-editor Red / Green / Blue tracks. Previously the editor overwrote RGB with a rotation-tangent encoding that bore no relation to anything the user had authored and diverged from in-game behaviour.

### Known issues
- Mod-bundled `.meg` archives are not yet read. Mods that ship loose files work fully; total conversions that package assets in their own megafiles (Mod's Revenge, Awakening of the Rebellion) are not yet supported.
- Spawner configuration does not persist across sessions and resets to defaults on each launch. Window position does persist.
- After undo / redo, live preview instances (Shift-spawned or Spawner-driven) are killed. Re-spawn to see the reverted state.

---

## v0.1.0 — 2026-05-10

First tagged release of the DrKnickers fork of Mike.NL's GlyphX particle editor. Builds on Visual Studio 2022 (x64), targeting modern Windows. Includes a programmable spawner, a mods menu with hot file resolution, hot-reload for textures and shaders, and a set of preview-correctness fixes.

### New features
- **Programmable Spawner.** *Emitters → Spawner…* / `F7` opens the Spawner panel; replaces the legacy Shift+click preview. Manual mode fires a single burst on "Spawn now" or `Shift+Space`; Auto mode fires bursts on a recurring 0–60 s schedule. Per-burst configuration: size (1–10), spacing (0–10 s), world position, initial velocity, optional jitter, per-instance lifetime (0–600 s, 0 = unlimited). Capped at 50 active instances and 5 emissions per frame.
- **Mods menu.** Top-level menu listing every subfolder of `<game>\GameData\Mods\` and `<game>\corruption\Mods\`. Selecting a mod hot-swaps the file-resolution chain — no restart required. Right-click any mod entry to set a friendly nickname (renders in italics). Selection persists across sessions.
- **Hot-reload.** `F5` flushes the texture cache and re-fetches every active emitter. `F6` reloads shaders all-or-nothing — if any shader fails to compile, the previous set stays alive and the failure is reported on the status bar.
- **View-setting persistence.** Background colour, ground-plane toggle, and the 16 custom colours in the colour-picker dialog all persist across sessions. *View → Reset View Settings* restores defaults.

### Emitter management
- Move Up / Move Down buttons on the emitter-list toolbar for reordering root emitters. Whole subtree moves with the parent.
- `Alt+↑` / `Alt+↓` shortcuts mirroring the toolbar buttons.
- Move Up / Move Down on the emitter right-click context menu.
- Duplicate on the emitter right-click context menu — copy lands directly below the original, no clipboard round-trip.
- Naming on duplicate / paste changed from `_(copy)` to `_<n>` numeric suffixes (`Fire`, `Fire_1`, `Fire_2`). Increment scans every existing emitter so duplicates never collide.

### Input
- Mouse-wheel input on every spinner control. Hover and scroll to nudge the value.
- `Shift` modifier on spinner wheel for 10× step.
- `Ctrl` modifier on spinner wheel for 0.1× step (float spinners only).
- `Shift+Space` shortcut for manual Spawner burst.

### Viewport & preview
- Overlapping emitters now draw in the in-game order. Top-of-list emitters appear on top of the stack as they do at runtime.
- Tailed particles no longer spin in the preview when they would not spin in-game. Rotation track is ignored on tailed emitters, matching engine behaviour.

### File compatibility
- Loading `.alo` files where a `spawnOnDeath` or `spawnDuringLife` index points past the end of the emitter list no longer crashes. Bad indices are clamped to the no-emitter sentinel and a `[Load]` warning is logged. Re-saving commits the cleanup.

### Stability
- Editor now starts on x64. Fixed a pointer-truncation bug at 20 sites in 9 files.
- Toolbar icons now appear in the top toolbar, the emitter-list toolbar, and the tree view.
- Toolbar buttons now respond to clicks on x64.
- Fixed crash on the next render frame after deleting an emitter that had spawned live particles.
- Fixed vector-out-of-range assert when opening certain `.alo` files (root cause: 32-bit sentinel widened to 64-bit without sign extension).

### Localization
- Fixed ~73 mojibake sites in the German UI (`de.rc`) where umlauts and `s²` units rendered as `ï¿½`. Every umlaut, `°`, `±`, and `²` now displays correctly.
- Resource files now stored as UTF-8 with BOM so any modern editor will round-trip the file safely.

---

## Credits

- Original editor by **Mike.NL** ([GlyphXTools/particle-editor](https://github.com/GlyphXTools/particle-editor)).
- Fork maintained by **DrKnickers**, with help from Claude.
