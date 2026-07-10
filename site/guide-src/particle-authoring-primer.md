# Particle Authoring Primer

Particle `.alo` files are particle systems, not mesh/model files. A particle system contains
emitters, and emitters produce particles over time.

Most effects come from a small set of first principles: lifetime, emission rate or bursts,
color, alpha, scale, texture or atlas frame, blend mode, and motion. Tracks change those values over
each particle's lifetime.

Use additive blending for glows and energy effects. Use transparent blending for smoke, dust, and
debris-like effects.

## Emitters

A finished effect is usually several simple emitters working together. One emitter might make the
main smoke shape, another might add a hot glow, and a third might add sparks. When editing an
existing particle, select one emitter at a time and watch the Preview Viewport until you know what
that emitter contributes.

## Render Order

When several emitters overlap on screen, the order they draw in — top to bottom in the Emitter
Tree — decides which one appears on top. Particles do not sort themselves by depth or distance;
the first emitter in the list draws first, and every emitter after it draws over what came before,
the same way the game itself renders them.

This matters most once blend modes are involved. An Additive glow drawn after a Transparent smoke
layer shows through the smoke and brightens it; the same glow drawn before the smoke gets covered
by it instead. If a layered effect is not reading the way you expect, check the emitter order
before you start second-guessing the individual emitter's settings.

<!-- Media (planned): ref-render-order -->

Reorder emitters by dragging them in the Emitter Tree. One thing to know: reordering does not
reach into a preview that is already running. If you drag an emitter to a new position and the
Preview Viewport looks unchanged, stop and restart the preview (or reload the particle) so it
picks up the new order.

## Lifetimes

Most particle values change from birth to death. A smoke particle might start small and opaque, grow
larger, drift away, and fade out. A flash might start bright and vanish almost immediately. The Curve
Editor is where those lifetime changes become visible — [Curve Editor Basics](curve-editor-basics)
covers its controls.

Useful starting questions:

- What should the particle look like at birth?
- What should it look like halfway through its life?
- How should it disappear?

## Textures and Atlas Frames

Every particle draws an image, and the image carries as much of the effect's character as the
motion does — the same emitter reads as smoke, fire, or energy depending on the texture it draws.

Many particle textures are **atlases**: one image holding a grid of frames. The emitter's
**Index** track chooses which frame each particle shows, and because Index keys step from one
whole frame to the next rather than blending, a rising Index curve plays the frames like a
flipbook across the particle's life. That is how a single particle can show a rolling, animated
flame. Use the Texture/Atlas Picker to see a texture's frames and pick one; use the Index track
in the Curve Editor when the frames should animate.

## Game Context

Particles are not just abstract visuals. A good effect helps the player read what is happening in
the game. A hardpoint damage effect should sit near the damaged hardpoint. A weapon impact should
face or imply the surface it hit. A projectile trail should make the projectile's motion easier to
follow without covering the target.

## Common Rules of Thumb

- Use short lifetimes for flashes, sparks, and impacts.
- Use longer lifetimes for smoke, dust, and lingering energy.
- Fade smoke out with alpha rather than letting it vanish abruptly.
- Let smoke grow as it fades so it feels like it is dispersing.
- Keep glows and energy effects brighter near the source.
- Keep expensive-looking or noisy effects readable at normal gameplay zoom.
- Make one change at a time, then preview it in motion.

Treat these as starting points, not rules. Once you understand why an effect reads clearly, you can
choose when to bend the pattern.
