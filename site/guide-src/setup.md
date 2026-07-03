# Setup

The tutorial course assumes you already have Particle Editor installed.

The editor automatically locates the game install and mod folders. The tutorials use a small loose
file mod called `ParticleTutorial` so every edit is easy to find and easy to remove.

## Create the Tutorial Mod Folder

Create this folder under your Forces of Corruption install:

```text
corruption\Mods\ParticleTutorial\Data\Art\Models\
```

That path mirrors the game's internal asset path:

```text
DATA\ART\MODELS\
```

When a particle file is placed there, the game can load it as a loose override.

## Extract Shipped Particle Files

Most stock particles are packed inside the game's `.meg` archives, especially `Models.meg`. Use an
external MEG archive tool such as [MEG Editor](https://modtools.petrolution.net/tools/MegEditor) to
extract the files used by these tutorials.

For the first two tutorials, extract:

```text
DATA\ART\MODELS\P_HP_IMPERIAL_DAMAGE.ALO
```

Then place the extracted copy here:

```text
corruption\Mods\ParticleTutorial\Data\Art\Models\P_HP_IMPERIAL_DAMAGE.ALO
```

For the shield impact tutorial, extract:

```text
DATA\ART\MODELS\P_SHIELD_BLAST_LARGE00.ALO
```

Then place it beside the hardpoint damage particle in the same `Art\Models` folder.

## Open the Tutorial Mod in the Editor

Launch Particle Editor and choose the tutorial mod when you open files. The editor normally finds
the game install and mod folders automatically. Your job is to keep the loose file path consistent
with the game's `DATA` layout.

If an edit is not showing up in game, first check that the file is in the tutorial mod folder with
the same path and filename as the stock asset you meant to override.
