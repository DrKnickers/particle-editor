# App icon source

Generates `../logo.ico` (the window / taskbar / exe icon) plus the site
favicons.

The app icon is a Windows 11-style rounded tile: a dark editor-native material
plate with a no-handle spline, endpoint nodes, and a taskbar-weighted azure
particle derived from the site's accent (`#4a8bff`, see `site/styles.css`).
The standalone mark exports drop the tile and keep only the transparent
spline + particle.

Sizes ≤ 40px use a legibility ramp (`RAMP` in `build.py`): the glow, highlight,
and endpoint rings are dropped (the highlight/ring strokes are sub-pixel at
those sizes; the glow only muddies the downscale), the tube and particle are
fattened on a taper, and the plate is lifted brighter so the tile silhouette
reads against Win11 dark taskbar/titlebar chrome.

**Source of truth is `build.py`** -- the shape and shading are defined in code.
`build.js` is a compatibility wrapper for the old command. Everything else here
(including `logo.svg`) is a generated output, not an input.

## Files

- `build.py` -- the generator. Defines the geometry/shading and writes every
  output: each PNG size, the packed app/site ICOs, the tile-backed
  `logo.svg`, `site/favicon.svg`, transparent `mark.svg`, `master_1024.png`,
  `mark_1024.png`, and a `preview_sheet.png`.
- `build.js` -- compatibility wrapper that runs `build.py`.
- `logo.svg` -- a 256px master vector **generated** by `build.py` (a handy
  standalone export; editing it has no effect -- change `build.py` instead).
- `mark.svg` -- transparent standalone mark for non-icon brand use.
- `master_1024.png` -- large render for the landing page / README / store art.
- `mark_1024.png` -- transparent standalone mark render.

## Regenerate

```powershell
python build.py
# or: node build.js
```

`logo.ico` and `site/favicon.ico` pack 16/20/24/32/40/48/64/128/256, each a
PNG-compressed entry (Windows Vista+; this app is Win11 x64-only). 20/24/40
give the common taskbar (24 @ 100% DPI) and titlebar (16/20/24 across DPI)
cases exact entries, so the shell doesn't have to resample 16/32 on those
surfaces. Tune colour/geometry via the constants and drawing helpers in
`build.py`.
