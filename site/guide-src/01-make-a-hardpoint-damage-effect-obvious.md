# Tutorial 1: Make a Hardpoint Damage Effect Obvious

This first tutorial makes one intentionally loud edit: it overrides the Star Destroyer hardpoint
damage smoke particle and turns the smoke bright green.

The goal is not to make a pretty effect yet. The goal is to prove that the editor can open a shipped
particle, save a loose override into a tutorial mod, and produce a change that is obvious in game.

<!-- Media: tutorial-01-opening-result -->

## Outcome

By the end of this page, your tutorial mod will contain this loose override:

```text
Data\Art\Models\P_HP_IMPERIAL_DAMAGE.ALO
```

When a Star Destroyer hardpoint is damaged in a normal battle, its damage smoke should be bright
green instead of the stock smoke color.

## What this teaches

- Opening an existing particle from a mod folder.
- Reading the Emitter Tree well enough to find the visible smoke emitter.
- Using the Property Panel and Curve Editor to make an obvious color change.
- Saving a loose override without changing the rest of the game files.
- Checking the result in game with one clear proof point.

## Before You Start

Create a small tutorial mod folder using the normal game path layout:

```text
corruption\Mods\ParticleTutorial\Data\Art\Models\
```

Extract `P_HP_IMPERIAL_DAMAGE.ALO` from the game's `.meg` archives, then place a copy at:

```text
corruption\Mods\ParticleTutorial\Data\Art\Models\P_HP_IMPERIAL_DAMAGE.ALO
```

The editor automatically locates the game install and mod folders. If you need to extract the
shipped `.alo` first, use an external MEG archive tool such as
[MEG Editor](https://modtools.petrolution.net/tools/MegEditor).

## 1. Open the Override Copy

Open the copy in your tutorial mod folder:

```text
ParticleTutorial\Data\Art\Models\P_HP_IMPERIAL_DAMAGE.ALO
```

Use the Preview Viewport to confirm the particle is alive. If the effect is hard to see, use the
Spawner so the effect repeats while you inspect it.

<!-- Media: tutorial-01-open-override -->

## 2. Find the Smoke Emitter

In the Emitter Tree, select the emitters one at a time and watch the Preview Viewport. For this
lesson, look for the emitter that produces the broad damage smoke rather than small sparks or glow.

The useful habit is simple: select one emitter, observe what it contributes, then move to the next.
Particles become much easier to author when you can connect an emitter row to a visible part of the
effect.

## 3. Make the Smoke Bright Green

With the smoke emitter selected, use the Property Panel to find its color controls. If the color is
animated over the particle lifetime, use the Curve Editor to edit the color keys.

For this proof edit, make the smoke an unmistakable green:

```text
Red:   low
Green: high
Blue:  low
Alpha: leave readable
```

This is deliberately exaggerated. Later tutorials will bring the effect back toward a believable
game-art style.

<!-- Media: tutorial-01-green-color-edit -->

## 4. Save the Override

Save the file back into the tutorial mod folder. The important part is that the file remains a loose
override at the same game path:

```text
Data\Art\Models\P_HP_IMPERIAL_DAMAGE.ALO
```

Because this tutorial edits an existing shipped particle at the same path, the game can use it as an
override. No new particle registration is needed for this lesson.

<!-- Media: tutorial-01-save-override -->

### Note: Names, Filenames, and New Particles

The particle name and the particle filename are related, but they are not the same thing. If you want
to make a genuinely separate particle instead of overriding an existing one, use **Save As** in the
editor so the file and internal particle identity are saved intentionally.

A separate particle also needs to be referenced somewhere the game can reach it. Common routes are
game XML/`Particles.xml` entries or an Alamo proxy placed in an `.alo` model.

## 5. Verify the Result In Game

Start a normal battle where a Star Destroyer can take hardpoint damage. Once a hardpoint begins
using the damage particle, the smoke should be bright green. That obvious color is your proof that
the loose override is loading.

<!-- Media: tutorial-01-ingame-proof -->

## Takeaways

You made the smallest useful particle edit: one file, one visible emitter, one obvious change. That
workflow is the base loop for the rest of the course:

- open or create a particle;
- identify what each emitter contributes;
- change one visual idea at a time;
- save into a mod path the game can load;
- verify the result in context.
