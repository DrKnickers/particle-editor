# Draft Tutorial: Recolor and Orient a Shield Impact

This tutorial overrides `P_SHIELD_BLAST_LARGE00.ALO`, recolors it into an obvious purple variant,
and uses the editor preview to study impact orientation.

Shield impacts are directional effects. They need to read as something striking a surface, with
shape and facing that support the color change.

<!-- Media: tutorial-04-opening-result | still -->

## Outcome

By the end of this page, your tutorial mod will contain this loose override:

```text
Data\Art\Models\P_SHIELD_BLAST_LARGE00.ALO
```

In the editor preview, the shield impact will read as a clear purple hit effect. You will also know
how to inspect the effect's orientation so the visual suggests a surface being struck.

## What this teaches

- Opening another shipped particle as a loose override.
- Finding the visible impact emitters in the Emitter Tree.
- Recoloring an energy impact while preserving its hit shape.
- Using the Preview Viewport to inspect direction and surface readability.
- Judging an impact by shape, timing, and orientation instead of color alone.

## Before You Start

Extract `P_SHIELD_BLAST_LARGE00.ALO` from the game's `.meg` archives and place a copy in the
tutorial mod folder:

```text
corruption\Mods\ParticleTutorial\Data\Art\Models\P_SHIELD_BLAST_LARGE00.ALO
```

This tutorial edits the override copy. Keep the filename and path the same so the game can still
use it as an override if you test it later.

The shield blast ships as a family — `P_SHIELD_BLAST_SMALL00`, `_MED00`, and `_LARGE00` — one per
impact size class. This tutorial edits only the large one; a real mod would usually re-apply the
same recolor to all three so every hit matches.

## 1. Open the Override Copy

Open the tutorial mod copy:

```text
ParticleTutorial\Data\Art\Models\P_SHIELD_BLAST_LARGE00.ALO
```

Use the Preview Viewport to watch the impact play a few times before changing anything. Look for the
main flash, any ring or splash shape, and any fading glow after the hit.

## 2. Identify the Impact Emitters

This particle has two emitters: `ripple` and `splash`. Select them one at a time in the Emitter
Tree and watch what changes in the Preview Viewport — this is the select-and-observe habit from
Tutorial 1, now on a particle where it earns its keep. Work out which emitter makes the broad
shield-face ripple and which makes the splash of energy at the hit point before changing
anything.

Then dig into how each one is built, because the two emitters answer "how do I make an effect
feel attached to a surface?" in two different ways:

- **`ripple`** is a flat expanding ring *on* the shield face. Check its Appearance tab: the
  world-oriented option is on, so the flat square carrying the ring stays lying against the
  surface instead of turning to face the camera. Its Initial position is `Exact (0, 0, 0.02)`:
  two hundredths of a unit *above* the surface, just enough that the ring never clips into the
  shield it sits on. It never moves — its Scale track does all the work, expanding from `2` to
  `25` across a 0.3-second life. And its Index track holds a single atlas frame (a ring shape)
  rather than the default glow.
- **`splash`** is the energy kicked back off the surface. Ten particles, born in a tight
  0.4-radius sphere at the hit point, launched along **positive Z** — its Initial speed is a Box
  reaching from `(0,0,0)` to `(0,0,20)`, i.e. straight up off the surface at up to 20 units/s.
  A positive **Inward speed** of `5` aims each particle slightly back toward the center at birth,
  tightening the spray into a fountain rather than a straight jet. It changes only the launch
  direction — it does not keep pulling as the particle flies. These are camera-facing (the
  default), because sparks of energy should read from any angle.

One structural detail worth noticing: both emitters set `Minimum lifetime` to 100% — *no*
lifetime randomness. An impact is a single crisp event; every particle arriving and dying on
schedule is what makes it feel synchronized to the hit. Compare that with the smoke tutorials,
where lifetime randomness is exactly what you want. Randomization is a choice, not a default.

The useful habit is to connect each selected emitter to one visible part of the impact. On
particles with many emitters, toggling an emitter's visibility in the tree is another quick way
to isolate its contribution.

## 3. Recolor the Impact Purple

With `ripple` and `splash` selected one at a time, use the Curve Editor's color tracks to shift
the color toward purple (the mechanics are in [Curve Editor Basics](curve-editor-basics)). Both
emitters are energy effects, so their additive blending means brighter values read as more light
— see [Blend Modes](blend-modes).

Read the stock color first: both emitters are a blue-teal built from Green and Blue (the ripple
starts around Green `0.5`, Blue `0.75`) with Red left low, and every channel falls to `0` by the
end of the life — an additive effect fades out by dimming to black, since alpha does nothing
here. Recoloring to purple is therefore a *channel-balance* change: raise Red, cut Green, keep
Blue.

A simple starting target (start values; every channel still ends at `0`):

| Parameter | Value |
|---|---|
| Red | ~0.6 (raised from ~0 — this is what turns the blue into purple) |
| Green | ~0.1 (cut from ~0.5) |
| Blue | ~0.75 (kept — the stock blue anchor) |
| End | all channels to 0 — keep the dim-to-black ending intact |

Because these emitters are additive, only the color tracks matter — the Alpha track has no effect,
and brightness *is* the color. Keep the `ripple` ring a touch brighter (try Red `~0.7`) so the
core of the impact reads as energy, and let `splash` sit slightly dimmer (Red `~0.5`) as the softer
outer kick. If everything becomes the same flat purple, widen that brightness gap between the two.

## 4. Preserve the Impact Shape

After recoloring, watch the effect in motion. A shield impact should still feel like a hit: quick
arrival, strong center, expanding or fading energy, then a clean disappearance.

Use these questions while you tune:

- Does the effect still read as an impact?
- Is the first flash easy to see?
- Does the outer glow support the hit instead of covering it?
- Does the effect fade out cleanly?

If the recolor makes the shape harder to read, dim the outer glow's color or shorten the lingering
part of the effect.

## 5. Inspect the Orientation

Use the Preview Viewport camera to look at the effect from different angles. A shield impact is not
just a ball of light. It should imply a plane or surface being struck.

Look for the direction of the bright face, ring, or splash. From the front, the impact should read
as a hit on a shield surface. From the side, you should be able to tell whether the effect is too
flat, too thick, or misaligned for the context you imagine.

### Note: Link Particles to the Impact Instance

For impact-style particles, you can check `Link particles to instance` in the Basic tab's
Connection section on emitters that should ride with the impact instance as it moves. This
checkbox makes already-spawned particles move with the effect instance's position; it does not
rotate them, and it is separate from the `Parent speed inherit` control used in Tutorial 2. The
effect's *orientation* comes from how it is authored and placed, covered next — not from this
checkbox.

When you study a shipped directional effect, check which axis its important motion points along —
select each emitter and read its Initial speed values in the Physics tab. In this particle the
answer is unambiguous: the splash launches along **positive Z** (its speed Box reaches from zero
to `+20` on Z only), and the ripple lies flat in the X/Y plane facing the same +Z direction. So
the effect's "outward" is authored as +Z. Author your own directional effects along +Z to match
the stock ones. The preview can't show how the game aligns that axis onto a struck surface, so
test each new directional effect in game before relying on it.

## 6. Final Preview Check

Watch the finished particle at a normal preview distance. The goal is a clear purple shield hit, not
the loudest possible purple glow.

Use this final check:

- Purple is obvious.
- The center still reads as the hit point.
- The outer glow supports the strike.
- The effect has a readable facing direction.
- The impact disappears without a distracting pop.

## Takeaways

Impact particles need shape and direction as much as color. Recoloring can prove the edit, but
orientation is what makes the effect feel attached to a surface in the game.
