# Draft Tutorial: Polish Hardpoint Damage Smoke

This tutorial starts from the obvious green proof edit and turns it into believable dark hardpoint
damage smoke with a subtle warm tint near the source.

Keep the edit easy to understand while moving from proof-of-loading toward a usable game effect.

<!-- Media: tutorial-02-opening-result -->

## Outcome

By the end of this page, the hardpoint damage smoke will read as dark gray smoke with a little warm
light near the root of the effect. This tutorial stays in the editor preview; Tutorial 1
already proved the override path in game.

## What this teaches

- Turning an exaggerated proof edit into a believable effect.
- Using color to imply heat and lighting.
- Using alpha to fade smoke out cleanly.
- Using scale to make smoke disperse over its lifetime.
- Using the randomization parameters — Minimum lifetime, Minimum scale, Rotation variance — so the
  puffs stop looking like copies of one particle.
- Judging an effect by first principles rather than exact numeric recipes.

## Before You Start

Start with the `P_HP_IMPERIAL_DAMAGE.ALO` loose override from Tutorial 1:

```text
ParticleTutorial\Data\Art\Models\P_HP_IMPERIAL_DAMAGE.ALO
```

Open it in the editor. The smoke may still be bright green from the proof edit.

## 1. Return the Smoke to a Dark Base Color

Select the `smoke` emitter in the Emitter Tree. In the Curve Editor, bring the color tracks back
toward a dark gray — same mechanics as Tutorial 1's green edit: focus a channel, select its keys,
step the value (see [Curve Editor Basics](curve-editor-basics) if you need the controls).

A useful target is not pure black. Smoke still needs to catch some light, especially near the damage
source — and the stock file itself tells you where "believable" lives: before Tutorial 1's green
edit, all three of its color channels converged to a dark neutral `0.255` by the end of the life.
Think in terms of a dark neutral base:

| Parameter | Value |
|---|---|
| Red | ~0.28 (a hair warmer than the others) |
| Green | ~0.25 |
| Blue | ~0.22 |
| End | all three converging to ~0.25 — smoke cools into one gray |

(These are dark-gray targets — well below the stock's bright `0.5–1.0` start. The exact numbers
are not sacred; keep them low and close together.)

The convergence matters as much as the darkness: early in the life the channels can differ (that
is the next section's warm tint), but by death they should meet at the same value, because aged
smoke has no color of its own.

Watch the Preview Viewport while you adjust. If the smoke disappears, it is probably too dark, too
transparent, or both.

## 2. Add a Warm Tint Near the Source

Hardpoint damage usually implies heat, sparks, or fire somewhere inside the damaged part. Fake that
lighting by making the early smoke slightly warmer, then letting it cool as it ages.

In the Curve Editor, adjust the color near the start of the smoke lifetime so it has a small red or
orange bias — at the `0%` key push Red up to about `0.35` while keeping Green and Blue near `0.22`.
Keep the later part of the lifetime closer to neutral gray, letting all three channels meet around
`0.25` by the midpoint.

This does not need to become flame. A subtle warm tint is enough to connect the smoke to the damage
source.

## 3. Fade the Smoke with Alpha

Use the Alpha track to make the smoke fade out — this is the worked example from
[Curve Editor Basics](curve-editor-basics): focus Alpha, select the key at the right edge, bring
its value to zero. But look at the stock file's own Alpha curve before writing yours, because it
does something the naive version misses — it fades **in** as well as out:

| Point in life | Alpha | Meaning |
|---|---|---|
| 0% | 0 | invisible at birth |
| ~10% | ~0.58 | peak — fully faded in; this is the stock file's own peak value |
| 100% | 0 | fully dispersed |

Both ends matter, for different reasons. Smoke that fades out feels like it disperses; smoke that
simply stops feels switched off. And smoke that starts at full opacity *pops* into existence —
the quick ramp over the first tenth of the life is what makes each puff appear softly. Fast in,
slow out is the standard envelope for almost anything soft, and you will meet it again on the
explosion's smoke in [Tutorial 3](05-build-an-explosion).

## 4. Let the Smoke Grow as It Fades

Use the Scale track to make the smoke larger over its lifetime. This helps sell the idea that hot
smoke is spreading away from the damage source.

The shape does not need to be dramatic — the stock emitter grows from `25` to `40` across its
whole eight-second life, a gentle 60% swell:

| Point in life | Scale |
|---|---|
| Start | compact |
| Middle | wider |
| End | widest, but fading out |

Preview the color, alpha, and scale together. These controls work as a group: darker smoke may need
more alpha, larger smoke may need a softer fade, and a warmer start color may need a shorter visible
duration.

## 5. Break the Uniformity

Play the effect and look closely: if every puff of smoke is the same size, lives the same length,
and holds the same angle, the effect reads as *copies of one particle* — and real smoke never
does. The fix is not more emitters. The stock file already demonstrates it — check its spinners
and you will find every one of these in use:

- **Minimum lifetime** (Basic tab, in the Generation section under **Maximum lifetime**). The
  maximum is the lifetime you designed; the minimum, given as a percentage of it, lets each
  particle live a random span in between. The stock smoke sets it all the way down to `18%` of
  its eight-second maximum — some puffs are gone in under two seconds while others drift for
  eight, which is most of why the plume looks alive.
- **Minimum scale** (Appearance tab, Textures section). The same idea for size: each particle's
  whole Scale curve is multiplied by a random factor between this percentage and 100%. The stock
  smoke uses `75%` — family-resembling puffs, no two identical.
- **Random rotation direction** (Appearance tab, Rotation section) plus a small value on the
  **Rotation** track (try around `0.2`, easing toward `0`). Each particle spins slowly, half of
  them clockwise and half counter-
  clockwise, so the cloud churns instead of holding one frozen orientation. (The **Rotation
  average/variance** spinners next to it serve a different purpose — a random *starting angle*,
  rolled as the average scaled by ± the variance, used when **Fixed random rotation** is checked;
  Tutorial 3's flipbook fireball shows that one in action.)
- **Affected by wind** (Physics tab) — the stock smoke also checks this, letting the scene's wind
  push the plume so it does not rise in a perfectly straight column.

The principle behind all of them: **variety should come from randomization inside the emitter,
not from duplicating emitters.** A duplicate costs performance and future maintenance; these
spinners cost nothing. This idea returns at full scale in
[Tutorial 3](05-build-an-explosion), where it carries most of the explosion's texture.

## 6. Check the Effect at Gameplay Readability

Use the Preview Viewport to judge the effect as a whole. A good hardpoint damage smoke effect is
visible enough to tell the player something is damaged, but not so loud that it becomes the most
important thing on screen.

Use these questions as a quick check:

- Does the effect still read as smoke?
- Does the warm tint feel like heat from the damage source?
- Does the smoke fade instead of popping away?
- Do the puffs differ in size, angle, and life span — or do they look like copies?
- Does the effect stay readable without covering too much of the ship?

## Takeaways

The exact values of a particle matter less than the result they create: color, alpha, and scale
should work together to communicate heat, smoke, and fading motion. Change one idea at a time and
preview the result in motion.
