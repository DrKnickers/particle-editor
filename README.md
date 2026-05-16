# Particle Editor

Editor for the `.alo` particle systems used by Petroglyph's Alamo engine — *Star Wars: Empire at War* and *Forces of Corruption*. The DrKnickers fork of Mike.NL's original [GlyphX Particle Editor](https://github.com/GlyphXTools/particle-editor), updated to build on modern toolchains and extended with quality-of-life features for the particle workflow.

## Install

Grab the latest zip from [Releases](https://github.com/DrKnickers/particle-editor/releases), extract anywhere, and run `ParticleEditor.exe`. On first launch you'll be asked to point at your EaW / FoC install folder.

The required `d3dx9_43.dll` is bundled in the zip alongside the exe — no separate DirectX runtime install needed.

## What's in it

The fork keeps the original editor's UI and `.alo` reader/writer and adds, among other things:

- **Editing safety net** — full undo/redo (`Ctrl+Z` / `Ctrl+Y`), two-tier autosave with crash recovery, multi-select, drag-and-drop reorder and reparent.
- **Bulk emitter work** — *linked emitters* (groups that share parameter edits in lock-step), cross-file emitter import, duplicate-with-atlas-index-increment.
- **Preview fidelity** — bloom loaded from the game's canonical `SceneBloom.fx`, adjustable environment lighting, selectable skydome background, selectable ground texture, pause / frame-step, adjustable ground height.
- **Workflow conveniences** — frequently-used textures palette, mods menu with hot file resolution, hot-reload for textures (`F5`) and shaders (`F6`), programmable particle spawner (`F7`).

See [`CHANGELOG.md`](CHANGELOG.md) for the version-by-version release history.

## Build

- **Toolchain:** Visual Studio 2022 (Platform Toolset v143), Windows 10 SDK or later, x64.
- **Dependencies:** DirectX SDK June 2010 (for `d3dx9.h` / `d3dx9_43.dll`). Header path is referenced from `DXSDK_DIR`.
- Open `ParticleEditor.sln`, pick **Debug | x64** or **Release | x64**, build. No package manager needed; everything else is in the box (the source bundles its own copies of small libs).

Debug-build notes and the per-PR engineering log live in [`DEVELOPMENT_LOG.md`](DEVELOPMENT_LOG.md).

## Project documents

- [`CHANGELOG.md`](CHANGELOG.md) — public-facing release history, grouped by version.
- [`DEVELOPMENT_LOG.md`](DEVELOPMENT_LOG.md) — per-PR engineering detail (what shipped, how it was tackled, issues encountered).
- [`ROADMAP.md`](ROADMAP.md) — planned work, grouped by horizon.

## Credits

- Original editor: **Mike.NL** ([GlyphXTools/particle-editor](https://github.com/GlyphXTools/particle-editor)) — without which none of this exists.
- Fork: **DrKnickers**, with help from Claude.
- *Star Wars: Empire at War* and *Forces of Corruption* are © Petroglyph Games / LucasArts. This project is an unofficial fan tool with no affiliation.
