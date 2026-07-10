# Troubleshooting: Why Can't I See My Effect?

The fixes for "nothing is showing" live in several different places — the viewport, the emitter
settings, the file path, the game launch. This page collects them in the order worth checking.

## Nothing Appears in the Preview Viewport

Loading a particle does not, by itself, play it in the viewport.

- **Hold Shift.** The effect spawns at the cursor and follows it while Shift is held. Click
  while holding Shift to drop a copy that keeps playing. This is the single most missed control
  in the editor — see [Basic Editor Controls](basic-controls).
- **Use the Spawner** to repeat the effect automatically while you work, which is easier than
  holding Shift for a long tuning session.
- **Brief effects are easy to miss.** A one-burst flash plays once and is gone; use the Spawner
  to re-trigger it.

## The Effect Plays but Is Nearly Invisible

- **Additive blending makes dark colors disappear.** Additive only ever brightens the scene, so
  a black or near-black additive particle draws nothing. Either brighten the color or switch to
  Transparent blending — see [Blend Modes](blend-modes).
- **Check the Alpha track.** If alpha is at or near zero across the lifetime, the particle is
  there but transparent. See [Curve Editor Basics](curve-editor-basics).
- **Check the Scale track.** A scale near zero renders the particle too small to see; a huge
  scale can also read as nothing when the camera is inside the effect. Zoom out.
- **Check emitter visibility.** Emitters can be hidden in the Emitter Tree; a hidden emitter
  keeps its settings but draws nothing.

## An Edit Doesn't Show Up in the Preview

- **Reordering emitters does not reach a running preview.** If you dragged an emitter to a new
  position in the tree and the viewport looks unchanged, restart the preview (or reload the
  particle) so it picks up the new draw order.
- **Bump map and Decal bump map need the normal texture.** If that texture is missing or flat,
  these modes have nothing to shade against — see [Blend Modes](blend-modes).
- **Physics fields grayed out?** Weather generation disables most of the Physics tab — see
  [Motion and Physics](motion-and-physics).

## The Edit Works in the Editor but Not in the Game

Work through these in order:

1. **Was the game launched with the mod?** A mod folder does nothing unless the game is started
   with its Modpath — see [Setup](setup). This is the most common miss: everything on disk is
   correct, but the game was launched plain and loaded only stock files.
2. **Does the loose path match the internal path exactly?** The override only works when the
   file sits at the same path the game uses internally:
   ```text
   Stock internal path: DATA\ART\MODELS\P_HP_IMPERIAL_DAMAGE.ALO
   Mod loose path:      Mods\ParticleTutorial\Data\Art\Models\P_HP_IMPERIAL_DAMAGE.ALO
   ```
   A typo in any folder name, or the file landing one level too deep or too shallow, silently
   loses the override. See [File Structure](file-structure).
3. **Is the right thing being damaged/fired in game?** A hardpoint damage particle only plays
   once a hardpoint reaches its damage state; a death explosion only plays on death. Make sure
   the moment you are watching for is actually the moment that references your particle — see
   [Where Particles Are Used In-Game](where-particles-are-used-in-game).
4. **Did you save the file you think you saved?** Check the titlebar for the open filename and
   an unsaved-changes marker, and confirm the file's modified time on disk.

## Related Pages

- [Basic Editor Controls](basic-controls) — Shift preview, camera, selection.
- [Setup](setup) — mod folder layout and launching the game with the mod.
- [File Structure](file-structure) — internal paths and loose overrides.
