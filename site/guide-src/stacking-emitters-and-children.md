# Stacking Emitters and Using Children

Most finished effects are not one emitter doing everything — they are several simple emitters working
together. This page covers three ways to organize that: stacking independent emitters side by side,
attaching a child emitter to another emitter so it rides along with each particle, and linking
several emitters into a **Link Group** so copies of one design stay editable as one.

## Stacking Emitters (Siblings)

The straightforward way to build a complex effect is to stack several **root emitters** in the
Emitter Tree. Each one is independent, plays at the same time, and originates from the same place, so
their visuals layer on top of one another. An explosion might be one emitter for the flash, one for
the fireball, one for the smoke, and one for the flying sparks.

Add a new root emitter by right-clicking in the Emitter Tree and choosing **New Root Emitter** —
the tree's **+** button and the Emitters menu offer the same commands, so use whichever is under
your cursor. The habit that keeps this manageable is to keep each emitter responsible for one
visual part, and to select emitters one at a time while previewing so you always know what each
one contributes.

Stacking is the default answer. Reach for child emitters only when a part of the effect needs to be
tied to another emitter's particles rather than standing on its own.

## Child Emitters

A child emitter is attached to a parent emitter and spawns relative to the parent's individual
particles, not to the effect as a whole. That is the key difference from a sibling: a sibling plays
from the origin, while a child follows or reacts to each parent particle wherever it goes.

Each emitter can have up to one child of each kind. Add them by right-clicking a parent emitter in
the Emitter Tree:

- **Add Lifetime Child** — a child attached to each parent particle *for as long as that particle
  is alive*. It travels with the parent particle and emits at its own rate (its own Generation
  setting), so it is how you put a trail on something that moves. If the parent emits fast-moving
  sparks, a lifetime child can give each spark its own smoke trail.
- **Add Death Child** — a child that fires *when each parent particle dies*. It bursts at the spot
  where the parent particle ended, so it is how you make an ending event. If the parent emits embers,
  a death child can pop a small flash as each ember burns out.

Because the child is bound to each parent particle, one parent emitting many particles produces many
little child effects — one trailing or triggering per particle. That multiplication is the whole
point, but it also means a busy parent with a heavy child can add up quickly, so keep child effects
light.

## Link Groups: Copies That Stay in Sync

Sometimes the right design is *several copies of the same emitter*. Tutorial 3's debris is the
canonical case: each debris emitter throws one chunk per burst, so four identical copies give four
independent chunks — but four separate copies would be a maintenance trap, because every future
tweak would have to be repeated on all of them by hand.

A **Link Group** solves this. Linked emitters share their design: edit any one member and the edit
propagates to every other member. To create one, select the emitters (click, then **Ctrl+click**
the others, or **Shift+click** a range), right-click → **Set Link Group…**, and choose **Create
new group**. Members show a colored link dot in the Emitter Tree.

Two related commands complete the tool:

- **Link Group Settings…** lists every parameter with a share/exempt checkbox. Checked fields
  propagate across the group; unchecked (*exempt*) fields stay per-emitter. Exempting is how you
  deliberately vary members — for example, exempt **Initial spawn delay** to stagger the copies in
  time while their look stays locked together.
- **Leave Link Group** removes one member without disturbing the rest.

Joining an *existing* group adopts that group's shared values — the Set Link Group dialog previews
which of your fields would be overwritten before you confirm.

Do not reach for a Link Group to create *variety* — the randomization parameters (Minimum
lifetime, Minimum scale, Rotation variance) already make one emitter's particles differ from each
other, at no cost. A Link Group is for when you need more *instances* of a design: more debris
chunks, more identical jets, more of the same thing happening at once.

## Which One to Use

Ask where the sub-effect should come from:

- From the same place as the rest of the effect, playing alongside it → make it a **sibling** (a new
  root emitter).
- Following each parent particle as it moves → make it a **Lifetime Child**.
- Triggered at the moment each parent particle ends → make it a **Death Child**.
- Several identical instances of one design → duplicate it and make the copies a **Link Group**.

When in doubt, stack siblings. Children are the specialized tool for the cases where a part of the
effect must ride on another emitter's particles.

[Tutorial 3: Build an Explosion](05-build-an-explosion) puts the patterns to work — stacked root
emitters for the layers, and a four-member Link Group for the debris.

<!-- Media: ref-lifetime-child-trail -->
<!-- Media: ref-death-child-pop -->
