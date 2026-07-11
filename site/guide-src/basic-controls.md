# Basic Editor Controls

A few core viewport interactions are not obvious from the UI alone. This page covers the ones a new
user has no reliable way to discover: previewing a particle in the scene, moving the camera, and
selecting things.

## Preview a Particle: Hold Shift

Loading a particle does not, by itself, put it in the Preview Viewport. To see the current effect,
**hold Shift**. While Shift is held, the particle spawns at the cursor and follows it around the
viewport, so you can move the mouse to judge the effect from different positions.

Release Shift and the preview goes away. This makes Shift a quick, non-destructive way to check an
effect: hold to look, release to clear.

## Place a Particle: Click While Holding Shift

To leave a preview in the scene instead of clearing it, **click the left mouse button while Shift is
held**. The effect detaches from the cursor and keeps emitting where you dropped it, which is useful
when you want to compare an edit against a copy already running, or inspect an effect from a fixed
spot while you work.

After the click, dragging the mouse up and down adjusts the placed effect's height before you let go.
Left and right stay fixed at the point where you clicked.

This Shift-to-preview, click-to-place pattern is easy to miss — nothing on screen announces it — so
reach for it whenever an effect does not appear to be showing.

<!-- Media: ref-shift-preview -->

## Move the Camera

The camera orbits a target point in the scene. Every camera move scales with how far you are zoomed
in, so it stays controllable whether you are close to a small effect or pulled back from a large one.

| Action | Input |
|--------|-------|
| Orbit (rotate around the effect) | Right mouse button, drag |
| Pan (slide the view) | Left mouse button, drag |
| Zoom | Hold Ctrl and drag either button, or use the scroll wheel |

The scroll wheel zooms in small steps and is usually the easiest way to close in on detail. Ctrl-drag
zoom is smoother for large distance changes.

<!-- Media: ref-camera-controls -->

## Select an Emitter

Select emitters in the Emitter Tree, not the viewport. Selecting one makes it the edit target — its
properties and lifetime curves fill the panels — so you can work on that emitter's settings without
disturbing the others. (Selection changes what you edit, not what is drawn; to hide an emitter in
the viewport, toggle its visibility.)

- **Click** a row to select a single emitter.
- **Shift-click** another row to select every emitter from the first selection to that row.
- **Ctrl-click** (or Cmd-click) a row to add or remove it from a multi-selection.

## Select and Move a Reference Object

If the scene has a reference object — a unit or model shown for context — click its body in the
viewport to select it, as long as it is not locked. A selected reference object shows drag handles
for moving and rotating it.

While dragging a handle, **hold Shift for finer control**: movement and rotation slow to a fraction
of normal speed, which helps when you need to line an effect up precisely against the model.
