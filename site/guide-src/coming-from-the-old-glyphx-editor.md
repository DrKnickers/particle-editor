# Coming from the Old GlyphX Editor?

This page assumes you already know Mike.NL's GlyphX Particle Editor v1.5. Your existing `.alo`
files still open unchanged, and the file format, parameter meanings, and engine-accurate rendering
carry over. The parts you need to relearn are how you load assets, organize emitters, inspect the
preview, and recover your work.

Each section starts with the old workflow, then gives the current control path. Use the quick table
when something familiar behaves differently; use the grouped sections when you want to find a tool
that did not exist in v1.5.

## Quick Comparison

Check these differences when a familiar action appears to be missing or behaves differently.

| In the old editor | Here | Why |
| --- | --- | --- |
| Seven curve tabs — you saw one channel at a time | One canvas: tick the channels you want and they draw together, with the focused one editable | Compare alpha against scale without flipping tabs. The Y axis is shared, so turning on a large-range channel rescales the view. |
| Typing in a numeric field changed the effect on every keystroke | The value applies when you press Enter or leave the field | Each change is a round-trip to the render host, and committing per keystroke floods it. Arrows, wheel, and drag still apply instantly. |
| Delete removed the emitter and everything under it, immediately | A leaf still goes straight away; deleting a parent or a multi-selection asks first | Deleting a parent takes its whole subtree with it, which the row you selected does not show. |
| Copy put the emitter on the Windows clipboard, so it survived into a second editor window | Copy and paste work within the running editor only | The clipboard now lives in the app rather than the OS. Cross-track and cross-emitter paste in one session are unaffected. |
| A colour swatch set the background, full stop | The colour is the fallback; a selected game skydome takes over, and choosing a colour clears the dome | Backgrounds can now be a real in-game dome, so the two settings are mutually exclusive. |

## Authoring

The emitter and curve data are familiar. The difference is that you can work on more of the
particle as a unit instead of rebuilding or switching views one item at a time.

### Curves: One Canvas, Visibility Checkboxes

**Old editor:** You selected one of seven tabs and saw one curve at a time.

**This editor:** The **Curve channels** list and one shared canvas replace the tabs. A checkbox
shows or hides a curve; clicking the rest of the row focuses the channel whose keys you can edit.
RGB and Alpha can be overlaid. Scale, Index, and Rotation start in solo mode because their ranges
do not match the 0-to-1 colour tracks; after opening one of them, you can tick colour channels back
on for a direct comparison. The toolbar also adds multi-key selection and **Snap to grid**.

**Where to find it:** Use the **Curve Editor** at the bottom of the workspace. See
[Curve Editor Basics](curve-editor-basics#the-channel-list) for focus, visibility, key selection,
and interpolation.

<!-- Media: ref-curve-visibility -->

### Organize Emitters in the Tree

**Old editor:** The emitter tree disabled drag and drop. Reordering usually meant copying an
emitter, pasting it in the new position, then deleting the original.

**This editor:** In the **Emitter Tree**, drag roots to insertion gaps to reorder them. Drag an
emitter onto another row to reparent it into an available Lifetime or Death child slot. You can
Ctrl/Cmd-click, Shift-click, or marquee-select roots and move the multi-selection as one block.
Each selected root carries its whole subtree.

**Where to find it:** Work directly in the **Emitter Tree**. Its footer and context menu also
provide **Duplicate**, **Move emitter up**, and **Move emitter down**. See
[Render Order](particle-authoring-primer#render-order) before changing an effect whose smoke and
additive layers overlap.

<!-- Media: ref-render-order -->

### Duplicate and Link Repeated Variants

**Old editor:** You copied and pasted each variant, changed its Index values by hand, and repeated
the same property edits on every copy. There was no link-group UI.

**This editor:** **Increment Index…** duplicates an emitter while advancing its Index, and its
**Repeat** control can make a run of indexed copies. Multi-select related emitters, right-click,
then use **Set Link Group…** to create or join a group. **Link Group Settings…** chooses which
properties stay shared; unchecked properties remain per-emitter.

**Where to find it:** Both commands are in the Emitter Tree context menu. A new group needs at
least two selected emitters. Joining an existing group adopts that group's shared values, so check
the preview before accepting. See [Link Groups](stacking-emitters-and-children#link-groups-copies-that-stay-in-sync).

<!-- Media: tutorial-05-sparks-children -->

### Import Emitters from Another Particle

**Old editor:** Cross-file transfer used the Windows clipboard, commonly with the source and
destination open in two editor windows.

**This editor:** **Import Emitters…** previews another particle without replacing your active
document. You can select whole branches or individual emitters, then import them together in one
undoable operation.

**Where to find it:** Choose **File → Import Emitters…**, click **Browse…**, tick the branches or
rows you want, then click **Import N selected**. A parent checkbox selects its branch; clear any
children you do not want before importing.

<!-- Media: ref-returning-import-emitters -->

## Assets and Mods

The old editor could open an `.alo` from any folder. What it could not do was resolve textures,
models, and shaders through an ordered mod stack or give you visual shortcuts for finding them.

### Load Assets from Your Mod Stack

**Old editor:** The editor read assets from the detected base-game installation. Opening a particle
from a mod folder did not give the editor a mod-aware asset load order.

**This editor:** Add one or more mods to **Active load order** and arrange them in precedence order.
The top layer wins when several layers contain the same internal path, and **Base game** remains the
last fallback.

**Where to find it:** Use **Mods → Add mod…**. Choose **Expand to full editor** for the full
**Mod Load Order** dialog, **Refresh Mod List** after adding a mod on disk, or **Reset** to return to
base-game assets. This stack controls the editor; it does not launch the game with the same
`MODPATH`. See [Setup](setup#open-the-tutorial-mod-in-the-editor).

<!-- Media: ref-returning-mod-stack -->

### Reuse Frequently-used Textures

**Old editor:** Color and bump textures were text fields with `…` browse buttons. You needed to
remember the filename or find it again on disk.

**This editor:** The **Frequently-used textures** button opens thumbnail lists for **Color** and
**Bump** textures. **Pinned** keeps deliberate choices handy; **Recent** follows the textures you
have used in the active mod.

**Where to find it:** Select an emitter, open **Appearance → Textures**, then use the palette button
beside **Color:** or **Bump:**. The palette is stored per mod and reports **No mod selected** until
you choose an active mod.

<!-- Media: ref-returning-texture-palette -->

### Pick Atlas Frames Visually

**Old editor:** You changed Index keys numerically and watched the particle preview to work out
which numbered frame you had selected.

**This editor:** The **Atlas Frames** dock shows the current texture as numbered tiles. Select one
or more Index keys, then click a tile to assign that frame. Hovering a tile previews it before you
commit the change.

**Where to find it:** Focus the Index channel, select its key or keys, then choose
**View → Atlas Frame Picker…** or use the toolbar toggle. The picker needs a color texture with
multiple **Texture elements**. Use **Step** interpolation when the frames should change cleanly
rather than blend.

<!-- Media: ref-returning-atlas-frame-picker -->

## Scene Context

These controls affect the editor view, not the saved particle. Use them to judge scale, contrast,
ground contact, and lighting against something closer to the effect's eventual game context.

### Choose a Skydome, Ground, and Lighting

**Old editor:** You chose a flat background colour and toggled a fixed ground plane with
**View → Show Ground**. Lighting and the ground material were fixed.

**This editor:** **Background:** can use a base-game or active-mod **Game dome**, with **Space** or
**Land** and primary/secondary model choices. **Ground:** adds height, grid spacing, built-in ground
materials, a solid colour, or a custom texture. The **Lighting** panel exposes sun, fill, ambient,
shadow, and bloom controls.

**Where to find it:** Use the **Background:** and **Ground:** controls above the viewport, then
choose **View → Lighting…**. A solid background colour and a game dome are alternatives: choosing
one clears the other. See [Game Context](particle-authoring-primer#game-context).

<!-- Media: ref-returning-skydome-picker -->

### Place and Move a Reference Object

**Old editor:** There was no model preview beside the particle, so scale and contact usually had to
be checked in-game.

**This editor:** Pick a game or mod unit, structure, or hero and render it at game scale beside the
effect. Clear **Lock object**, click the model in the viewport, then use the move or rotate gizmo.
The Transform fields, **Snap to grid**, and **Reset** provide precise alternatives.

**Where to find it:** Open **Object: None** above the viewport and search the **Reference object**
picker. **Visible** hides the model without clearing it. A locked object cannot be selected, dragged,
or nudged; if the gizmo is missing, unlock the object and click the model. See
[Select and Move a Reference Object](basic-controls#select-and-move-a-reference-object).

<!-- Media: ref-returning-scene-context -->

<!-- Media: ref-returning-reference-gizmo -->

## Testing and Safety

Shift-preview still works. The newer controls add repeatable launch conditions, inspection one
frame at a time, and ways back from a bad edit or interrupted session.

### Launch Effects with the Spawner

**Old editor:** Holding Shift spawned an instance under the cursor, and Shift-click left a placed
copy. There was no programmable launcher.

**This editor:** The **Spawner** can launch bursts from a fixed position with chosen velocity,
lifetime, spacing, jitter, and acceleration. **Manual** mode uses **Spawn now**; **Auto** mode uses
**Enabled** and **Interval** to repeat the test.

**Where to find it:** Choose **Emitters → Spawner** or press **F7**. Use it when an effect is brief,
needs a consistent direction, or would otherwise require an in-game trigger. Shift-preview remains
the faster check when you only need to place one copy.

<!-- Media: tutorial-03-spawner-direction -->

### Pause and Step the Preview

**Old editor:** The preview ran continuously. You could not hold one frame or advance the simulation
in fixed steps.

**This editor:** **Pause** freezes the simulation. **Step** advances one frame and **Step 10**
advances ten, which is useful for checking burst timing, atlas changes, and short-lived children.

**Where to find it:** Use the playback controls in the toolbar or **View → Pause** and
**Step Forward**. The shortcuts are **F8**, **F9**, and **F10**. Pause before stepping; frame-step
commands are ignored while the preview is playing.

### Undo, Recover, and Catch Overloads

**Old editor:** There was no Undo/Redo stack or crash-recovery prompt. A particle that spawned too
much could make the preview unusable without an editor-side overload guard.

**This editor:** **Undo** and **Redo** cover ordinary editor changes, including tree moves and
reference transforms. Autosave snapshots can restore work after an interrupted session, and the
preview overload guard warns before continued spawning overwhelms the editor.

**Where to find it:** Use **Edit → Undo/Redo** or the toolbar buttons. After an orphaned autosave,
**Recover unsaved changes?** offers **Restore recent**, **Restore stable**, and **Discard**. A
restored particle opens as unsaved work under its original filename, so save it after checking the
result.

## Smaller Changes Worth Knowing

These do not need a separate workflow, but they replace limitations you may still work around from
habit.

| Change | What you use now |
| --- | --- |
| Reload assets without reopening | Use View → Reload Shaders or View → Reload Textures after changing files on disk. |
| Resize and keep your layout | Drag the panel dividers; panel sizes and view settings are restored the next time you open the editor. |
| Switch theme and work from the keyboard | Light and dark themes, keyboard focus, and the Keyboard Shortcuts dialog are part of the current UI. |
| Keep the previous file if saving fails | Saves replace the existing `.alo` atomically, so a failed save leaves the previous file intact. |
| Get clearer failure and deletion prompts | Load and save failures stay visible, and deleting a subtree asks before its descendants are removed. |
