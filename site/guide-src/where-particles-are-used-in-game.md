# Where Particles Are Used In-Game

Particles show up in many parts of Empire at War and Forces of Corruption: damage states,
explosions, weapon fire, shield hits, ion effects, ability effects, projectiles, and model-attached
proxies. Knowing the game context helps you decide what the effect should communicate.

## Hardpoint Damage

Hardpoints can reference damage particles that appear when the hardpoint is damaged. The XML tags
often point at bones in the unit model, and geometry or particles parented to those bones are hidden
until the hardpoint reaches the relevant damage state.

The first two tutorials use `P_HP_IMPERIAL_DAMAGE.ALO` because hardpoint damage smoke is easy to
recognize and easy to prove in a normal battle.

For authoring, location and readability matter most. Damage smoke should feel attached to the
damaged part of the ship without hiding too much of the model.

## Death and Explosion Effects

Units and hardpoints can reference death explosion particles. These effects usually combine flashes,
smoke, debris-like motion, and sound. Large set-piece effects, such as an ion cannon-style blast,
use the same authoring concerns at a bigger scale: the player should understand the source,
direction, and result quickly.

## Weapon Fire, Impacts, and Ion Effects

Weapon particles can appear at muzzle flashes, projectile visuals, trails, shield hits, armor hits,
ground impacts, and ion-style disruption effects. These can use similar authoring ideas, but
orientation and surface context matter. Tutorial 4 uses a shield impact because it gives a clear
reason to think about direction.

## Ability and State Effects

Some ability and special-effect visuals use particles to communicate state instead of direct damage:
activation, disablement, recharge, area of effect, or an ionized target. These effects need to read
quickly because the player is usually trying to understand what a unit can do next.

## Projectiles and Muzzle Flashes

Projectile visuals may be meshes, particles, or a mix of both. Some modders use a particle as the
projectile model itself. A common layout uses one group of emitters for the projectile core and trail,
and another group for a muzzle flash that does not inherit the projectile's travel speed.

Tutorial 3 builds both ideas in one particle so you can practice the authoring pattern without
opening a modeling tool.

## Model-Attached Particles and Proxies

An Alamo model can include a proxy that points to a particle. This is a common way to attach effects
to a model, including damage effects, engine or glow details, and custom muzzle flashes. For
authoring, the attachment point is part of the effect: the same particle can read very differently
depending on where the model places it.

The editor tutorials mention proxies as game context, but they stay focused on particle authoring
rather than model authoring.
