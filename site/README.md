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
WIP referenced `hero.mp4` / `hero-poster.jpg`, `faith.mp4` / `faith-poster.jpg`,
`preview-poster.jpg`, and `workspace-poster.jpg`; those binaries remain out of git.
