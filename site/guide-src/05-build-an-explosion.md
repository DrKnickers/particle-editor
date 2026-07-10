# Tutorial 5: Build an Explosion

This tutorial starts from a blank particle and builds a complete explosion: a white-hot flash,
an expanding fireball, lingering smoke, and a shell of flying sparks that leave their own trails.

An explosion is the classic stacked effect — every layer is simple on its own, and the result
comes from how the layers are timed against each other. This is also the first tutorial where
child emitters and the Physics tab do real work.

<!-- Media (planned): tutorial-05-opening-result -->

## Outcome

By the end of this page, the particle will contain four coordinated parts:

- a short white-hot flash that sells the instant of detonation;
- a fireball that expands and burns out;
- dark smoke that grows, lingers, and fades last;
- a radial shell of sparks, each trailing its own smoke.

This tutorial stays in the editor preview, like Tutorial 3. At the end there is a short note
about how explosions are referenced by the game.

## What this teaches

- Choreographing several burst emitters against one another with lifetime and delay.
- Animating a texture atlas with the Index track and step interpolation.
- Using render order deliberately in a layered effect.
- Giving particles radial motion with the Initial speed shape in the Physics tab.
- Attaching a Lifetime Child so each moving particle carries its own sub-effect.
- Keeping a multi-emitter effect readable and affordable.

Three reference pages carry the background for this tutorial:
[Particle Generation Types](generation-types), [Motion and Physics](motion-and-physics), and
[Stacking Emitters and Using Children](stacking-emitters-and-children).

## Before You Start

Create a blank particle with **File → New**, then save it into the tutorial mod folder with
**Save As**:

```text
ParticleTutorial\Data\Art\Models\P_TUTORIAL_EXPLOSION.ALO
```

This is a new tutorial particle, not an override of a shipped one. Keep it in the editor for
this lesson. No extraction is needed for this tutorial.

## 1. Make the Flash

A new particle starts with one default emitter. Rename it `Flash` in the **Name** field at the
top of the **Basic** tab.

The flash is the shortest, brightest thing in the effect — the frame or two that reads as
*detonation*:

```text
Generation:  Bursts — Bursts: 1, a small Particles/burst
Lifetime:    very short (a fraction of a second)
Blend mode:  Additive
Color:       white or nearly white at birth, warming toward orange at death
Scale:       starts small, grows fast across its short life
```

Set the Generation mode to `Bursts` in the **Basic** tab's **Generation** section, exactly as in
Tutorial 3's muzzle flash — one burst, so the flash happens once rather than streaming. Use the
Curve Editor's Scale track to make it swell over its brief life.

Preview with the Spawner. At this stage the whole effect is one bright pop — that is correct.

<!-- Media (planned): tutorial-05-flash-burst -->

## 2. Add the Fireball

Add a second root emitter (right-click in the Emitter Tree → **New Root Emitter**) and name it
`Fireball`. This is the rolling ball of flame that follows the flash:

```text
Generation:  Bursts — one burst, a few particles
Lifetime:    short, but clearly longer than the flash
Blend mode:  Additive
Color:       orange, cooling toward red as it ages
Scale:       grows over the lifetime
```

Now give it motion inside the texture itself. Open the Texture/Atlas Picker and choose a
texture with flame or explosion frames. If the frames form an animation sequence, drive them
with the **Index** track in the Curve Editor: put the first frame's number at 0% and the last
frame's number at 100%. Index keys use **step** interpolation by default, which is what you
want — each frame holds and then flips to the next, playing the sequence like a flipbook
across the particle's life (see [Curve Editor Basics](curve-editor-basics)).

Even without an animated sequence, picking a noisy flame frame and letting scale and color do
the work reads well. The fireball should feel like it inflates out of the flash.

<!-- Media (planned): tutorial-05-fireball-index -->

## 3. Add the Smoke — and Put It in Its Place

Add a third root emitter named `Smoke`. This layer is pure Tutorial 2 skills:

```text
Generation:  Bursts — one burst, a few particles
Lifetime:    the longest in the effect, several times the fireball's
Blend mode:  Transparent
Color:       dark gray, slightly warm at birth
Alpha:       visible at birth, fading to 0 at death
Scale:       growing the whole time
```

The smoke is what remains after the fire: it should still be dispersing after the flash and
fireball are gone.

Now use render order deliberately. Emitters draw top-to-bottom in the Emitter Tree — later
emitters draw over earlier ones. Drag the emitters so the additive flash and fireball draw
**after** the smoke; that way the fire brightens through the smoke instead of being covered by
it. One trap from the [Primer](particle-authoring-primer): reordering does not reach a preview
that is already running — restart the preview to see the new order.

<!-- Media (planned): tutorial-05-smoke-render-order -->

## 4. Add the Sparks — and Give Each One a Trail

Add a fourth root emitter named `Sparks`: one burst with more, much smaller particles, additive,
bright yellow-white fading toward orange.

The sparks need to *fly*. Open the **Physics** tab and, in the **Initial speed** section, set
the shape `Type:` to **Sphere** and give it a radius — the radius is the launch speed. Every
spark now gets a velocity pointing somewhere on that sphere, so the burst sprays outward in all
directions. Check `Constrain to surface` for an even shell, or leave it unchecked for a more
ragged, natural spray. (The full explanation of speed shapes is in
[Motion and Physics](motion-and-physics).)

Two touches make the sparks read better:

- In the Generation section, set **Minimum lifetime** and **Maximum lifetime** apart so sparks
  die at different times instead of vanishing as one.
- A little **Gravity acceleration** in the Physics tab bends the straight radial flight into
  arcs — right for a ground explosion; skip it for a space effect.

Now the child emitter. Right-click `Sparks` in the Emitter Tree and choose **Add Lifetime
Child**. The child emits continuously *from each spark* while that spark is alive — this is how
every spark gets its own trail. Keep the child light:

```text
Blend mode:  Transparent
Color:       dark, ember-smoke gray
Lifetime:    short
Scale:       small, and much smaller than the smoke layer
Rate:        a low Particles/second
```

If you want an ending beat as well, **Add Death Child** on `Sparks` pops a tiny flash where
each spark burns out.

<!-- Media (planned): tutorial-05-sparks-children -->

### Keep the Multiplication in Check

A child runs once *per parent particle*: thirty sparks with a trailing child is thirty little
emitters. That multiplication is the point — and it is also how an effect quietly becomes
expensive. If the editor slows or the effect turns into mush, lower the spark count or the
child's rate first. Keep child effects light.

## 5. Choreograph the Timing

Watch the whole effect with the Spawner and tune the layers as a sequence, not as four separate
things. The target rhythm:

```text
Flash:     gone almost immediately
Fireball:  inflates through the flash, burns out quickly
Sparks:    fly while the fireball burns, die off raggedly
Smoke:     revealed as the fire fades, lingers longest, fades last
```

Two timing tools beyond lifetime:

- **Initial spawn delay** (Basic tab, **Emitter Timing** section) holds an emitter back — a
  small delay on the smoke lets the fireball own the first instant.
- **Burst delay** with more than one burst turns a single boom into a rumble of secondary pops,
  if you want one.

## 6. Final Check

Use these questions on the finished effect:

- Does the first instant read as a detonation (flash), not a fade-in?
- Does the fireball feel like it comes *out of* the flash?
- Do the sparks fly in all directions and die at different times?
- Does each spark visibly carry its own trail?
- Is the smoke the last thing standing, and does it fade rather than pop?
- Does the whole effect still read at a normal gameplay camera distance?

<!-- Media (planned): tutorial-05-final-preview -->

## Game Use Note

In a full mod, an effect like this is what plays behind tags such as
`Death_Explosion_Particles` on units and hardpoints — usually paired with a sound event so the
audio and visual land together. Since this tutorial's particle is a new file rather than an
override, the game would need something pointing at it before it appears in play; overriding an
existing shipped explosion at its own path (the Tutorial 1 workflow) is the simplest route if
you want to see your explosion in game.

## Takeaways

An explosion is not one effect — it is a flash, a fireball, smoke, and debris agreeing about
timing. Stack simple emitters, give each one job, use the Physics speed shape for radial
motion, use children for per-particle sub-effects, and tune the layers as a sequence.
