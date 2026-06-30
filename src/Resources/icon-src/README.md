# App icon source

Generates `../logo.ico` (the window / taskbar / exe icon) plus the site
favicons.

The app icon is a Windows 11-style rounded tile: a dark editor-native material
plate with a no-handle spline, endpoint nodes, and an azure particle in the
site's accent (`#4a8bff`, see `site/styles.css`). The standalone mark exports
drop the tile and keep only the transparent spline + particle.

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

`logo.ico` and `site/favicon.ico` pack 16/32/48/64/128/256, each a
PNG-compressed entry (Windows Vista+; this app is Win11 x64-only). Tune
colour/geometry via the constants and drawing helpers in `build.py`.
