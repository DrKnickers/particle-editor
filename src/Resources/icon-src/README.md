# App icon source

Generates `../logo.ico` (the window / taskbar / exe icon).

The mark is an **orbital** — a glowing azure core with a tilted orbit and
particles (an emitter/atom) — in the site's azure accent (`#4a8bff`, see
`site/styles.css`). Flat with consistent single-light shading; transparent;
no plate; no bloom.

**Source of truth is `build.js`** — the shape and shading are defined in code.
Everything else here (including `logo.svg`) is a generated output, not an input.

## Files
- `build.js` — the generator. Defines the geometry/shading and writes every
  output: each PNG size, the packed `logo.ico`, the 256px `logo.svg`,
  `master_1024.png`, and a `preview_sheet.png`. Encodes the size ramp:
  full orbit + 3 particles at ≥48px, a thicker orbit + 2 particles at 32px,
  and a tilted orbit **line** + 2 particles through the core at 16px (a curved
  ring can't survive that small — the line keeps it reading as an orbit).
- `logo.svg` — a 256px master vector **generated** by `build.js` (a handy
  standalone export; editing it has no effect — change `build.js` instead).
- `master_1024.png` — large render for the landing page / README / store art.

## Regenerate
```
npm i @resvg/resvg-js      # rasterizer (ABI-stable prebuilt; no native build)
node build.js              # writes ./out/* including logo.ico
cp out/logo.ico ../logo.ico
```
`logo.ico` packs 16/32/48/64/128/256, each a PNG-compressed entry (Windows
Vista+; this app is Win11 x64-only). Tune colour/geometry via the constants and
the `large()` / `HINTED` configs at the top of `build.js`.
