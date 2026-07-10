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

<!-- Media (planned): ref-mod-folder-tree -->

## Extract Shipped Particle Files

Most stock particles are packed inside the game's `.meg` archives, especially `Models.meg`. The
editor works on loose `.alo` files, so a shipped particle has to be extracted to disk before you
can open and edit it. Use an external MEG archive tool such as
[MEG Editor](https://modtools.petrolution.net/tools/MegEditor) to extract the files used by these
tutorials.

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

## Launch the Game with the Tutorial Mod

A mod folder does nothing on its own — the game only loads it when launched with a `Modpath`
argument naming it. This is the step that makes Tutorial 1's in-game check work.

**Steam:** right-click *Star Wars Empire at War: Forces of Corruption* in your library, open
**Properties**, and put this in the launch options:

```text
MODPATH=Mods\ParticleTutorial
```

**Disk / shortcut installs:** add the same argument to a shortcut that launches `swfoc.exe`:

```text
swfoc.exe MODPATH=Mods\ParticleTutorial
```

The path is relative to the `corruption` folder, so it matches the mod folder you created above.
When you are done with the tutorials, remove the launch option and the game goes back to loading
only stock files.

## If an Edit Is Not Showing Up in Game

Check two things in order: first, that the game was actually launched with the `Modpath` argument
above — this is the most common miss; second, that the file is in the tutorial mod folder with the
same path and filename as the stock asset you meant to override. The
[Troubleshooting](troubleshooting) page has the full checklist.
