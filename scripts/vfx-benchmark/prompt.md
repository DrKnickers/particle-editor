# Review this particle effect

You are reviewing one particle effect from *Star Wars: Empire at War: Forces of Corruption*, rendered by a particle editor that uses the game's own shaders. Decide whether the effect has a defect against its stated intent, and if it does, diagnose it and propose one correction.

## What you have

- `intent.md`: what the effect is meant to look like and when each part happens, with a *Targets* section giving the intended value of each timing and count quantity.
- `rules.md`: the categories you may use, what evidence counts, and which corrections are accepted. Read it before answering.
- `findings.schema.json`: the exact shape of your answer.
- `textures.json`: the texture names a texture correction may use.
- The observation: `frames/` (selected frames, PNG, named by simulation frame), `filmstrip.png` with its tile map `filmstrip.json`, `selected.png` with its tile map `selected.json`, `summary.json` (the effect's lifecycle and each emitter's first, peak and last active frames) and `frames.jsonl` (one line per simulated frame: live particles in total and per emitter).
- `inspect.json`, when present: every value the effect file stores, per emitter.

Emitters are named by number and name. In a pack without `inspect.json`, the numbers only name the emitters: they need not follow the order in which the emitters are drawn.

Some packs hold three observations of the same unchanged file under different random seeds, in `bundle-a/`, `bundle-b/` and `bundle-c/`. There, a defect is a bundle that misbehaves against the intent; name it with `bundleId`, and use the seed rule in `rules.md` (moving a value toward its target does not apply when the file itself is unchanged).

## What to do

1. Compare what the effect does with what `intent.md` says it should do. Use the frames, the telemetry and, when present, the stored values.
2. If nothing is wrong, answer `{ "findings": [] }`. Many effects have no defect, and that answer is scored like any other.
3. If something is wrong, write one finding per defect: the category and direction from `rules.md`, the emitters involved (index and name), the frames or values that show it, one correction that follows `rules.md`, a severity and your confidence from 0 to 1.

Write only your answer file, as JSON that matches `findings.schema.json`.
