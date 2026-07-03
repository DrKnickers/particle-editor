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

## Lifetimes

Most particle values change from birth to death. A smoke particle might start small and opaque, grow
larger, drift away, and fade out. A flash might start bright and vanish almost immediately. The Curve
Editor is where those lifetime changes become visible.

Useful starting questions:

- What should the particle look like at birth?
- What should it look like halfway through its life?
- How should it disappear?

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
