# Build Your Own Effect

The tutorials handed you the recipe each time. This page does not — it is where you find out
whether the ideas have stuck, by building something new from a description alone. Reproducing a
tutorial is not the same as being able to make an effect nobody handed you; this closes that gap.

Work in the editor, keep the reference pages open, and give yourself permission to get it wrong a
few times. Nobody authors a good effect in one pass.

## The Brief

**Make an "ion hit" — the effect of an ion weapon striking a ship's hull.** You have seen the
building blocks in Tutorials 1–5; none of the exact values below are given to you on purpose.

What it should communicate, at a glance:

- A **sharp electric flash** at the moment of impact — bright, blue-white, and over almost
  instantly.
- A **short burst of crackling sparks** thrown off the hit point, scattering outward and dying
  quickly.
- A **brief lingering glow** at the hit point that fades over a beat, so the impact leaves a mark
  rather than vanishing cleanly.

That is the whole spec. How you build it is the exercise.

## Plan Before You Place a Single Emitter

Answer these on paper (or in your head) *first* — deciding after you start placing emitters is how
effects turn into a tangle:

- [ ] **How many emitters, and what is each one's job?** Name them. One emitter, one visual role.
- [ ] **For each emitter: does it happen once, or keep going?** That picks its generation type —
      see [Particle Generation Types](generation-types). An impact is an event; a lingering glow
      might be a short stream.
- [ ] **For each emitter: what texture and blend mode?** The flash, sparks, and glow all give off
      light → additive (or transparent for a softer glow). See [Blend Modes](blend-modes).
      *(Optional advanced touch: a scorch that darkens the hull is what the decal blend modes are
      for.)*
- [ ] **For each emitter: how do its particles move?** Sparks scatter (a Sphere on Initial
      speed); the flash barely moves (scale does the work). See [Motion and Physics](motion-and-physics).
- [ ] **What changes over each particle's life?** Sketch the Alpha, Scale, and Color curves before
      you draw them — start bright and snap out, or ease in and fade? See
      [Curve Editor Basics](curve-editor-basics).
- [ ] **What draws on top of what?** Order the emitters in the tree so the flash reads over the
      sparks and glow. See [Stacking Emitters and Using Children](stacking-emitters-and-children).

## Build, Preview, Adjust

Build one emitter at a time and preview after each — hold Shift, or use the Spawner to re-trigger
the impact while you tune. Add the next layer only once the current one reads the way you meant.
Change one thing at a time; if the whole thing turns to mush, hide emitters until you find the one
fighting you.

## Check Your Work

Score the finished effect against the brief, not against a screenshot:

- [ ] Does a first-time viewer read it as an **electric impact**, not a generic puff or explosion?
- [ ] Is the **flash genuinely brief** — a very short lifetime, well under half a second, so it
      snaps rather than lingers?
- [ ] Do the **sparks scatter and die**, rather than drifting or hanging around?
- [ ] Does the **glow or scorch leave a mark that fades**, giving the hit some weight?
- [ ] At a normal gameplay camera distance, does it still **read clearly without swallowing the
      ship**?
- [ ] Could you explain, emitter by emitter, **why** each one is set the way it is? If any answer
      is "I copied it and it looked okay," go back and understand that one — that is the muscle
      this whole guide was building.

If you can hit those, you are no longer following recipes. You are authoring.

## Where to Go Next

- Re-open any tutorial and change its *intent* — recolor the laser to an ion bolt, make the
  explosion a small one, turn the smoke into steam — and see how few settings you actually need to
  touch.
- Study a shipped effect you like: open it, select each emitter, and reverse-engineer the plan
  behind it using the same checklist above.
