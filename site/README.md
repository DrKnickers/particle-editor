# Particle Editor — landing page + guide

A static site for the public mirror (`DrKnickers/particle-editor`): the landing page at the
root plus a user guide under `guide/`. Plain HTML/CSS/JS — no framework, no CI build step
(the guide's HTML is prerendered locally and committed). Decoupled from the source-sync
pipeline (it is **not** in `tasks/public-manifest.txt`). Fonts are self-hosted in `fonts/`
(latin woff2 subsets of Schibsted Grotesk + IBM Plex Mono, preloaded from each page head —
no third-party requests from the shipped pages).

## Guide (Markdown → committed HTML)

Guide pages are authored as Markdown in `guide-src/` and prerendered into `guide/*.html` by
a zero-dependency script (Node built-ins only — no npm install):

```bash
# from the repo root
node scripts/build-guide.mjs           # render guide-src/*.md -> site/guide/*.html
node scripts/build-guide.mjs --check   # exit 1 if committed HTML is stale vs the Markdown
node scripts/guide-edit.mjs            # WYSIWYG editing session (see below)
```

For prose editing, `scripts/guide-edit.mjs` serves a side-by-side editor at
`http://localhost:4178/` — a rich WYSIWYG pane (Toast UI, loaded from its CDN) on the left and
the *real* built page on the right. Saves write the Markdown, re-run the real build, and a
rendered-diff guard in the header shows exactly what changed in the shipped HTML vs the last
commit, so an editor round-trip can never silently alter a page. Media anchors appear as locked
chips; a Markdown-mode toggle sits at the bottom-left for raw edits. Dev-only — not part of any
build or gate. Pages marked `"publish": false` still receive a preview-only render in this editor;
that render is served from memory and never written into `site/guide/`.

Adding a page = drop `guide-src/<slug>.md`, list it in `guide-src/nav.json` (which drives
the sidebar, the per-page kicker, and the prev/next pager), rebuild, and **commit both the
Markdown and the generated HTML** — deploys ship the committed HTML as-is; nothing builds
it in CI (by design). Set a nav entry to `"publish": false` to retain a validated Markdown
draft without generating its HTML route or exposing it in the sidebar, pager, or public link
resolver. A normal build removes stale generated HTML for unpublished or deleted entries.
Run `--check` before review to catch a forgotten rebuild or an orphaned page. The renderer
covers a documented Markdown subset (headings, lists, fenced code, blockquotes, links/images,
pipe tables, raw HTML blocks). Internal page links are written as bare lowercase page slugs
(`[Setup](setup)`) — the build resolves published pages against the published nav set and
**fails on an unknown or unpublished target, or a leftover wiki-style page ref** (external
`http(s)`/`mailto`, `#anchors`, and explicit `./`/`../` paths pass through unchanged), so
broken cross-references can't ship. Hidden `<!-- Media: … -->` comments in tutorial pages
are anchors for the clip workstream — preserve them while editing prose. `GUIDE_MEDIA` in
`tests-site/guide.spec.ts` pins the exact ordered filenames each public page renders. Leaving
manifest items in place for an unpublished draft is intentional: the media pipeline can keep
rendering work-in-progress material without exposing the tutorial on the site.
The `<!-- Media: id | still -->` modifier renders a **clip** item as its poster frame instead of
the video, for a page that should keep a visual without shipping the recording. Prefer it over
retyping the item as `kind: "image"` in the manifest: the manifest describes what the pipeline
produces, and `runImageExport` requires a `.png` output while a clip's poster is a `.jpg`.

## Local preview

Serve the folder over HTTP (a `file://` open also works, but a server matches production):

```bash
# from the repo root
node web/apps/editor/tests-site/serve.mjs   # serves site/ on http://localhost:5175
```

By default the page loads posters/clips from the `site-media` release URLs. To preview with
**local placeholder media**, drop files into `site/media-local/` (gitignored) — the page
references `hero.mp4`, `faith.mp4`, `f02.mp4` (atlas picker), `f02-reorder.mp4` (F2 emitter
reorder), and `f04.mp4` (F4 mod stack), each with a matching `*-poster.jpg`, plus the paired
`interface-theme-dark.jpg` and `interface-theme-light.jpg` stills — then point the page at
them with the **`?media=` query parameter**:

```
http://localhost:5175/?media=/media-local/
```

Both `main.js` (landing clips) and `guide-media.js` (guide tutorial clips) read the media base
in the same order: `window.__MEDIA_BASE__` (set by the smoke) → the `?media=` query param → the
site's own `media/` directory (the Pages workflow mirrors the `site-media` release into it at
deploy time — the release download URLs themselves serve `Content-Disposition: attachment`,
which browsers refuse to play as `<video>` sources) — so the root-relative
`?media=/media-local/` preview also drives guide media (the guide references
each clip/still by its `tutorial-XX-…` manifest id). The repo's smoke
(`pnpm --filter @particle-editor/editor test:site`) generates the landing placeholders with ffmpeg.

## Media (HARD RULE)

**No clip or poster binary ever enters the git tree.** They live on a `site-media` GitHub
Release and are referenced by URL. `media-local/` and `*.mp4 *.jpg *.jpeg *.png *.webp *.gif`
are ignored by the nested `site/.gitignore`; the smoke's `git ls-files` guard is the
cross-platform enforcement layer.

## Deploy (at rollout — gated)

GitHub Pages cannot serve a `/site` subfolder, so deployment uses a GitHub Actions Pages
workflow that stages `site/` (minus the dev-only `deploy/`, `README.md`, `.gitignore`, and
`media-local/`) and uploads it as the Pages artifact. The reference copy is `site/deploy/pages.yml`.

At rollout (user-gated, on the fork):
1. Create the fork and the `site-media` release; upload the clips + posters.
2. The Pages workflow lives at `.github/workflows/pages.yml` (deploys `site/` on every master push that touches it).
3. Enable Pages (source: GitHub Actions).
4. Confirm `MEDIA_BASE` in **both** `main.js` and `guide-media.js` defaults to the site's `media/` mirror (never the release download URL), and the OG image URL resolves.
5. Run the rollout-gate URL check (every `site-media` URL returns 200 — landing clips AND the guide's `tutorial-XX-…` media) before the deploy.
6. Manual iOS Safari smoke (Chromium can't cover iOS `playsinline` / Low-Power-Mode).

Default URL: `https://drknickers.github.io/particle-editor/`.

