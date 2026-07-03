# Introduction

> **Placeholder page.** This exists to prove the guide pipeline end-to-end — the shell,
> the sidebar, the "On this page" rail, and the Markdown → HTML prerender. Replace this file
> with real content; add more pages by dropping `.md` files into `guide-src/` and listing
> them in `nav.json`.

Particle Editor is a modern editor for `.alo` particle systems, built for Empire at War and
Forces of Corruption modders who want to author, tune, and preview effects against the scene
they actually ship in.

## What it gives you

- **Scene context** — bring in an in-game unit for scale and load a skydome for backdrop.
- **Mod-stack awareness** — edit against the same content your game setup will load.
- **Visual atlas picking** — browse texture frames as images, not filenames.
- **A live viewport** — timing, color, and alpha curves stay beside the preview.

## Quick start

Grab the latest release `.zip`, unzip it anywhere, and run the executable:

```text
x64\Release\ParticleEditor.exe
```

The required `d3dx9_43.dll` ships **bundled** next to the executable, so there is no separate
DirectX runtime to install — just unzip and run.

## Authoring this guide

Each page is a Markdown file under `site/guide-src/`. The sidebar comes from `nav.json`, so a
new page is two steps:

1. Add `site/guide-src/my-page.md`.
2. Add `{ "slug": "my-page", "title": "My Page" }` to a section's `pages` in `nav.json`.

Then run the prerender locally and commit the generated HTML:

```text
node scripts/build-guide.mjs
```

See [the source on GitHub](https://github.com/DrKnickers/particle-editor) for the editor itself.
