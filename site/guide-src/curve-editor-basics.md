# Curve Editor Basics

Every tutorial in this course asks you to change a value "over the particle's lifetime" — fade
the alpha, grow the scale, shift a color. The Curve Editor is where all of that happens, so it is
worth five minutes to learn its parts before Tutorial 1 asks you to use them.

<!-- Media: ref-curve-editor-tour -->

## What a Track Is

A track is a curve that answers one question: *what should this value be at each moment of a
particle's life?* The horizontal axis runs from 0 to 100% — start on the left, death on the
right. It is a percentage, not seconds, so the same curve works whether the particle lives for
a tenth of a second or ten seconds. The vertical axis is the value itself.

Each emitter has seven tracks: **Red**, **Green**, **Blue**, **Alpha**, **Scale**, **Index**,
and **Rotation**. The color and alpha tracks run from 0 to 1. Scale is the particle size,
Index picks the texture atlas frame, and Rotation controls spin.

The curve is made of **keys** — points that pin a value at a time. Between keys, the editor
fills in the value using the key's interpolation (see below). A track with a single key is
simply a constant value.

## The Channel List

The channel list on the side of the Curve Editor shows the seven tracks with a toggle for each.
Red, Green, and Blue display together so you can see a color as one picture. Alpha can be
toggled in alongside them. Turning on **Scale**, **Index**, or **Rotation** initially hides the
other channels because its value range differs from the 0-to-1 color tracks. After that initial
solo view, use the toggles to bring color channels back for a direct comparison on the shared
axis.

Clicking a channel row focuses that channel: its keys become the ones you select and edit.

## Selecting and Editing Keys

With the **Select tool** active (the default):

- **Click a key** to select it. Click an empty part of the canvas to clear the selection.
- The **Time:** and **Value:** spinners show the selected key's numbers. Editing the Value
  spinner is the precise way to set a key — more repeatable than dragging.
- You can also **drag a key** directly on the canvas to move it.
- The **first and last keys of a track are pinned** to 0% and 100%: you can change their value,
  but not their time. Every particle needs a defined value at start and at death.
- Remove keys with the **Delete selected keys** button (or the Delete key).

## Adding Keys

Switch to the **Insert tool** — the cursor becomes a crosshair — and click on the canvas where
the new key should go. The key is added to the focused channel at the time and value you
clicked. Switch back to the Select tool to fine-tune it with the spinners.

## Interpolation

Each key carries an interpolation mode that shapes the curve after it:

- **Linear** — a straight line to the next key. The default for most tracks.
- **Smooth** — an eased curve into the next key, for changes that should feel gradual.
- **Step** — holds the value flat, then jumps at the next key. This is the default for the
  **Index** track, and the right choice for it: an atlas frame is a whole number, and stepping
  between frames plays them like a flipbook instead of blurring between them.

Select a key (or several) and use the interpolation buttons to change its mode.

## Locked Channels

Some shipped particles drive several channels from a single curve — you may open a file and find
a channel marked as locked to another track. A locked channel is read-only and mirrors the track
it is locked to. Use the **Lock-to** control to change this: set it to **None** to give the
channel its own independent curve, or lock it to another track when you want two channels to
always match.

## A Worked Example: Fade Smoke Out

The most common curve in this course, start to finish:

1. Select the smoke emitter in the Emitter Tree.
2. In the channel list, toggle **Alpha** on and click its row to focus it.
3. Click the key at the right edge (100% — the particle's death) and set its
   **Value:** to `0`.
4. If the alpha needs to hold before fading, switch to the Insert tool and click to add a key
   around 50% at the start value, then switch back to the Select tool.

Watch the Preview Viewport: the smoke now thins as it ages and disappears instead of popping
away. That one habit — pick a track, pick a key, set a value, watch the preview — is most of
what particle authoring is.

This alpha fade works because smoke uses **Transparent** blending. An **Additive** effect — a
glow, a flash, fire — ignores the Alpha track entirely, so to fade one out you bring its color
or scale down toward the end instead (see [Blend Modes](blend-modes)).

## The Envelope: Fade In, Then Out

One curve shape appears in shipped particles more than any other, and it is worth knowing by
name. Almost nothing starts at its maximum: the value begins at `0`, rises to its peak within
the first ~10% of the life, and spends the remaining ~90% falling back to `0`.

```text
value
 peak |   /\_
      |  /   \___
      | /        \____
    0 |/              \____
      0%   10%              100%  (particle life)
```

Smoke uses it on Alpha so puffs appear softly instead of popping into existence. Flashes use it
on the color tracks so a detonation has attack and decay instead of switching on and off. When an
effect feels mechanical and you cannot say why, check whether its curves start at full value —
the missing ramp-in is usually the answer.

## Related Pages

- [Particle Authoring Primer](particle-authoring-primer) — why values change over a lifetime.
- [Motion and Physics](motion-and-physics) — the motion controls that pair with these curves.
- [Tutorial 1](01-make-a-hardpoint-damage-effect-obvious) — uses the color tracks for its
  first real edit.
