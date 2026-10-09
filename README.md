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

## Command-line tools

`ParticleEditor.exe --tool` reads, checks and edits an `.alo` without opening a window, so
scripts and agents can work on effects directly. It needs no game install.

```text
ParticleEditor.exe --tool inspect  <in.alo> --out <inspect.json>
ParticleEditor.exe --tool validate <in.alo> --out <report.json>
ParticleEditor.exe --tool patch    <in.alo> <request.json> --out <candidate.alo> --report <report.json>
```

`inspect` writes every value the file saves as JSON, with `sourceDocumentHash` (the
SHA-256 of the input). `validate` reports whether the file loads exactly as written.
`patch` applies a request's edits to one emitter, which it selects by that hash and the
emitter's name. It writes the candidate only after verifying it, and never overwrites the
input. The exit code says what happened: 0 ok, 2 usage, 3 invalid source, 4 the selector
did not match, 5 an edit was refused, 6 a verification failed, 13 a file could not be
written.

The exe is a Windows program, so wait for it explicitly. From Node:

```js
const { status } = spawnSync("ParticleEditor.exe", ["--tool", "inspect", "fx.alo", "--out", "fx.json"]);
```

From PowerShell (`&` does not wait for a Windows program):

```powershell
$p = Start-Process ParticleEditor.exe -ArgumentList '--tool','validate','fx.alo','--out','report.json' -Wait -PassThru
$p.ExitCode
```

The request format, every edit and the report are documented in
[docs/CAPTURE_MODES.md](docs/CAPTURE_MODES.md#window-free-cli----tool-inspect----tool-validate----tool-patch).

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
