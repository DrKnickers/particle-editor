# Tutorial 2: Polish Hardpoint Damage Smoke

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
- Judging an effect by first principles rather than exact numeric recipes.

## Before You Start

Start with the `P_HP_IMPERIAL_DAMAGE.ALO` loose override from Tutorial 1:

```text
ParticleTutorial\Data\Art\Models\P_HP_IMPERIAL_DAMAGE.ALO
```

Open it in the editor. The smoke may still be bright green from the proof edit.

## 1. Return the Smoke to a Dark Base Color

Select the smoke emitter in the Emitter Tree. In the Property Panel or Curve Editor, bring the color
back toward a dark gray.

A useful target is not pure black. Smoke still needs to catch some light, especially near the damage
source. Think in terms of a dark neutral base:

```text
Red:   low to medium-low
Green: low to medium-low
Blue:  low to medium-low
Alpha: strong at birth, fading later
```

Watch the Preview Viewport while you adjust. If the smoke disappears, it is probably too dark, too
transparent, or both.

<!-- Media: tutorial-02-color-warm-tint -->

## 2. Add a Warm Tint Near the Source

Hardpoint damage usually implies heat, sparks, or fire somewhere inside the damaged part. Fake that
lighting by making the early smoke slightly warmer, then letting it cool as it ages.

In the Curve Editor, adjust the color near the start of the smoke lifetime so it has a small red or
orange bias. Keep the later part of the lifetime closer to neutral gray.

This does not need to become flame. A subtle warm tint is enough to connect the smoke to the damage
source.

## 3. Fade the Smoke with Alpha

Use the Alpha track to make the smoke fade out. A common shape is:

```text
Birth:      visible
Middle:     still readable
End:        transparent
```

The ending matters most. Smoke that fades out feels like it disperses. Smoke that simply stops feels
like it was switched off.

<!-- Media: tutorial-02-alpha-fade -->

## 4. Let the Smoke Grow as It Fades

Use the Scale track to make the smoke larger over its lifetime. This helps sell the idea that hot
smoke is spreading away from the damage source.

The shape does not need to be dramatic. A small-to-larger scale curve is enough:

```text
Birth:      compact
Middle:     wider
End:        widest, but fading out
```

Preview the color, alpha, and scale together. These controls work as a group: darker smoke may need
more alpha, larger smoke may need a softer fade, and a warmer birth color may need a shorter visible
duration.

<!-- Media: tutorial-02-scale-growth -->

## 5. Check the Effect at Gameplay Readability

Use the Preview Viewport to judge the effect as a whole. A good hardpoint damage smoke effect is
visible enough to tell the player something is damaged, but not so loud that it becomes the most
important thing on screen.

Use these questions as a quick check:

- Does the effect still read as smoke?
- Does the warm tint feel like heat from the damage source?
- Does the smoke fade instead of popping away?
- Does the effect stay readable without covering too much of the ship?

<!-- Media: tutorial-02-final-preview -->

## Takeaways

The exact values of a particle matter less than the result they create: color, alpha, and scale
should work together to communicate heat, smoke, and fading motion. Change one idea at a time and
preview the result in motion.
