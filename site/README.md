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
build or gate.

Adding a page = drop `guide-src/<slug>.md`, list it in `guide-src/nav.json` (which drives
the sidebar, the per-page kicker, and the prev/next pager), rebuild, and **commit both the
Markdown and the generated HTML** — deploys ship the committed HTML as-is; nothing builds
it in CI (by design). Run `--check` before review to catch a forgotten rebuild or an
orphaned page. The renderer covers a documented Markdown subset (headings, lists, fenced
code, blockquotes, links/images, pipe tables, raw HTML blocks). Internal page links are
written as bare lowercase page slugs (`[Setup](setup)`) — the build resolves them against
`nav.json` and **fails on an unknown slug or a leftover wiki-style page ref** (external
`http(s)`/`mailto`, `#anchors`, and explicit `./`/`../` paths pass through unchanged), so
broken cross-references can't ship. Hidden `<!-- Media: … -->` comments in tutorial pages
are anchors for the clip workstream — preserve them.

## Local preview

Serve the folder over HTTP (a `file://` open also works, but a server matches production):

```bash
# from the repo root
node web/apps/editor/tests-site/serve.mjs   # serves site/ on http://localhost:5175
```

By default the page loads posters/clips from the `site-media` release URLs. To preview with
**local placeholder media**, drop files into `site/media-local/` (gitignored) — the page
references `hero.mp4`, `faith.mp4`, `f02.mp4` (atlas picker), `f02-reorder.mp4` (F2 emitter
reorder), and `f04.mp4` (F4 mod stack), each with a matching `*-poster.jpg` — then point the
page at them with the **`?media=` query parameter**:

```
http://localhost:5175/?media=media-local/
```

Both `main.js` (landing clips) and `guide-media.js` (guide tutorial clips) read the media base
in the same order: `window.__MEDIA_BASE__` (set by the smoke) → the `?media=` query param → the
release URL — so the `?media=media-local/` preview also drives guide media (the guide references
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
2. Install `site/deploy/pages.yml` as `.github/workflows/pages.yml` on the fork.
3. Enable Pages (source: GitHub Actions).
4. Confirm `MEDIA_BASE` in **both** `main.js` and `guide-media.js` points at the release, and the OG image URL resolves.
5. Run the rollout-gate URL check (every `site-media` URL returns 200 — landing clips AND the guide's `tutorial-XX-…` media) before the deploy.
6. Manual iOS Safari smoke (Chromium can't cover iOS `playsinline` / Low-Power-Mode).

Default URL: `https://drknickers.github.io/particle-editor/`.

## Temporary VPS demo (2026-06-29)

There is a friend-facing WIP demo on a standalone VPS vhost:

```
https://particle.example.com/
```

This is separate from Plex and from the planned GitHub Pages rollout. Caddy serves
static files directly from:

```
/var/www/particle-editor-demo
```

The Caddy block is intentionally simple:

```caddy
particle.example.com {
    root * /var/www/particle-editor-demo
    encode zstd gzip
    file_server
}
```

**⚠ `main.js` / `guide-media.js` deploy trap (lesson):** the VPS copies of `main.js`
**and `guide-media.js`** carry a local-only patch — their `MEDIA_BASE` fallback is
`"media-local/"`, while the repo copies fall back to the `site-media` release URL. Both files
share the identical fallback line, so any deploy that uploads either must re-apply the patch to
**both** and verify, or the demo's clips silently break (`main.js` = landing clips,
`guide-media.js` = guide tutorial clips):

```powershell
ssh -i $env:USERPROFILE\.ssh\particle-demo-site-key particledeploy@66.163.126.124 `
  "cd /var/www/particle-editor-demo && sed -i 's#override ?? \"https://github.com/DrKnickers/particle-editor/releases/download/site-media/\"#override ?? \"media-local/\"#' main.js guide-media.js"
curl.exe -sL https://particle.example.com/main.js | Select-String media-local/
curl.exe -sL https://particle.example.com/guide-media.js | Select-String media-local/
```

Normal content updates do **not** need sudo or Caddy changes. This Windows machine has
a site-only SSH key at:

```
<path>
```

It logs in as `particledeploy@66.163.126.124`. That VPS user owns only
`/var/www/particle-editor-demo` and has no sudo. A verified access check was:

```powershell
ssh -i $env:USERPROFILE\.ssh\particle-demo-site-key particledeploy@66.163.126.124 `
  "whoami && test -w /var/www/particle-editor-demo && echo writable && sudo -n true 2>/dev/null || echo no-sudo"
```

Expected output includes `particledeploy`, `writable`, and `no-sudo`.

To refresh the whole demo from a prepared local staging directory:

```powershell
$stage = "$env:TEMP\particle-editor-vps-demo"
$tar = "$env:TEMP\particle-editor-vps-demo.tar.gz"
if (Test-Path $tar) { Remove-Item $tar -Force }
tar -czf $tar -C $stage .
scp -i $env:USERPROFILE\.ssh\particle-demo-site-key $tar `
  particledeploy@66.163.126.124:/tmp/particle-editor-vps-demo.tar.gz
ssh -i $env:USERPROFILE\.ssh\particle-demo-site-key particledeploy@66.163.126.124 `
  "set -e; cd /var/www/particle-editor-demo; find . -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +; tar -xzf /tmp/particle-editor-vps-demo.tar.gz -C ."
```

Then verify:

```powershell
curl.exe -L --max-time 30 -o NUL -w "root=%{http_code} bytes=%{size_download}`n" https://particle.example.com/
curl.exe -L --max-time 30 -o NUL -w "hero=%{http_code} bytes=%{size_download}`n" https://particle.example.com/media-local/hero.mp4
curl.exe -L --max-time 30 -o NUL -w "faith=%{http_code} bytes=%{size_download}`n" https://particle.example.com/media-local/faith.mp4
```

### Update a single clip in place (targeted — no full refresh)

The full-refresh recipe above **wipes** `media-local/` (`rm -rf`), so it needs *every*
clip staged locally. To push just one re-rendered clip (e.g. after re-shooting `hero`)
without disturbing the others, copy the new file(s) to a `.tmp` name and then **atomically
`mv`** into place — so Caddy never serves a half-written file mid-upload:

```powershell
$key  = "$env:USERPROFILE\.ssh\particle-demo-site-key"
$dest = "/var/www/particle-editor-demo/media-local"
scp -i $key site/media-local/hero.mp4        "particledeploy@66.163.126.124:$dest/hero.mp4.tmp"
scp -i $key site/media-local/hero-poster.jpg "particledeploy@66.163.126.124:$dest/hero-poster.jpg.tmp"
ssh -i $key particledeploy@66.163.126.124 `
  "set -e; cd $dest; mv -f hero.mp4.tmp hero.mp4; mv -f hero-poster.jpg.tmp hero-poster.jpg"
```

Verify the served bytes match the new local file (size is a quick discriminator; an `md5`
compare is definitive), and confirm an untouched clip is unchanged:

```powershell
curl.exe -L --max-time 40 -o NUL -w "hero=%{http_code} bytes=%{size_download}`n"  https://particle.example.com/media-local/hero.mp4
curl.exe -L --max-time 40 -o NUL -w "faith=%{http_code} bytes=%{size_download}`n" https://particle.example.com/media-local/faith.mp4
```

Initial deployment note: the uploaded demo was captured from the running local preview
at `http://localhost:5175/?media=media-local/`, not from committed media. The served
WIP referenced `hero.mp4`, `faith.mp4`, `f02.mp4`, `f02-reorder.mp4`, and `f04.mp4` (each
with its `*-poster.jpg`); those binaries remain out of git. (The F2/F4 cards were stills
— `preview-poster.jpg` / `workspace-poster.jpg` — until the drag clips landed in #445.)

### Backup — re-pull to OneDrive after content changes

The VPS docroot is **expendable / redeployable**; the authoritative backup of the served
`media-local/` binaries (which are out of git) lives on **this Windows box under OneDrive**
(established 2026-07-13, Phase 0 — see DrKnickers/vps-infra registry gap #1). Because every
deploy recipe above overwrites the VPS copy, **re-pull after any content change** so the
backup stays current:

```powershell
$key = "$env:USERPROFILE\.ssh\particle-demo-site-key"
$dst = "$env:USERPROFILE\OneDrive\Backups\particle-editor-demo"
New-Item -ItemType Directory -Force -Path $dst | Out-Null
scp -r -i $key particledeploy@66.163.126.124:/var/www/particle-editor-demo/* "$dst\"
```

Then confirm OneDrive has synced (folder shows the "up to date" check) and optionally
spot-check a file's size against the VPS. The Phase 0 true-up pulled 120 files / 77 MB and
was checksum + restore-test verified. Runs as the sudo-less `particledeploy` user (same
site-only key as deploys) — no root, no Caddy interaction. A full restore = this backup
back to the docroot via the refresh recipe above, then re-apply the `MEDIA_BASE`
patch.
