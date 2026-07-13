# Game Concepts Glossary

Terms used throughout the tutorials.

## Particle `.alo`

A particle system file. In these tutorials, `.alo` means an effect made from emitters, not a mesh
model.

## Emitter

One part of a particle system. An emitter creates particles over time or in bursts.

## Track

A lifetime curve for a value such as Red, Green, Blue, Alpha, Scale, Index, or Rotation. Tracks make
particles change from start to death.

## Track Key

A point on a track that pins a value at a moment of the particle's life. The Curve Editor fills in
the values between keys using each key's interpolation (linear, smooth, or step).

## Burst

A discrete release of particles, as opposed to a continuous stream. Bursts are the generation mode
for one-off events such as flashes, impacts, and explosions.

## Root Emitter

An emitter that plays on its own from the effect's origin. Stacking several root emitters is the
standard way to build a layered effect.

## Lifetime Child

A child emitter that emits continuously from each of its parent emitter's particles while that
particle is alive — how a moving particle gets a trail.

## Death Child

A child emitter that fires once where each of its parent emitter's particles dies — how a particle
gets an ending event, like a pop or flash.

## Link Group

Several emitters joined so they share one design: an edit to any member propagates to all of them.
Fields can be marked *exempt* in Link Group Settings to stay per-emitter. Used for identical
copies of one emitter — like an explosion's debris chunks — that must stay editable as one.

## Minimum Lifetime / Minimum Scale

Randomization parameters. Maximum lifetime is the designed value; Minimum lifetime (a percentage
of it) lets each particle live a random span in between. Minimum scale does the same for size,
scaling each particle's whole Scale curve by a random factor. The main tools for variety without
extra emitters.

## Tail

A built-in streak drawn behind a particle, enabled per emitter with `Has tail` and sized with
`Tail length`. Used for projectile cores and glows.

## Parent Speed Inherit

A Physics-tab percentage controlling how much of the spawning object's motion a particle keeps.
Full inheritance rides along (projectile layers); zero stays put (muzzle flashes).

## Link Particles to Instance

A Basic-tab Connection option that makes particles follow the orientation of the effect instance —
used for directional impacts such as shield hits.

## Spawner

The editor panel that repeats or launches the effect on demand, including with a chosen velocity,
so brief or moving effects are easier to inspect.

## Texture or Atlas

The image used by a particle. Some textures contain many frames in one image; the Index track chooses
which frame to show.

## Blend Mode

How the particle mixes with the scene behind it. Additive blending is useful for glow and energy.
Transparent blending is useful for smoke, dust, and debris-like effects.

## MEG Archive

A packed game archive. Stock particles are usually extracted from `Models.meg`.

## Internal Path

The path the game uses inside its archives, such as `DATA\ART\MODELS\P_HP_IMPERIAL_DAMAGE.ALO`.

## Loose Override

A file placed directly in a mod's `Data` folder using the same internal path as a stock file.

## Modpath

The launch argument that tells the game which mod folder to load.

## Hardpoint

A damageable part of a unit, often with its own visuals, particles, and death effect.

## `Damage_Particles`

A hardpoint XML reference used for damage-state particle effects.

## `Death_Explosion_Particles`

An XML reference used for the particle effect that plays when something dies or explodes.

## `Particles.xml`

A game XML file used by particle references. The tutorials mention it only when explaining how new
separate particles become reachable by the game.

## Alamo Proxy

A reference inside an Alamo model that can point to another asset, including a particle effect.

## `GameObjectFiles.xml`

A file list that tells the game which object XML files to load. Mods may use this kind of list to
add extra XML files and keep object definitions organized.
