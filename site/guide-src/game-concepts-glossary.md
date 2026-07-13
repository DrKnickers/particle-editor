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

## Channel

A single row in the Curve Editor's channel list — Red, Green, Blue, Alpha, Scale, Index, or
Rotation. Selecting a channel row focuses its track for editing. The channel is the row you pick;
the [track](#track) is the curve it holds.

## Burst

A discrete release of particles, as opposed to a continuous stream. Bursts are the generation mode
for one-off events such as flashes, impacts, and explosions.

## Continuous Stream

A generation mode that releases particles at a steady per-second rate for as long as the effect is
active — the opposite of a burst. Right for ongoing effects like rising smoke or an engine trail.

## Weather Particle

A generation mode for large-scale environmental effects such as rain and snow. Instead of emitting
from a point, it fills a camera-following cube of space so the effect blankets the view.

## Root Emitter

An emitter that plays on its own from the effect's origin. Stacking several root emitters is the
standard way to build a layered effect.

## Render Order

The order emitters draw in, set top-to-bottom in the Emitter Tree: the first draws first and each
later emitter draws over the ones before it. It decides which layer appears on top when emitters
overlap. (Heat-shimmer emitters are the exception — they draw in a separate pass.)

## Lifetime Child

A child emitter attached to each of its parent's particles for as long as that particle is alive —
how a moving particle gets a trail. It emits at its *own* rate (its own burst or continuous
Generation setting), so how thick the trail is depends on the child's settings, not the parent's.

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

## Rotation and Color Randomization

Further per-particle variety controls: **Random rotation direction** (spin each particle clockwise
or counter-clockwise), **Rotation average / Rotation variance** (a random starting angle, active
when *Fixed random rotation* is on), and **Random color addition** (a small random color offset per
particle). Like Minimum lifetime and Minimum scale, they add variety without extra emitters.

## Tail

A built-in streak drawn behind a particle, enabled per emitter with `Has tail` and sized with
`Tail length`. Used for projectile cores and glows.

## Parent Speed Inherit

A Physics-tab percentage controlling how much of the spawning object's motion a particle keeps.
Full inheritance rides along (projectile layers); zero stays put (muzzle flashes).

## Link Particles to Instance

A Basic-tab Connection option that makes already-spawned particles move with the effect instance
as it moves. It follows position only — it does not rotate the particles — so it keeps them riding
along with a moving instance rather than being left behind at the spawn point.

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

## Mod Stack

The ordered set of mods the editor loads assets from (the *Active load order* in the Mods menu).
Higher entries override lower ones, so a loose file in your active mod replaces the stock version.

## Game Unit

The world-space unit the editor and game measure in. Sizes, speeds, and positions are in game
units, so an effect authored at the right scale reads correctly in game.

## Skydome

The background environment drawn behind the effect in the preview — the space or sky backdrop.
Matching the skydome to the one the effect will appear against makes its brightness and color read
truthfully.

## Hardpoint

A damageable part of a unit, often with its own visuals, particles, and death effect.

## `Damage_Particles`

In a hardpoint's XML, `Damage_Particles` does not name a particle file. It names the hardpoint
bone whose damaged version appears after that hardpoint is destroyed.

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
