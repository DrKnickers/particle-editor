# Concepts Before You Build

Tutorials 1 and 2 changed an effect that already existed — you adjusted colors and curves on a
particle that was handed to you. From Tutorial 3 on, you build effects **from a blank file**. That
is a bigger jump than it sounds: with nothing on screen, you have to decide what every emitter is
and does before anything appears.

This short page is the bridge. It is not new reference material — it is the handful of decisions
you now have to make yourself, each pointing at the fuller page that covers it. Read it once, then
keep those pages open as you build.

## The Model, in One Line

You already met the chain in the [Particle Authoring Primer](particle-authoring-primer): a system
holds emitters, each emitter spawns many short-lived particles, and each particle draws a texture,
changes over its life via tracks, and blends with the scene. Building from scratch is just *filling
that chain in*, one emitter at a time.

## Three Decisions Every New Emitter Needs

When you add a blank emitter, you are really answering three questions:

1. **How does it release particles?** Once, in a burst? Or continuously, in a stream? An impact or
   flash is a burst; rising smoke or a trail is continuous. This is the emitter's *generation
   type* — see [Particle Generation Types](generation-types).
2. **How do its particles move and look?** Their launch direction and speed, any acceleration or
   gravity, the texture they draw, and the blend mode that mixes them with the scene. Motion lives
   in [Motion and Physics](motion-and-physics); mixing lives in [Blend Modes](blend-modes).
3. **How does it change over each particle's life?** The Alpha, Scale, Color, and other tracks
   that make a particle fade, grow, or shift color from birth to death — see
   [Curve Editor Basics](curve-editor-basics).

## A Worked Example

Say you want a **rising column of engine smoke**. Filling in the three decisions for its one
emitter looks like this:

| Decision | Choice for engine smoke |
|---|---|
| How it releases | Continuous stream — it keeps going while the engine runs |
| Texture + blend | A soft smoke texture, Transparent blend |
| How it moves | A gentle upward push (negative gravity or a small +Z acceleration), little sideways spread |
| How it changes | Alpha fades in then out; Scale grows as the smoke rises and disperses |

That is one emitter's plan. A finished effect is a few of these, each with its own filled-in row.

## How Layers Combine

A finished effect is almost never one emitter. You stack several, each with one clear job — a
flash, a fireball, smoke, sparks — and their draw order (top to bottom in the Emitter Tree) decides
what shows over what. When to stack siblings versus attach a child is covered in
[Stacking Emitters and Using Children](stacking-emitters-and-children).

Keep those five pages within reach. With the model above in mind, the from-scratch tutorials become
a matter of *filling in the three decisions* for each emitter you add — not inventing from nothing.
