# File Structure

Particle tutorial files live in a mod folder that mirrors the game's `DATA` paths. The matching path
inside the game archives is the internal path.

For example, this loose file:

```text
Mods\ParticleTutorial\Data\Art\Models\P_HP_IMPERIAL_DAMAGE.ALO
```

maps to this internal game path:

```text
DATA\ART\MODELS\P_HP_IMPERIAL_DAMAGE.ALO
```

Because the loose file uses the same internal path as the stock particle, the game can load it as an
override.

## Where Particle Files Live

Shipped particle `.alo` files normally live under:

```text
DATA\ART\MODELS\
```

They are packed in `Models.meg` in a stock install. Tutorial overrides use the matching loose path
inside the mod:

```text
corruption\Mods\<ModName>\Data\Art\Models\
```

## Loose Overrides

A loose override is a file placed directly in a mod's `Data` folder instead of packed into a `.meg`.
For tutorial work, loose files are useful because you can see, replace, and remove them directly.

The important habit is path matching:

```text
Stock internal path: DATA\ART\MODELS\P_HP_IMPERIAL_DAMAGE.ALO
Mod loose path:      Mods\ParticleTutorial\Data\Art\Models\P_HP_IMPERIAL_DAMAGE.ALO
```

## XML Organization

Particles can also be referenced from XML. Some mods place all object references in the stock-style
XML files, while others use file lists such as
[`GameObjectFiles.xml`](game-concepts-glossary) — a list that tells the game which object XML files
to load — to reference additional XML files for better organization.

When you override an existing particle file at the same path, you usually do not need to touch XML.
When you create a new separate particle, something in the game or a model proxy still
needs to point at it.
