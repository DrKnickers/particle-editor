# Tutorial 4: Recolor and Orient a Shield Impact

This tutorial overrides `P_SHIELD_BLAST_LARGE00.ALO`, recolors it into an obvious purple variant,
and uses the editor preview to study impact orientation.

Shield impacts are directional effects. They need to read as something striking a surface, with
shape and facing that support the color change.

<!-- Media: tutorial-04-opening-result -->

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

## 1. Open the Override Copy

Open the tutorial mod copy:

```text
ParticleTutorial\Data\Art\Models\P_SHIELD_BLAST_LARGE00.ALO
```

Use the Preview Viewport to watch the impact play a few times before changing anything. Look for the
main flash, any ring or splash shape, and any fading glow after the hit.

<!-- Media: tutorial-04-open-override -->

## 2. Identify the Impact Emitters

Select emitters one at a time in the Emitter Tree and watch what changes in the Preview Viewport.
For this lesson, focus on the emitters that make the visible shield-hit read:

- the bright center or first flash;
- the broader energy ring or splash;
- any fading glow that lingers after the strike.

Start with the emitters you can identify clearly. The useful habit is to connect each selected
emitter to one visible part of the impact.

## 3. Recolor the Impact Purple

With the visible impact emitters selected one at a time, use the Property Panel or Curve Editor to
shift the color toward purple.

A simple target is:

```text
Red:   high
Green: low
Blue:  high
Alpha: keep readable
```

Keep the center of the impact bright enough to read as energy. If everything becomes the same flat
purple, give the core a little more brightness and let the outer glow be softer.

<!-- Media: tutorial-04-recolor-purple -->

## 4. Preserve the Impact Shape

After recoloring, watch the effect in motion. A shield impact should still feel like a hit: quick
arrival, strong center, expanding or fading energy, then a clean disappearance.

Use these questions while you tune:

- Does the effect still read as an impact?
- Is the first flash easy to see?
- Does the outer glow support the hit instead of covering it?
- Does the effect fade out cleanly?

If the recolor makes the shape harder to read, reduce the outer glow alpha or shorten the lingering
part of the effect.

## 5. Inspect the Orientation

Use the Preview Viewport camera to look at the effect from different angles. A shield impact is not
just a ball of light. It should imply a plane or surface being struck.

Look for the direction of the bright face, ring, or splash. From the front, the impact should read
as a hit on a shield surface. From the side, you should be able to tell whether the effect is too
flat, too thick, or misaligned for the context you imagine.

### Note: Link Particles to the Impact Instance

For impact-style particles, select emitters that should follow the impact orientation and check
`Link particles to instance` in the Basic tab's Connection section. This is separate from the
`Parent speed inherit:` control used in Tutorial 3.

A useful authoring convention is to make the particle's important motion point along positive Z. For
a shield hit, that means the effect can be oriented to move outward from the impact, back toward the
direction the projectile came from before it struck the shield.

<!-- Media: tutorial-04-orient-preview -->

## 6. Final Preview Check

Watch the finished particle at a normal preview distance. The goal is a clear purple shield hit, not
the loudest possible purple glow.

Use this final check:

- Purple is obvious.
- The center still reads as the hit point.
- The outer glow supports the strike.
- The effect has a readable facing direction.
- The impact disappears without a distracting pop.

<!-- Media: tutorial-04-final-preview -->

## Takeaways

Impact particles need shape and direction as much as color. Recoloring can prove the edit, but
orientation is what makes the effect feel attached to a surface in the game.
