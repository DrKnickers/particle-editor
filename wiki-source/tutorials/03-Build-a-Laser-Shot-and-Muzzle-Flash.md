# Tutorial 3: Build a Laser Shot and Muzzle Flash

This tutorial starts from a blank particle and builds a bright green laser shot with a short muzzle
flash in the same particle system.

The goal is to practice a common projectile layout: one group of emitters travels with the shot, and
another group flashes briefly at the launch point.

<!-- Media: tutorial-03-opening-result -->

## Outcome

By the end of this page, the particle will contain two readable parts:

- a bright green laser shot with a short trail;
- a quick muzzle flash that stays near the firing point.

This tutorial stays in the editor preview. At the end, there is a short note about how this kind of
particle would normally be used by a game projectile.

## What this teaches

- Starting from a blank particle.
- Building an effect from separate emitter groups.
- Using additive color for a readable energy shot.
- Using `Parent speed inherit:` to separate moving projectile emitters from a stationary muzzle
  flash.
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

## 2. Add a Short Trail

Add a second emitter for the trail or afterglow. Keep it in the same projectile group in the Emitter
Tree so the moving-shot part of the particle stays easy to understand.

The trail should support the core rather than overpower it. Make it slightly softer, shorter, or
more transparent than the projectile body. If the whole effect becomes a glowing block, reduce the
trail alpha, scale, or lifetime.

## 3. Let the Projectile Emitters Ride the Shot

Select the projectile core and trail emitters. In the Property Panel, find `Parent speed inherit:`
and set these emitters near full inheritance so they ride with the parent motion.

This makes the projectile part behave like it belongs to the moving projectile object. The core and
trail should travel together instead of being left behind at the spawn point.

<!-- Media: tutorial-03-inherit-parent-speed -->

## 4. Add the Muzzle Flash

Add a new emitter group for the muzzle flash. Keep the name direct, such as `Muzzle_Flash`, so the
Emitter Tree shows the two ideas clearly:

```text
Projectile_Core / Projectile_Trail: moving shot
Muzzle_Flash: launch flash
```

The muzzle flash should be wider and shorter than the projectile core. It can share the same green
energy color, but it should feel like a burst at the weapon mouth rather than a second projectile.

<!-- Media: tutorial-03-muzzle-flash -->

## 5. Keep the Muzzle Flash at the Launch Point

Select the muzzle flash emitter. In the Property Panel, set `Parent speed inherit:` near zero so the
flash stays close to the launch point.

This is the main distinction in the tutorial. Projectile and trail emitters ride the projectile.
Muzzle flash emitters do not. If the flash stretches into a long streak, shorten its lifetime,
reduce its scale, or lower its parent-speed inheritance.

<!-- Media: tutorial-03-no-parent-speed -->

## 6. Check the Two Groups Together

Use the Preview Viewport to watch the full particle in motion. The shot should have a clear leading
core, a supporting trail, and a brief launch flash.

Use these questions as a quick check:

- Can you tell which part is the moving projectile?
- Does the muzzle flash disappear quickly?
- Does the trail support the shot without covering the target?
- Do the emitter names still make the two groups easy to understand?

<!-- Media: tutorial-03-final-preview -->

## Game Use Note

In a full mod, this kind of particle could be used as the visual model for a projectile. The
projectile XML would still control the game behavior, while the particle controls how the shot and
launch flash read on screen.

## Takeaways

Projectile particles are easier to reason about when you separate the moving shot from the launch
flash. Use `Parent speed inherit:` to decide which emitters ride with the projectile and which ones
stay near the firing point.
