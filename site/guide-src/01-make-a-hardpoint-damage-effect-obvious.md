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

Use the Preview Viewport to confirm the particle is alive. Loading a file does not, by itself,
play the effect — **hold Shift** to preview it at the cursor, or click while holding Shift to
drop a copy that keeps playing (see [Basic Editor Controls](basic-controls)). If the effect is
hard to see, use the Spawner so the effect repeats while you inspect it.

<!-- Media: tutorial-01-open-override -->

## 2. Find the Smoke Emitter

This particle is a friendly first case: it contains a single emitter, named `smoke`, and that one
emitter is the whole effect. Select it in the Emitter Tree — its properties fill the Property
Panel and its lifetime curves fill the Curve Editor, making it the edit target.

Most shipped particles have several emitters, and then the useful habit is: select one emitter,
observe what it contributes in the Preview Viewport, then move to the next. Tutorial 4 opens a
two-emitter particle and leans on exactly that habit. Particles become much easier to author when
you can connect an emitter row to a visible part of the effect.

## 3. Make the Smoke Bright Green

Color lives on the emitter's Red, Green, and Blue tracks in the Curve Editor (new to the Curve
Editor? [Curve Editor Basics](curve-editor-basics) covers the mechanics). Understand what these
tracks actually are before editing them: each one is a *tint multiplier* applied to the particle's
texture over its life. `1.0` on all three channels shows the texture's own colors; lowering a
channel removes that color component from the result. A colored particle is usually a grayscale
texture times whatever the R/G/B tracks say.

Before changing anything, *read* the stock values: click each channel row in turn and look at the
key values the file actually uses. In this particle you will find the smoke starts at
Red `0.5`, Green `1.0`, Blue `1.0` — a pale blue-green — and all three channels converge to a
dark neutral `0.255` by the end of the life, with a stop near 50% shaping the transition. That
is the whole color story of the stock effect: a tinted birth cooling into gray ash. Getting into
the habit of reading a shipped particle's numbers before touching them is how the rest of this
guide's "open something stock and study it" advice works in practice.

While you are looking, notice two other things about this emitter — both come back in later
tutorials:

- **It streams; it doesn't burst.** The generation is a Continuous stream at 5 particles/second,
  because damage smoke should pour out for as long as the hardpoint is damaged. One-off events
  (explosions, muzzle flashes) use Bursts instead — that contrast is
  [Particle Generation Types](generation-types).
- **The Alpha track starts at `0`**, rises to its peak within the first tenth of the life, and
  then spends the rest fading back out. Smoke that faded only *out* would pop into existence at
  full opacity; the quick ramp-in is what makes each puff appear softly.

Now make the smoke unmistakably green by removing everything that isn't green, one channel at a
time:

1. In the Curve Editor's channel list, click the **Blue** row to focus it.
2. Click the track's key at the left edge (0% — the particle's start) to select it.
3. Step the key's **Value:** spinner down to `0.05`.
4. Repeat for the **Red** channel: focus it, select its key at 0%, set the value to `0.05`.
5. Leave **Green** at `1.0`, and leave Alpha alone so the smoke stays readable.

The Red and Blue tracks each carry two more keys — one around 50% and one at the end (both near
`0.255` in the stock file). Select each of those and set it to `0.05` too, so green dominates from
start to death; if you leave them, the smoke drifts back toward blue-white over its life as those
later keys pull the color up. Watch the Preview Viewport as you go: the smoke should turn a loud,
saturated green.

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
game XML / `Particles.xml` entries or an Alamo proxy placed in an `.alo` model — both defined in the
[Game Concepts Glossary](game-concepts-glossary).

## 5. Verify the Result In Game

Launch the game with the tutorial mod active — the `MODPATH=Mods\ParticleTutorial` launch
argument from [Setup](setup#launch-the-game-with-the-tutorial-mod). Without it, the game loads
only stock files and this check cannot pass.

Start a normal battle where a Star Destroyer can take hardpoint damage. Once a hardpoint begins
using the damage particle, the smoke should be bright green. That obvious color is your proof that
the loose override is loading. If it is still stock-colored, the
[Troubleshooting](troubleshooting) page walks the likely causes in order.

<!-- Media: tutorial-01-ingame-proof -->

## Takeaways

You made the smallest useful particle edit: one file, one visible emitter, one obvious change. That
workflow is the base loop for the rest of the course:

- open or create a particle;
- identify what each emitter contributes;
- change one visual idea at a time;
- save into a mod path the game can load;
- verify the result in context.
