# Motion and Physics

The Physics tab in the Property Panel answers three questions, in order, for every particle an
emitter makes:

1. **Where is it born?** — the *Initial position* section.
2. **Which way does it launch, and how fast?** — the *Initial speed* section.
3. **How does its path bend over time?** — the *Acceleration* section.

Every motion effect you will ever build is some combination of answers to those three questions,
so it is worth reading each section below with its question in mind. Tutorial 3 uses exactly one
of these controls (`Parent speed inherit`); this page covers the rest, and it is the reference to
have open when you build the explosion in [Tutorial 5](05-build-an-explosion).

<!-- Media: ref-radial-burst -->

## Random Shapes: How Position and Speed Are Rolled

The **Initial position** and **Initial speed** sections each start with a shape picker
(`Type`) that decides how the value is randomized for every new particle:

| Type | Fields | What each particle gets |
|------|--------|--------------------------|
| Exact | `Value` | The same fixed X/Y/Z every time. No randomness. |
| Box | `Min` / `Max` | A random point inside a box between the two corners. |
| Cube | `Side length` | A random point inside a cube of that size, centered on the emitter. |
| Sphere | `Radius` | A random point inside a sphere. Check `Constrain to surface` to pick points **on** the sphere's shell instead. |
| Cylinder | radius and height | A random point inside a cylinder, with its own `Constrain to surface` option. |

The trick to reading this table: for **Initial position** the shape scatters *where particles
spawn*; for **Initial speed** the same shape scatters *which direction and how fast they fly*.
A speed vector is just a point in the shape — its direction from the center is the flight
direction, and its distance is the speed.

That makes **Sphere on Initial speed the radial-burst tool**: every particle gets a velocity
pointing somewhere on a sphere, so the emitter sprays in all directions at once. With
`Constrain to surface` checked, every particle flies at exactly the same speed (the radius);
unchecked, speeds vary from zero up to the radius, which reads more ragged.

## Two Ways to Fly Outward

"Everything flies away from the center" can be built two different ways, and they read
differently on screen:

- **Random-direction spray** — put the Sphere on **Initial speed**. Each particle rolls its own
  direction, unrelated to where it was born. The result is chaotic and lively: embers, sparks,
  splinters. This is how Tutorial 5's sparks work.
- **Radial launch** — put the Sphere on **Initial position** with `Constrain to surface` checked,
  then drive the speed with a large *negative* **Inward speed** (see below). Each particle is
  born on a shell and pushed straight along the line from the center through its own birth
  point — every path is dead-straight and radial, like shrapnel. This is how Tutorial 5's debris
  works.

The spray randomizes *direction*; the radial launch locks direction to *birth position*. Choose
by what the layer represents: burning fragments scatter, thrown wreckage flies straight.

## Initial Speed Extras

- **Inward speed:** adds motion along the line between each particle and the emitter's center.
  A positive value pulls particles inward — useful for implosion or intake effects. A negative
  value pushes them outward, on top of whatever the speed shape rolled.
- **Parent speed inherit:** how much of the spawning object's motion the particle keeps, from
  0 to 100%. Tutorial 3 covers the classic use: projectile layers at full inheritance ride the
  shot; muzzle-flash layers at zero stay at the launch point.
- **Affected by wind:** lets the scene's wind push the particles.

## Acceleration

Where Initial speed sets motion once at start, the Acceleration section bends it continuously:

- **X / Y / Z:** a constant push in a fixed direction. The everyday use is a small +Z value to
  make smoke rise.
- **Gravity acceleration:** a single number that scales a built-in straight-down push. `1` is a
  standard downward pull; `0` is weightless; and a **negative value pushes up** — the idiomatic
  way to make hot smoke and fire rise (Tutorial 5's smoke uses `-0.9`) without configuring an
  acceleration vector by hand. Give debris a little positive gravity and its straight radial
  flight becomes an arc.
- **Inward acceleration:** like Inward speed, but applied continuously — particles curve toward
  the emitter (positive) or away from it (negative) over their whole life.
- **Object space acceleration:** applies the X/Y/Z push in the emitter's own orientation
  instead of world directions, so the push turns with the object the effect is attached to.

## Ground Interaction

`Behavior` decides what a particle does when it reaches the ground:

- **None** — ignores the ground and passes through.
- **Disappear** — the particle dies on contact.
- **Bounce** — the particle reflects; `Bounciness` (enabled only for this behavior) sets how
  much energy it keeps.
- **Stick** — the particle stops and stays where it landed.

## One Caveat: Weather Mode

When an emitter's Generation is set to **Weather particle** (see
[Particle Generation Types](generation-types)), most of this tab disables — weather particles
fill a camera-following volume rather than flying from an emitter, so initial position,
acceleration, gravity, and ground interaction do not apply. Initial speed, Inward speed, and
Affected by wind stay active. If Physics fields are unexpectedly grayed out, check the
Generation section first.

## Related Pages

- [Tutorial 5: Build an Explosion](05-build-an-explosion) — puts the Sphere speed shape,
  lifetime variation, and gravity to work.
- [Particle Generation Types](generation-types) — bursts vs. streams vs. weather.
- [Curve Editor Basics](curve-editor-basics) — the lifetime curves these motions pair with.
