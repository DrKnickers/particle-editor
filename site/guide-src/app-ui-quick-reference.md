# App UI Quick Reference

These are the editor surfaces named in the tutorials.

<!-- Media: ref-ui-overview -->

## Emitter Tree

The Emitter Tree lists the emitters in the current particle. Select an emitter here when you want to
isolate one visual part of the effect. Add emitters from the **Emitters** menu → **New Emitter**
(**Root Emitter**, or **Lifetime Child** / **Death Child** when a parent is selected) — see
[Stacking Emitters and Using Children](stacking-emitters-and-children).

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

## Atlas Frame Picker

Open the **Atlas Frame Picker…** from the **View** menu (the docked panel is titled **Atlas
Frames**). It helps you choose the particle texture or atlas frame. Texture choice can change the
feel of an effect, even when the emitter motion stays the same.

## Spawner

Open the Spawner from the **Emitters** menu → **Spawner** (shortcut **F7**). It repeats or places
the effect so it is easier to inspect — especially useful when an effect is brief, hard to trigger,
or easier to judge from a specific angle. To make the effect play over and over while you tune it,
turn the Spawner on with its **Enabled** checkbox (off by default), set the mode to **Auto**, and
use the **Interval** spinner to control how often it re-fires.

## Mods / Load Order

The **Mods** menu tells the editor which game and mod assets to use. **Add mod…** adds a mod to the
**Active load order**; the **Expand to full editor** button opens the full **Mod Load Order** dialog
to review and reorder the stack; **Refresh Mod List** rescans the game install. For these tutorials,
add the `ParticleTutorial` mod so loose override files are opened and saved in the tutorial folder.

## File Actions

Use **File → New** to start a blank particle, **Open…** (Ctrl+O) to load one, **Save** to update
the current file, and **Save As** when you want to create a separate particle rather than overwrite
the current one.
