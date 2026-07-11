# App UI Quick Reference

These are the editor surfaces named in the tutorials.

<!-- Media: ref-ui-overview -->

## Emitter Tree

The Emitter Tree lists the emitters in the current particle. Select an emitter here when you want to
isolate one visual part of the effect. Right-click (or use the tree's **+** button) to add root and
child emitters — see [Stacking Emitters and Using Children](stacking-emitters-and-children).

## Property Panel

The Property Panel shows the editable fields for the selected emitter. The tutorials use the visible
UI labels whenever possible, including the Basic, Appearance, and Physics tabs. The Physics tab's
motion controls are covered in [Motion and Physics](motion-and-physics).

## Curve Editor

The Curve Editor edits lifetime tracks such as Red, Green, Blue, Alpha, Scale, Index, and Rotation.
Use it when a value needs to change over a particle's life instead of staying constant. Its
controls — channels, keys, the value spinners, interpolation — are covered in
[Curve Editor Basics](curve-editor-basics).

## Preview Viewport

The Preview Viewport shows the particle in motion. Particle edits are easiest to judge while the
effect is playing, fading, and repeating.

## Texture/Atlas Picker

The Texture/Atlas Picker helps choose the particle texture or atlas frame. Texture choice can change
the feel of an effect, even when the emitter motion stays the same.

## Spawner

The Spawner repeats or places the effect so it is easier to inspect. It is especially useful when an
effect is brief, hard to trigger, or easier to judge from a specific angle.

## Mods/Load Order

Mods and load order controls tell the editor which game and mod assets to use. For these tutorials,
choose the `ParticleTutorial` mod so loose override files are opened and saved in the tutorial
folder.

## File Actions

Use New to start a blank particle, Open to load one, Save to update the current file, and Save As
when you want to create a separate particle rather than overwrite the current one.
