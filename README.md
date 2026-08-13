# Particle Editor

A modern editor for Alamo-engine particle effects — the `.alo` particle files used by
*Star Wars: Empire at War* and *Forces of Corruption*.

A fork of [Mike.NL's GlyphXTools particle editor](https://github.com/GlyphXTools/particle-editor),
rebuilt around a new interface while keeping the original's file-format fidelity.

## Download

Grab the latest [release](../../releases), unzip it anywhere, and run
`x64\Release\ParticleEditor.exe`. The download is self-contained — the interface and the
DirectX component it needs ship inside it, so there is nothing to install separately.

**Requirements:** 64-bit Windows 10 or 11, and the Microsoft Edge WebView2 runtime —
already present on Windows 11 and on any Windows 10 with Edge. If it's missing, the editor
opens Microsoft's download page for it.

## What it does

- **Live preview** — effects render and animate in-process as you edit, with pause and
  playback.
- **Scene context** — drop real game units or structures into the preview at true in-game
  scale, with hardpoints, stencil shadows, skydomes, and the game's ambient lighting.
- **Mod-stack editing** — compose mod layers in an explicit load order and edit the stack
  in place; switching stacks re-resolves assets immediately.
- **Curve editor** — smooth key morphs, a snap-to-grid toggle, and full keyboard operation.
- **Atlas Frame Picker** — choose atlas frames visually from a grid previewed with the
  emitter's own blend mode.
- **Safety net** — autosave with crash recovery, atomic saves, real undo/redo, and
  defensive parsing of untrusted `.alo` / `.meg` / `.xml` files.

## Learning it

The [project site](https://drknickers.github.io/particle-editor/) carries a full guide:
setup, a particle-authoring primer, reference pages for blend modes and generation types,
and hands-on tutorials with recorded walkthrough clips of the editor.

## Building from source

The editor is a C++ host plus a React UI, built web-first — see
[CONTRIBUTING.md](CONTRIBUTING.md) for the toolchain and the two-step build.

## Credits & license

Based on [GlyphXTools/particle-editor](https://github.com/GlyphXTools/particle-editor) by
Mike.NL. Licensed under the [MIT License](LICENSE). Developed with assistance from Claude.

Particle Editor is an independent, unofficial tool — not affiliated with or endorsed by
Lucasfilm, Disney, Petroglyph Games, or any third-party mod or its authors. *Star Wars:
Empire at War* and *Forces of Corruption* are trademarks of their respective owners.
