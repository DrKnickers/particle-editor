# Particle Editor Guide

## What You Are Building

A particle effect in Empire at War is not a model — it is a small system that throws out many tiny,
short-lived images and animates them. A laser bolt, a plume of engine smoke, an explosion, a shield
flash: all of them are particle effects. You author them as `.alo` particle files, and each file
holds one or more *emitters* — the little engines that spawn particles. Each emitter decides how its
particles are born, how they move, what image they draw, and how they change as they age; stack a
few emitters together and you get a finished effect.

This guide teaches you to build and tune those effects in Particle Editor, judging them against the
same references the game uses — correct scale, the right background, and your mod's own assets —
with a live viewport right beside the controls.

Start with [Setup](setup), then follow the pages in order. Returning users can jump straight to the
quick references.

## Tutorial Path

| Page | What It Teaches | Time |
|---|---|---|
| [Setup](setup) | Create a small tutorial mod folder, extract shipped particles, and launch the game with the mod. | ~15 min |
| [Basic Editor Controls](basic-controls) | The viewport controls that nothing on screen announces: Shift to preview, camera movement, selection. | ~5 min |
| [Particle Authoring Primer](particle-authoring-primer) | Learn the basic ideas behind emitters, tracks, blend modes, and particle lifetime. | ~10 min |
| [Curve Editor Basics](curve-editor-basics) | Tracks, keys, the value spinners, and interpolation — the mechanics every tutorial uses. | ~5 min |
| [Tutorial 1: Make a Hardpoint Damage Effect Obvious](01-make-a-hardpoint-damage-effect-obvious) | Override a shipped Star Destroyer damage particle and make it bright green so the edit is easy to prove. | ~20 min |
| [Tutorial 2: Polish Hardpoint Damage Smoke](02-polish-hardpoint-damage-smoke) | Turn the proof edit into believable dark smoke with a subtle warm tint. | ~20 min |
| [Concepts Before You Build](concepts-before-you-build) | The handful of decisions every new emitter needs, before the from-scratch tutorials. | ~5 min |
| [Tutorial 3: Build a Laser Shot and Muzzle Flash](03-build-a-laser-shot-and-muzzle-flash) | Build a blank particle into a green laser shot with separate projectile and muzzle-flash emitters. | ~30 min |
| [Tutorial 4: Recolor and Orient a Shield Impact](04-recolor-and-orient-a-shield-impact) | Override a shield impact particle, recolor it purple, and study impact orientation in the editor preview. | ~20 min |
| [Tutorial 5: Build an Explosion](05-build-an-explosion) | Build a full explosion from scratch: flash, animated fireball, smoke, and flying sparks. | ~40 min |
| [Build Your Own Effect](build-your-own-effect) | A capstone brief with no recipe — plan and build a new effect from a description alone. | ~30 min |

## Quick References

| Page | Use It For |
|---|---|
| [App UI Quick Reference](app-ui-quick-reference) | Finding the main editor panels and controls by their visible labels. |
| [Particle Generation Types](generation-types) | Choosing between bursts, continuous streams, and weather particles. |
| [Blend Modes](blend-modes) | Choosing how a particle mixes with the scene behind it. |
| [Motion and Physics](motion-and-physics) | Initial position and speed shapes, acceleration, gravity, and ground behavior. |
| [Stacking Emitters and Using Children](stacking-emitters-and-children) | Combining emitters: siblings, lifetime children, and death children. |
| [File Structure](file-structure) | Understanding loose overrides, internal paths, and where particle files live. |
| [Where Particles Are Used In-Game](where-particles-are-used-in-game) | Seeing the common places particles are referenced by the game. |
| [Troubleshooting](troubleshooting) | Working out why an effect is not visible — in the editor or in the game. |
| [Game Concepts Glossary](game-concepts-glossary) | Looking up the terms used throughout the tutorials. |
