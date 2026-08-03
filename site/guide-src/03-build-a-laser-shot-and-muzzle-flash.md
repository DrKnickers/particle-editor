# Tutorial 2: Build a Laser Shot and Muzzle Flash

This tutorial starts from a blank particle and builds a bright green laser shot with a projectile
core, projectile glow, and short muzzle flash in the same particle system.

The goal is to practice a common projectile layout: a small stack of emitters forms the moving shot
and its glow, and a muzzle-flash group bursts briefly at the launch point.

<!-- Media: tutorial-03-opening-result -->

## Outcome

By the end of this page, the particle will contain two readable parts:

- a bright green laser core and softer projectile glow, both using built-in tails;
- a quick muzzle flash with a white-hot core and a softer green outer glow.

This tutorial stays in the editor preview. At the end, there is a short note about how this kind of
particle would normally be used by a game projectile.

## What this teaches

- Starting from a blank particle.
- Building a projectile core with the built-in tail controls.
- Adding a projectile glow that also uses the built-in tail controls.
- Building a muzzle flash from a core and an outer glow.
- Using `Bursts` generation so the muzzle flash fires once instead of streaming continuously.
- Using additive color for a readable energy shot.
- Using `Parent speed inherit` to separate a moving projectile from a stationary muzzle flash.
- Using the Spawner panel to launch particle instances in a chosen direction at a chosen speed.
- Judging projectile effects by motion, timing, and silhouette.

## Before You Start

Create a blank particle with **File → New**, then use **Save As** to save it in the tutorial mod
folder with a new tutorial name, for example:

```text
ParticleTutorial\Data\Art\Models\P_TUTORIAL_GREEN_LASER.ALO
```

This is a new tutorial particle, not an override of a shipped particle. Keep it in the editor for
this lesson.

## 1. Create the Projectile Core

A new particle starts with one default emitter — that becomes the moving laser body. Select it in
the Emitter Tree and rename it in the **Name** field at the top of the **Basic** tab. A clear name
such as `Projectile_Core` makes it easy to separate from the muzzle flash emitters later.

For the visual target, think narrow, bright, and short-lived. These are starter values — tune them
in the preview, they are not a fixed recipe:

| Parameter | Value |
|---|---|
| Color | bright green — R 0.15 / G 1.0 / B 0.25 |
| Blend mode | Additive |
| Lifetime | ~0.3s (Maximum lifetime) |
| Scale | narrow — around 3 (a tight bolt, not a cloud) |
| Generation | Continuous stream, ~10 particles/second (a steady bolt) |
| Texture | the default master atlas (a soft round dot) |

Keep the rate low: because these are additive, overlapping particles *sum*, so a dense stream of
full-green cores clips to a shapeless white line. If the bolt whites out, lower the rate or bring
the green value down until the green reads again.

The blend mode lives in the **Appearance** tab's Rendering section — Additive is the right choice
for anything that glows (see [Blend Modes](blend-modes)). Lifetime is set with the Minimum/Maximum
lifetime fields in the **Basic** tab's Generation section.

Use the Preview Viewport while you adjust. The projectile core should read immediately as the center
of the shot.

<!-- Media: tutorial-03-projectile-core -->

## 2. Use the Built-In Tail on the Core

With `Projectile_Core` still selected, use the Tail section in the Property Panel. Enable `Has tail`
and adjust `Tail length` (start around `5` and raise it) until the shot has a clear streak behind
it — long enough to read as motion, short enough that the bolt still has a defined head.

The tail should support the core rather than overpower it. If the whole effect becomes a glowing
block, shorten the tail, reduce the scale, or dim the color. (The core is additive, so lowering
alpha would do nothing — see [Blend Modes](blend-modes).) The goal is a readable laser shot,
not a solid rectangle.

## 3. Add the Projectile Glow

Add a second projectile emitter — a **root emitter**, one that plays on its own from the effect's
origin (as opposed to a *child* emitter, which hangs off another emitter's particles; see
[Stacking Emitters and Using Children](stacking-emitters-and-children)). Add it from the **Emitters**
menu → **New Emitter → Root Emitter** (right-clicking in the Emitter Tree offers the same choice).
Name it something like `Projectile_Glow`. This emitter should be
slightly wider, softer, and less intense than the core. It gives the shot presence without replacing
the crisp center.

Use the Tail section on this emitter too. Enable `Has tail` and give the glow its own `Tail length`.
The glow tail can be a little broader or softer than the core tail, but it should still point in the
same direction and support the same motion.

If the shot still feels thin, a third, even softer layer works well: duplicate the glow idea as
`Projectile_Glow_Soft`, make it noticeably wider and much fainter, and let it act as a halo around
the other two. Very low intensity at a large scale adds presence without blowing out the shot.

<!-- Media: tutorial-03-glow-layers -->

## 4. Let the Projectile Emitters Ride the Shot

Set `Parent speed inherit` near full inheritance on both the projectile core and the projectile
glow — it lives in the **Physics** tab's Initial speed section (see
[Motion and Physics](motion-and-physics)) — one emitter at a time, so the projectile rides with
the parent motion.

This makes the projectile part behave like it belongs to the moving projectile object. The core and
glow tails should travel together instead of being left behind at the spawn point.

<!-- Media: tutorial-03-inherit-parent-speed -->

## 5. Launch Test Instances with the Spawner

Open the Spawner panel (**Emitters** menu → **Spawner**, or **F7** — see the
[App UI Quick Reference](app-ui-quick-reference)) and use it to preview the projectile as a launched
particle instance rather than a static loop. Start with `Manual` mode so each test spawn is
deliberate.

Set the Velocity fields to launch the particle in a clear direction. For example, set
`Velocity X` to about `50` (units per second) and leave `Velocity Y` and `Velocity Z` at `0`, so
the shot travels straight along one axis. Higher X values make the shot move faster; lower values
(try `20`) make it easier to inspect the tail.

Use `Spawn now` to fire a test instance. If the shot is hard to read in motion, adjust the core,
glow, tail length, or velocity and spawn another instance.

<!-- Media: tutorial-03-spawner-direction -->

## 6. Add the Muzzle Flash

Add a new emitter for the bright center of the muzzle flash. Keep the name direct, such as
`Muzzle_Core`, so the Emitter Tree shows the two ideas clearly:

| Emitter | Role |
|---|---|
| Projectile_Core | moving shot core and tail |
| Projectile_Glow | softer projectile glow and tail |
| Projectile_Glow_Soft | faint wide halo around the shot |
| Muzzle_Core | white-hot launch flash |

Make this emitter very short-lived, bright, and compact. A white or nearly white core works well
because it reads as the hottest part of the flash. Starter values:

| Parameter | Value |
|---|---|
| Lifetime | ~0.1s |
| Scale | compact — around 10 |
| Blend mode | Additive |
| Generation | Bursts — 1 burst, 1–2 particles |
| Color | near-white with a green tint — R 0.6 / G 1.0 / B 0.6, peaking within the first 10% then decaying to black |

Keep the particle count tiny here (one or two). These muzzle layers are additive and will overlap
the projectile and each other at the launch point; more particles just sum toward the shapeless
white square the section below warns about.

Shape the brightness with the color tracks rather than a constant: start the channels at zero,
peak them early — within the first tenth of the life — and let them decay to black. This
fast-in, slow-out envelope is a reliable way to make a flash read well, and you will see it in
shipped flash effects (the explosion example in [Tutorial 3](05-build-an-explosion) peaks its
flash at 10% too): the pop reads as an *event* with attack and decay rather than a sprite
switching on and off. And since
several additive layers will overlap at the launch point, keep each layer's peak modest — additive
layers sum, and stacked layers at full brightness clip to a shapeless white square.

A muzzle flash fires once at the moment of launch, so it should generate as a burst, not a
continuous stream. In the **Basic** tab, open the **Generation** section and set the Generation mode
to `Bursts` (rather than `Continuous stream`). A single burst — `Bursts: 1` with a small
`Particles/burst` — gives one clean flash instead of an emitter that keeps re-emitting for the
whole life of the shot. This burst-versus-stream choice, together with the short lifetime, is what
makes a launch flash read as a flash and not a steady jet. (The full rundown of the three
generation modes is in [Particle Generation Types](generation-types).)

<!-- Media: tutorial-03-muzzle-flash -->

## 7. Add the Outer Glow and Keep It at the Launch Point

Add a second muzzle-flash emitter named something like `Muzzle_Glow`. Make it wider, softer, and
greener than the white core. Give it the same `Bursts` Generation mode as the core, so the outer
glow fires with the flash instead of streaming on its own. The same halo idea from the projectile
applies here too: a third `Muzzle_Glow_Soft` emitter, wider and much fainter again, rounds the flash
off without making it bigger in feel.

<!-- Media: tutorial-03-muzzle-glow-props -->

Set `Parent speed inherit` near zero on both muzzle flash emitters, one at a time in the Property
Panel, so the flash stays close to the launch point.

> **The one idea to remember here.** `Parent speed inherit` is what separates a *moving* layer from
> a *staying* one. The projectile core and glow ride the shot (near-full inheritance); the muzzle
> flash stays at the launch point (near-zero). Same control, opposite settings.

If the flash stretches into a long streak, shorten its lifetime, reduce its scale, or lower its
parent-speed inheritance.

<!-- Media: tutorial-03-no-parent-speed -->

## 8. Check the Two Groups Together

Use the Preview Viewport to watch the full particle in motion. The shot should have a clear leading
core, a softer projectile glow, built-in tails, a white-hot muzzle core, and a softer outer flash.

Use these questions as a quick check:

- Can you tell which part is the moving projectile?
- Do the built-in tails make the projectile motion easier to read?
- Does the projectile glow support the core without blurring the shot into a blob?
- Does the muzzle flash fire as a single burst rather than a continuous stream?
- Does the muzzle flash disappear quickly?
- Does the outer glow support the white core without becoming a second projectile?
- Do the emitter names still make the two groups easy to understand?

<!-- Media: tutorial-03-final-preview -->

## Game Use Note

In a full mod, this kind of particle could be used as the visual model for a projectile. The
projectile XML would still control the game behavior, while the particle controls how the shot and
launch flash read on screen.

## Takeaways

Projectile particles are easier to reason about when the moving shot and launch flash have separate
jobs. Use built-in tails for the projectile core and glow, use the Spawner panel to test direction
and speed, then use `Parent speed inherit` to decide which emitters ride with the projectile and
which ones stay near the firing point.
