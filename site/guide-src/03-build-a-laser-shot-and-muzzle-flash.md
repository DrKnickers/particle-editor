# Tutorial 3: Build a Laser Shot and Muzzle Flash

This tutorial starts from a blank particle and builds a bright green laser shot with a projectile
core, projectile glow, and short muzzle flash in the same particle system.

The goal is to practice a common projectile layout: two emitters form the moving shot and its glow,
and a small muzzle-flash group bursts briefly at the launch point.

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
- Using additive color for a readable energy shot.
- Using `Parent speed inherit:` to separate a moving projectile from a stationary muzzle flash.
- Using the Spawner panel to launch particle instances in a chosen direction at a chosen speed.
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

## 2. Use the Built-In Tail on the Core

With `Projectile_Core` still selected, use the Tail section in the Property Panel. Enable `Has tail`
and adjust `Tail length:` until the shot has a clear streak behind it.

The tail should support the core rather than overpower it. If the whole effect becomes a glowing
block, shorten the tail, reduce the scale, or lower the alpha. The goal is a readable laser shot,
not a solid rectangle.

## 3. Add the Projectile Glow

Add a second projectile emitter named something like `Projectile_Glow`. This emitter should be
slightly wider, softer, and less intense than the core. It gives the shot presence without replacing
the crisp center.

Use the Tail section on this emitter too. Enable `Has tail` and give the glow its own `Tail length:`.
The glow tail can be a little broader or softer than the core tail, but it should still point in the
same direction and support the same motion.

## 4. Let the Projectile Emitters Ride the Shot

Select the projectile core and projectile glow emitters. In the Property Panel, find
`Parent speed inherit:` and set them near full inheritance so the projectile rides with the parent
motion.

This makes the projectile part behave like it belongs to the moving projectile object. The core and
glow tails should travel together instead of being left behind at the spawn point.

<!-- Media: tutorial-03-inherit-parent-speed -->

## 5. Launch Test Instances with the Spawner

Open the Spawner panel and use it to preview the projectile as a launched particle instance rather
than a static loop. Start with `Manual` mode so each test spawn is deliberate.

Set the Velocity fields to launch the particle in a clear direction. For example, put a positive
value in `Velocity X` and leave `Velocity Y` and `Velocity Z` near zero. Higher velocity values make
the shot move faster; lower values make it easier to inspect the tail.

Use `Spawn now` to fire a test instance. If the shot is hard to read in motion, adjust the core,
glow, tail length, or velocity and spawn another instance.

<!-- Media: tutorial-03-spawner-direction -->

## 6. Add the Muzzle Flash

Add a new emitter for the bright center of the muzzle flash. Keep the name direct, such as
`Muzzle_Core`, so the Emitter Tree shows the two ideas clearly:

```text
Projectile_Core: moving shot core and tail
Projectile_Glow: softer projectile glow and tail
Muzzle_Core: white-hot launch flash
```

Make this emitter very short-lived, bright, and compact. A white or nearly white core works well
because it reads as the hottest part of the flash.

<!-- Media: tutorial-03-muzzle-flash -->

## 7. Add the Outer Glow and Keep It at the Launch Point

Add a second muzzle-flash emitter named something like `Muzzle_Glow`. Make it wider, softer, and
slightly greener than the white core.

Select both muzzle flash emitters. In the Property Panel, set `Parent speed inherit:` near zero so
the flash stays close to the launch point.

This is the main distinction in the tutorial. The projectile core and projectile glow ride the
projectile. Muzzle flash emitters do not. If the flash stretches into a long streak, shorten its
lifetime, reduce its scale, or lower its parent-speed inheritance.

<!-- Media: tutorial-03-no-parent-speed -->

## 8. Check the Two Groups Together

Use the Preview Viewport to watch the full particle in motion. The shot should have a clear leading
core, a softer projectile glow, built-in tails, a white-hot muzzle core, and a softer outer flash.

Use these questions as a quick check:

- Can you tell which part is the moving projectile?
- Do the built-in tails make the projectile motion easier to read?
- Does the projectile glow support the core without blurring the shot into a blob?
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
and speed, then use `Parent speed inherit:` to decide which emitters ride with the projectile and
which ones stay near the firing point.
