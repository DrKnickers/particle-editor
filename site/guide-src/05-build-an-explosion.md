# Tutorial 5: Build an Explosion

This tutorial builds a complete small explosion — the kind that plays when a fighter or a small
structure dies. It is modelled on two example particles, `P_EXPLOSION_EXAMPLE.ALO` and
`P_EXPLOSION_EXAMPLE_FLIPBOOK.ALO`. They are the *same* explosion built two different ways, and the
only thing that differs between them is how the fireball is made.

An explosion is the classic stacked effect: a flash, a shockwave, a fireball, rising smoke, flying
sparks, and tumbling debris — each simple on its own, all agreeing about timing. It is also where
three bigger ideas come together: how the Physics tab actually moves particles, how the
randomization parameters create variety inside a single emitter, and how a **Link Group** lets
several emitters act as one.

<!-- Media: tutorial-05-opening-result -->

## Outcome

By the end of this page the particle contains six kinds of layer, built from eleven emitters
(twelve if you build the layered fireball in Option B):

- a **flash** — the instant bright warm pop of detonation, two additive emitters at two sizes;
- a **shockwave** — a single expanding ring that sells the pressure of the blast;
- a **fireball** — the rolling ball of flame, built one of two ways (this is the fork below);
- **smoke** — two transparent layers that rise, grow, and fade last;
- **sparks** — bright additive specks that spray outward and burn for a while;
- **debris** — chunks that rocket outward, built as four linked copies of one emitter.

> **The minimum readable explosion.** If eleven emitters feels like a lot, build only the first
> three layers — **flash, fireball, and smoke** — and stop there. Those three already read as an
> explosion on their own; the shockwave, sparks, and debris are polish you layer on once the core
> works. Get the minimum reading right first, previewing after each layer, then add the rest.

The example is a *space* explosion, so nothing falls. Keep that in mind through the physics
sections: every motion choice below is "hot gas and thrown wreckage in vacuum", not "objects
dropping to a floor".

## The Two Versions

The whole difference between the two example files is the fireball:

- [`P_EXPLOSION_EXAMPLE_FLIPBOOK.ALO`](./downloads/P_EXPLOSION_EXAMPLE_FLIPBOOK.ALO) builds the
  fire from a single emitter that plays an animated explosion texture — a flipbook — with the
  **Index** track. It looks the most like pre-rendered fire, and it needs an explosion atlas
  texture.
- [`P_EXPLOSION_EXAMPLE.ALO`](./downloads/P_EXPLOSION_EXAMPLE.ALO) builds the fire from two plain
  additive emitters and lets color and scale do the work. It needs no special texture and is the
  fallback when you do not have a flame flipbook to hand.

Download either file and open it in the editor — this page constantly refers to what the example
actually does, and the fastest way to learn is to click through its emitters while you read.
Everything except the fireball is identical between the two.

## What this teaches

- How the Physics tab's three questions — *where is a particle born, which way does it launch,
  how does its path bend* — map onto Initial position, Initial speed, and Acceleration.
- Two different ways to make things fly outward, and when each is right.
- Using **Minimum lifetime**, **Minimum scale**, and **Rotation variance** to get variety from a
  single emitter instead of adding more emitters.
- Creating and tuning a **Link Group** — several emitters that share one design and are edited as
  one.
- Building fire as a flipbook (Index track) or as layered additive emitters.
- Choreographing burst emitters against one another with lifetime and delay.

Three reference pages carry the background:
[Particle Generation Types](generation-types), [Motion and Physics](motion-and-physics), and
[Stacking Emitters and Using Children](stacking-emitters-and-children).

## Before You Start

Create a blank particle with **File → New**, then save it into the tutorial mod folder with
**Save As**:

```text
ParticleTutorial\Data\Art\Models\P_TUTORIAL_EXPLOSION.ALO
```

This is a new tutorial particle, not an override of a shipped one. Every layer below is a **root
emitter** — add each with right-click in the Emitter Tree → **New Root Emitter** — and every one
uses **Bursts** generation, because an explosion happens once; nothing here streams continuously.

## 1. How This Explosion Moves — Read This First

Before building anything, understand the motion model, because every layer uses the same three
questions. The Physics tab answers them in order:

1. **Where is each particle born?** — the **Initial position** section. A shape (`Type`) picks a
   random birth point for every particle: `Exact` means "always at the emitter's origin";
   `Sphere` with a radius means "a random point inside a ball around the origin" — or, with
   `Constrain to surface` checked, "a random point on the ball's *shell*".
2. **Which way does it launch, and how fast?** — the **Initial speed** section. The same shapes
   apply, but the value rolled here is a *velocity*, not a position. `Exact 0,0,0` means "born
   standing still". A `Sphere` speed means "launched in a random direction". The **Inward speed**
   field adds one more push: along the line from the particle *toward the emitter's center*.
   Positive pulls in; **negative pushes out**. Negative inward speed is this file's main engine
   of "explosion" — it flings particles away from the center along the exact line through their
   birth point.
3. **How does the path bend over time?** — the **Acceleration** section. X/Y/Z is a constant
   push; **Gravity acceleration** is a shortcut for "a push straight down, scaled by this
   number". A *negative* gravity value therefore pushes *up* — that is how this file makes hot
   smoke and fire rise in space without configuring an acceleration vector by hand.

Now look at how the example answers those questions differently per layer — this table *is* the
design of the explosion:

| Layer | Initial position | Initial speed | Acceleration |
|---|---|---|---|
| Flash | exactly at center | not at all (Exact 0,0,0) | no |
| Shockwave | exactly at center | not at all — only its Scale grows | no |
| Fire | inside a ball (r≈7.5) | tiny drift + gentle outward push | rises (gravity −0.9) |
| Smoke | inside a ball (r≈10) | tiny drift + gentle outward push | rises (gravity −0.9) |
| Sparks | inside a ball (r≈10) | random direction + outward push | no (vacuum) |
| Debris | ON a shell (r≈4) | hard outward push (Inward −103) | no (vacuum) |

Two principles fall out of this table:

- **Things that "expand" are often not moving at all.** The flash and shockwave never travel —
  their *Scale track* grows over a short life. Movement you can fake with scale is cheaper and
  easier to control than movement you simulate.
- **There are two different "fly outward" recipes.** The sparks are born *inside* a ball and
  launched in *random* directions — a chaotic spray. The debris is born *on* a shell and pushed
  *radially* by a big negative Inward speed — every chunk flies mostly straight outward from the
  center (with just a little scatter), like shrapnel. Random-direction spray reads as embers; radial launch reads as
  wreckage. Choosing between them is a design decision, not a technicality.

## 2. The Flash and the Shockwave

The flash is the shortest, brightest thing in the effect — the frame or two that reads as
*detonation*. The example makes it from **two** additive emitters, `Flash Small` and
`Flash Large` — the same brief pop at two sizes, the larger one wider *and* dimmer, so it reads
as a hot core inside a softer halo. Rename
the default emitter `Flash Small`, add a second root emitter named `Flash Large`, and give both:

| Parameter | Flash Small | Flash Large |
|---|---|---|
| Generation | Bursts — 2 bursts, 1 particle each, Burst delay 0.08s | same |
| Lifetime | 0.12s (Maximum lifetime, in the Basic tab's Generation section) | same |
| Blend mode | Additive | same |
| Texture | the default master atlas; hold Index frame 2 (a soft round glow) | same |
| Physics | nothing — both position and speed stay Exact 0,0,0 | same |
| Scale — grows across the life (the growth IS the flash) | 46 → 84 | 81 → 119 (the wider, dimmer halo behind the hot core) |
| Color — a warm orange-white (R>G>B) that peaks early, then fades to black | R 0.20 / G 0.10 / B 0.05 (key at ~10%) | R 0.10 / G 0.05 / B 0.02 (same shape, dimmer) |

(These are additive, so the **Alpha track does nothing** — the fade comes entirely from the
color dropping to black, and from the 0.12-second lifetime.)

Why **two bursts 0.08 seconds apart**? A single flash frame can read as a camera artifact; two
overlapping pops a fraction apart read as *detonation*. This trick — a couple of bursts with a
tiny **Burst delay** — recurs in almost every layer of this file. It stays inexpensive: it avoids
adding a second emitter, though the extra burst does spawn one more particle that briefly overlaps
the first — cheap, but not literally free.

Because both emitters are additive, only color and scale matter — the Alpha track does nothing on
an additive emitter (see [Blend Modes](blend-modes)). Fade the flash by letting its color fall
toward the end of the life, not by touching alpha. And study the example's color curve closely,
because it teaches two counter-intuitive things at once:

- **The color starts at 0, peaks around 10% of the life, then decays** — the same fast-in,
  slow-out envelope the smoke's alpha uses. Even a flash is not born at maximum.
- **The peak value is only ~0.2, not 1.0.** Additive layers *stack*: two flash particles, the
  fireball, and the shockwave all brighten the same pixels at t=0, so each layer stays dim to
  keep the sum from clipping to a white square. When an additive effect blows out, lower each
  layer's color rather than fighting the scale.

The shockwave adds one new idea: check **its** example emitter and you will find the
world-oriented option enabled — the flat sprite lies flat in the world instead of turning to face the
camera. A flat ring expanding along the ground plane (its Scale track runs from under 1 to
nearly 190 across 0.375s!) reads as a pressure wave in a way a camera-facing sprite cannot.
Build one additive root emitter named `Shockwave`:

| Parameter | Value |
|---|---|
| Blend mode | Additive |
| Lifetime | 0.375s |
| Generation | 1 burst × 1 particle, Initial spawn delay ~0.05s |
| Appearance | world-oriented ON; default master atlas, hold Index frame 3 (a ring) |
| Physics | Exact 0,0,0 — it never travels |
| Scale | ~0.9 → 187.5 (the whole effect IS the expansion) |
| Color | dim orange — R 0.12 / G 0.06 / B 0.04, peaking ~5% then fading to black |

The small **Initial spawn delay** makes it read as a *consequence* of the flash rather than
simultaneous with it. Tutorial 4's shield ripple uses this same world-oriented trick to lie flat
on the shield surface.

Preview with the Spawner. At this stage the whole effect is one bright pop with a ring pushing out
of it — correct so far.

<!-- Media: tutorial-05-flash-burst -->

## 3. The Smoke — Volume, Rise, and Render Order

Add two transparent root emitters, `Smoke` and `Smoke Slow`. Two, because one smoke cloud with
one lifetime fades as a single object and the eye notices; two layers with different lifetimes
dissolve unevenly, which is what real smoke does:

| Parameter | Smoke | Smoke Slow |
|---|---|---|
| Generation | 3 bursts × 10, 0.05s apart | 3 bursts × 5, 0.05s apart |
| Lifetime | 1.3s max, Minimum 25% | 2.0s max, Minimum 75% |
| Blend mode | Transparent | Transparent |
| Texture | the default master atlas (a soft round puff) | same |
| Color | warm gray → dark gray: R 0.70 / G 0.49 / B 0.29 → 0.10 / 0.10 / 0.10 | same |
| Alpha | 0 → ~0.80 by 10% → fade to 0 | 0 → ~0.50 by 10% → fade to 0 |
| Scale | 14 → ~19 | 18 → ~22 |
| Position | Sphere, radius ≈ 10 | same |
| Speed | a small random drift | same |
| Inward speed | ≈ −3 (gentle outward) | same |
| Gravity | −0.9 (rises) | same |
| Rotation | Rotation track ~0.3 falling to ~0, Random rotation direction ON | same |

Read the physics choices against section 1's model. The **Sphere position** gives the cloud
*volume* — thirty flat sprites born at one point look like one flickering card; born inside a ball
they look like a cloud. The gentle **negative Inward speed** inflates the cloud outward from the
center. The **negative gravity** makes it rise like hot gas. None of these numbers is sacred —
what matters is that each one answers one of the three questions deliberately.

Three curve details are worth studying in the example:

- **The alpha fades *in*, not just out.** The Alpha track starts at `0`, peaks around 10% of the
  life, then spends the remaining 90% falling back to `0`. Without that ramp-in, every puff would
  *pop* into existence at full opacity — the single most common tell of an unpolished effect.
  Fast in, slow out is the standard envelope for almost anything soft.
- **The Scale swells and settles** (about `14 → 19 at 60% → 18`, with *smooth* interpolation)
  rather than growing forever — the puff expands while it is dense, then hangs.
- **The spin is the Rotation track.** A starting rotation speed of ~0.3 easing toward ~0, plus
  the **Random rotation direction** checkbox (Appearance tab, Rotation section) so each particle
  spins clockwise or counter-clockwise at random — the cloud churns without visibly rotating as
  one piece.

Note `Minimum lifetime: 25%` on the fast smoke. That single spinner means every particle lives a
random 25–100% of 1.3 seconds — some puffs vanish quickly, some linger. That is the first
appearance of the idea section 6 makes explicit: *randomize inside the emitter before you reach
for another emitter.*

Smoke is transparent, so here the **Alpha track is the fade** — this is the worked example from
[Curve Editor Basics](curve-editor-basics).

Now use render order deliberately. Emitters draw top-to-bottom in the Emitter Tree — later
emitters draw over earlier ones. Drag the emitters so the additive flash, shockwave, and fireball
draw **after** the smoke; that way the fire brightens *through* the smoke instead of being buried
by it. The reorder applies live, so keep the effect playing while you drag and watch the fire
change from covered to shining through.

<!-- Media: tutorial-05-smoke-render-order -->

## 4. The Fireball — Pick One of Two Ways

This is where the two example files diverge.

### Option A — Flipbook (the `_FLIPBOOK` example)

One emitter named `Fire`. Open the **Atlas Frame Picker** and choose an explosion atlas — a
texture whose cells are successive frames of a burning fireball (the example uses
`p_particle_explosion1.tga`). Then, in the **Appearance** tab's Textures section, set
**Texture elements** to match the atlas grid (the example uses 16 — a 4×4 sheet of frames).

| Parameter | Value |
|---|---|
| Generation | Bursts — 2 bursts × 2 particles |
| Lifetime | 0.7s — long enough to play the frames |
| Blend mode | Additive |

Animate the frames with the **Index** track in the Curve Editor. The example steps through all
sixteen frames — a key per frame, `0` at 0% through `15` at 100% — with **step** interpolation
(the Index default), so each frame holds and then flips, playing the sheet like a flipbook across
the particle's 0.7s life (see [Curve Editor Basics](curve-editor-basics)). One particle now
carries a whole pre-animated explosion, which is why this version needs so few particles.

Two supporting details from the example are worth copying:

- **The ending is a color fade.** The R/G/B tracks hold at `1.0` until about 63% of the life,
  then fall to `0` — the additive way to fade out (alpha would do nothing). The flipbook's last
  frames dissolve to black instead of cutting off.
- **Fixed random rotation is ON** here — the one emitter in the file that uses it. With only a
  couple of flipbook particles alive at once, playing the identical animation, a random *starting
  angle* per particle is what keeps the copies from looking stamped from the same stencil. The
  controls (Appearance tab): **Rotation average** sets the base angle in degrees, and **Rotation
  variance** widens it into a range by *scaling* the average — each particle rolls
  `average × (1 ± variance)`. Note the mechanic: the variance multiplies the average, so it needs
  a non-zero average to act on.

<!-- Media: tutorial-05-fireball-index -->

### Option B — Layered additive (the plain example)

No flipbook texture? Build the fire the way `P_EXPLOSION_EXAMPLE.ALO` does — with motion, count,
and randomness instead of animation frames. Two additive emitters:

| Parameter | Fire | Fire Details |
|---|---|---|
| Generation | 2 bursts × 11 | 3 bursts × 23 |
| Lifetime | 0.4s max, Minimum 76% | 0.35s max, Minimum 14% |
| Position | Sphere r ≈ 7.5 | Sphere r ≈ 5 |
| Inward speed | ≈ −7 (outward) | ≈ −13 (faster outward) |
| Gravity | −0.9 (rises) | −0.9 |
| Color | bright orange → red | same, smaller and busier |
| Delay | none | Initial spawn delay ≈ 0.09s |

`Fire` is the body of the fireball; `Fire Details` is the same idea smaller, faster, more numerous,
and — look at that spinner — with **Minimum lifetime 14%**: its particles live anywhere from 14%
to 100% of 0.35s, so the detail layer flickers and churns instead of pulsing in lockstep. The tiny
spawn delay makes the detail *follow* the body by a frame or two. That is what sells "burning"
without a single animation frame.

Either way, the fireball should feel like it inflates out of the flash.

<!-- Media: tutorial-05-fireball-layered -->

## 5. The Sparks

One additive emitter named `Sparks` — the chaotic spray recipe from section 1:

| Parameter | Value |
|---|---|
| Generation | Bursts — 2 bursts × 20, 0.1s apart |
| Lifetime | 2.0s max, Minimum 22% — embers die at very different times |
| Blend mode | Additive |
| Texture | default master atlas, hold Index frame 1 (a small spark) |
| Color | bright orange — R 1.0 / G 0.50 / B 0.25, peaking almost immediately, fading to black |
| Scale | ≈ 0.7 (small); Minimum scale 50% (Appearance tab, Textures section) |
| Position | Sphere, radius ≈ 10 (inside the ball, not the shell) |
| Speed | Sphere, radius ≈ 10 — a random launch direction for every spark |
| Inward speed | ≈ −10 (outward bias) |
| Gravity | 0 — this is space; embers coast |

The speed **Sphere** is what makes this a spray: every spark rolls its own direction. The negative
Inward speed then biases all of them away from the center so the spray reads as *from the blast*
rather than aimless. And the two randomization spinners — Minimum lifetime 22%, Minimum scale
50% — mean twenty sparks from one emitter arrive in twenty sizes and die across two seconds
instead of blinking out together.

For a ground explosion you would add **Gravity acceleration** here to arc the sparks down; the
example leaves it at zero because it is a space effect.

## 6. The Debris — One Design, Four Linked Copies

The debris is the shrapnel recipe: chunks born **on** a shell (`Constrain to surface` checked,
radius ≈ 4) and launched radially with a huge outward push (**Inward speed ≈ −103** — by far the
fastest thing in the file). Each chunk flies nearly straight outward from the center — a small
Initial speed adds just enough scatter to keep it natural — tumbling as it goes. Build one emitter
named `Debris`:

| Parameter | Value |
|---|---|
| Generation | Bursts — 3 bursts × 1 particle, 0.1s apart, Initial spawn delay 0.1s |
| Lifetime | 1.5s max, Minimum 75% |
| Blend mode | Transparent (dark chunks against the bright fire) |
| Scale | Minimum scale 1% — chunks roll ANY size from tiny to full |
| Position | Sphere r ≈ 4, Constrain to surface ON |
| Speed | Sphere r ≈ 5 (a little scatter so paths aren't perfectly radial) |
| Inward speed | ≈ −103 |
| Rotation | Random rotation direction ON — each chunk tumbles its own way |
| Texture | Index track holds ONE atlas frame — this emitter's chunk sprite |
| Gravity | 0 |

Three details deserve a pause:

- **Minimum scale 1%.** This one spinner gives you everything from dust flecks to full-size
  wreckage out of a single emitter. Before this file adds a second debris emitter, it has already
  extracted the maximum variety from the first one's randomization. That order of operations —
  *exhaust the random parameters, then duplicate* — is the habit to copy.
- **One particle per burst.** Each chunk is an individual event, launched on its own schedule
  (three bursts, 0.1s apart). Wreckage reads as *pieces*, not as a swarm.
- **The Index track picks the chunk's sprite.** With step interpolation and a single held value,
  the Index track simply selects which atlas frame this emitter's particles show — a jagged
  chunk, a plate, a strut. Keep that in mind for the next step, because it is the *reason* the
  example has four debris emitters.

### Now multiply it with a Link Group

One emitter gives you three chunks — but every one of them shows the **same sprite**, because the
Index track is a property of the emitter, not something that can be randomized per particle. Real
wreckage is not the same silhouette repeated. So the example runs **four copies** of the
debris emitter — `Debris`, `Debris_1`, `Debris_2`, `Debris_3` — identical in every parameter
except one: **each copy's Index track holds a different atlas frame** (the example uses frames 5,
6, 7, and 15). Four emitters, four chunk shapes, each rolling its own random sizes and
directions.

But four near-copies would be a maintenance trap: change the color or lifetime of one and you
must remember to change all four, forever. This is exactly what a **Link Group** solves. Linked
emitters share their design — edit any one member and the edit propagates to all of them — while
the fields you deliberately vary (here, the atlas frame) stay per-member.

Create it:

1. Finish tuning the single `Debris` emitter first.
2. Right-click it → **Increment Index…**. In the dialog, leave *Increment by* at **1** and set
   *Repeat* to **3**, then confirm. That chains three duplicates in one undo step — each made from
   the previous copy, so its atlas Index climbs by 1 — leaving four emitters (`Debris`, `Debris_1`,
   `Debris_2`, `Debris_3`) that already hold indexes 0, 1, 2, and 3: a different chunk sprite each,
   for free. (The example file hand-picks frames 5, 6, 7, and 15; the increment gives you
   consecutive frames as a fast start you can re-pick per member afterward.)
3. Select all four: click the first, then **Ctrl+click** each of the others (or **Shift+click**
   for a range).
4. Right-click the selection → **Set Link Group…**. In the dialog choose **Create new group** and
   confirm. Every linked emitter now shows a colored link dot in the Emitter Tree.

Try it: select any one member and change its color or scale. All four update together, and the
preview shows the whole debris field changing at once.

Two more controls complete the picture:

- **Link Group Settings…** (right-click any member) lists every parameter with a checkbox:
  **checked = shared** (edits propagate), **unchecked = exempt** (each member keeps its own
  value). This is how the example's four members keep four different atlas frames while sharing
  everything else: the **Index track** stays per-member, so re-picking one member's frame
  never stamps it onto the rest. The same move works for any deliberate divergence — exempt
  **Initial spawn delay**, for instance, and stagger each copy's timing while the look stays
  locked together.
- **Leave Link Group** (right-click a member) pulls one emitter out of the group without
  affecting the rest.

When you join emitters into an *existing* group, the dialog also previews which of their fields
the group is about to overwrite — read that list before confirming, because joining adopts the
group's shared values.

<!-- Media: tutorial-05-sparks-children -->

### Keep the Multiplication in Check

Eleven or twelve emitters, some bursting 20+ particles, all in the first half second — an explosion adds up
fast. If the editor slows or the effect turns to mush, lower the particle counts first — `Fire
Details` (3×23) and `Sparks` (2×20) are the usual culprits. A readable explosion is almost always
cheaper than it looks like it needs to be.

## 7. Variety Without More Emitters

Step back from the build and notice the pattern in every layer above. The example never has two
particles that look identical, yet its emitters are few and simple. The variety comes from five
randomization parameters, and knowing them is what keeps your emitter count down:

- **Minimum lifetime** (Basic tab, next to Maximum lifetime) — each particle lives a random
  fraction between the minimum and 100% of the maximum. The example uses this *everywhere*:
  14% on `Fire Details` (churn), 22% on `Sparks` (ragged ember deaths), 25% on `Smoke` (uneven
  dissolve).
- **Minimum scale** (Appearance tab, Textures section) — each particle's whole Scale curve is
  multiplied by a random factor between the minimum and 100%. Debris takes this to the extreme
  with 1%.
- **Random rotation direction** (Appearance tab, Rotation section) — a coin flip per particle:
  spin clockwise or counter-clockwise. Paired with a **Rotation track** that eases from a small
  spin rate toward zero, it is what makes the smoke churn and the debris tumble without the whole
  effect visibly rotating one way.
- **Rotation average / Rotation variance** (same section) — a random *starting angle* per
  particle, active when **Fixed random rotation** is checked: each particle rolls
  `average × (1 ± variance)`, with the average in degrees. Because the variance scales the
  average, it needs a non-zero average to act on. The flipbook fireball uses this so its
  identical animation copies never sit at the same angle.
- **Random color addition** (Appearance tab) — adds a random per-particle color offset; the tool
  for "no two embers quite the same shade".

The rule of thumb: **when a layer looks too uniform, reach for these spinners before you reach
for another emitter.** A second emitter costs performance and maintenance; a randomization
parameter costs nothing. Add an emitter only when you need a genuinely different *behavior* — a
different blend mode, texture, timing, or motion — and if you need several behaving copies, link
them.

## 8. Choreograph the Timing

Watch the whole effect with the Spawner and tune the layers as one sequence. Almost everything in
this file happens inside the first half second; only smoke and embers persist past it:

| Time | What happens |
|---|---|
| t = 0.00s | Flash bursts (and again at 0.08s). Fireball ignites. |
| t ≈ 0.05s | Shockwave launches (its Initial spawn delay). |
| t ≈ 0.09s | Fire Details join the fireball. |
| t ≈ 0.10s | First debris chunks launch (more at 0.20s, 0.30s). |
| t ≈ 0.12s | Flash already dead. |
| t ≈ 0.40s | Fireball burnt out. Sparks still flying. |
| t ≈ 1.5s+ | Debris gone; slow smoke still rising and fading. |
| t ≈ 2.0s | Last embers and smoke fade — the effect ends. |

The two tools that build this schedule are **Initial spawn delay** (Basic tab, Emitter Timing —
holds a whole emitter back) and **Burst delay** (Generation — spaces an emitter's own bursts).
Notice that no layer starts later than a tenth of a second: an explosion is front-loaded, and the
*perceived* length comes from how things die, not when they start.

## 9. Final Check

Use these questions on the finished effect:

- Does the first instant read as a detonation (flash), not a fade-in?
- Does the shockwave visibly expand outward from the flash rather than sitting still?
- Does the fireball inflate out of the flash — and, if flipbook, do the frames play cleanly?
- Do the sparks spray chaotically while the debris flies straight and tumbles?
- Are there visibly small and large debris chunks (the Minimum scale roll), with different
  silhouettes (the per-member Index frames)?
- Does editing one linked debris emitter update all four — without stamping one atlas frame onto
  all of them?
- Is the smoke the last thing standing, rising and fading rather than popping off?
- Does the whole effect still read at a normal gameplay camera distance?

Here is the finished explosion both ways — the flipbook fireball (Option A) first, then the layered
additive build (Option B). Every other layer is identical; only the fireball differs.

<!-- Media: tutorial-05-final-preview -->

<!-- Media: tutorial-05-final-preview-layered -->

## Game Use Note

In a full mod, an effect like this is what plays behind tags such as `Death_Explosion_Particles`
on units and hardpoints — usually paired with a sound event so the audio and visual land together.
Since this tutorial's particle is a new file rather than an override, the game would need something
pointing at it before it appears in play; overriding an existing shipped explosion at its own path
(the Tutorial 1 workflow) is the simplest route if you want to see your explosion in game.

## Takeaways

An explosion is layers agreeing about timing — but the deeper lessons travel to every effect you
will build. Answer the three motion questions — *initial position, initial speed, acceleration* —
deliberately for every emitter. Fake motion with Scale when you can; simulate it when you must. Know the two
outward recipes: random-direction spray for embers, shell-plus-outward-push for shrapnel. Exhaust
the randomization spinners — Minimum lifetime, Minimum scale, Rotation variance — before adding
emitters. And when you do need several copies of one design, make it a Link Group so it stays
editable as one.
