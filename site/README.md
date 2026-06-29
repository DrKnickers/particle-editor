# Particle Editor — landing page

A static, single-page landing site for the public mirror (`DrKnickers/particle-editor`).
Plain HTML/CSS/JS — no build step, no framework. Decoupled from the source-sync pipeline
(it is **not** in `tasks/public-manifest.txt`).

## Local preview

Serve the folder over HTTP (a `file://` open also works, but a server matches production):

```bash
# from the repo root
node web/apps/editor/tests-site/serve.mjs   # serves site/ on http://localhost:5175
```

By default the page loads posters/clips from the `site-media` release URLs. To preview with
**local placeholder media**, drop files into `site/media-local/` (gitignored) named
`hero.mp4`, `hero-poster.jpg`, `faithful.mp4`, `faithful-poster.jpg`, `preview-poster.jpg`,
`workspace-poster.jpg`, then point the page at them with the **`?media=` query parameter**:

```
http://localhost:5175/?media=media-local/
```

`main.js` reads the media base in this order: `window.__MEDIA_BASE__` (set by the smoke) →
the `?media=` query param → the release URL. The repo's smoke
(`pnpm --filter @particle-editor/editor test:site`) generates the placeholders with ffmpeg.

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
2. Install `site/deploy/pages.yml` as `.github/workflows/pages.yml` on the fork.
3. Enable Pages (source: GitHub Actions).
4. Confirm `MEDIA_BASE` in `main.js` points at the release and the OG image URL resolves.
5. Run the rollout-gate URL check (every `site-media` URL returns 200) before the deploy.
6. Manual iOS Safari smoke (Chromium can't cover iOS `playsinline` / Low-Power-Mode).

Default URL: `https://drknickers.github.io/particle-editor/`.
