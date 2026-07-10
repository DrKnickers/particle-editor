# Particle Generation Types

The generation type decides *how* an emitter releases its particles: in timed bursts, in a steady
stream, or as camera-following weather. You pick one of the three in the Property Panel under Basic,
in the Generation section. They are mutually exclusive — choosing one turns the others off — and each
reveals its own set of fields.

<!-- Media (planned): ref-generation-types -->

## Bursts

Burst mode releases particles in discrete groups rather than continuously. It is the right choice for
one-off or repeating events: an explosion, an impact, a puff, a muzzle flash.

Its fields:

- **Bursts** — how many bursts to fire. Combined with the burst delay, this controls whether the
  effect happens once or repeats.
- **Burst delay** — the time, in seconds, between one burst and the next.
- **Particles/burst** — how many particles each burst releases at once.

For a single event, use one burst. For a repeating rhythm — a sputtering fire, a pulsing beacon —
use several bursts with a delay between them.

## Continuous Stream

Continuous mode releases particles at a steady rate for as long as the effect is active. It is the
right choice for ongoing effects: smoke rising from damage, an engine trail, a dust cloud, flowing
energy.

Its one field:

- **Particles/second** — the spawn rate. Higher values make a denser, thicker stream; lower values
  make a sparse, wispy one.

Continuous is usually the mode you reach for when an effect should simply keep going rather than
happen and stop.

## Weather Particle

Weather mode is a special rendering type for large-scale environmental effects like rain and snow.
Instead of emitting from a point, it fills a cube of space that follows the camera — centered on a
point out in front of the viewpoint — so the effect covers the view no matter where the camera moves,
the way falling weather should.

This is why you may not have seen it on a normal effect: it is not for hardpoint or weapon particles,
it is for blanketing a scene. If you have never used one, that is expected; reach for it only when you
want weather across the environment rather than a localized effect.

Its fields:

- **Particles** — how many particles fill the cube. Unlike Continuous, weather does not emit at a
  per-second rate: it spawns this many particles once, up front, to populate the volume.
- **Distance from camera** — how far in front of the camera the cube's center sits.
- **Cube size** — the size of the volume the weather fills. A larger cube covers more of the scene
  but spreads the same particle count more thinly.

## Particle Lifetime

Alongside the mode fields, the Generation section carries **Minimum lifetime:** and
**Maximum lifetime:** — how long each particle lives, in seconds, whichever generation mode is
active. With the two values equal, every particle lives the same span. Setting them apart gives
each particle a random lifespan in that range, which makes a burst die off raggedly — sparks
winking out one by one — instead of the whole group vanishing on the same frame.

## Choosing a Type

- Something that happens as an event → **Bursts**.
- Something that keeps going while active → **Continuous stream**.
- Weather that should blanket the whole scene around the camera → **Weather particle**.

Switching away from Weather returns you to whichever of Bursts or Continuous you were using before, so
you can toggle Weather on to test it without losing your earlier setup.
