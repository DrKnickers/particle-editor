# Particle Authoring Primer

Particle `.alo` files are particle systems, not mesh/model files. A particle system contains
emitters, and emitters produce particles over time.

Most effects are shaped by a small set of first principles: lifetime, emission rate or bursts,
color, alpha, scale, texture or atlas frame, blend mode, and motion. Tracks change those values over
each particle's lifetime.

Use additive blending for glows and energy effects. Use transparent blending for smoke, dust, and
debris-like effects.
