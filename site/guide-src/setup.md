# Setup

This course assumes you already have **Particle Editor built and running** on your machine, and
that you have a copy of *Star Wars: Empire at War — Forces of Corruption* installed. (If you still
need to build the editor, follow the build-and-run instructions in the project's README /
CONTRIBUTING guide first — this page picks up once the editor opens.)

The editor automatically locates the game install and mod folders. The tutorials use a small loose
file mod called `ParticleTutorial` so every edit is easy to find and easy to remove.

## Find Your Game Install

Everything below lives inside your Forces of Corruption install. The folder you want is
`corruption` — it holds `swfoc.exe` (the game executable) and the game's `Data\` folder. Common
locations:

- **Steam:** right-click *Star Wars: Empire at War — Gold Pack* in your library →
  **Manage → Browse local files**, then open the `corruption` folder. A typical path is
  `...\steamapps\common\Star Wars Empire at War Gold Pack\corruption`.
- **Disk / GOG installs:** wherever you installed the game, e.g. `C:\Games\...\corruption`.

If you are unsure, search your drive for `swfoc.exe` — the folder it sits in is your `corruption`
folder.

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
can open and edit it. You do this once, with an external MEG archive tool:

1. Download and open an external MEG tool such as
   [MEG Editor](https://modtools.petrolution.net/tools/MegEditor).
2. In it, open the game's `Models.meg` — it is in your `corruption\Data\` folder.
3. Find the file you need by its internal path (use the tool's search or file list). For the first
   two tutorials that is `DATA\ART\MODELS\P_HP_IMPERIAL_DAMAGE.ALO`.
4. Extract it to disk, **keeping its original filename** (`P_HP_IMPERIAL_DAMAGE.ALO`).
5. Copy the extracted file into your tutorial mod folder at the matching path (shown below).

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

1. Launch Particle Editor.
2. Open the **Mods** menu and use **Add mod…** to add `ParticleTutorial` — it then appears in the
   **Active load order**. (The editor auto-detects mods in your game install; if `ParticleTutorial`
   is not listed, choose **Refresh Mod List** first. If it still does not appear, the editor is
   likely pointed at a different game install than the one where you made the mod folder — confirm
   the folder really sits at `corruption\Mods\ParticleTutorial\` under that install.) You can review
   or reorder the whole stack with the **Expand to full editor** button in the Mods menu, which opens
the **Mod Load Order** dialog.
3. Open your particle file with **File → Open…** (Ctrl+O). With the tutorial mod active in the load
   order, the editor reads your loose override from it.

Throughout, keep the loose file path consistent with the game's `DATA` layout so the override lines
up with the stock asset.

## Launch the Game with the Tutorial Mod

> **The most common miss.** A mod folder does nothing on its own — the game only loads it when
> launched with a `Modpath` argument naming it. If your edit looks right on disk but never shows
> up in game, this is almost always why.

This is the step that makes Tutorial 1's in-game check work.

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
