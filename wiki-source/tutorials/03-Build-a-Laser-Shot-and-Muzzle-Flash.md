# Tutorial 3: Build a Laser Shot and Muzzle Flash

This tutorial starts from a blank particle and builds a bright green laser shot with a short muzzle
flash in the same particle system.

The goal is to practice a common projectile layout: one emitter forms the moving shot with a built-in
tail, and a small muzzle-flash group bursts briefly at the launch point.

<!-- Media: tutorial-03-opening-result -->

## Outcome

By the end of this page, the particle will contain two readable parts:

- a bright green laser core with a built-in tail;
- a quick muzzle flash with a white-hot core and a softer green outer glow.

This tutorial stays in the editor preview. At the end, there is a short note about how this kind of
particle would normally be used by a game projectile.

## What this teaches

- Starting from a blank particle.
- Building a projectile core with the built-in tail controls.
- Building a muzzle flash from a core and an outer glow.
- Using additive color for a readable energy shot.
- Using `Parent speed inherit:` to separate a moving projectile from a stationary muzzle flash.
- Judging projectile effects by motion, timing, and silhouette.

## Before You Start

Create a blank particle in the editor. Save it in the tutorial mod folder with a new tutorial name,
for example:

```text
ParticleTutorial\Data\Art\Models\P_TUTORIAL_GREEN_LASER.ALO
```

This is a new tutorial particle, not an override of a shipped particle. Keep it in the editor for
this lesson.

## 1. Create the Projectile Core

Start with one emitter for the moving laser body. In the Emitter Tree, give it a clear name such as
`Projectile_Core` so it is easy to separate from the muzzle flash emitters later.

For the visual target, think narrow, bright, and short-lived:

```text
Color:       bright green
Blend mode:  additive
Lifetime:    short
Scale:       narrow enough to read as a shot, not a cloud
```

Use the Preview Viewport while you adjust. The projectile core should read immediately as the center
of the shot.

<!-- Media: tutorial-03-projectile-core -->

## 2. Use the Built-In Tail

With `Projectile_Core` still selected, use the Tail section in the Property Panel. Enable `Has tail`
and adjust `Tail length:` until the shot has a clear streak behind it.

The tail should support the core rather than overpower it. If the whole effect becomes a glowing
block, shorten the tail, reduce the scale, or lower the alpha. The goal is a readable laser shot,
not a solid rectangle.

## 3. Let the Projectile Ride the Shot

Select the projectile core emitter. In the Property Panel, find `Parent speed inherit:` and set it
near full inheritance so the projectile rides with the parent motion.

This makes the projectile part behave like it belongs to the moving projectile object. The core and
tail should travel together instead of being left behind at the spawn point.

<!-- Media: tutorial-03-inherit-parent-speed -->

## 4. Add the Muzzle Flash

Add a new emitter for the bright center of the muzzle flash. Keep the name direct, such as
`Muzzle_Core`, so the Emitter Tree shows the two ideas clearly:

```text
Projectile_Core: moving shot and tail
Muzzle_Core: white-hot launch flash
```

Make this emitter very short-lived, bright, and compact. A white or nearly white core works well
because it reads as the hottest part of the flash.

<!-- Media: tutorial-03-muzzle-flash -->

## 5. Add the Outer Glow and Keep It at the Launch Point

Add a second muzzle-flash emitter named something like `Muzzle_Glow`. Make it wider, softer, and
slightly greener than the white core.

Select both muzzle flash emitters. In the Property Panel, set `Parent speed inherit:` near zero so
the flash stays close to the launch point.

This is the main distinction in the tutorial. The projectile core rides the projectile. Muzzle flash
emitters do not. If the flash stretches into a long streak, shorten its lifetime, reduce its scale,
or lower its parent-speed inheritance.

<!-- Media: tutorial-03-no-parent-speed -->

## 6. Check the Two Groups Together

Use the Preview Viewport to watch the full particle in motion. The shot should have a clear leading
core, a built-in tail, a white-hot muzzle core, and a softer outer flash.

Use these questions as a quick check:

- Can you tell which part is the moving projectile?
- Does the built-in tail make the projectile motion easier to read?
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
jobs. Use the built-in tail for the projectile streak, then use `Parent speed inherit:` to decide
which emitters ride with the projectile and which ones stay near the firing point.
