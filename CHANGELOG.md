# Particle Editor — Build & Development Notes

This file is split into three parts:

1. **[Changelog](#changelog)** — change events in reverse chronological order, latest on top. Each entry carries a date, the merge-commit short hash on `master`, and (where applicable) the PR number.
2. **[Reference](#reference)** — long-lived build / runtime documentation that doesn't track individual commits.
3. **[Open Issues](#open-issues)** — known gaps not currently scheduled.

Conventions:

- **Commit hashes** point at the merge commit on `master` (or the direct commit, before the PR-everything workflow began at PR #1).
- **PR links** are authoritative for code-review history.
- **Conventional Commits** (`feat:` / `fix:` / `docs:` / etc.) is used in commit messages; section titles below use plain prose for readability.

---

## Changelog


### Reference-object picker — units & structures only, built off the UI thread

*2026-06-15 · [`TODO`](https://github.com/DrKnickers/particle-editor/commit/TODO) · [#TODO](https://github.com/DrKnickers/particle-editor/pull/TODO)*

The reference-object picker now lists **units and structures** instead of every game object — the noise
(props, projectiles, planets, markers, dummies, death-clone debris) is hidden, so the list isn't
thousands of prop-dominated entries. A **search box** narrows it further by name. And the object
catalog — which parses every object XML the active content exposes — now **builds on a background
thread** instead of on the UI thread, so opening the picker after stacking a large submod (e.g. Mod) no
longer **freezes the whole window** for several seconds; the picker shows "Loading objects…" while the
catalog builds and fills in the moment it's ready.

**How we tackled it.** A new `IsPickerListedCategory` predicate ([`src/GameObjectCatalog.cpp`](src/GameObjectCatalog.cpp))
filters `Engine::EnumerateReferenceObjects`. It's **exclusion-based** — list everything with a model
*except* props, projectiles, and the explicit model-bearing non-units (planets / markers / dummies /
death-clones / particles, the new `Excluded` category). Crucially the `Other` catch-all is now *shown*:
a first cut kept only the recognised unit categories and dropped `Other`, which silently lost every unit
whose container tag `categorize()` didn't recognise (Mod's `flagshipunit` capital ships, `groundcompany`,
capturable bunkers, …). The catalog itself still holds every category, so hardpoint/variant lookups are
unaffected. For the freeze: `BuildGameObjectCatalog`
now runs on a worker `std::thread` with an **isolated `FileManager`** (its own MEG handles, so no
seek-race against the UI thread's `FileManager`); `Engine::Update()` ([`src/engine.cpp`](src/engine.cpp:648))
is the *sole* launcher — it harvests the finished catalog (generation-checked, so a build started under a
now-changed mod context is discarded) and only then (re)launches, which is what makes the worker/UI
hand-off race-free. The bridge reference-object-list query reports `building`, and a new
`referenceCatalogBuilding` engine-state field drives the picker's "Loading…" state; the picker re-queries
the list **level-triggered** (while it doesn't yet have a ready list), not on every `engine/state/changed`,
so a gizmo drag doesn't trigger a fetch storm.

**Issues encountered and resolutions.** Two adversarial-review rounds caught design flaws a green build
+ test pass missed (see): (1) launching the worker from the bridge handlers (not just
`Update()`) opened a timing window where a second worker could spawn and the next harvest's `join()`
would block the UI thread on it — *reintroducing the freeze*; fixed by making `Update()` the sole
launcher. (2) `ReloadTextures` runs on F5 / every file-open, not just mod switches, so invalidating the
catalog there made the selected object **vanish** for the async rebuild; fixed by invalidating only when
the mod/submod roots actually change (the texture-only path re-resolves the object in place). (3)
edge-triggering the picker's re-query on `referenceCatalogBuilding` `true→false` hung the picker on
first open, because the snapshot is fetched before the list query sets the engine's "wanted" flag, so the
rising edge is never observed — fixed by level-triggering. The freeze fix itself is the user's live test
(a real Mod + Mod install); the filter, search, and "Loading…" state are covered by Vitest + a
browser-preview check.

---

### Per-submod selection — stack Mod submods in precedence order

*2026-06-14 · [`05d0baf`](https://github.com/DrKnickers/particle-editor/commit/05d0baf) · #184*

A mod whose content is split into submods — like Mod, which keeps `Mod`/`GCW`/`Rev`/`TR` campaign
folders layered on a shared `Core` core — can now **stack several submods in explicit precedence
order**, mirroring how mods are stacked in the game's launch parameters. **Mods ▸ Submods…** (shown
only when the active mod has submods) opens a reorderable checklist: tick the submods to include and
use ↑/↓ to order them — **the top of the list wins** on a shared asset name. Each ticked submod's
`Data\Art` layers on top of the shared core (and the mod root still wins over all of them). The stack
persists across sessions; picking a different mod resets it.

**How we tackled it.** `FileManager` ([`src/managers.cpp`](src/managers.cpp)) holds an ordered submod
stack; `BuildModContentRoots()` resolves `[mod root, submod₁…submodₙ, Core]` (first match wins, so
front = highest precedence) — an empty stack is byte-identical to the prior behaviour. `ModManager`
([`src/ModManager.cpp`](src/ModManager.cpp)) discovers submods by scanning the active mod root for
immediate subfolders with a `Data\Art` tree (excluding `Core`) — generalised, not hardcoded to
Mod's names — and `SelectSubmods` validates the requested list against the discovered set (drops
unknowns + dups, adopts on-disk casing, preserves order) then reuses the mod-select reload chain
(rebuild → persist `LastSubmods` as `REG_MULTI_SZ` → thumbnail-cache clear → engine reload). The wire
surface adds `submods`/`activeSubmods` to `mods/list` plus a `mods/set-submods` request; the React
**Mods** menu opens a new reorderable
[`SubmodsDialog`](web/apps/editor/src/screens/SubmodsDialog.tsx) that batches the final order into one
`mods/set-submods` (one engine reload) on **Apply**.

**Issues encountered and resolutions.** Why "top wins" + user-ordered: a web check confirmed FoC's
official `MODPATH` loads one mod at a time and multi-mod stacking is a community technique with no
single documented precedence rule — so making the editor's stack a user-ordered, first-match-wins list
lets the user reproduce *whatever* precedence their launch setup uses rather than the editor hardcoding
one. Submods + the active stack ride the `mods/list` payload (not the engine snapshot), so the menu
re-fetches after a mod change rather than on every `engine/state/changed`. Switching mods resets the
persisted stack; on launch, a persisted stack is filtered to the submods still present under the
restored mod, preserving order and adopting on-disk casing. `SelectSubmods` no-ops when Unmodded
(submods only mean something under an active mod, keeping native/mock in lockstep). The legacy Win32
Mods menu does not get the affordance (legacy is opt-out, slated for removal) — the
FileManager/ModManager core serves both modes regardless.

---

### Reference-object picker — "model file not found" vs "couldn't load"

*2026-06-14 · [`53a49ef`](https://github.com/DrKnickers/particle-editor/commit/53a49ef) · #183*

Selecting an imported game object whose model file is genuinely **absent** from the active mod/base —
a Name listed in `GameObjectFiles.xml` whose `.alo` isn't shipped (e.g. Mod's `Prop_ElectricBox_00`)
— now reports **"Model file not found in the current mod/base game."** instead of the misleading
"Couldn't load this object's model." The latter is now reserved for a file that *is* present but won't
decode (corrupt / non-mesh).

**How we tackled it.** Split the probe's file-miss path from the decode-failure path: a new
`ModelProbeResult::NotFound` ([`src/GameObjectCatalog.cpp`](src/GameObjectCatalog.cpp:226), returned
on a `getFile` miss) flows through a new `ReferenceObjectStatus::ModelMissing`
([`src/engine.cpp`](src/engine.cpp) `RebuildReferenceObjectMesh`) → `RefStatusToString`
`"model-missing"` ([`src/host/BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp:466)) → the
schema's `ReferenceObjectStatus` union → a distinct picker note
([`web/apps/editor/src/screens/ReferenceObjectPicker.tsx`](web/apps/editor/src/screens/ReferenceObjectPicker.tsx)).
Empty-path and corrupt/non-mesh cases stay `LoadFailed`. Mirrors the existing `referenceObjectStatus`
plumbing; the mock derives `model-missing` from a canned `MOCK_MISSING_MODELS` set.

**Issues encountered and resolutions.** The GameObjectCatalog leaf test asserted a missing file →
`LoadFailed`; that assertion now correctly expects `NotFound`, and the `--probe` dump printer gained
the `NotFound` label. No render-path change — the status is a picker hint only.

---

### Skydome picker — surfaces a dome that won't load, and drops the unused custom-texture slots

*2026-06-14 · [`8692156`](https://github.com/DrKnickers/particle-editor/commit/8692156) · #182*

Picking a game skydome whose model fails to load no longer silently shows the solid-colour
background while the dropdown still reads as if the dome applied. The Background popover now shows
an **active-mode indicator** ("Showing: Game dome" / "Custom texture" / "Solid colour") reflecting
what the renderer is *actually* displaying, plus a per-slot **"Couldn't load this dome's model."**
note when a chosen dome's `.alo` can't be loaded — so a dome absent from the current mod/base, or one
whose asset is missing, is obvious instead of mysteriously inert. The three "custom skydome texture"
slots (the unused "recent images" browse tiles) were removed; the popover is now Game dome (Land/Space
primary + secondary) + Solid colour only.

**How we tackled it.** Mirrored the `referenceObjectStatus` plumbing end to end: a new
`SkydomeSlotStatus` (`None` / `Ok` / `LoadFailed`) set per slot in `Engine::RebuildSkydomeMeshes`
([`src/engine.cpp`](src/engine.cpp:3306)) inside the *same* branches that already
clear / continue / load — so the render path is byte-for-byte unchanged and the status is pure
metadata; `SkyStatusToString` + two snapshot fields in
[`src/host/BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp:465); a `SkydomeSlotStatus` union +
DTO fields in [`web/packages/bridge-schema/src/index.ts`](web/packages/bridge-schema/src/index.ts:198);
the mock derives the status ([`web/apps/editor/src/bridge/mock.ts`](web/apps/editor/src/bridge/mock.ts:367),
default `"none"`); and [`BackgroundPicker.tsx`](web/apps/editor/src/screens/BackgroundPicker.tsx)
splits the previously-overloaded `gameDomeActive` into `gameDomeSelected` (a Name is chosen — drives
the dropdowns) vs `gameDomeRendering` (a slot actually loaded — drives the indicator + the
solid-colour fallback state). That split is the crux: a selected-but-failed dome must read as "not
rendering".

**Issues encountered and resolutions.** The end-to-end Vitest that drives the mock through the live
`engine/state/changed` subscription was flaky — the global mock store wasn't reset between tests and
the initial-snapshot promise could resolve after (and clobber) the post-dispatch broadcast. Fixed by
resetting `useMockEngineState` in `beforeEach` and dispatching via a direct awaited `bridge.request`
(the `ReferenceObjectPicker` test pattern) so the initial snapshot drains first. An adversarial
multi-agent pre-merge review then caught two honesty gaps in the first cut: (1) the new indicator,
derived only from the game-dome status, falsely read "Solid colour" when a **persisted legacy
texture slot** (the engine's `RenderSkydome()` fallback, slots 1–11) was still rendering — fixed by
reading `skydomeSlot` and surfacing it as "Custom texture"; and (2) `RebuildSkydomeMeshes` set `Ok`
without checking `SkydomeMesh::Resolve()`'s return, so a dome that loaded but whose shaders didn't
resolve would render the fallback while reporting "ok" — fixed by mirroring `SetReferenceObject`'s
resolve guard (Clear + `LoadFailed`). The toolbar trigger swatch
([`BackgroundDropdown.tsx`](web/apps/editor/src/components/BackgroundDropdown.tsx)) was likewise
re-gated on render status so it can't show the dome gradient for a failed dome. The engine's custom
skydome slots 9–11 and their persistence are left intact (the legacy `--legacy` UI still uses them);
only the new-UI surfacing was removed, so there is no state migration.

---

### Reference-object fixes — skinned units render, mod Core content loads, a gizmo Reset, grid moves to the Ground popup

*2026-06-14 · [`396464d`](https://github.com/DrKnickers/particle-editor/commit/396464d) · #180*

Four fixes to the imported reference-object feature. **Skinned units now render** — AT-ST, the
Gallofree HTT, infantry, and the like were invisible (their animated bodies were skipped, leaving only
a stray muzzle-flash plane); they now draw in **bind pose**. **A mod's shared `Core` content now
loads** — Mod keeps hundreds of models loose in `Core\Data\Art` that the editor never searched,
so picking them reported "couldn't load"; the editor now resolves the mod root **plus** its `Core`
core. A selected object gains a **Reset** button (position + rotation back to 0), and the **Unit-grid**
toggle moved out of the Object picker into the **Ground** popup, where viewport scenery belongs.

**How we tackled it.** *Skinned (bind pose):* the game's `RSkin*` shaders are 1-bone rigid skinning
(`P = mul(In.Pos, m_skinMatrixArray[Normal.w])`); the alo-viewer builds each skin matrix as
`invBind·current`, which at bind pose collapses to the object world. So the editor stops skipping
`RSkin*` sub-meshes, widens the runtime NORMAL to float4 (the bone index), and binds a **uniform
`objectWorld` skin palette** + `m_viewProjection` ([`src/engine.cpp`](src/engine.cpp), new
`Effect::Handles::hSkinMatrixArray`, [`src/ReferenceObjectMesh.cpp`](src/ReferenceObjectMesh.cpp)
transcoder). B4I4 (4-bone) stays deferred. *Core:* `FileManager` now keeps a list of mod content
roots (`BuildModContentRoots` in [`src/managers.cpp`](src/managers.cpp)) — the mod root + its `Core`
core — searched before the base paths, root-first so existing lookups are byte-for-byte unchanged.
*Grid move:* pure UI relocation to `GroundTexturePanel` (same `engine/set/grid-*` bridge kinds), test
moved with it.

**Issues encountered and resolutions.** The float4×3 skin palette via `SetMatrixArray` was the one
empirical unknown — verified correct by headless capture (AT-ST + Gallofree render in true bind pose,
not collapsed). The `Prop_ElectricBox_00` failure turned out NOT to be a code bug: that model isn't
shipped in the Mod install at all (only in a separate extraction), so it genuinely can't load — a
"model file not found" diagnostic is a follow-up. Mod's per-submod folders (`Mod`/`GCW`/`Rev`/`TR`)
are deliberately **not** auto-loaded — a mod runs one submod at a time and they'd cross-contaminate on
shared asset names; per-submod selection is a follow-up. The skydome "doesn't always apply" report is
the same Core-reachability root cause (now fixed for in-Core domes) plus a swallowed load
failure (surfacing that is the same follow-up).

---

### Rotate handles for the imported-object gizmo — translate arrows + world-axis rotate rings

*2026-06-14 · [`1182386`](https://github.com/DrKnickers/particle-editor/commit/1182386) · #178*

A selected reference object's in-viewport gizmo now **rotates** as well as translates. Alongside the
three colour-matched **translate arrows** (X/Y/Z) it draws three colour-matched **rotation rings** —
drag a ring to spin the object about that world axis. Rings are drawn always-combined with the arrows,
always-on-top, and hover-highlit; the grab is no-jump and accumulates for unlimited multi-turn
rotation, writing the same yaw/pitch/roll the numeric picker spinners own (viewport and spinners stay
in lock-step). The previously-known hover dead-zone is fixed too — the whole hovered arrow, arrowhead
included, is now grabbable.

**How we tackled it.** Engine-owned and additive — the working translate math is left byte-identical.
`PickManipulatorAxis`→`PickManipulatorHandle` ([`src/engine.cpp`](src/engine.cpp:2999)) now ray-scores
all six handles (3 arrows + 3 rings) by *miss ÷ its own threshold*, so an arrow wins ties and a ring's
`|radius − R|` band is comparable to an arrow's ray-to-segment gap; new `ManipulatorRingAngle`
([`src/engine.cpp`](src/engine.cpp:3092)) intersects the cursor ray with each ring's world-axis plane
and returns the in-plane angle in an `(axis+1, axis+2)` basis chosen so each ring maps **1:1 onto its
Euler component** (Z→yaw, X→pitch, Y→roll) — keeping the existing Euler state and the picker in sync
with no quaternion machinery. The host ([`src/host/HostWindow.cpp`](src/host/HostWindow.cpp:3060))
accumulates wrapped per-move ring-angle deltas onto the grab-snapshot rotation, reusing the translate
gesture's commit/persist path verbatim.

**Issues encountered and resolutions.** World-axis Euler-component rings are an *exact* world rotation
only for the outermost axis (yaw/Z); for a mixed pose, dragging the pitch/roll ring rotates in the
partially-rotated frame — accepted (it's the "rings == the picker spinners" contract the world/global
choice implies; a true world-axis quaternion path is a deferred upgrade). Verified by building Debug
**and** Release (the `/WX` gate,) plus a headless render capture, then live-tested by
**synthesising real OS mouse input** (`SendInput`) against the running build and reading the result
back from window screenshots — the computer-use harness won't grant a custom dev `.exe`, so this was
the way to drive the actual drag end-to-end (blue ring → yaw, green ring → a distinct tilt, the rings
staying world-fixed while the object turned).

---

### imported game objects — render correctness: hidden meshes, collision, transparency, normals + a selection box

*2026-06-14 · [`713d418`](https://github.com/DrKnickers/particle-editor/commit/713d418) · #174*

Imported game objects now render the way the game shows them. Meshes flagged **hidden** in the
`.alo` no longer appear — a capital ship's hidden shield/engine/collision meshes were being drawn,
and the hidden shield (forced opaque) **warped the view like a mirror**. A *visible* collision mesh
renders as flat blue (its real `MeshCollision.fx`). **Transparent** sub-meshes (shields, additive
engine glows, glass) blend correctly via a faithful opaque→transparent pass instead of painting
opaque. Objects are no longer **inside-out** (the editor renders right-handed, so the `.alo` needs
`CULL_CW`). Selecting an object draws an **amber bounding box**, and clicking inside that exact box
selects it (the box == the clickable region); clicking empty space deselects + orbits. Picking an
object the editor can't fully render now reports it failed instead of silently showing nothing. A
latent crash — loading a mod whose `MegaFiles.xml` carries the standard `<Info>` element — is fixed.

**How we tackled it.** The `.alo` decoder ([`src/AloModel.cpp`](src/AloModel.cpp)) now reads the
per-mesh `0x402` hidden/collision flags (cross-verified against the `max2alamo` writer + the
`alo-viewer` reader), and a new `AloClassifyShader` maps each game shader to its render phase/blend
(opaque / additive / alpha / heat / shadow / occluded), grounded in the FoC shader corpus.
`Engine::RenderReferenceObject` ([`src/engine.cpp`](src/engine.cpp)) draws two phases reusing the
skydome blend pattern; the selection box + ray-vs-AABB pick share `Engine::ReferenceObjectWorld`.
`FileManager` ([`src/managers.cpp`](src/managers.cpp)) now tolerates non-`<File>` `MegaFiles.xml`
children. A headless `--capture` self-verification path
([`tasks/capture-refobj.ps1`](tasks/capture-refobj.ps1)) was built to confirm the render — and it
flushed out the `MegaFiles.xml` crash.

**Issues encountered and resolutions.** The faithful **Heat** phase (a two-stage scene-composite) is
deferred to its own design pass (rare on reference objects; folds into); a visible heat mesh is
logged + skipped. Two multi-agent reviews (the s46 render work, then the capture/load hardening) drove
the fixes — including a scoped process-kill in the capture script (), dropping an over-built
load-guard layer, and a Release `/WX` follow-up (). The normals/selection-box/drag *interaction*
remains a user live-test (mouse + eye); the gizmo gets a dedicated design pass.

---

### imported game objects + unit grid — drop a real game/mod object in the preview as a scale reference

*2026-06-14 · [`713d418`](https://github.com/DrKnickers/particle-editor/commit/713d418) · #174*

You can now place a real game/mod object in the preview to size effects against. A new toolbar
**Object** picker (beside Background) lists every in-game object by Name — enumerated live from the
active mod/base's `GameObjectFiles.xml`, grouped by category (Vehicle / Infantry / Structure /
Turret / Hero / Prop / Space / Projectile) — and loading one renders it 1:1 via each sub-mesh's own
game shader, placed by its skeleton (a turret's barrel sits horizontally, etc.). Skinned units
(animated troopers) are detected and shown as "skinned — not yet supported" rather than rendered
wrong. Position + rotation are set either with six numeric spinners (X/Y/Z + yaw/pitch/roll in
degrees) **or by dragging the object's red/green/blue axis handles directly in the viewport**. A
separate **unit grid** (toggle + spacing) draws a ground reference grid at known native spacing. The
object, its transform, and the grid persist across restarts. This is the user-facing half of
(/B/C earlier added the `.alo` skeleton decode, the rigid multi-part renderer, and the by-Name
catalog): **** engine state + bridge + persistence + the React `ReferenceObjectPicker`, ****
the unit grid, **** the drag manipulator.

**How we tackled it.** The feature mirrors the skydome dual-slot plumbing
([`src/engine.cpp`](src/engine.cpp), [`src/host/BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp),
[`ReferenceObjectPicker.tsx`](web/apps/editor/src/screens/ReferenceObjectPicker.tsx) cloned from
`BackgroundPicker`). The object list comes from a pure-data leaf
[`src/GameObjectCatalog.cpp`](src/GameObjectCatalog.cpp) that two-phase-parses `GameObjectFiles.xml`
and resolves `Variant_Of_Existing_Type` inheritance case-insensitively (matching the engine). The
grid + the manipulator handles share one new fixed-function line primitive,
[`DrawWorldLines`](src/engine.cpp) (the engine's first `D3DPT_LINELIST`). The manipulator reuses the
existing screen→world unproject for ray-picking — factored into `Engine::BuildCursorRay` and shared
with the cursor-spawn path so the two can't drift — drags along the grabbed axis with a no-jump grab
offset, and persists once per gesture (per-move only updates the live view, throttled to ~30 Hz). The
rotation transform is built **Z-up** (yaw = heading about world Z), matching the engine's up-axis.

**Issues encountered and resolutions.** Model resolution is case-insensitive because vanilla ships
`Variant_Of` references whose casing differs from the parent's declared Name (`V-Wing_Fighter` vs
`V-wing_Fighter`) — an exact match silently dropped ~15 real objects. The rotation was first built
with `D3DXMatrixRotationYawPitchRoll` (a Y-up helper), which mislabelled the spinners in this Z-up
engine; fixed to an explicit Z-up build. The manipulator's persist/dirty fire exactly once per
gesture (on release, RMB-interrupt, or capture-loss) and the per-move snapshot emit is throttled.
**Pending user verification:** the manipulator's drag *feel* — axis-correctness under a real mouse,
the no-jump grab, and the picker spinners tracking the drag live — is mouse-driven and can't be
verified headlessly (the handle *render* and the drag *math* are verified via capture + adversarial
review). The ROADMAP "Shipped" marking is deferred until that live-test confirms the drag.

---

### space skydome — the secondary nebula now composites over the primary starfield

*2026-06-14 · [`c399357`](https://github.com/DrKnickers/particle-editor/commit/c399357) · #171*

Picking a **space** dual-slot dome (a starfield primary + a nebula secondary) now renders
both layers as it does in-game — previously the nebula was invisible. No user-facing
behaviour change for **land** domes.

**How we tackled it.** A one-line compose-order fix in
[`Engine::RenderSkydomes`](src/engine.cpp:2588). The space branch drew the two domes in the
order `{secondary, primary}`; because the skydome pass runs with depth off, the layer drawn
later wins. Every space secondary is an **additive** `MeshAdditiveVColor` nebula (`ONE/ONE`),
and every space primary has an **opaque** star-sphere sub-mesh (`MeshGloss`, classified
`SKYBLEND_OPAQUE` → `ALPHABLENDENABLE=FALSE`). Drawing the secondary first and the opaque
primary second meant the star sphere overwrote every nebula pixel. The game draws all Opaque
meshes before all Transparent ones and tags the primary `Sort_Order_Adjust=-1` (back layer),
so the correct order is **primary first, secondary on top** — in both contexts (land was
already primary-first, so it's byte-identical). Verified against the first-party base assets
(`w_stars_medium.alo` = MeshGloss + sun billboard; `w_stars_nebula_*.alo` = a single additive
`MeshAdditiveVColor`) and the FoC `MeshAdditiveVColor.fx` / `MeshGloss.fx` blend states.

**Issues encountered and resolutions.** The bug only manifests on a space dome, which the
fixed-downward headless `--capture` camera can't show (the dome sits behind the ground) — so
the fix was confirmed via the `[SkyDraw]` per-sub-mesh blend/order trace (opaque MeshGloss now
draws before the additive nebula) plus the first-party asset/shader analysis, with the final
visual left to a live feel-test. Two related items were deferred: the primary starfield's
faint vertical **seam** is the UV-sphere longitude closure baked into the model's tiled UVs
(the game's `.fxo` re-applies `WRAP` at `BeginPass`, so the editor samples identically — it is
an asset/filtering matter, not a render-core bug); and `hLightScale` is still unbound in the
dome draw (benign at the shader default `{1,1,1,1}`, but not engine-driven).

---

### skydome renders for real — two render-blocker fixes + a render-capture bridge

*2026-06-13 · [`70fc6a8`](https://github.com/DrKnickers/particle-editor/commit/70fc6a8) · #165*

The game/mod skydome
shipped code-complete but **render-unverified**. A render-capture feel-test against a
real FoC install found it was broken two ways on any MEG-packed install, both now fixed:
the game-dome picker enumerated **zero** domes, and even when force-loaded the dome rendered
**black**. After the fixes a real dome (`Day_Blue_Sky` → `w_sky00.alo`) renders faithfully —
its blue `W_SkyBlue_clear` base texture, SH-lit, running `Skydome.fx`/`MeshAdditive.fx` 1:1 —
and a space `Stars_High` + `Star_Backdrop_Blue` nebula loads its `MeshGloss`/`MeshAdditive`/
`MeshAdditiveVColor` sub-meshes. This change also adds a debug-gated `debug/capture-frame`
bridge kind so the rendered viewport can be written to a PNG and inspected/diffed offline —
the tool that makes render work self-verifiable (and, paired with the existing pause +
step-frames, enables deterministic particle-lifecycle filmstrips).

**How we tackled it.** Two surgical fixes, no shader edits (the 1:1-no-fork rule holds).
(1) [`SubFile::read`](src/files.cpp:78) — the bounded MEG-entry view — never clamped a read
to its own remaining bytes, so `XMLTree::parse`'s 32 KB chunks on a small packed XML spilled
the **adjacent** MEG entry's bytes into expat, failing the parse; `LoadSkydomeList` then
returned false and the picker saw no domes. A three-line bounds clamp fixes it (also unblocks
game-object XML import). (2) [`loadMaterialTexture`](src/SkydomeMesh.cpp:98) didn't
mirror the engine's `.tga`→`.dds` extension fallback ([`main.cpp`](src/main.cpp:193)): `.alo`
materials name the source `.tga`, the packed game ships compiled `.dds`, so every dome texture
missed and the dome drew black — fixed by trying the `.dds`-swapped name too. The capture
bridge reuses the proven [`AlphaCompositor::CaptureSnapshotToFile`](src/host/AlphaCompositor.cpp:921)
readback through a new [`LayoutBroker`](src/host/LayoutBroker.cpp:543) forwarder, so it captures
the real `Engine::Render()` RT rather than a second renderer; the handler
([`src/host/BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp:1201)) is gated to Debug
builds / `--test-host` because it writes a caller-chosen path.

**Issues encountered and resolutions.** Both bugs were invisible to the session-42 leaf tests
because those parse only *loose* XML with mocks — the over-read needs a real bounded `SubFile`
over a packed MEG, and the texture miss needs the real `.tga`-named-but-`.dds`-shipped install.
A dedicated [`tests/test_subfile_read.cpp`](tests/test_subfile_read.cpp:1) regression test pins
the clamp (verified to fail without the fix). The "empty state blocks" risk (todo.md Risk 2) was
settled empirically along the way: a post-`BeginPass` `GetRenderState` dump showed the app's
render states **are** applied. The `#ifndef NDEBUG` `ALO_MT15_TEST_DOME` env hook was kept (not
removed as the #163 handoff planned) because it is the only way to select a dome for a *headless*
`--capture` render — the React picker only drives an interactive session.

---

### skydome foundation — `.alo` mesh decoder + environment reader

*2026-06-13 · [`72d55bf`](https://github.com/DrKnickers/particle-editor/commit/72d55bf) · #160*

Internal groundwork for (rendering the game's real skydome behind the
preview); no user-facing change yet. Two device-free modules now compile into
the host: `AloModel`, a decoder for the static-mesh + material subset of Alamo
`.alo` models, and `SkydomeEnvironment`, which enumerates skydome GameObjects
from the game/mod's `{Land,Space}{Primary,Secondary}Skydomes.xml` and resolves a
primary + secondary pair by name. A device spike confirmed the game's dome
shaders (`ps_1_1` / DX8 techniques) validate and render on the editor's D3D9Ex
device, so the render core can run each sub-mesh's own game shader 1:1.

**How we tackled it.** `AloModel` ([`src/AloModel.cpp`](src/AloModel.cpp:1)) is
ported from the maintainer's own MIT exporter `max2alamo-2026`, cross-checked
against GlyphXTools/alo-viewer; it keeps the raw 144-byte on-disk vertex blob +
16-bit indices verbatim so the render core (and the future game-object
import) transcode on their own terms. `SkydomeEnvironment`
([`src/SkydomeEnvironment.cpp`](src/SkydomeEnvironment.cpp:1)) reuses the
existing `XMLTree` + `FileManager` (mod→base→MEG) chain; its `MapEnvironment`
struct is the extension point will hang per-map colour-grade params off.
Both are leaf modules with no engine / D3D coupling, unit-tested
([`tests/test_alo_model.cpp`](tests/test_alo_model.cpp:1),
[`tests/test_skydome_environment.cpp`](tests/test_skydome_environment.cpp:1)) and
validated against real install assets. The §5a format questions were closed
against a real FoC install first (scope doc §0).

**Issues encountered and resolutions.** The on-disk vertex stride was the one
open format unknown; the `max2alamo` exporter pinned it to a fixed 144 bytes
(colour is a float4 at offset 80), confirmed against real `.alo`. A code-review
pass caught a 32-bit `long` overflow in the geometry size checks (MSVC `long` is
32-bit even on x64) that a crafted file could use to bypass validation — fixed
with 64-bit products plus count bounds.

---

### Background picker selections persist in the new UI

*2026-06-13 · [`247a716`](https://github.com/DrKnickers/particle-editor/commit/247a716) · #159*

A skydome (bundled or custom-path) or a solid background colour chosen from the
Background picker in the new UI now survives a restart. Previously the new-UI
host applied the selection to the live engine but never wrote it to the
registry, so the next launch fell back to the default — only the legacy UI
persisted these. The startup restore already read the keys; only the write-back
was missing.

**How we tackled it.** The `engine/set/skydome-slot`,
`engine/set/skydome-custom-path`, and `engine/set/background` bridge handlers
([`src/host/BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp:1319)) gained a
registry write that mirrors the legacy writers value-for-value — `SkydomeIndex`
(REG_DWORD), `SkydomeCustomSlot%d` (REG_SZ, deleted when cleared), and
`BackgroundColor` (REG_DWORD) under `HKCU\Software\AloParticleEditor`
([`src/main.cpp`](src/main.cpp:5564)) — so the two UIs round-trip the same keys
that [`HostWindow`](src/host/HostWindow.cpp:2062)'s startup restore already
reads. The write is gated on the same `m_testHost && !m_settingsLive` condition
as `settings/lighting-force-align/set`, so the accessibility harness never
mutates the dev box registry.

**Issues encountered and resolutions.** The custom-path write reuses the legacy
delete-on-empty contract (clearing a slot removes the value rather than storing
an empty string) so `HostWindow`'s sized REG_SZ read doesn't resurrect a stale
path. Background colour was folded in because the solid-colour option lives in
the same Background picker and had the identical gap; the ground picker is a
separate surface, left for a follow-up.

---


### Editor ↔ in-game transparency parity — opaque scene alpha

*2026-06-13 · [`460c070`](https://github.com/DrKnickers/particle-editor/commit/460c070) · #155*

Particles in the preview no longer look **washed-out / see-through** versus
in-game. Transparent and modulate-blended particles now read at the same
opacity the game shows. (This is the transparency half of; the broader
per-map colour-grade / tone difference — the dominant contributor to overall
"darker in-game" — is tracked separately as the game-pipeline work and is
**not** addressed here.)

**How we tackled it.** The session-40 triage
([`docs/superpowers/specs/2026-06-13-mt16-transparency-parity-triage.md`](docs/superpowers/specs/2026-06-13-mt16-transparency-parity-triage.md))
found the cause: the new-UI viewport is a per-pixel-alpha `WS_EX_LAYERED` /
DComp surface, so the scene render target's alpha is *meaningful* (it's the
viewport's opacity over the WebView page). But the particle render path was
written for a game backbuffer, where alpha is ignored — transparent/modulate
blends erode the RT alpha (no `SEPARATEALPHABLENDENABLE` anywhere), the final
`SceneHeat.fx` combine copies that eroded alpha through, and the compositor
faithfully shows it as transparency. The fix masks alpha writes
(`D3DRS_COLORWRITEENABLE = RED|GREEN|BLUE`) around the final scene-combine
draw in [`src/engine.cpp`](src/engine.cpp:1111), then restores full RGBA, so
the RT keeps the opaque alpha it was cleared to. The editor then presents the
engine's RGB at full opacity — exactly what a game backbuffer does (alpha
ignored) — achieving parity independent of blend mode and covering both the
DComp (default) and arch-B layered compositor paths, which sample the
same RT. **No shader was edited** (the no-fork render-parity rule): forcing
alpha GPU-side on the existing blit is a render-state change, not a shader
patch.

**Issues encountered and resolutions.** The mask had to be set **after**
`ID3DXEffect::BeginPass` (not before `Begin`) so the effect's pass-state apply
couldn't clobber it — though `SceneHeat.fx`'s pass sets only
`AlphaBlendEnable` / `AlphaTestEnable` / `ZWriteEnable` / `ZFunc`, never
`ColorWriteEnable`, so it was never in the effect's managed-state set anyway;
the placement is belt-and-suspenders. The explicit restore to full RGBA after
the draw keeps the next frame's particle alpha-blending into the scene RT
unchanged. Verified visually (maintainer feel-test, default composition mode)
plus the native `alpha-compositor-snapshot` readback/composite regression
(3/3) — note that path encodes JPEG, which has no alpha channel, so the
opaque-alpha result itself can only be confirmed by eye, not by that spec.

---

### Ground plane responds to lighting — bump-mapped terrain render

*2026-06-13 · [`056e2ed`](https://github.com/DrKnickers/particle-editor/commit/056e2ed) · #151*

The preview's ground plane now reacts to the **Lighting panel**. Drag the
sun's intensity / azimuth / altitude or the ambient colour and the ground
brightens, dims, and tints in step — previously it was drawn unlit and
ignored the lighting setting entirely (the original question was a
triage: the ground was simply *excluded* from scene lighting, not a sign
that world lighting was broken). The lit ground uses the game's bump-mapped
terrain model — per-pixel sun off a normal map, spherical-harmonic fill,
gloss-gated specular, ×2 terrain brightness — so when you point the editor
at a mod whose terrain textures ship a companion `<base>_bc.dds` normal map,
the ground also picks up per-pixel surface relief and a gloss-masked
highlight. With no such map it stays cleanly matte.

**How we tackled it.** New bundled effect [`GroundLit.fx`](src/Resources/Engine/GroundLit.fx)
adapts the game's `TerrainRenderBump` path (vendored, with the rest of the
Petroglyph FoC shader source, under `reference/foc-shaders/`). It's driven by
a new [`Engine::RenderGroundLit()`](src/engine.cpp) + `m_pGroundEffect`, built
to mirror the existing **skydome effect** pattern 1:1 — `D3DXCreateEffect`
from an `IDR_SHADER_GROUND_LIT` RCDATA blob, cached parameter handles,
`OnLostDevice`/`OnResetDevice` lifecycle, and a compile-failure
graceful-degrade back to the original unlit fixed-function quad. The lighting
reuses the spherical-harmonic matrices the engine already maintains for the
particle shaders (`m_sphLightFill`, `m_lights[0]`). Companion normal maps are
resolved at runtime from the active mod / base game through `FileManager`
(the same chain the skydome textures use), with a 1×1 flat-normal fallback
(alpha 0 → no specular) when a slot has none.

**Issues encountered and resolutions.** (1) The runtime D3DX compiler rejects
`ps_1_x` (`X3539`), so the planned ps_1_1 gloss-fallback technique was cut to a
single `vs_2_0`/`ps_2_0` path (). A cold-launch **stderr** smoke check
caught this in seconds — this GUI exe's stdout isn't captured when launched
detached. (2) First feel-test blew out the specular at grazing angles: the
gloss must be read from the **`_bc` map's alpha** (where this game stores it —
see `TerrainRenderBump.fx`), not the base-colour alpha, and the heightmap
terrain shader multiplies *only diffuse* by 2 (spec is ×1) — I'd carried the
×2 onto spec from the wrong sibling (`TerrainMeshBump`). The no-gloss fallback
also needed alpha 0 (matte), not 1 (). (3) The ground is a single huge
flat quad, so the game's per-vertex tangent-space half-vector smeared the
specular; because the quad is flat and world-axis-aligned, evaluating the
whole model **per-pixel in world space** is exact and avoids tessellation.
**Parity caveat:** this is therefore an *adaptation* of the game's terrain
shader (per-pixel on a preview quad), not the game's literal per-vertex
heightmap-terrain pipeline. For a flat preview aid the result is equivalent
and the maintainer feel-tested it; a stricter "run the game's real shader"
pass belongs with the broader render-parity work (/). The real
`_bc` relief path is also still unverified — the dev box had no mod configured,
so every slot exercised the matte fallback.

---

### Spawner jitter reworked — instances follow a shaped path (arc + squiggle)

*2026-06-13 · [`87a8654`](https://github.com/DrKnickers/particle-editor/commit/87a8654) · #149*

Spawner-emitted instances no longer fly off on a random straight line.
The old one-time **velocity jitter** is gone, replaced by **per-instance
path shaping** over each instance's lifetime. Two controls in the React
Spawner panel steer it: **Acceleration (arc)** — a gravity-like constant
acceleration Vec3 that bends the path into an arc (set Y negative for a
fountain) — and **Squiggle amplitude** (Vec3) + **Squiggle frequency**
(Hz), a smooth per-axis sinusoidal wander layered on top. Each instance
draws its own random phase per axis at spawn, so the siblings in a burst
diverge organically instead of moving in lockstep. The emit point traces
the shaped path, so the particle trail itself arcs and squiggles. Spawn-
point **Jitter position** is unchanged. Zero everything and you get a
plain straight line (the old non-jitter behaviour). Note: these controls
live only in the new React UI; the legacy `--legacy` Win32 dialog keeps
spawn-point jitter and no longer shows a velocity-jitter column.

**How we tackled it.** Motion in [`ParticleSystemInstance::Update`](src/ParticleSystemInstance.cpp:13)
switched from incremental Euler (`m_position += m_velocity·dt`) to an
**analytic** closed form evaluated from elapsed time τ, extracted into a
pure header-only [`EvalSpawnerPath`](src/SpawnerPath.h:39) so it has one
source of truth and is unit-testable headless (the rest of `Update` is
D3D-coupled). `pos(τ) = spawnPos + spawnVel·τ + ½·accel·τ² + Aᵢ·(sin(ωτ+φᵢ)
− sin(φᵢ))`. The `−sin(φᵢ)` term zeroes the squiggle at τ=0 so instances
still emanate from the exact spawn point (no frame-1 teleport). The new
fields thread through [`SpawnerConfig`](src/SpawnerDriver.h:18), the three
[`BridgeDispatcher`](src/host/BridgeDispatcher.cpp:320) JSON converters,
the [`SpawnerParamsDto`](web/packages/bridge-schema/src/index.ts:103), and
[`SpawnerPanel`](web/apps/editor/src/screens/SpawnerPanel.tsx:379).

**Issues encountered and resolutions.** Emitted particles inherit the
instance's **current** velocity ([`EmitterInstance.cpp:557`](src/EmitterInstance.cpp:557):
`velocity += GetVelocity()·parentLinkStrength`). Freezing `m_velocity` and
only moving `m_position` would make trailing particles inherit the stale
launch velocity, so the analytic step writes the **instantaneous** velocity
`vel(τ) = spawnVel + accel·τ + Aᵢ·ω·cos(ωτ+φᵢ)` back each tick. This
constraint is exactly why the closed form (which yields velocity for free)
beat incremental integration. A standalone test
([`tests/test_spawner_path.cpp`](tests/test_spawner_path.cpp:1), 12 cases)
pins the τ=0 spawn-point invariant, the accel parabola + live velocity, the
squiggle's amplitude bound and period return, and the all-zero straight
line.

---

### Link-group indicator reworked — left spine + row tint + group badge

*2026-06-12 · [`73727f2`](https://github.com/DrKnickers/particle-editor/commit/73727f2) · #146*

Emitters that share a link group are now marked three ways at once, all
at **fixed positions** that don't drift with emitter-name length: a
coloured **spine** on the row's left edge, a soft **row tint** in the
group colour, and a small **`G{n}` badge** at the right of the name. The
shared colour + the explicit `G{n}` label make it obvious that members
belong together even when they're scattered across non-adjacent rows.
Clicking either the spine or the badge selects the whole group, and
hovering any member still lights up the rest. This replaces the previous
end-of-name dot and the right-hand **bracket gutter** — the bracket was
absolutely positioned at the *measured longest name*, so a single long
emitter name shoved every bracket out of place.

**How we tackled it.** All in [`EmitterTree.tsx`](src/screens/EmitterTree.tsx:420):
the row computes its group hue once (`colorForGroup`) and, when linked and
unselected, overrides the existing 2px left border (the spine) and sets a
low-alpha inline `backgroundColor` (the tint; 12% at rest, 20% while the
group is hovered) — selection styling still wins when a row is selected.
The badge is a `shrink-0` outlined chip inside the name's flex cell; an
8px transparent hit-strip over the left edge makes the thin spine
clickable. Both call a new `onSelectLinkGroup` prop wired to the existing
`handleSelectLinkGroup`. The whole bracket subsystem
(`computeLinkGroupBrackets`, `laneCount`, the longest-name measurement
`useLayoutEffect`, and the absolute gutter layer) was deleted. The new
cues stay `aria-hidden` (mouse conveniences over the keyboard-accessible
row selection), matching the old bracket's a11y posture so the a11y
goldens are unchanged.

**Issues encountered and resolutions.** The row tint is an inline
`backgroundColor`, which beats the Tailwind `bg-accent/10` hover class on
specificity — so that class is now applied only where there's no inline
tint (`linkHover && spineTintColor === null`) to avoid a silently-dead
class. Group colours come back as 6-digit hex (or `null` for unlinked);
the tint appends an `"1f"`/`"33"` alpha suffix, guarded so a `null` hue
never produces a `"null1f"` string.

---

### Open file's name in the titlebar + "Particle Editor" rebrand

*2026-06-12 · [`8dc8256`](https://github.com/DrKnickers/particle-editor/commit/8dc8256) · #142*

The OS window title now shows the open particle file: `Untitled.alo —
Particle Editor` on a fresh launch, `plasma_blast.alo — Particle Editor`
once a `.alo` is open, and a leading `●` while there are unsaved changes
(`● plasma_blast.alo — Particle Editor`). Because it's the real Win32
title, the open file is also visible in the taskbar button and Alt-Tab,
not just inside the app. The title shows the filename only (no path),
matching the Recent Files basenames. Alongside this, the app is
**rebranded "AloParticleEditor" → "Particle Editor"** across every
user-visible surface (titlebar, browser tab, About dialog, host error
dialogs), and the redundant `AloParticleEditor` brand label that sat next
to **File** in the menu header is **removed** — the menu bar now starts at
File.

**How we tackled it.** React stays the single source of truth for the
title string: a new pure
[`formatWindowTitle`](web/apps/editor/src/lib/window-title.ts:18) helper
owns the format (dirty `●` + basename + app name), the
[`App.tsx`](web/apps/editor/src/App.tsx:111) effect assigns it to
`document.title`, and the **host mirrors that into the Win32 titlebar** via
a new `add_DocumentTitleChanged` handler in
[`FinishWebView2ControllerSetup`](src/host/HostWindow.cpp:1221) that calls
`SetWindowTextW(hMain, get_DocumentTitle())` — registered beside the
existing accelerator handler, with a registration token and a teardown
`remove_DocumentTitleChanged` mirroring the established `accelKeyTok`
pattern (placed in the `if (webView)` unsubscribe group, before
`webView.Reset()`). Keeping the format in TypeScript means it stays
unit-tested and there is exactly one definition. The rebrand was done as
an **enumerated eight-site change**, never a find-and-replace: the
hard-won boundary is that storage/identity strings keep
`AloParticleEditor` — the registry key
[`HKCU\Software\AloParticleEditor`](src/host/BridgeDispatcher.cpp:212),
the `%LOCALAPPDATA%\AloParticleEditor` log path, the
`%TEMP%\AloParticleEditor` autosave + WebView2 user-data folders, the
window class, and the `AloParticleEditor-DevProbe` user-agent — because
renaming any of those would silently orphan every existing user's
settings, recent files, and autosaves. The initial `CreateWindowExW` title
and `index.html`'s `<title>` both became `Particle Editor` so the
pre-WebView2 titlebar reads correctly with no `AloParticleEditor` flash.

**Issues encountered and resolutions.** *(1) The spec's golden-proof claim
was wrong.* The design assumed the UIA goldens would prove the
`SetWindowTextW` mirror via the root window Name, but the HWND/UIA (JSON)
golden lane **auto-skips** in the default composition hosting mode
([`a11y-chrome.spec.ts:11`](web/apps/editor/tests/a11y-chrome.spec.ts:11)),
and the composition (YAML) lane is a DOM snapshot with no Win32
window-Name node. The mirror is instead proven by
[`file-ops.spec.ts`](web/apps/editor/tests/file-ops.spec.ts:183)'s
`document.title` assertion against the **real host**, plus the cold-launch
smoke reading `MainWindowTitle` = `Untitled.alo — Particle Editor` via
`Get-Process` (a non-screenshot API). The dormant HWND lane's goldens stay
`AloParticleEditor` — known rebrand-exposed debt, not CI-gated (the native
harness is local-only); refresh them with an `ALO_HOSTING_MODE=legacy
--update` run only if that lane is ever reactivated. *(2) `toContain`
tautology.* `expect(title).toContain("Particle Editor")` passes against
the **old** `AloParticleEditor` too (it's a substring), so the native
assertions use exact `toBe`. *(3) A vacuous untitled test.* The
"`Untitled.alo` after `file/new`" native test initially passed without
exercising anything, because the spec-level `beforeEach` already
`file/new`s; it was rewritten to first save to a named path, then
`file/new`, so it actually verifies the named→untitled reset.

---

### Overload indicators made consistent — cap-tracking ⚠ glyph, predictive system-load chip, banner exit fix

*2026-06-11 · [`541c79a`](https://github.com/DrKnickers/particle-editor/commit/541c79a) · #140*

The two #138 feel-test bugs, fixed. The emitter-tree ⚠ glyph now warns at
the **configurable** guard cap (Preferences → Preview) instead of the
fixed 10,000, so an effect the gate refuses is always marked on its row
(disabling the guard restores the 10k advisory). Because the per-row
glyph is per-emitter / per-single-instance and the gate compares the
**system total × placed-instance count**, a new **system-load chip**
above the emitter rows covers the gap: it warns whenever the *next*
placement would be refused — `(placed + 1) × estimated total > cap` —
naming both the projected count and the cap, with distinct copy before
the first placement ("This effect ≈ N particles — over the M preview
limit") and after ("Another instance would exceed the preview limit").
And the transient refusal banner no longer flashes the stale "Preview
spawning limited" latch copy as it fades out.

**How we tackled it.** Entirely web-side — **zero engine/bridge changes**.
[`estimateChainLoad`](web/apps/editor/src/lib/chain-load.ts:53) gained an
optional `threshold`; a new
[`useOverloadGuardConfig`](web/apps/editor/src/lib/overload-guard.ts:64)
hook makes the localStorage guard config reactive via an
`alo:overload-guard-changed` `CustomEvent` dispatched inside
[`writeOverloadGuard`](web/apps/editor/src/lib/overload-guard.ts:53) (the
lib owns both ends of the contract, so the glyph and chip update live on
a Preferences edit with no reload). The new
[`SystemLoadChip`](web/apps/editor/src/components/SystemLoadChip.tsx:1)
subscribes to `stats/tick` itself, confining the 4 Hz re-render to that
leaf instead of the whole tree; its trigger is deliberately *predictive*
(`(instances + 1) × load > cap`) because the engine **clears** any
over-cap placed state via the edit-time check, which makes a
current-state condition self-erasing. The system-total estimate reuses
the same `estimateSystemLoad` walk
[`useEstimatedLoadPush`](web/apps/editor/src/lib/use-estimated-load-push.ts:1)
already runs for the gate — one formula, never divergent. Semantic note
worth remembering: with a cap above 10k the glyph now means "**will be
gated**," not "heavy" — effects between 10k and the cap lose the old
advisory glyph (guard-off keeps it).

**Issues encountered and resolutions.** The session-37 handoff blamed the
stale-banner bug on a spawn leaking through the estimate-push staleness
window and suggested an engine-side latch reset in `Engine::Clear()` —
but that reset **already existed** ([`src/engine.cpp`](src/engine.cpp:247)),
and the host emits the refusal event and `overload=false` in the same
4 Hz poll. The real bug was in the web render layer: when the 5 s refusal
window expires, [`usePresence`](web/apps/editor/src/lib/use-presence.ts:1)
keeps the banner mounted for the 150 ms exit fade, and the
`refusal ?? latch` ternary fell through to the latch copy for the whole
fade. Fix: freeze the rendered variant at exit start via a render-time
ref (`lastShownRef`), plus `setOverload(false)` in the refusal handler as
a delivery-order belt-and-suspenders (a genuinely latched engine
re-asserts on the next tick). The precedence contract — the latch
re-asserts after the refusal window if still latched — is pinned by an
updated test that models the real 4 Hz re-assert stream. Separately, the
plan's literal test values for the cap-tracking glyph were wrong against
the mock fixture (its child emitters carry default multipliers that push
the chain over 10k regardless of cap); the implementer caught it and the
tests now document the fixture math (`Smoke puff` id 2 at the default
10/s × 3 s = ×30). Finally, the native `preview-overload` specs flake at
the **tail** of the full single-process harness (cumulative Debug-host
pressure,) — they pass 5/5 in isolation and exercise only the
unchanged engine gate, so this is environmental, not a regression of
this change.

---

### Preview overload guard now refuses oversized spawns preemptively (estimate gate + clear)

*2026-06-11 · [`a50db90`](https://github.com/DrKnickers/particle-editor/commit/a50db90) · #138*

The overload guard gains a **preemptive** layer on top of the existing
reactive runtime budget. Before this change the editor let a heavy
effect start, then suppressed per-frame spawning once the live
population exceeded the cap — you watched it grind through a plateau.
Now, when the *estimated* alive-particle load for the whole preview
would exceed the guard cap, the editor refuses to make it worse and
cleans up. Placing a new instance (Shift-click or the spawner panel) is
refused when *(already-placed instances + 1) × the effect's estimated
alive-particles* exceeds the cap — the gate counts the **cumulative**
preview, not just the new instance. Cranking a parameter on an already
placed effect so that *placed × new-estimate* goes over the cap clears
the preview the same way (the **edit-time** gate). On either refusal the
**whole preview is cleared** — you see "nothing spawned, the effect is
too big," not a heavy partial scene — and a **transient ~5 s banner**
names both the estimate and the cap. An interval/auto spawner that is
refused **self-disables** after the first refusal (one banner, not one
per cycle); you re-arm it deliberately after lightening the effect or
raising the cap. The guard's **runtime budget remains the backstop**:
the static estimate cannot predict every runtime multiplication (deep
chains, death-spawn storms), so the actual-count suppression + decay +
latch banner stay behind the new gate. Disabling the guard
(Preferences) bypasses the gate entirely, consistent with #123's
"uncapped is an explicit power-user choice."

**How we tackled it.** The split is **host-enforced, web-supplied**: the
estimate formula lives ONLY in
[`web/apps/editor/src/lib/chain-load.ts`](web/apps/editor/src/lib/chain-load.ts:1)
(`estimateSystemLoad` — the same Little's-law walk that drives the ⚠
chain-warning glyph, so the gate and the glyph can never disagree). The
web pushes one number per tree update —
`engine/set/estimated-load { perInstance }`, via the
[`useEstimatedLoadPush`](web/apps/editor/src/lib/use-estimated-load-push.ts:1)
hook wired into
[`EmitterTree.tsx`](web/apps/editor/src/screens/EmitterTree.tsx:1285) —
and the engine multiplies by its live placed-instance count to run the
gate. All enforcement happens inside the single `SpawnParticleSystem`
choke point ([`src/engine.cpp`](src/engine.cpp:191)) plus
`SetEstimatedLoad`'s edit-time check, so every placement path is
covered. The key invariant: `SpawnParticleSystem` now returns
**`nullptr` if and only if the gate refused** (it was previously a
4-line never-null function), so callers treat null as a refusal —
`SpawnerDriver` self-disables on it ([`src/SpawnerDriver.cpp`](src/SpawnerDriver.cpp:197)).
Refusals are a one-shot engine record polled by the dispatcher's 4 Hz
stats path and emitted as `engine/overload/refused`
([`src/host/BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp:5344)),
driving a transient variant of the existing `OverloadBanner` (the latch
banner is unchanged; the refusal takes precedence for its 5 s window).
`BridgeDispatcher` caches the last pushed estimate and reapplies it on
`SetEngine`, mirroring the guard-config cache.

**Issues encountered and resolutions.** The native regression suite
surfaced a real interaction between the live push hook and the four
existing **runtime-backstop** specs (the two 1e9 "bomb" specs and the
two cap-bound specs). Those specs proved the backstop by cranking an
emitter to rate 1e9 *while the guard is enabled*, then spawning. With
the push hook live, that rate-crank fires `emitters/tree/changed` → the
hook recomputes an astronomical `estimateSystemLoad` → pushes it → and
the subsequent spawn is now (correctly) **refused preemptively** by the
new gate, so the live population stays at 0 and the backstop never
engages. A defensive `engine/set/estimated-load { perInstance: 0 }`
reset at the top of each bomb spec is **not** sufficient on its own,
because the hook re-pushes the huge estimate on the rate-crank that
follows the reset. The resolution is to accept that the preemptive gate
makes the old visible-rate backstop scenarios **unreachable by design**:
any spawn the estimate can see is refused before the runtime budget
engages. The four obsolete bomb / cap-bound specs were therefore
**retired**, since the five NEW gate specs (cumulative / edit-time /
single-instance-repeat / spawner-churn-stop / disabled-guard-bypass)
prove those over-budget cases are now *prevented* up front. The
engine's runtime particle/instance budget is **kept in the code** as
belt-and-suspenders for the hard-to-trigger case the estimate
undercounts (deep runtime chains, death-spawn storms) — it just no
longer has a dedicated regression spec, because no visible-rate test can
reach it past the gate.

---

### Selected curve key: inverted core replaces the grow + drop shadow

*2026-06-11 · [`12c22bd`](https://github.com/DrKnickers/particle-editor/commit/12c22bd) · #136*

The selected key on the focus curve no longer balloons and casts a dark
drop shadow (which read as a smudge at marker scale), and the
interim ring concept (#132, never merged) is superseded. The selected
dot now uses the **inverted core** treatment — the standard
selected-anchor from vector editors: a compact dot whose fill flips to
the canvas background with a thick stroke in the channel's own colour.
Crisp, zero blur, equally legible on both themes.

**How we tackled it.** In the focus-layer key render
([`src/screens/CurveEditor.tsx`](web/apps/editor/src/screens/CurveEditor.tsx:1988))
the marker styling is now a three-way precedence ladder —
locked-channel hollow (`focusReadOnly`, unchanged) > selected inverted
core (r 6.5, `fill: var(--curve-marker-core)`, channel-colour stroke
2.5) > unselected (r 5, solid channel fill). The old
`saturate()`/`drop-shadow()` CSS is gone; only the r transition
remains. The core's fill is a new semantic alias token
([`--curve-marker-core`](web/apps/editor/src/styles/tokens.css:77)
→ `var(--panel)`) — aliased rather than hardcoded so a future curve
canvas background change can't silently strand the marker core on the
old colour. Verified the canvas area sits on `.panel` with no
background override on `.ce-body` or the canvas wrapper.

---

### Morphing follower curves no longer paint over the focus channel's keys

*2026-06-11 · [`e8c1a14`](https://github.com/DrKnickers/particle-editor/commit/e8c1a14) · #135*

When a curve that other channels are locked to is edited, the followers
"catch up" with a morph animation. That morphing line used to render ON
TOP of the focus channel's curve and key markers — most visibly right
after dragging a master key, when the follower's line swept over the key
just edited. Morphing non-focus channels now render below the focus
layer, like their static counterparts always have.

**How we tackled it.** The morph overlay groups were rendered after the
focus layer in
[`src/screens/CurveEditor.tsx`](web/apps/editor/src/screens/CurveEditor.tsx:1878)
(an "in-flight shape always visible" choice that was wrong for follower
morphs — SVG paints in document order). The overlays now render between
the background layers and the focus layer, taking the same stacking
position as the static layers they replace; the morphing channel's own
static layer is visibility-hidden during its morph, so for the focus
channel the overlay stands in at the correct z-position anyway. Within
the overlays the focus channel sorts last, so a simultaneously-morphing
focus channel (e.g. undo restructuring several tracks) still draws above
morphing followers. A regression test pins the document order
(`compareDocumentPosition`).

---

### Spinner arrow buttons: contained background + a press state

*2026-06-11 · [`297ffd0`](https://github.com/DrKnickers/particle-editor/commit/297ffd0) · #133*

Two fixes to the spinner up/down arrows. (1) Pressing or hovering an
arrow no longer breaks the field's rounded border — the arrow column
sat flush against the input's right edge, so its hover/active
background painted over the right border line and the rounded corners;
the column is now inset 1px inside the border and clipped with matching
rounded-right corners, so the button background stays contained. (2) The
arrows now show a clear **press state** (accent-soft background + accent
glyph on `:active`), so a click or hold-to-repeat gives visible
feedback — previously only hover was styled.

**How we tackled it.** In
[`src/primitives/Spinner.tsx`](web/apps/editor/src/primitives/Spinner.tsx:364)
the absolutely-positioned arrow column changed from `right-0 top-0`
(full height, flush to the edge) to a 1px inset (`top/right/bottom: 1`,
`width: ARROW_W - 1`) with `overflow-hidden rounded-r-[3px]`. The 3px
radius is concentric with the input's 4px `rounded` corner, exactly 1px
inside — the border sits in the gap, and `overflow-hidden` clips the
square-cornered button fills to the rounded column. The press state is
an `active:bg-accent-soft active:text-accent` pair on the arrow buttons
(the clip now keeps that fill inside the box too).

---

### Overload guard default lowered to 10k; harness bomb specs pinned to 1k

*2026-06-11 · [`f85cdc2`](https://github.com/DrKnickers/particle-editor/commit/f85cdc2) · #134*

The preview overload guard's default particle cap drops from 15,000 to
**10,000** — user feel verdict: even a 15k simultaneous burst makes the
editor struggle, and 10k keeps the preview responsive while staying far
above what vanilla effects use. (The cap remains fully adjustable in
Preferences → Preview, 1,000–1,000,000, and the instance ceiling still
derives at 20:1 → 500.)

**How we tackled it.** The default lives in two deliberately-synced
places — [`Engine::kDefaultMaxPreviewParticles`](src/engine.h:104) and
[`OVERLOAD_GUARD_DEFAULT`](web/apps/editor/src/lib/overload-guard.ts:16)
— both now 10k with cross-reference comments. Separately, the native
overload regression specs were re-pinned: the two 1e9-bomb specs now pin
the cap at **1,000** (the clamp minimum), the mid-run-lowering spec
starts at 2k → 1k (was 50k → 5k), and the disabled-guard spec runs a
1k/s rate (was 4k/s).

**Issues encountered and resolutions.** The re-pin is not cosmetic — it
fixes real harness instability: heavy Debug-host plateaus at the tail of
the full 180-spec single-process run were **killing the test host**
(page crashes mid-bomb with failed cleanup poisoning the shared WebView2
user-data folder, and dumpless mid-run host exits — the
environmental signature; reproduced repeatedly at 15k pins, and the
50k-start spec died even after the bombs were cheapened). The behaviours
under test (plateau, latch, recovery, refusal, decay, uncapped) are all
cap-independent, so low pins prove the same contracts. Result: the
overload block dropped from ~5–7 minutes of grinding to ~30 seconds and
the full harness from ~5–8 min to ~2 min, with the host-death flakes
gone.

---

### Group-drag now live-updates the Time/Value spinners

*2026-06-11 · [`d5b6f1d`](https://github.com/DrKnickers/particle-editor/commit/d5b6f1d) · #130*

When multiple curve keys are selected, dragging the group around now
live-updates the Time and Value spinner readouts (which show the
selection average) in real time, the same way a single-key drag always
has. Previously the numbers froze at the pre-drag average until release.

**How we tackled it.** The renderer's group-drag branch
([`src/screens/CurveEditor.tsx`](web/apps/editor/src/screens/CurveEditor.tsx:1370))
returned after updating the preview without firing any live-move
callback — only the single-key branch fired `onKeyDragMove`. Added an
`onGroupDragMove(dTime, dValue)` callback (fired once past the slop
threshold); the panel stashes it in a `liveGroup` state and
[`multiSelected`](web/apps/editor/src/components/CurveEditorPanel.tsx:845)
averages the LIVE shifted positions while a group drag is active,
falling back to the committed average otherwise. The per-key
shift+clamp math (borders pinned in time but shifted in value, interior
keys clamped inside the endpoints, value clamped to the channel's
spinner bounds) was extracted into a single `computeGroupMoves` helper
consumed by BOTH the commit (`applyGroupShift`) and the live average, so
the displayed numbers always match what will commit — and there's now
one source for the clamp logic the morph drag-suppression recorder
depends on. `liveGroup` clears on drag-end and cancel.

**Issues encountered and resolutions.** The Time/Value spinners are
keyed by their value, so they remount on every live change — the new
tests must re-query the input each read rather than caching a reference
(a cached handle goes stale on the remount and reads the pre-drag
value). Verified the `applyGroupShift` refactor is commit-identical
(same `Math.fround` engine times, same clamps) so the morph recorder's
`movesMatch` is unaffected.

---

### Curves animate smoothly on structural changes (morph + key choreography)

*2026-06-11 · [`dc7f81f`](https://github.com/DrKnickers/particle-editor/commit/dc7f81f) · #128*

Editing a curve no longer snaps. Adding or deleting a key, pasting,
nudging values with the spinners, undo/redo, switching interpolation
mode (linear ↔ smooth ↔ step), or watching a locked follower re-mirror
its master — the curve now **morphs** smoothly from its old shape to
its new shape over ~180 ms, with matched key markers gliding to their
new positions, added keys popping in, and removed keys fading out.
Dragging a key stays live (the drag preview already tracks the cursor —
no animation piled on top), and `prefers-reduced-motion` turns the whole
thing off (curves snap, as before).

**How we tackled it.** One mechanism covers every trigger:
**sample-and-tween**. A pure core
([`src/lib/curve-morph.ts`](web/apps/editor/src/lib/curve-morph.ts:1))
samples the old and new *rendered* curves at a shared x-grid and the
hook tweens the sample arrays in projected pixel space — so an
interpolation-mode change or an auto-range growth morphs as one coherent
shape, not a special case. Two facts made the hard parts easy: the
legacy smooth curve reduces to a **fixed monotonic cubic + smoothstep**
(`x(t)=0.75t+0.75t²−0.5t³`, `y`=smoothstep), so uniform-x sampling is one
4-iteration Newton solve of a single polynomial; and the grid is
augmented with an epsilon pair at every key time so **step
discontinuities stay true verticals** even mid-morph. The animation
itself is a hook
([`src/lib/use-curve-morph.ts`](web/apps/editor/src/lib/use-curve-morph.ts:1))
that diffs consecutive `tracks` props per channel and drives one shared
`requestAnimationFrame` loop, drawing into an imperatively-owned overlay
`<g>` per morphing channel (React mounts/unmounts the group; every frame
is a direct DOM write — the FLIP/dock-anim idiom, no per-frame setState);
the static curve hides while its overlay animates and reappears at the
final geometry (snap-to-truth). The whole feature is gated on
`window.matchMedia` existing **and** reduced-motion being off — jsdom has
no `matchMedia`, so the entire existing test suite runs in snap mode
untouched and morph tests opt in by stubbing it. No native, bridge, or
schema changes.

**Issues encountered and resolutions.** (1) **Drag-commit double-glide:**
a committed drag re-delivers the tracks via `tree/changed`, but the
dragged curve already moved during the preview — without suppression it
would visibly snap back and re-glide. The renderer records the committed
move(s) at the drag-commit site; the hook skips re-morphing the channel
when the incoming change matches. The matcher is **reorder-tolerant**
(content multiset, not array index) so a group drag that moves a
selected key past an unselected neighbor — which re-sorts the keys —
still suppresses correctly; a locked follower of the dragged master is
*not* recorded, so it morphs (the intended mirror glide). (2) **Live-host
morphs vs. a11y goldens:** the WebView2 host is Chromium, so morphs run
in the native harness too; `captureDomA11y` now settles on the absence
of `[data-testid="curve-morph-overlay"]` (mirroring the tooltip-exit
settle) before snapshotting. (3) **Interruption folding:** rapid edits
(spinner auto-repeat, undo spam) retarget an in-flight morph from the
currently-displayed shape rather than restarting from a stale one; the
leak-guard fallback timeout is refreshed on each retarget so a sustained
fold is never cut off early. (4) **`lockedTo` is styling, not shape:** the
change classifier ignores it (a re-mirror always arrives as key deltas),
so toggling a lock never spuriously triggers a morph.

---

### Locked curve channels are now genuinely read-only (dashed mirror treatment)

*2026-06-11 · [`75cbe6d`](https://github.com/DrKnickers/particle-editor/commit/75cbe6d) · #126*

Locking a curve channel to an earlier one (Green/Blue → Red in the curve
editor's "Lock to" dropdown) is a live one-way mirror: the follower
displays and tracks the master's curve. Previously the lock was only
half-enforced — a locked curve's keys could still be dragged, inserted,
deleted, or spinner-edited, and because the lock shares the master's key
storage, those edits silently mutated the master and moved the whole
trio. Now a locked focus channel is fully inert: no drag, insert,
marquee/click selection, Delete, cut, spinner commit, or context-menu
delete can touch it. Visually the locked curve renders as a **dashed
line in its own colour** (lying exactly over the dimmed master — the
overlap *is* the mirror) with **hollow ring markers**, the Select/Insert
tools disable, and a small **lock glyph** beside the Lock-to dropdown
explains "Green is locked to Red… Unlock to edit" on hover. Editing the
master still carries the followers live; unlocking restores the
follower's own curve and full editing.

**How we tackled it.** Three defensive layers, all web-side (the native
pointer-alias model is untouched — once no follower edit path exists,
the alias *is* the correct mirror). (1) The renderer
([`src/screens/CurveEditor.tsx`](web/apps/editor/src/screens/CurveEditor.tsx:1195))
derives `focusReadOnly` from the track DTO's `lockedTo` — no prop
threaded — and gates `startDrag`/marquee/insert/context-menu/key-click
at the gesture source, plus the dashed/hollow treatment
(`READONLY_DASH`). (2) The panel
([`src/components/CurveEditorPanel.tsx`](web/apps/editor/src/components/CurveEditorPanel.tsx:1186))
withholds all ten interactive handler props via an `interactiveHandlers`
spread when `focusLocked`. (3) Commit-site guards (`handleDelete`, both
spinner handlers, the key-context-menu delete) refuse under
`focusLocked` — the load-bearing defense for lock state changing
*underneath* a live gesture (undo accelerators or `propagateLinkGroup`
can deliver a lock via `tree/changed` refetch while a selection, an
open context menu, or a half-typed spinner edit is in flight; the
guards read current lock state at commit time). The mock bridge was
redesigned from copy-at-lock to **derive-at-read**
([`src/bridge/mock-state.ts`](web/apps/editor/src/bridge/mock-state.ts:1272)
`deriveLockViews`, applied only at the `get-tracks` boundary): master
edits now mirror live, unlock restores the canonical pre-lock curve,
and chained locks (Blue→Green while Green→Red) present the
intermediate channel's canonical content — all 1:1 with the native
alias semantics.

**Issues encountered and resolutions.** (1) The original design gated
only drag + insert; review found **selection is the gateway** — marquee
select still worked on a locked curve, exposing Delete, the spinners,
and the context menu (none of which checked the lock). The sweep that
followed produced the three-layer posture above. (2) The spinner guards
are not redundant with the `disabled` attribute: `Spinner` commits on
blur, so a value typed *before* the lock lands commits *after* it — a
test pins exactly this. (3) `deriveLockViews` must be applied only at
the read boundary, never inside the overlay's `read()` — the mutators
read-modify-write through `read()`, and deriving there would bake the
mirror into canonical data. (4) Known residual divergence (UI-
unreachable): a direct bridge mutation addressed AT a locked follower
edits the mock's hidden canonical, whereas native would route through
the alias to the master; acceptable until something other than the UI
drives the mock. (5) Tips on the newly-disableable Select/Insert
buttons needed the T6 span shim (disabled elements fire no pointer
events) with lock-aware copy — the Delete button's existing pattern.

---

### Configurable preview overload guard (Preferences toggle + tunable cap)

*2026-06-10 · [`bd52588`](https://github.com/DrKnickers/particle-editor/commit/bd52588) · #123*

The preview overload guard shipped in #121 (which stops an extreme effect from
OOM-crashing the editor) is now configurable from **Preferences → Preview**.
The live-particle ceiling defaults to **15,000** — well under the old fixed
100k, so the preview stays light — and is adjustable in a number field
(1,000–1,000,000). A **"Limit preview particle count"** checkbox turns the
guard off entirely: spawning then runs fully uncapped (the pre-#121 behavior),
which *can* OOM the editor on an extreme effect — an amber line under the
toggle says so, and autosave is the backstop. The setting persists and applies
at startup; the instance ceiling derives from the particle cap (the same 20:1
ratio #121 used, so the default allows 750 live instances).

**How we tackled it.** The engine's compile-time budget constants became
runtime members behind one clamped setter
([`Engine::SetOverloadGuard`](src/engine.cpp:232)); the per-particle / per-round
spawn gates short-circuit to "allow" when the guard is disabled, and the
`Update` refill + latch block are skipped — so "uncapped" records no refusals
and never lights the banner. "Bail earlier" needed no new code: a lower cap
makes the existing `SpawnBudgetExhausted` bail engage sooner. One new bridge
command [`engine/set/overload-guard`](web/packages/bridge-schema/src/index.ts:606)
carries `{ enabled, maxParticles }`; the web owns persistence
([`lib/overload-guard.ts`](web/apps/editor/src/lib/overload-guard.ts:1), the
`lib/theme.ts` localStorage pattern) and pushes the config on every change and
once at app mount. `BridgeDispatcher` caches the last config and reapplies it
on `SetEngine` so a recreated engine never silently reverts (insurance — the
engine is constructed once per process today).

**Issues encountered and resolutions.** (1) The clamp is duplicated across the
web lib, and the engine setter — deliberate: the bridge schema is a TypeScript
type-union with **no runtime validation**, so the engine must not trust its
caller (a cap of 0 would zero the spawn budget forever and read as "editor
broken"). (2) The `readOverloadGuard()` default paths returned the shared
module constant by reference; `Object.freeze` on it closes the
mutate-the-singleton hazard. (3) The native regression specs couldn't be pinned
to 100k as first planned — a 100k particle plateau OOM-crashes the *Debug test
host* at the tail of the full 178-spec single-process harness run (cumulative
heap pressure, not an engine fault — the enabled path is byte-equivalent to
#121's 100k behavior). Pinned the existing bomb tests to the default cap and added three
new specs (cap honored at 5k, mid-run lowering decays to the new ceiling,
disabled = no latch on a moderate effect). Verified: web 700, `tsc -b` 0,
native 180/0, host Debug x64 clean.

---

### Styled, animated tooltips app-wide + one motion family for modal and banner

*2026-06-10 · [`bd52588`](https://github.com/DrKnickers/particle-editor/commit/bd52588) · #123*

Interactive controls (buttons, toolbar glyphs, the emitter-tree affordances)
now carry a styled, animated tooltip instead of a native HTML `title` —
themed surface with a soft drop shadow that adapts to dark/light mode, a
fade + 4px slip entrance/exit, a 400 ms first-open delay that collapses to
instant when sweeping across controls, and full `prefers-reduced-motion`
support. Passive readouts (property-tab labels, the status-bar counts) carry
no tooltip — they don't need one. The heavy-emitter ⚠ glyph gained a
rich tooltip: an amber header band stating plainly *"This chain may spawn
too many particles"* (or *"…emitter…"* when a single emitter pins the
threshold on its own) over an aligned per-generation breakdown. The same motion
family now animates the Modal (fade + 8 px rise; its previous entrance
classes were silent no-ops) and the preview-overload banner (fade + 6 px
drop, soft shadow replacing the old hard ring), driven by shared
`--motion-*` / `--slip-*` / `--shadow-soft` tokens in
[`src/styles/tokens.css`](web/apps/editor/src/styles/tokens.css:48).

**How we tackled it.** One `Tip` primitive
([`src/primitives/Tip.tsx`](web/apps/editor/src/primitives/Tip.tsx:1)) on
`@radix-ui/react-tooltip` (asChild trigger, portaled content), with motion
as hand-rolled CSS keyframes keyed to Radix `data-state`/`data-side` — the
shipped `popover-animate` pattern extended, no animation library. Two
architectural points worth remembering: (1) tooltips are portaled DOM, so
any site whose tooltip can reach the D3D-composited viewport popup
registers `useViewportOcclusion` via an opt-in `occlusionId` prop (the
OccludingPopover precedent), and (2) Radix Tooltip renders its content
TWICE (visible + a VisuallyHidden a11y duplicate), so the occlusion body
arms only for the visible copy via a `closest('[role="tooltip"]')`
discriminator — without it the hidden copy clobbers the occlusion rect.
The ~45-site sweep used six per-site classes (keep/add `aria-label`,
truncation labels get NO added label so a11y goldens stay byte-stable,
titles inside Radix menus deleted as redundant, disabled controls get a
span shim because they fire no pointer events). The OverloadBanner's exit
plays through a new generic `usePresence` hook
([`src/lib/use-presence.ts`](web/apps/editor/src/lib/use-presence.ts:1)) —
animationend unmount with a timeout fallback so reduced-motion (which
fires no animationend) can never leak the banner's occlusion registration.

**Issues encountered and resolutions.** Three gotchas a future contributor
would step on. (1) Radix Tooltip's `data-state` vocabulary is
`delayed-open`/`instant-open`/`closed` — NOT Dialog/Popover's `open`;
verified against the installed dist before writing the CSS selectors.
(2) The `?demo=` routes return before the AppShell and so bypassed the
app-level `Tooltip.Provider` — `Tooltip.Root` throws without one, which
white-screened `?demo=primitives` in the native harness; every demo
return is now Provider-wrapped. (3) Keyboard-focus a11y goldens flaked:
tabbing opens the focused control's tooltip (deterministic) but the
PREVIOUS stop's tooltip is mid-exit-animation at snapshot time —
sometimes mounted, sometimes not. `captureDomA11y` now settles
(`waitForFunction` for no `.tip-animate[data-state="closed"]`) before
capturing; the only legitimate golden delta is tab-stop-2's open tooltip,
and regenerating goldens via a single-test `--grep` is invalid anyway
(earlier spec files establish selection state — regenerate from the full
run only). (4, found at the user feel test) The modal and overload banner
spawned mis-centered and snapped into place mid-animation: Tailwind v4
compiles `-translate-x/y-1/2` to the standalone CSS `translate` PROPERTY,
not `transform`, so keyframes that repeated `translate(-50%, …)` inside
`transform` double-shifted the surface; the keyframes now slip via
`transform` only and leave centering to the `translate` property (the two
compose). Verified: web 688, `tsc -b` 0, native 177/0, host Debug x64
clean; modal + banner centering reconfirmed in-browser.

---

### Preview overload guard: the editor survives any spawn parameters

*2026-06-10 · [`2ed99d5`](https://github.com/DrKnickers/particle-editor/commit/2ed99d5) · #121*

The live preview can no longer be crashed by extreme spawn values. During
the feel test an accidental Shift (the spinner's ×10 step modifier)
committed a six-figure Particles/sec and the editor hard-died — OOM, no
exception, host.log stops mid-stream. The engine now enforces hard budgets
on the simulation (100,000 live particles / 5,000 live emitter instances):
over budget it **suppresses spawning** — existing particles live out their
lives, authored `.alo` values are never clamped or modified — and surfaces
a latched overload flag. The UI shows a non-modal amber banner over the
viewport ("Preview spawn limit reached — spawning paused…") that clears
itself when the population decays back under budget, and the StatusBar
particle counter tints amber. A new ~2-test native spec drives a 1e9/s
bomb plus a death-child refusal storm against the real host and asserts
plateau + recovery — the regression test that previously read "the editor
dies".

**How we tackled it.** Root cause first: no global budget existed — the
per-instance uint16 index cap (16,383) can't help because chained emitters
allocate one child `EmitterInstance` **per parent particle**
([`src/EmitterInstance.cpp`](src/EmitterInstance.cpp:362)), so instances ×
per-instance particles is unbounded. The guard is a per-frame spawn budget
refilled at the top of `Engine::Update` (90% resume hysteresis + a 0.5 s
clear debounce so the latch doesn't flicker at a pinned cap), spent via
`TryConsumeSpawnBudget()` in the burst loop and
`TryConsumeInstanceBudget()` in `ParticleSystemInstance::SpawnEmitter`
(which now returns nullptr on refusal — all callers made null-safe). The
flag rides the existing 4 Hz `stats/tick` (payload + `overload: boolean`);
the banner registers a `useViewportOcclusion` cut-out (the context-menu
precedent) so the composited D3D viewport doesn't overpaint it. The
debounce deliberately uses the pausable `GetTimeF()` clock: a paused
over-budget population doesn't decay, so the banner truthfully holds.

**Issues encountered and resolutions.** (1) The plan assumed "1e9/s alone
exceeds the budget" — wrong: the per-instance index cap plateaus a single
emitter at 16,384, *below* the global budget, and the refused-allocation
churn (~80k alloc/free rounds/frame) tanked FPS to 3 — fixed with a
refused-round `m_nextSpawnTime` snap ("dropped, not deferred"), and
index-cap refusals also latch the banner (hence "spawn limit reached", not
"budget exceeded"). (2) **Latent counting bug found**: refused spawns were
still counted into `Engine::m_numParticles` (+1, never decremented) —
`SpawnParticle` now returns bool and callers count only successes; same
class fixed in `RemoveEmitter`, which destroyed instances with live
particles without subtracting them (a permanent budget leak). Counters
must track *actual* work, not intended work. (3) The banner is the first
occluder that's content-sized but container-positioned — a splitter drag
moved it without resizing it, leaving the alpha cut-out stale;
`useViewportOcclusion` gained an opt-in `observeParent`. (4) The a11y
spec quartet paused the preview clock in `beforeEach` and never reverted
it in `afterAll`, so every later spec file in the shared-host harness ran
with frozen sim time — all eight a11y files now unpause in cleanup (the
overload spec also unpauses defensively at its own start).

---

### Soft chain-load warning on the emitter tree ()

*2026-06-10 · [`e67e5e5`](https://github.com/DrKnickers/particle-editor/commit/e67e5e5) · #120*

The emitter tree now shows an advisory amber ⚠ on every row of a chain whose
estimated alive-particle count exceeds 10,000 — the class of authoring
mistake that crashed the game in the v1 chain test (each generation spawns
one child-emitter instance **per parent particle**, so counts multiply down
a chain). Hovering the glyph shows the estimated total plus a per-generation
breakdown (`sparkle: ~18 alive` → `→ highlight: ×30 → ~540` → …). Purely
advisory: nothing blocks — save, undo, reparent, and authoring proceed
untouched, and vanilla-scale effects (tens to hundreds alive) never trigger
it. The warning updates within one edit whenever the tree structure or any
spawn parameter (rate, lifetime, bursts) changes.

**How we tackled it.** The host mirrors the six spawn-quantity params it
already has onto every tree node (`spawn` sub-object on `EmitterTreeNode`,
serialized in [`src/host/BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp:543)'s
`BuildEmitterTreeNode` + three synthetic-root literals), so the estimator is
ONE pure web-side function ([`src/lib/chain-load.ts`](web/apps/editor/src/lib/chain-load.ts:1):
Little's law per emitter — continuous `rate × lifetime`, burst
`perBurst × concurrent bursts` — multiplied down the chain, node + ancestors
marked above `CHAIN_WARN_THRESHOLD`). Threshold tweaks never need a native
rebuild. The mock achieves parity without duplicating the formula or
mirroring state from its ~35 tree-changed emit sites: a single decoration
choke point in `emit()` + `emitters/list`
([`src/bridge/mock.ts`](web/apps/editor/src/bridge/mock.ts:246)) injects live
spawn values from the properties overlay at payload time, while the stored
tree literals carry a frozen `ZERO_SPAWN` placeholder purely for the type.
No new refresh mechanism was needed — `emitters/set-properties` already
re-emits `emitters/tree/changed` on both host and mock.

**Issues encountered and resolutions.** (1) The plan named two synthetic-root
JSON literals to extend in `BridgeDispatcher.cpp`; the implementer found a
**third** inside `EmitEmittersTreeChanged()` — the event payload itself, the
most-trafficked path. A required schema field means *every* node-shaped
literal must carry it; grep for the `"visible"`/`"children"` key pair when
widening the tree DTO. (2) `Math.round` in the tooltip rendered sub-1 chain
multipliers as `×0` ("×0 → ~20,000" reads as nonsense) — small values now
keep one decimal. (3) `ZERO_SPAWN` started as a plain exported object;
review flagged that dozens of fixture nodes share that one reference, so an
accidental in-place write would corrupt every sharer invisibly — it ships
`Object.freeze`d with a `Readonly<>` type, matching the `TRACK_NAMES`
precedent. (4) PowerShell 5.1 `Get-Content -Raw`/`Set-Content` round-trips
mojibake'd the UTF-8 em-dashes in ROADMAP.md during the ship bookkeeping —
use `[System.IO.File]::ReadAllText/WriteAllText` with an explicit
`UTF8Encoding($false)` for scripted edits of UTF-8 docs.

---

### Splitter-drag cost trims: scene-rect send dedupe + per-message log hygiene (chase-lerp built and reverted)

*2026-06-10 · [`b37f198`](https://github.com/DrKnickers/particle-editor/commit/b37f198) · #118*

Two objective cost trims on the splitter-drag path: the web side stops
re-sending identical scene rects (a measured 2× send rate during window
resizes), and the host stops paying a synchronous disk flush per interactive
bridge message (a 3 s drag now adds ~50 host.log lines, was 200+). A third
piece — smoothing the scene-rect stream with host-clocked chase lerps — was
built, user-tested, and **reverted**; see issues below for the lesson.

**How we tackled it.** Attribution first: a per-kind tally in the bridge
probe + a real-mouse `SendInput` drag driver
([`tasks/tool-splitter-drag-probe.mjs`](tasks/tool-splitter-drag-probe.mjs))
identified the actual streams — `layout/scene-rect` at ~22-28/s while
dragging, plus `viewport/input` at mouse rate (~60-140/s) whenever the cursor
crosses the viewport (the unranked ~104/s in the user's live log; it's the
input pipeline, functional traffic). **C1**: `ViewportSlot`'s `send()`
dedupes on (rect, DPR) — DPR in the key so a monitor swap at an identical CSS
rect is never dropped; the "redundant" listeners stay as the mid-dock-slide
safety net, dedupe makes their overlap free. **C2**: the per-message `WebMsg`
host.log line is skipped for the two interactive kinds (the 1 Hz per-kind
tally carries their rates); `SetSceneViewport`'s per-apply printf is
1 Hz-throttled; the DComp transform log moved to a `quiet` flag — per-frame
anim applies (the dock slide) are silent, instant + anim-terminal applies
log, so the settled clip always appears and the rate self-throttles by
construction.

**Issues encountered and resolutions.** (1) **C3 (chase-lerp) built and
reverted —.** Streamed scene-rects became short host-clocked glides
(duration = inter-arrival gap) to fix the engine-edge-vs-panel-edge judder.
The numbers looked right; the feel was wrong, twice over: a chase's
steady-state lag is ONE PACKET INTERVAL by construction, and under real drag
load the stream runs ~12/s → 80-160 ms of visible edge lag plus an end snap;
and during window resizes `PredictAndApply` cancels the anim every size tick,
starving the chases so newly revealed window area sat on backing colour for
up to a second. Smoothing cannot beat the data rate — the residual splitter
stutter is Chromium's own relayout cadence under drag, which instant
application already tracks as tightly as the architecture allows. (2) A first
C2 attempt time-throttled the transform log to 1 Hz — wrong: it dropped the
SETTLE line (the one state you want) and broke a spec pinning per-dispatch
transform logging; the quiet-flag design keeps both. (3) The investigation's
audit had "refuted" per-message log-flush materiality — correct in isolation,
but at the measured combined stream rates the flushes sum to real UI-thread
time; per-kind attribution is what made the call defensible either way.

---

### Render loop paced to display cadence; GPU sync wait yields

*2026-06-10 · [`b37f198`](https://github.com/DrKnickers/particle-editor/commit/b37f198) · #118*

The editor's render loop no longer free-runs: it used to render ~3000 fps at
idle (measured by the `[PERF]` probe), pegging one CPU core and saturating
the GPU with queued frames — headroom WebView2's renderer needed during
splitter drags, making panel resizes feel far heavier than they are. The
loop now renders once per display refresh period (~240 Hz cap on a 240 Hz
monitor, ~60 on a 60 Hz one) and sleeps between frames; input still wakes it
instantly, so interaction latency is unchanged. Idle CPU dropped from a full
core to ~20% of one. The cross-device GPU sync wait also yields its
timeslice while polling instead of burning the core.

**How we tackled it.** Fix B of the resize-perf plan, two independent
commits. B1 ([`src/host/HostWindow.cpp`](src/host/HostWindow.cpp:3500)): the
main loop renders only when a QPC frame budget elapses (one display
refresh period, read via `EnumDisplaySettings` at startup, 60 Hz fallback)
and otherwise blocks in `MsgWaitForMultipleObjectsEx(…, QS_ALLINPUT,
MWMO_INPUTAVAILABLE)` — messages wake it immediately, the timeout wakes the
next frame; `timeBeginPeriod(1)` for the loop's lifetime keeps the wait from
quantizing to the ~15.6 ms default timer; a slow frame schedules from "now"
(cap semantics — no vsync, no catch-up bursts); QPC failure degrades to the
old free-run; capture mode keeps its `Sleep(16)` path untouched. B2
([`src/engine.cpp`](src/engine.cpp:1622)): `WaitEndFrameQuery` polls tight
for 64 iterations (the common already-signalled case) then `SwitchToThread()`s
between polls; the 100k hung-GPU cap is unchanged. The `[PERF]` line gains
`rps=` — actual renders/sec — because its `fps` field is `1/frame-cost`
(theoretical max) and stopped tracking cadence once the loop was paced.

**Issues encountered and resolutions.** (1) Exactly that `fps` field
initially made the pacing look broken (`fps=2400` post-fix) — it never
measured cadence, the unpaced loop just made cost and cadence coincide;
`rps=` disambiguates permanently. (2) The 1 ms wait quantization means the
cap runs slightly under the budget on fast monitors (~190 rps at a 4.17 ms /
240 Hz budget) — accepted: it's a load cap, not vsync, and sub-ms spin-waits
to hit exact cadence would reintroduce the burn being removed. (3) Verified
the timing-sensitive surfaces explicitly: the native a11y/bridge harness
(174/0 — bridge round-trips now ride a ≤1-frame-budget pump latency) and the
resize-storm regression (per-tick cheap resets + one settle, unchanged).

### Closed right-dock hardening: inert splitter + correct collapsed mount

*2026-06-10 · [`9b4dcc8`](https://github.com/DrKnickers/particle-editor/commit/9b4dcc8) · #117*

Two closed-dock bugs fixed. (1) With the Spawner/Lighting dock closed,
dragging at the window's right edge could open the empty dock slot — the
splitter was invisible but still live. It is now fully inert while the dock
is closed: no resize cursor, no drag, no keyboard resize. (2) Booting with
the dock closed while a saved panel layout existed rendered the dock slot
**open and empty** (at its remembered share) and it never collapsed; it now
mounts collapsed, and the first open from that state expands properly
(previously a masked zero-width open). Open behaviour is otherwise
unchanged.

**How we tackled it.** The dock `Separator` in
[`src/components/PanelLayout.tsx`](web/apps/editor/src/components/PanelLayout.tsx)
already carried `invisible pointer-events-none` while closed — but
react-resizable-panels (4.11) hit-tests separators by **document-level
pointer coordinates against their rects**, not via DOM events on the element,
so CSS could never block the drag (an invisible element still has a rect).
The supported switch is the `disabled` prop, applied to BOTH the `Separator`
("cannot be used to resize its neighboring panels") and the `Panel` (per the
lib's own docs, else it can still be resized indirectly). Verified in the lib
source that the imperative `expand()`/`collapse()` the dock toggle uses
bypasses `disabled` (`trigger === "imperative-api"` sets
`overrideDisabledPanels`), so open/close animation is unaffected.

The mount bug's root cause: the outer Group's `defaultLayout` (the persisted
blob, with the dock's remembered OPEN share) and the dock Panel's `"0%"`
`defaultSize` disagreed at a closed mount, and which one wins depends on the
library's init path (immediate vs deferred) — with the mount effect's
`isCollapsed()` check racing the deferred apply. Fixed at the SOURCE: a pure
`collapseDockShare` helper folds the stored dock share into the centre
column when seeding the Group for a closed mount, so both seeds agree on
spawner=0 (the stored blob itself is untouched — persistence is
dockVisible-gated, so the remembered width still serves open-at-mount
sessions). The first open from a mount-collapsed dock relies on the lib's
documented `expand()` fallback to `minSize` (= `DOCK_MIN_PX`, which is also
the dock-slide anim's first-open assumption, so panel and host anim agree),
with an explicit `resize()` chain kept as a zero-share safety net.

**Issues encountered and resolutions.** (1) Verification used a synthetic
pointer-drag in the vite preview with a **positive control**: with the dock
open the same dispatched pointer sequence resizes it 260→360 px (proving the
synthetic events drive the library), with it closed the drag is a no-op and
the lib renders `aria-disabled`/`data-disabled` (also asserted by new vitest
cases; the mount fix adds a flex-grow assertion seeded from both localStorage
keys, proven red without the seed change). (2) The mount bug was discovered
DURING the splitter-fix verification and confirmed pre-existing via an A/B
stash test before being fixed here. (3) Two preview-environment traps cost
debugging time and are worth remembering: vite **HMR-stale module epochs**
(the dep-array size-change error after editing a hook's deps live) made the
fixed code appear broken — always hard-reload after editing effect deps; and
`getSize().inPixels` reads `offsetWidth`, which is **stale (0) in the same
tick** as an `expand()` — gate imperative-API decisions on `asPercentage`,
which updates synchronously, or a correct expand gets "corrected" into
clobbering the remembered width.

---

### Smooth window resize: cheap ResetEx-based per-tick device reset, plus resize-perf probes

*2026-06-10 · [`e7cf484`](https://github.com/DrKnickers/particle-editor/commit/e7cf484) · #116*

Resizing the app window no longer tanks: the full D3D9Ex device reset that
fires on **every mouse-move tick** of the modal sizemove loop used to cost
~24 ms (full render-target/shader teardown, whole texture-cache wipe with disk
re-decode — ~20 ms of the total) and now costs **~3-5 ms**, because resize-only
resets switched to `IDirect3DDevice9Ex::ResetEx`, which keeps all surfaces,
textures, and shaders alive across the reset ("all other surfaces persistent"
— first-party docs). The scene still renders at the **correct size on every
tick** — no deferred work, no end-of-gesture snap. Resizing the window also
no longer **rescales the world**: the per-pixel-FoV projection now anchors to
a fixed reference (45° per 768 px of viewport height) instead of the current
render-target height, so growing the window reveals more scene at the edges —
the dock-slide behaviour — rather than zooming the existing content. Always-on
`[resize-perf]` log probes (1 Hz aggregates in `host.log`) quantify the reset
rate, the per-reset sub-stages, and the bridge scene-rect message rate — the
measurement basis for the remaining splitter-drag fixes.

**How we tackled it.** New `Engine::ResetForResize`
([`src/engine.cpp`](src/engine.cpp:1494)): `ResetEx` + rebuild of only the
size-keyed targets (scene/distort/bloom RTs + depth-stencil via the existing
`ResetParameters`, the AlphaCompositor shared RT via `Resize`); the
OnLostDevice dance, texture-cache wipe, and ground/skydome re-decode that
plain `Reset()` legally requires are all skipped — D3D9Ex semantics make them
unnecessary for a windowed size change. All three resize-driven reset sites in
[`src/host/LayoutBroker.cpp`](src/host/LayoutBroker.cpp:251) funnel through
one `ResetEngineForResize` helper (cheap path → full `Reset()` fallback →
`RecoverDeviceIfNeeded`), per the no-hand-copies rule. Full `Reset()`
remains the device-loss recovery primitive. `WM_ENTERSIZEMOVE`/`EXITSIZEMOVE`
handlers gate a main-window `WM_ERASEBKGND → 1` suppression (DefWindowProc was
GDI-filling the full client with the class brush per tick) and arm a 150 ms
quiescence timer that re-resets only if a mid-gesture reset failed.
Engine-side, `Engine::ResetPerf` ([`src/engine.h`](src/engine.h:200)) mirrors
the `RenderPassTimingsUs` diagnostic pattern: the engine publishes per-reset
sub-stage wall-clock + a cheap-path counter, the host logs it at 1 Hz.

**Issues encountered and resolutions.** (1) **The first iteration of this fix
was rejected by the user and redesigned.** It deferred the reset to gesture
settle (`WM_EXITSIZEMOVE`) — numerically a 30× win (21 resets/s @ 27 ms →
0 resets @ 0.9 ms/tick) but the end-of-gesture snap "feels like a regression"
(the scene rendered into the old-size target mid-gesture, then jumped). The
probe data itself pointed at the snapless fix: ~20 ms of every reset was
texture re-decode that `ResetEx` makes unnecessary. Lesson recorded as:
prefer making per-tick work cheap (continuous-correct) over deferring it
(deferred-correct); measured wins don't override a feel verdict. (2) A
`put_Bounds` ~30 Hz throttle shipped with the first iteration was **reverted**
— it halved the panels' tracking rate during resize, plausibly reading as the
very regression being fixed (corollary: never bundle feel-degrading
riders with the headline fix). (3) The investigation's sketch said "stretch
the last-presented surface via the dock-slide path" — but the dock-slide
transform (`SetEngineVisualTransform`) only offsets and **clips**; it cannot
scale (a dock slide never changes the window size). Moot after the redesign.
(4) The smoke is **programmatic**: a `SetWindowPos` storm with/without the
`WM_ENTERSIZEMOVE` bracket
([`tasks/tool-sizemove-storm.ps1`](tasks/tool-sizemove-storm.ps1)) reproduces
the modal loop's message sequence without interactive input. On the final
build: every tick resets on the cheap path (zero fallbacks), reset
tot ≈ 3.5-4 ms (`reload` 20 ms → 0.4 ms), native harness 174/0. (5) The probes
confirmed two predictions for the next phases: the scene-rect stream runs at
**2×** the tick rate (redundant `window resize` listener in
`ViewportSlot.tsx` — fix C1), and the idle pump free-runs at ~3000 fps with
~4000 GPU-query spins per frame (fix B). (6) Smooth resize **exposed a
pre-existing framing wart**: `SetSceneViewport`'s per-pixel-FoV reference was
the *current* RT height ([`src/engine.cpp`](src/engine.cpp:1765)), so a window
resize rescaled the world while a dock slide revealed it (the user noticed
the inconsistency only once resize was smooth enough to watch). Anchoring the
reference to a constant (45°/768 px, ≈ the default window's client height so
default framing is unchanged within ~1%) unifies both gestures on the reveal
behaviour; fovY clamps at 120° to stay clear of the perspective-projection
breakdown at extreme heights. Legacy arch-A's `ResetParameters` 45° full-RT
projection is deliberately untouched.

---

### Emitter-tree drag hardening: ten audit fixes (data-corruption, mid-drag safety, parity)

*2026-06-09 · [`e4ef42b`](https://github.com/DrKnickers/particle-editor/commit/e4ef42b) · #110*

A resumed multi-agent audit of the click-drag reorder/reparent feature
surfaced ten adversarially-confirmed defects; all are fixed. The headline:
**a routine root reorder could silently swap a parent's life/death child
slots** — corrupting the `.alo` on save — whenever one child's new index
aliased a sibling's old index. Beyond that: a mid-drag undo/redo/paste no
longer commits the move against stale ids (it cancels the gesture); alt-tab
or a second mouse mid-drag no longer strands or double-commits a drag;
dragging a root no longer drops a separately-selected child from the
highlight; the mock no longer reports the document dirty after a *refused*
drag-commit; autoscroll reaches the end-of-list gap in a very short tree
panel; and a drag that ends over a different row no longer eats your next
click on an unrelated row.

**How we tackled it.** The data-corruption bug
([`ParticleSystem.cpp`](src/ParticleSystem.cpp)) was a read-modify-write
aliasing fault, hand-copied across three KEEP-IN-SYNC reorder functions
(`moveEmitter` / `moveEmitterToRootIndex` / `reorderManyRootsToIndex`). Fixed
once by consolidating them into a single two-pass helper,
`rewriteParentSpawnIndices`, that **snapshots each parent's slot fields before
writing any of them** so a comparison never reads a half-rewritten field —
eliminating the duplication that let the bug into all three. A new standalone
unit test ([`tests/test_emitter_reorder.cpp`](tests/test_emitter_reorder.cpp))
reproduces the swap through both bridge-reachable entry points. The mid-drag
class (stale-id commit, gap-before-wrong-row, wrong-rows-dim) shared one root
cause — the `emitters/tree/changed` subscription refetched unconditionally
while the drag held a pointerdown snapshot — so one `activeDragCancelRef` in
[`EmitterTree.tsx`](web/apps/editor/src/screens/EmitterTree.tsx) **aborts the
in-flight gesture before refetching**, killing all three. The controller also
gained a `pointerId` re-entrancy guard, `setPointerCapture` + a `blur` /
`visibilitychange` teardown, and a scoped (next-macrotask) reset of the
click-suppression flag. Selection-follow now captures untouched members'
`stableId`s before a reorder and re-resolves them against the fresh tree
([`lib/emitter-reorder.ts`](web/apps/editor/src/lib/emitter-reorder.ts)), so
they follow the host reindex instead of being dropped. The mock's dirty bit
moved behind a `didMutate` result check mirroring the host's per-handler
`markDirty` ([`bridge/mock.ts`](web/apps/editor/src/bridge/mock.ts)).
Autoscroll now tie-breaks overlapping edge zones by the nearer edge
([`lib/drag-autoscroll.ts`](web/apps/editor/src/lib/drag-autoscroll.ts)).

**Issues encountered and resolutions.** The audit run that found these had
itself been rate-limited mid-flight in a prior session and reported a
*false* clean pass (`confirmed: []`) — its workflow scored a dead verifier's
missing verdict as "refuted". The re-run hardened the accounting to a
three-way `confirmed` / `refuted` / **`unverified`** split with an
`isCleanRun` flag, so an incomplete run can no longer masquerade as a pass.
Building a standalone C++ test for `ParticleSystem` (whose headers drag the
D3D types) linked with a single no-op stub for one rendering method
`~Emitter` references but the test never exercises — keeping the harness on
the pure data model. Two findings need live host smoke (no unit harness can
drive a true second physical pointer or an OS window-blur): the re-entrancy
guard and the blur/visibility teardown — both unit-tested for the reachable
parts, flagged for a manual pass.

---

### Emitter rows glide to their new positions on reorder

*2026-06-09 · [`71b5c41`](https://github.com/DrKnickers/particle-editor/commit/71b5c41) · #108*

Reordering emitters — drag-drop (single or multi), Move Up/Down, delete,
paste — now **glides** the affected rows to their new positions (~200 ms ease)
instead of snapping. `prefers-reduced-motion` disables the glide.

**How we tackled it.** The blocker was identity: an emitter "id" is a
positional index that reshuffles on every structural change, so React keyed
by it had to unmount/remount rows on reorder — unanimatable. Fixed at the
root: every `ParticleSystem::Emitter` now carries a **`stableId`** assigned
from a process-monotonic counter in all three constructors
([`ParticleSystem.cpp`](src/ParticleSystem.cpp) — the copy ctor deliberately
issues a *fresh* id, since a duplicate is a new emitter), surfaced on the
`emitters/list` DTO ([`BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp))
and required by the schema. The mock mirrors it with an offset counter
(stableIds ≠ ids) so code confusing the two fails fast in tests. With rows
keyed by `stableId`, the glide is a standard **FLIP** pass
([`lib/flip.ts`](web/apps/editor/src/lib/flip.ts) pure deltas + a layout
effect in [`EmitterTree.tsx`](web/apps/editor/src/screens/EmitterTree.tsx)):
measure `offsetTop` per row before paint, apply the inverted `translateY`,
transition to zero. The pass also runs **during a drag** (user call after the
first smoke): the effect keys on the gap indicator too, so the make-room
reflow glides as the gap inserts/moves/clears — snappier mid-drag
(`FLIP_DRAG_MS` 120 ms) than the post-drop settle (`FLIP_SETTLE_MS` 200 ms).
Interrupted glides (rapid gap hops) restart from the row's current *visual*
position by folding the in-flight computed-transform residual into the next
inversion — without that, re-inverting from layout positions teleports rows
mid-flight. Resolution stays correct while rows animate because gap/onto
targets come from the drag-activation geometry snapshot, never the live DOM.
stableIds are runtime-only — never persisted to `.alo`; undo/redo
rebuilds emitters and issues fresh ids, so a reorder undo remounts (snaps)
rather than glides: accepted.

**Issues encountered and resolutions.** Making the schema field *required*
turned `tsc -b` into an exhaustive call-site inventory (the lesson,
compiler-enforced): it flagged every fixture and mock creation site — but
NOT the spread-based clones (`{...node}` satisfies the type while silently
*duplicating* the source's stableId). Those needed manual auditing: mock
duplicate/paste reassign walks now issue fresh stableIds (the C++ copy-ctor
rule), while structural clones in reorder/reparent walkers correctly
preserve them (same emitter). A scripted fixture update also over-matched
TWO rename-params literals (rename params aren't a tree node): one `toEqual`
expectation was caught by the suite, but a request-INPUT literal in the
bridge-contract suite shipped green — the mock destructures params loosely
and TypeScript skips excess-property checks when `request<R extends
Request>(req: R)` infers R from the literal; an adversarial review pass
caught it. The same review caught two real bugs the suites missed: a fourth
Emitter-copy path (`copySharedParamsFrom`, the link-group propagation) doing
`*this = src` without restoring `stableId` — every shared-field edit on a
linked emitter would have stamped the source's identity onto all siblings
(duplicate React keys); now restored + pinned by an `NDEBUG` assert. And a
stale reparent latch: once the onto ring was acquired, moving to a no-op zone
left `lastParams` set, so a release on the block's own footprint committed
the stale reparent — the idempotent gap-setter couldn't see onto state; now
cleared explicitly (+ regression test). Measure `offsetTop`, not
`getBoundingClientRect`, in the FLIP pass — and snap any in-flight glide
before the drag controller snapshots geometry, since `getBoundingClientRect`
DOES include live transforms (a re-grab inside the 200 ms glide window would
otherwise capture mid-animation positions).

---

### Multi-select drag-reorder: drag a whole selection as one block

*2026-06-09 · [`71b5c41`](https://github.com/DrKnickers/particle-editor/commit/71b5c41) · #108*

Dragging a root emitter that is part of a multi-selection now reorders the
**entire selection as one contiguous block** instead of just the grabbed row —
consistent with the selection-aware Move Up/Down arrows. A non-contiguous
selection collapses together at the drop point in its current top-to-bottom
order; the highlight follows the moved emitters to their new positions. While
dragging, the other emitters slide aside to open a **make-room gap** at the drop
point — sized to the lifted block's true height — and the **whole lifted
subtree dims** (children ride along with their roots; a single-row drag dims its
subtree too), so you see exactly where the block will land and exactly what is
moving. A **vertical chip** by the cursor lists up to four of the rows being
carried (+ a "+k more" line) and is **magnetized toward the active gap** — it
glides partway into the gap so the emitters visibly "flow in", and returns to
the pointer when a release would do nothing. Dropping a block onto its own
footprint does nothing.

**Single drag gets the same treatment.** Dragging one unselected root now shows
the identical make-room gap + cursor chip (one name) and lifts its whole subtree,
instead of a thin insertion line — and the highlight **follows** the emitter to
its new spot after the drop (previously it stayed on the slot the emitter left,
which then held a different emitter). Reparenting is unchanged: drop a root onto
the **middle third** of any row to nest it under that row (its sky onto-ring,
never a gap), and the highlight follows there too.

**How we tackled it.** A new atomic host op `emitters/reorder-many { ids,
rootIndex } -> { newIds }` ([`BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp)
+ [`ParticleSystem::reorderManyRootsToIndex`](src/ParticleSystem.cpp)) computes
the final root order in a single pass — remove the selected block, reinsert it
contiguously at the gap shifted by the count of selected roots ahead of it — then
reuses `moveEmitterToRootIndex`'s subtree-reassembly to relayout and return the
moved roots' new indices. The React pointer-drag controller
([`EmitterTree.tsx`](web/apps/editor/src/screens/EmitterTree.tsx)) gained a
multi-drag branch (pure intent resolution in
[`lib/multi-drag.ts`](web/apps/editor/src/lib/multi-drag.ts)) that dispatches the
op and re-selects the returned `newIds` through the existing `applyNewSelection`
path; the dev mock mirrors the host algorithm exactly, so the Vitest suite is the
behavioural contract for both. The drop target is resolved **geometrically**, not
by DOM hit-testing: at drag activation the controller snapshots every root
block's measured extent in scroll-content space
(`captureRootBlockGeometry`), and each pointer move maps to a gap index with
pure math ([`resolveGapFromGeometry`](web/apps/editor/src/lib/multi-drag.ts) —
un-shift the pointer past the rendered gap, then the classic
midpoint-crossing rule). Every pointer position resolves to a gap or the
footprint no-op — there are no dead zones, so the preview tracks the pointer
continuously. The chip's magnet is a small rAF spring toward a blend of the
pointer and the gap's center (`computeChipTarget`, pull/spring constants
`CHIP_PULL`/`CHIP_SPRING` in `EmitterTree.tsx`); `prefers-reduced-motion`
skips the glide but keeps the position. The chip also **pops in on spawn**
(`drag-chip-in`, components.css) and **flies into the landing spot on
release** — the reorder gap's center, or the reparent target row — while
fading (`CHIP_EXIT_MS`); cancels and no-ops fade in place. The flight target
is computed in `finish()` from the same geometry snapshot the resolvers use. Single drag unifies onto the same
controller: a single root is a size-1 block, so its reorder reuses
`resolveGapFromGeometry` and commits through `reorder-many` (which is why the
highlight follows — the `newIds` re-select). The one thing single drag adds is
**reparent**, which needs per-row hit-testing; to stay flicker-free the
controller also snapshots every row's extent (`captureRowGeometry`) and
`resolveSingleRootDrop` resolves both outcomes in one geometric pass — middle
third → onto (reusing the tested `resolveDropIntent` for reparent validity),
else the reorder gap. The host re-selects the moved emitter after an
`emitters/drop` (by scanning for the dragged `Emitter*`'s new index in the
re-laid-out vector) and emits `emitters/selected`, so reparent's highlight
follows even though ids are positional and reshuffle.

**Issues encountered and resolutions.** A pre-coding adversarial design pass
caught that the no-op rule must refuse **every** gap on an already-contiguous
block's own footprint `[first, last+1]` (both edges *and* the interior gaps), not
just the two edges — reinserting a block into its own vacated span is the
identity layout. The smooth glide animation of rows sliding to their new
positions was **deferred**: the data model has no stable emitter identity (ids
are positional and reshuffle on every reorder), so a robust glide needs an
imperative FLIP controller rather than a React-keyed transition — captured as a
follow-up ([`tasks/next-reorder-glide-animation.md`](tasks/next-reorder-glide-animation.md))
to take on after a stable id is added. The first preview iteration resolved the
drop target from the **live** DOM, which a make-room gap inherently fights: the
gap reflows the list, the pointer lands on the (pointer-events-none) gap, the
target resolves null, the gap clears, the rows snap back — flicker. A
hold-on-dead-zone workaround stopped the flicker but made the gap "stick" for
~a block height of travel on tall blocks. The geometric snapshot resolver
replaced both; its stability is pinned by a vitest **fixed-point property**
(re-resolving a stationary pointer against the gap it produced never moves the
gap; iteration from any reachable state converges without cycles — the property
test caught a genuine transient on its first run). Synthetic-pointer testing
gotcha: a dispatched `pointerdown` with the default `pointerType: ""` held over
a Radix `ContextMenu.Trigger` opens the menu via the touch long-press path —
drive synthetic drags with `pointerType: "mouse"`. Extending the gap to single
drag risked the same flicker plus a new one: single drag toggles between a gap
(reorder) and no gap (reparent onto), and clearing a flow gap reflows the list by
the gap height under a stationary pointer, which can oscillate at the
onto/reorder boundary. Rather than add speculative hysteresis, the no-cycle
property test was extended to the single-root resolver and the **simplest**
un-shift-only resolver was tried first — it converged for every reachable state
and pointer (including a childless root where the gap height equals one row), so
no hysteresis was needed. The highlight-not-following bug was a missing
host/mock re-select: `emitters/drop` returned only `{ ok }`, so the stale
positional id kept highlighting the slot the emitter vacated.

---

### Multi-select move / duplicate now mark the document dirty (mock parity fix)

*2026-06-09 · [`3479fe6`](https://github.com/DrKnickers/particle-editor/commit/3479fe6) · #107*

The dev **mock** bridge's `isMutating` allowlist was missing `emitters/move-many`
and `emitters/duplicate-many` (both added in #104), so a multi-select move or
duplicate left the document falsely **clean** in browser/mock mode — no dirty bit,
no save-prompt gate. The native C++ host marks dirty per-handler, so the shipping
app was unaffected; this was a mock↔host contract divergence that could mask a
regression in tests. Both kinds are now flagged, restoring `isMutating`'s
documented invariant ("the dirty-bit + save-prompt gate matches the native host's
markDirty rule").

**How we tackled it.** Two lines in
[`mock.ts`](web/apps/editor/src/bridge/mock.ts) `isMutating`, beside their
single-item siblings `emitters/duplicate` / `emitters/move`, plus bridge-contract
tests asserting each kind flips the mock engine-state `dirty` bit.

---

### Multi-aware emitter operations: delete / duplicate / move act on the whole selection, order preserved

*2026-06-09 · [`6f462e0`](https://github.com/DrKnickers/particle-editor/commit/6f462e0) · #104*

Selecting several emitters and then deleting, duplicating, or reordering now
acts on the **whole selection** from every entry point — the panel toolbar, the
row right-click menu, the `Delete` key, and `Alt+↑/↓` — instead of just one
emitter. After a **duplicate** the selection jumps to the new copies; after a
**move** the highlight **follows** the moved emitters to their new positions.
Moving a multi-selection up/down behaves as a **rigid block**: it translates by
one or, if the edge-most member is pinned at the top/bottom, **freezes** — it
never deforms by compacting trailing members past non-selected emitters (order
is load-bearing in a particle system). The Move Up/Down buttons (and
context-menu items) now grey out exactly when the move would do nothing,
symmetrically at both edges.

**How we tackled it.** Because an emitter "id" is a host-side position index
that reshuffles on every structural change, the batch ops are done host-side:
two new bridge messages `emitters/move-many` and `emitters/duplicate-many`
([`BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp)) perform the op
atomically and return `newIds` — the affected emitters' final indices — which
the React side re-selects ([`emitter-reorder.ts`](web/apps/editor/src/lib/emitter-reorder.ts)).
The block-move preserve rule (skip the edge-anchored run; move the rest as a
unit) lives in the host **and** the mock, plus a shared `canMoveSelection`
predicate ([`move-enabled.ts`](web/apps/editor/src/lib/move-enabled.ts)) that the
toolbar and context-menu Move items share so their enabled state can't drift.

**Issues encountered and resolutions.** *The disabled-state asymmetry* (up
greyed at the top, down didn't at the bottom) traced to the buttons reading the
*primary* emitter's row position — single-select logic — so the result depended
on which member was primary; fixed by computing from the selection. *The
context-menu / trash / right-click delete originally ignored the multi-selection*
(acted on the clicked or primary row); routed through the selection like the
`Delete` key. *Compact-vs-preserve at the edge* was a real design fork settled
with the user in favour of preserve.

---

### Confirm destructive emitter deletes + surface failed Save / Open

*2026-06-09 · [`6f462e0`](https://github.com/DrKnickers/particle-editor/commit/6f462e0) · #104*

Two safety gaps in the now-default new UI are closed. **(1)** A failed Save,
Open, or Save As no longer fails silently: `file/save` · `file/open` ·
`file/save-as` all return `{ok:false, error}`, but every call site previously
discarded it — a save that failed (disk full, read-only target, locked file)
left the document dirty while the user believed it saved. Now a non-cancel
failure pops an error modal naming the problem; a user-cancelled native picker
stays silent. **(2)** Deleting an emitter that has children (the host deletes
the whole subtree) or deleting a multi-selection now asks first — *"Delete X
and its N child emitters?"* / *"Delete N emitters?"* — while a single childless
leaf still deletes immediately (it is undoable). A new **Preferences → Confirm
before deleting emitters** toggle (default on) governs the confirm. Behaviour
under `--legacy` is unchanged.

**How we tackled it.** Three small additive `lib/` modules + two App-mounted
modals. [`file-op.ts`](src/lib/file-op.ts:1)'s `runFileOp(bridge, req)` wraps a
file request and, on `error !== "user-cancelled"`, sets a zustand error store
that [`FileOpErrorModal`](src/components/FileOpErrorModal.tsx:1) renders.
[`delete-emitters.ts`](src/lib/delete-emitters.ts:1) owns `computeDeleteImpact`
(deduped subtree union → `isDestructive` + affected-count), `performDelete`
(the descending-order loop, centralised from its two former inline copies), and
`requestDeleteEmitters` — the single entry point all four delete sites call,
which either deletes immediately or opens a confirm store that
[`DeleteConfirmModal`](src/components/DeleteConfirmModal.tsx:1) consumes. The
constraint that shaped the API: `bridge` is created once in
[`App.tsx`](src/App.tsx:35) and passed as a prop (no module singleton), so every
helper takes it as a parameter and the confirm store never touches it (the
modal, which holds the prop, runs the delete on confirm). To let `MenuBar` and
the delete helper compute subtree impact, the emitter tree was lifted from
`EmitterTree`'s local `useState` into a shared
[`emitter-tree.ts`](src/lib/emitter-tree.ts:1) store.

**Issues encountered and resolutions.** *The tree-store lift exposed a latent
crash.* `EmitterTree` derefs `tree.root` assuming a truthy tree has a root —
true in production, but the layout tests' stub bridge resolves `emitters/list`
to `{}`. With per-component `useState` that malformed value reached a render only
asynchronously (after assertions); through the global store it leaks across test
cases and is present at the next test's *synchronous* initial render →
`PanelLayout.test.tsx` crashed. Fixed by enforcing the store invariant
"null-or-well-formed" at the single setter
([`EmitterTree.tsx`](src/screens/EmitterTree.tsx:1174): ignore a response with no
`root`). *A fourth Save call site was missed and caught in review.*
[`SaveChangesPrompt`](src/screens/SaveChangesPrompt.tsx:49)'s "Save" — fired on
every dirty New/Open/Recent — conflated user-cancel with real failure and showed
no error: the exact silent-data-loss path the feature targets. Routed through
`runFileOp` so a real failure surfaces while the pending op still aborts (unsaved
work survives). *`emitters/delete` is already undoable* (it `captureUndo()`s
pre-mutation host-side), which is why the confirm is proportional — gating only
the non-obvious destructive cases rather than every delete.

---

### Make the maximized save-modal backdrop snapshot near-instant

*2026-06-09 · [`d431bde`](https://github.com/DrKnickers/particle-editor/commit/d431bde) · #101*

Opening a modal (e.g. the save-changes dialog) with the editor **maximized** now
paints its frosted-glass backdrop in **~6 ms** instead of **~70 ms** — effectively
instant, matching the windowed case. There are no new controls; this is pure
latency removal on the `viewport/capture-snapshot` path that every modal triggers
on open. Behaviour under `--legacy` is unchanged.

**How we tackled it.** Two complementary cuts in
[`AlphaCompositor::CaptureSnapshotPng`](src/host/AlphaCompositor.cpp:572).
**(1) GPU `StretchRect` fast path** — instead of reading the full
3440×1369 render target back to system memory and downscaling it on the CPU with
GDI+ `DrawImage`, the scene-rect crop is `StretchRect`-blitted (with the existing
2×/1024-cap downscale formula reused verbatim) into a small `CreateRenderTarget`
surface on the GPU, and `GetRenderTargetData` reads back **that** — so the readback
collapses from ~8 ms to ~1.5 ms and stops scaling with window size, and the ~19 MB
memcpy + the DrawImage downscale disappear. **(2) JPEG backdrop** — the snapshot is
shown blurred under `Dialog.Overlay`'s `backdrop-blur-sm`, so it is now encoded as
JPEG (q82) rather than PNG: lossy is invisible, the encode drops from ~28 ms to
~1.7 ms, and the payload shrinks ~7× (905 KB → ~120 KB), which also slashes the
base64 + IPC + browser-decode transit. The response field was renamed
`pngBase64` → `imageBase64` across the host
([`BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp:1047)), the bridge schema
([`bridge-schema/src/index.ts`](web/packages/bridge-schema/src/index.ts:1064)), the
consumer ([`Modal.tsx`](web/apps/editor/src/components/Modal.tsx:233), now a
`data:image/jpeg` URL), the mock, and the native dim test (JPEG `/9j/` signature).
[`CaptureSnapshotToFile`](src/host/AlphaCompositor.cpp:791) (the `--capture`
offline-diff path) deliberately stays full-resolution PNG.

**Issues encountered and resolutions.** *Profiling overturned the ROADMAP's
premise.* Once avenue (a) cut the readback, maximized was **still ~53 ms** —
because the PNG encode + base64/IPC of a ~905 KB image, not the readback, was the
real bottleneck. JPEG was the actual lever (it took the combination to ~6 ms);
avenue (a) alone was a ~27% win. *Three D3D9 preconditions the naïve `StretchRect`
would have silently broken* were caught by a first-party-docs verification pass +
an adversarial red-team **before** coding, then guarded in code: (i) `offscreenRT`
is the engine's **currently-bound** slot-0 render target
([`engine.cpp:674`](src/engine.cpp:674)/[`:943`](src/engine.cpp:943)), and
`StretchRect` from the active RT can return `D3DERR_INVALIDCALL` — so slot 0 is
parked on the swap-chain back buffer for the blit and restored immediately after;
(ii) the source is an RT *texture*, needing `D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES`;
(iii) `D3DTEXF_LINEAR` needs `StretchRectFilterCaps & D3DPTFILTERCAPS_MINFLINEAR`.
Any cap miss or runtime failure falls back to the original full-readback + GDI+
path — a zero-regression fallback verified by forcing it through the full 174-test
native harness (still 174/0).

---

### Smooth the viewport edge during the right-dock open/close slide

*2026-06-08 · [`3eb6e28`](https://github.com/DrKnickers/particle-editor/commit/3eb6e28) · #97*

Opening or closing the right dock (Spawner / Lighting) animates the panel edge
over ~200ms; the D3D9 viewport used to **judder** against it — its cropped edge
advancing in irregular integer steps while the panel glided smoothly. Now the
viewport edge **glides with the panel** for the whole slide, in both directions
and under rapid re-toggling. `prefers-reduced-motion` users get an instant snap
(both edges together) instead of a tween. Behaviour is unchanged under `--legacy`.

**How we tackled it.** The root cause was **multi-clock sampling**, not the
1-frame clip lag an early hypothesis assumed (an adversarial pass corrected it):
the browser animates the panel on its own compositor clock with sub-pixel
flex-grow, while the uncapped host render loop sampled a clumpy, gap-ridden
`layout/scene-rect` stream (Δt mean 13ms / **stdev 20ms**, gaps to 109ms) and
snapped to integer pixels. Smoothing the stream is impossible — it's irregular at
the *emit* side — so the host now **generates** the rect itself. On the dock
toggle the web sends ONE `animate-scene-rect { from, to, durationMs, easing,
msElapsedAtSend }` ([`PanelLayout.tsx`](web/apps/editor/src/components/PanelLayout.tsx));
[`LayoutBroker`](src/host/LayoutBroker.cpp) then re-renders the engine at a
wall-clock-lerped scene rect every frame (a WebKit-style `UnitBezier` matched to
the CSS `ease` curve, off a QPC clock **back-dated** by `msElapsedAtSend` to the
CSS origin across the IPC hop), advanced in the render loop just before
`engine->Render()` ([`HostWindow.cpp`](src/host/HostWindow.cpp)). The host owns
the rect for the slide's duration, so the web **suppresses** its per-frame
`ResizeObserver` sends (RO-only — scroll/resize/DPR stay live) via a small zustand
signal channel ([`lib/dock-anim.ts`](web/apps/editor/src/lib/dock-anim.ts)), and
the host **self-defends** (`SetSceneRect` drops stray scene-rects while a slide
owns the rect). The target width `to` is computed **analytically** (the dock's
remembered width transfers to the centre column; verified against the panels
library's `getSize()` + `expandToSize`), and an authoritative `layout/scene-rect`
at the 260ms cleanup pins host and web to the true settled rect. The animation is
gated to **architecture C** on both sides (the host path is the DComp compositor;
the web extracted `isLegacyMode()` into [`lib/hosting-mode.ts`](web/apps/editor/src/lib/hosting-mode.ts)
so the suppression + send no-op under `--legacy`, else legacy would freeze-then-snap).

**Issues encountered and resolutions.** *Two load-bearing premises were proven
false before any code* (the trust-but-verify discipline,): the "1-frame clip
lag" root cause, and a planned "synchronous flexGrow read" for `to` — impossible,
since react-resizable-panels only emits on the next React commit. *The
cross-component suppression signal introduced a regression an adversarial review
caught*: a rapid re-toggle could cancel the 260ms timer that was the signal's only
clear, stranding it `true` and silencing the viewport's ResizeObserver
indefinitely — fixed by clearing the signal in the effect *teardown* (which also
covers unmount and a pre-existing CSS-class stick), with a regression test. *A
window-resize mid-slide* drove the anim toward a stale target (spec risk #4's
mitigation was unimplemented) — added `LayoutBroker::CancelSceneAnim()`, called
from the resize paths (`Apply` / `PredictAndApply`) on a genuine size change only,
so a pure window *move* (scene rect is client-relative) doesn't cancel. *Animation
timing can't be judged from agent screenshots or the throttled preview tab*
() — the bezier/phase were confirmed by the user in the real host (aligned on
the first try — the back-dated clock + matched curve left no residual jitter).

---

### UI polish: consistent padding, no clipped fields, softer curve keys, a denser emitter list, a Preferences menu, and mod-aware Open

*2026-06-08 · [`1239ff9`](https://github.com/DrKnickers/particle-editor/commit/1239ff9) · #94*

A batch of new-UI refinements. The **Physics** inspector tab now matches
Basic/Appearance padding; the **toolbar** has breathing room above the viewport
so a pressed button no longer sits flush against the preview; numeric fields no
longer **clip** 2-decimal values (the curve Time/Value fields and the inspector
spinner column were widened); curve **keys** carry a soft drop-shadow instead of
a hard black outline; the **emitter list** is denser; and the **autosave
recovery** dialog widened so long mod paths wrap instead of scrolling. Theme
switching moved out of the toolbar into a new **Edit → Preferences…** dialog with
a 3-way **Dark / Light / System** control (System follows the OS). Finally,
**File → Open** and **Import Emitters** now default to the selected mod's
`Data\Art\Models` folder (texture pickers still default to Textures).

**How we tackled it.** Mostly localized CSS/React edits under
[`web/apps/editor/src`](web/apps/editor/src) (`components.css`,
`EmitterPropertyTabs.tsx`, `CurveEditor.tsx` + `CurveEditorPanel.tsx`,
`EmitterTree.tsx`, a new `lib/theme.ts` + `PreferencesDialog.tsx`, `MenuBar.tsx`,
`App.tsx`, `Toolbar.tsx`). The density change kept the hard-coded `ROW_HEIGHT_PX`
in lockstep with the row padding so the link-group bracket gutter stays aligned.
The curve drop-shadow uses a CSS `filter: drop-shadow` on a `.curve-key-marker`
class rather than per-SVG `<filter>` defs. The mod-aware Open dir is one gated
edit to the shared `file/open` host handler
([`src/host/BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp:1986)) —
`filterId.empty()` restricts it to the `.alo` particle case so the skydome/ground
texture variants of the same handler are untouched, and it covers Import (which
reuses `file/open`).

**Issues encountered and resolutions.** *The inspector was already mostly
cut-off-proof* — Basic used a 73px spinner column and wide fields used
`widthBoost`; unifying the default column to 73px closed the remaining
Appearance/Physics gap. *Removing the toolbar theme toggle cascaded* across 19
a11y goldens (every composition snapshot includes the toolbar) plus a Playwright
`toolbar.spec` assertion — all rebaselined/updated. *Browser-mode screenshots
needed the `<canvas>` hidden* to settle the headless capture (—
agent renders of the real host are unreliable; values were tuned with the user).

---

### Fix crash when editing a shared property on a linked emitter (`xtree:181` dangling cursor)

*2026-06-08 · [`1239ff9`](https://github.com/DrKnickers/particle-editor/commit/1239ff9) · #94*

Editing a shared parameter on a member of a link group while the simulation had
live particles (e.g. Ctrl+scrolling **Burst delay** on one of several linked
emitters) crashed in Debug with *"cannot dereference value-initialized map/set
iterator"* (`xtree:181`). Pre-existing engine bug, surfaced while testing the UI
polish build.

**How we tackled it.** `BridgeDispatcher::propagateLinkGroup` copies the edited
emitter's shared params to each sibling via `copySharedParamsFrom`, which
reassigns the sibling's track multisets and orphans its live particles' cached
cursor iterators. It then calls `Engine::OnParticleSystemChanged(-1)` to reseat
them — but the `-1` branch of `EmitterInstance::onParticleSystemChanged` only
recomputed composites/textures/blend; the cursor reseat lived solely in the
per-track (`track >= 0`) branch. So the orphaned cursors stayed singular and the
next `Engine::Update` dereferenced one. The fix runs the cursor reseat for both
branches: `track == -1` now reseats EVERY track (a `track != -1` guard also
short-circuits the otherwise out-of-bounds `tracks[-1]` read). This makes `-1`
honor the "reseat everything" contract the comment already claimed, fixing
both the new-UI link path and the legacy `main.cpp` one.

**Issues encountered and resolutions.** The mitigation comment asserted
`OnParticleSystemChanged(-1)` reseated cursors; it never did. Verified by the
user's deterministic repro (the crash no longer fires) plus the native a11y
harness staying green; an automated regression test is hard here (the crash needs
live particles in linked siblings and *aborts* the process rather than failing
cleanly) — noted as a follow-up.

---

### New WebView2/React UI is now the default; `--legacy` opts back into the classic chrome

*2026-06-08 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Launching the x64 editor with **no flag** now opens the new WebView2 + D3D9 UI —
the same interface previously gated behind `--new-ui`. To run the classic Win32
chrome, pass the net-new **`--legacy`** flag (alias **`--legacy-ui`**, mirroring
`--new-ui`). The x86 build is unaffected: it has
no host and always runs legacy. `--new-ui` still works but is now redundant (it
selects the default). The new-UI **About** dialog also gains the upstream
attribution the legacy About already showed — *"Forked from Mike.NL's GlyphX
Particle Editor v1.5"*. This change also lands the standard public-repo
scaffolding (`CONTRIBUTING.md`, `SECURITY.md`, the bug-report issue template, and
`DEVELOPMENT_LOG.md`).

**How we tackled it.** The flip is a localized edit to the arg block in
[`src/main.cpp`](src/main.cpp:8058). The `newUi` initializer is **x64-gated**
(`#ifdef _WIN64` → `true`, `#else` → `false`): a flat default-true would break x86,
whose dispatch `#else` branch hard-`return -1`s for a host it can't provide. A
net-new `--legacy` flag (plus the `--legacy-ui` alias — the name the codebase's
comments already used, which only "worked" before because legacy was the default)
is collected in the existing flag loop and applied after it as
`if (legacy) newUi = false;` — placed *before* the `--capture` clamp so a headless
`--capture` run (which needs the host to own the Engine) still wins. The flag logic
is layered clamps rather than nested conditionals, so reading top-to-bottom is the
precedence spec: capture > legacy > x64-default. The native test harness is
unaffected — it passes `--new-ui --test-host`, and post-flip `--new-ui` is a harmless
no-op while `--test-host` alone enters the host.

**Issues encountered and resolutions.** *The x86 gate is the one real correctness
constraint.* x86 isn't in the `.sln` (the host is x64-only), so the gate is verified
by inspection of the two preprocessor guards — the initializer `#ifdef _WIN64` and the
dispatch `#else return -1` — rather than an x86 run. *Scaffolding docs were ported
byte-identical from `master`* (verified by matching blob hashes); they're add-only on
`integration-branch`, so they survive the eventual `merge -s ours` supersede. One pre-existing wrinkle
carried over verbatim: `master`'s `CONTRIBUTING.md` references a
`.github/PULL_REQUEST_TEMPLATE.md` that exists on neither branch — a dangling link
inherited from upstream, left as-is to keep the port faithful.

---

### Per-tick undo coalescing for curve-key spinner edits

*2026-06-08 · [`dd3db53`](https://github.com/DrKnickers/particle-editor/commit/dd3db53) · #92*

Streaming a curve key's **Time** / **Value** spinner — by wheel, hold-arrow, or
arrow-column scrub — now records a **single** undo step per gesture instead of one per
tick, and a multi-key group shift records one step instead of N-per-tick. One Ctrl+Z
reverts the whole gesture, matching how the emitter-property spinners already behave.
This closes the last open item in the UI delta report.

**How we tackled it.** Host-only: [`src/host/BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp:3605)'s
`emitters/set-track-key` handler now computes a per-track/per-emitter `coalesceKey`
(`0x80000000 | (trackIdx << 16) | id`) and passes it to the existing `captureUndo` →
[`UndoStack::CapturePreCoalesced`](src/UndoStack.cpp:126) (PRE-mutation skip-coalescing,
1500 ms window) — previously it captured with `coalesceKey = 0` (never coalesce). The key
layout mirrors the shipped emitter-property coalescing, substituting `trackIdx` for the
field-name hash. **Per-track** keying (legacy's `track<<16|emitterIdx`) is the only stable
choice: a Time spinner's `oldTime` changes every tick, so a per-key scheme can't match
tick-to-tick. No React changes — the spinners already dispatch one `set-track-key` per
tick; only the host's undo bookkeeping changed. Every other track-mutating command
(add / delete / interpolation / lock / duplicate-index / rescale) deliberately stays
`coalesceKey = 0` so each remains its own undo step.

**Issues encountered and resolutions.** *Granularity is a conscious divergence from the
emitter path.* The emitter spinners coalesce per-**field**; track keys coalesce
per-**track**, so editing two different keys on one track within 1.5 s folds into one
undo. This is legacy-faithful and unavoidable for the Time spinner (its key identity
drifts per tick); a finer Value-only scheme is a possible future follow-up. *Accepted
bit-collision (negligible).* Track keys put `trackIdx` (0–6) in the same bits the
emitter path fills with a 15-bit field hash; a cross-type fold needs the hash to land in
{0..6} on the same emitter within the window (≈1/4681) and its worst case is one extra
fold — whole-system snapshots + per-entry `selectedIndex` mean no data loss. *Tests need
no key seeding:* the `--test-host` default fixture pre-seeds every track with border keys
at t=0/t=100, so the regressions move the distinct `scale` / `rotationSpeed` tracks
directly (Green/Blue/Alpha alias Red and are avoided).

---

### Animated dock open/close + left-pane flicker fix (new-UI)

*2026-06-07 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

The right-dock (Spawner / Lighting) now **slides open and closed** instead of snapping, and
toggling it no longer **flickers the left pane**. Opening tweens the column out (0→~260px);
closing slides it back. Splitter drags and window resizes stay instant (the transition is
armed only for the duration of a toggle). Spawner↔Lighting swaps are an instant content swap
(the column is already open).

**How tackled.** The outer Group in [`PanelLayout.tsx`](src/components/PanelLayout.tsx) used to
carry `key={dockVisible ? "3col" : "2col"}`, so every toggle **remounted the whole layout —
left pane included** (the flicker, and the reason animation was impossible). It's now a single
always-mounted `collapsible collapsedSize={0}` Panel driven by an imperative `usePanelRef()`
(collapse/expand), with a toggle-scoped `flex-grow` transition (`.dock-animating` in
[`components.css`](src/styles/components.css)) and a ~260ms `displayDock` content-lag so the
pane slides out rather than popping. One persistence key replaces the old 2col/3col dual-key
machinery. (Original work re-applied from the reverted `ddb0777`.)

**Issues encountered and resolutions.** The animation was reverted once with a misdiagnosis —
"native host hang needing a debugger." It is **not** a host hang: the host stays healthy across
the whole native run (clean `host.log`, no crash dumps, 170+ specs pass *after* the failure,
harness exit 1 not 2). A Playwright trace (`--trace retain-on-failure`) pinned the real cause:
the harness helper `closeAnyPanel` clicks a panel's Close button, and during the ~260ms close
slide-out the panel is still mounted (for the animation) but collapsing-to-zero and about to
unmount — so the click is "intercepted by the animating group" then "detached," and Playwright
retries the full 30s. It only surfaces in the *full ordered* run (a prior test's dock-close is
still animating when the next test's `closeAnyPanel` fires) and never in isolation; **real
users are unaffected** (a human doesn't click a panel sliding shut). Fix: a closing panel no
longer presents as an open, interactive dialog — [`ToolPanel.tsx`](src/components/ToolPanel.tsx)
takes a `closing` prop that stamps `data-state="closing"` (so it leaves the
`[role="dialog"]:not([data-state])` selector), [`LightingPanel.tsx`](src/screens/LightingPanel.tsx)
forwards it, and [`PanelLayout.tsx`](src/components/PanelLayout.tsx) computes
`dockClosing = dock===null && displayDock!==null`, passes it down, and marks the closing
`<aside>` `inert` (also an a11y correctness win — a sliding-out panel isn't interactive).
`splitters.spec.ts` was rewritten to assert the new collapse-not-remount behaviour (a marker
on the left pane survives the toggle = the flicker fix), and `ToolPanel.test.tsx` guards the
`closing`→`data-state` contract. Native harness **175/0** (a11y goldens unchanged — the
always-mounted dock is a11y-equivalent in the default open state); web vitest **510/0**.

---

### Interaction polish: stable scrollbar gutter, wider texture field, clickable curve keys, splitter cursor (new-UI)

*2026-06-07 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Four usability fixes. (1) Expanding an inspector/tool-panel section that overflows into a
scrollbar no longer **shoves the panel sideways** — the scrollbar gutter is reserved. (2) The
Appearance tab's **texture filename field is wider** now that "Color:" / "Bump:" labels are
short (the freed space flows into the input). (3) **Curve keys are easier to click** — the
hit-pad is larger and, crucially, the axis labels no longer steal clicks from endpoint keys.
(4) The **splitter resize cursor** only appears over the splitter now, not 3px onto the
viewport.

**How tackled.** (1) `scrollbar-gutter: stable` ([`base.css`](src/styles/base.css)) on the
inspector tab + tool-panel scrollers. (2) Texture row label column `96px → 44px`
([`components.css`](src/styles/components.css)), keeping the two rows aligned. (3) Key hit-pad
radius `10/12 → 14/16` ([`CurveEditor.tsx`](src/screens/CurveEditor.tsx)) **plus**
`pointer-events: none` on the axis-label containers
([`CurveEditorPanel.tsx`](src/components/CurveEditorPanel.tsx)). (4) Splitter visible band
`4px → 8px` and `resizeTargetMinimumSize.fine = 8` on the Groups
([`PanelLayout.tsx`](src/components/PanelLayout.tsx)) so react-resizable-panels stops inflating
the thin handle to its 10px default.

**Issues encountered and resolutions.** The curve-key fix turned out to be two bugs in one:
enlarging the pad helped interior keys, but **endpoint keys (value 0/1) were unclickable** —
they draw into the label gutter via the SVG's `overflow:visible`, and the label span (being
outside the plot SVG) made the grid's `onPointerDown` treat the press as a *gutter* press and
start a marquee instead of selecting the key. Making the labels click-through routes the press
to the key while still letting an empty-gutter press start a marquee (the session-19
marquee-from-gutters feature — verified preserved with real Playwright input,). Widening
the splitter to 8px also drifted the `splitters.spec.ts` default-layout check past its ±1%
tolerance (its floor formula divided by the full group width instead of the available
space-minus-handles); corrected the formula. No a11y golden change; native harness 169/0.

---

### Animated expand/collapse for collapsible sections (new-UI)

*2026-06-07 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Every caret/disclosure section in the new UI now opens and closes with a smooth height
animation instead of snapping. This covers the inspector property-tab sections, the Lighting
and Spawner tool-panel sections, and the Link Group Settings category groups — all the
"groups with a caret."

**How tackled.** A single shared CSS utility, `.collapse-anim`
([`components.css`](src/styles/components.css)), animates a one-row grid's track from `1fr`
to `0fr` (the modern dependency-free collapse trick); a padding-free clip div inside it lets
the body reach a true `0`, and the collapsed body is pulled out of the a11y tree + tab order
via `visibility:hidden` applied *after* the collapse finishes (transition-delay), so the
content is still visible while it slides shut. `prefers-reduced-motion` disables the tween.
All three disclosure patterns route through it. The tool-panel sections
([`ToolPanel.tsx`](src/components/ToolPanel.tsx)) were converted from native `<details>` to a
controlled `useState` header (matching `Section.tsx`) because native `<details>` toggles
content instantly and can't tween.

**Issues encountered and resolutions.** Two things surfaced during the native re-baseline. (1)
The body's `6px+2px` vertical padding kept the collapsed grid track at ~8px until the
padding-free clip div was added — padding can't be clipped by `min-height:0`. (2) The
`<details>`→`button` conversion re-baselined 19 composition goldens under one shared cause
(the Spawner panel, present in every full-page golden, plus the Lighting dialog), and broke
two native specs that clicked `summary:has-text("Bloom")` — updated to
`[role="button"]:has-text("Bloom")`. Collapsed sections correctly stay out of the a11y tree
(the real browser honours `visibility:hidden`), matching the prior unmount behaviour.

---

### Curve, link-group, and Appearance polish (new-UI)

*2026-06-07 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Three small parity/usability tweaks. The Appearance tab's texture labels shorten from "Color
texture:" / "Bump texture:" to **"Color:" / "Bump:"**. The curve editor's **Rotation** channel
is now **exclusive** like Index and Scale — soloing it hides the others (its degrees/sec scale
doesn't share the 0..1 RGBA band). And clicking a **link-group bracket** in the emitter tree
now **selects every member of that group** (it was visual-only before).

**How tackled.** Labels: two `label=` props in
[`EmitterPropertyTabs.tsx`](src/screens/EmitterPropertyTabs.tsx). Rotation: one entry added to
the central `EXCLUSIVE_CHANNELS` set ([`CurveEditorPanel.tsx`](src/components/CurveEditorPanel.tsx)),
which three call-sites already read. Bracket selection: the bracket gained a one-lane-wide
(10px) clickable hit-zone wrapping the visible 2px line, dispatching `setIds(members, members[0])`
+ an `emitters/select` primary sync ([`EmitterTree.tsx`](src/screens/EmitterTree.tsx)).

**Issues encountered and resolutions.** The bracket was deliberately `pointer-events-none`
(: a clickable bracket once "wiped an in-progress selection" because it overlays the
full-width row buttons). The fix keeps pointer events on *only* the 10px hit-zone (the gutter
stays inert) and `stopPropagation`s the click + pointerdown so the press never reaches the row
or starts a marquee. Verified live via `elementFromPoint` — the bracket owns only its 10px
band; the row owns every other x.

---

### Paste As ▸ Lifetime / Death Child (new-UI · /)

*2026-06-07 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

The emitter-tree context menu regains the legacy **"Paste As ▸ Child"** capability. After
copying or cutting an emitter, right-click another emitter and choose **Paste As ▸ Lifetime
Child** or **Death Child** to drop the clipboard emitter (with its whole subtree) into that
emitter's child slot, instead of pasting it as a new root. Each item is greyed unless the
clipboard has content **and** that slot is free — the same single-occupancy rule as the
existing "Add Lifetime/Death Child" items and legacy's `spawnDuringLife/spawnOnDeath == -1`
gates. The "Paste As" submenu itself greys out while nothing is copied.

**How tackled.** A new `emitters/paste-as-child { parentId, slot }` bridge command,
implemented in both the C++ host ([`BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp:4605))
and the MockBridge. The host handler is a splice of two paths already in the file — the
`emitters/paste` deserialise (`MemoryFile` + `ChunkReader` + `GenerateDuplicateName`) and the
`emitters/add-lifetime-child` attach (`addLifetimeEmitter`/`addDeathEmitter`, which self-guard
by returning `NULL` on an occupied slot). The tree submenu uses Radix `ContextMenu.Sub` with
its own `OccludingContextSubContent` wrapper, so the submenu — rendered in a separate portal —
registers its own viewport-occlusion rect and isn't overpainted by the layered D3D viewport
popup. A multi-emitter clipboard pastes only the first buffer (a slot holds one child).

**Issues encountered and resolutions.** The mock's `pasteAsChildFromClipboard` first re-id'd
only the *top* pasted node, so copying an emitter whose descendant ids collided with existing
emitters produced duplicate React keys (caught live: pasting "Smoke" — children ids 1, 2 — over
a tree that already had ids 1, 2). Fixed by re-id'ing the *whole* subtree via the existing
`reassignIdsInPlace`, with a regression test asserting global id-uniqueness. The native engine
was never affected (it assigns sequential indices on insert), but the bug is a good reminder
that mock tree helpers must re-id entire subtrees. No a11y golden change — the tree context
menu is opened only transiently to reach dialogs and is never itself a captured surface. Web
suite 510; native harness 169 (incl. a new real-host round-trip spec).

---

### Import Emitters dialog "Clear" button (new-UI ·)

*2026-06-07 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

The new-UI **Import Emitters** dialog regains the legacy **"Clear"** button. Legacy ships two
selection buttons in that dialog's footer — "Select all" and "Clear" — but the React port had only
"Select All". "Clear" sits immediately to its right and deselects every emitter in one click (the
inverse of Select All), so you can drop a full multi-select and re-pick without unticking nodes one
at a time. It's disabled while nothing is selected — matching how the dialog already greys "Select
All" on an empty tree and "Import" on an empty selection.

**How tackled.** A two-line change in
[`ImportEmittersDialog.tsx`](web/apps/editor/src/screens/ImportEmittersDialog.tsx:132):
`handleClear = () => setPicks(new Set())` mirrors the existing `handleSelectAll`, reproducing legacy
`IDC_IMPORT_CLEAR` → `SetCheckRecursive(…, FALSE)` ([`main.cpp`](src/main.cpp:7326)). Footer layout
keeps both selection buttons grouped at the left by moving the `mr-auto` flex spacer from "Select
All" onto the new "Clear" — so the row reads `Select All │ Clear … Cancel │ Import`, the legacy
left-to-right order (`Select all` x=170, `Clear` x=226 in `ParticleEditor.en.rc`).

**Issues encountered and resolutions.** The one a11y composition golden that captures this dialog
(`dialog-import-emitters.composition.golden.yaml`) had to be re-baselined — it gains exactly one
node, `button "Clear selection" [disabled]: Clear`, inserted after "Select all emitters". The YAML
goldens key on role + accessible name + state (not `className`), so dropping `mr-auto` from Select
All produced no spurious diff. Regenerating required restoring the native lane in a fresh worktree
(NuGet copy + MSBuild Debug x64 + dist build) and rebuilding `dist` before the
harness so it served the new bundle rather than a stale one (). Suite 502 web tests; native
harness 168/0.

---

### Status-bar parity: shift-to-spawn hint, PAUSED indicator, 2dp cursor (new-UI)

*2026-06-06 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

The new-UI status bar regains three elements the legacy Win32 bar had and the React port had
dropped (/7/8). A persistent **"⇧ Shift: spawn instance"** hint now sits pinned to the far
right of the bar — the on-screen cue for the hold-Shift-over-the-viewport spawn gesture. A
**PAUSED** indicator (amber) appears just left of it whenever the preview is paused, mirroring the
toolbar's Play/Pause state so the paused state is legible without hunting for the button. And the
viewport **cursor readout** now shows 2 decimal places (e.g. `0.00, 0.00, 0.00`) to match legacy,
up from the 1dp the port had used.

**How tackled.** All three are render-only changes in
[`StatusBar.tsx`](web/apps/editor/src/components/StatusBar.tsx). PAUSED reuses the exact pause
signal the toolbar already consumes — an `engine/state/snapshot` read plus the
`engine/state/changed` subscription, off `EngineStateDto.paused` (the host's `IsPreviewPaused()`) —
so no new bridge command was needed and the two indicators can never disagree. The hint is pinned
right with a single `ml-auto` flex wrapper, reproducing legacy's rightmost-pane intent
([`main.cpp`](src/main.cpp:2036)). The hint shortens legacy's "Press SHIFT to spawn an instance"
to fit the denser dark bar (user choice).

**Issues encountered and resolutions.** The a11y composition goldens capture the status bar as a
`contentinfo` landmark, so the new always-on hint cascaded into 19 surface goldens (one identical
`contentinfo` text delta each —). Re-baselining surfaced a native-harness gotcha (now
****): `pnpm a11y:update --rebuild` only rebuilds `dist/` when its baked *hosting mode*
mismatches — it does **not** detect source changes, so a matching-mode-but-stale `dist/` silently
serves the old UI and the run falsely passes with no golden diff. The fix is to `pnpm build`
manually before the harness. Because the capture spec pauses the preview for determinism
(`engine/set/paused {paused:true}`), the re-baselined goldens now lock in *both* the hint and the
PAUSED indicator rendering, rather than just the hint.

---

### Curve-editor marquee can start from the axis-label gutters (new-UI)

*2026-06-05 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

You can now begin a rubber-band selection in the curve editor's axis-label margins — the 36px
column of Y-axis numbers on the left and the 22px row of time labels along the bottom — not just
inside the plot itself. A marquee that starts in a gutter begins right at the press point — the
selection rectangle extends from the margin into the plot, with no snapping to the grid edge — and
then sweeps and selects exactly as before. Esc still
cancels an in-progress marquee, and the behaviour is scoped to Select mode (a gutter press in
Insert mode does nothing).

**How tackled.** The interactive curve editor is `MultiChannelCurves`
([`CurveEditor.tsx`](web/apps/editor/src/screens/CurveEditor.tsx:1071)), whose marquee already
tracks everywhere via `setPointerCapture` once started — so the only gap was *starting* from a
gutter, which lies outside the plot `<svg>`. The fix is additive: `MultiChannelCurves` exposes an
imperative `startMarquee(clientX, clientY, shift, pointerId)` (via a `marqueeRef` prop threaded
through `CurveEditor`) that maps the client point into the plot's viewBox (kept **un-clamped**, so
a gutter origin renders into the margin via the SVG's `overflow="visible"` rather than snapping to
the edge), seeds the marquee, and captures the pointer to the SVG — so the existing, unchanged
move/up/Esc machinery drives the rest. `CanvasWithAxisLabels`
([`CurveEditorPanel.tsx`](web/apps/editor/src/components/CurveEditorPanel.tsx:263)) gains an
`onGutterPointerDown` that fires when a primary press lands outside `[data-testid="curve-editor-svg"]`,
which `CurveEditorPanel` routes to `startMarquee` in Select mode.

**Issues encountered and resolutions.** The original deferral note assumed a risky
"margin-inclusive viewBox" rework to fight `preserveAspectRatio="none"` — but that described the
*single-track* `CurveEditor` branch, which the app never renders. The real editor
(`MultiChannelCurves`) already uses a CSS-pixel-measured viewBox (no aspect distortion) and a
`svgRef`, so the rework was unnecessary; reusing its existing pointer-capture marquee made the
change purely additive and left the delicate (and previously unit-untested) state machine
intact. This task also added the first marquee unit coverage for the multi-channel editor.
A second bug surfaced only under REAL mouse input (a synthetic `dispatchEvent` check was a false
positive): because the gutter marquee captures the SVG rather than the backdrop, the browser's
trailing synthetic `click` lands on the SVG, whose `onClick` guarded only the key-drag flag
(`dragConsumedClickRef`) and so fell through to `onCanvasClick`, clearing the just-made selection.
Fix: the SVG `onClick` now also honours `marqueeConsumedClickRef`, mirroring the backdrop
([`CurveEditor.tsx`](web/apps/editor/src/screens/CurveEditor.tsx:1631)); verified with Playwright
real-input drag (see). A third pass dropped the original start-point clamp: anchoring a
gutter origin to the plot edge read as the marquee "snapping to the grid" instead of beginning in
the margin, so `startMarquee` now keeps the raw press coordinate.

---

### Emitter-tree reorder-drag polish: edge autoscroll + Esc/right-click cancel (new-UI)

*2026-06-05 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Reordering emitters in a long list is no longer cramped by the viewport. Dragging a row near
the top or bottom edge of the emitter tree now autoscrolls the list — speed ramps up the closer
you get to the edge — and the drop indicator keeps tracking the rows that slide under the
pointer (). An in-progress reorder drag can now be cancelled with **Esc** or a
**right-click**, matching the legacy editor; the right-click also suppresses the row context
menu so it doesn't pop up over the abort (). A right-click *before* a drag starts still
opens the context menu as normal.

**How tackled.** Both behaviours live in the existing pointer-drag controller in
[`EmitterTree.tsx`](web/apps/editor/src/screens/EmitterTree.tsx:1220) — no new drag library. The
autoscroll decision is a pure, unit-tested helper
([`lib/drag-autoscroll.ts`](web/apps/editor/src/lib/drag-autoscroll.ts:1)) driven by a
`requestAnimationFrame` loop that adds the delta to the scroll viewport each frame; because a
held pointer fires no `pointermove` while content scrolls under it, the loop re-resolves the
drop target each frame via `elementFromPoint`. The cancel listeners (`keydown` Escape +
capture-phase `contextmenu`) attach when the drag goes *active* and detach in the existing
`finish()` teardown, so a pre-threshold right-click is untouched.

**Issues encountered and resolutions.** (1) The first instinct — route all hit-testing through
`document.elementFromPoint` — would have broken every existing drag test, since jsdom has no
layout and returns null; the event-driven path keeps using `ev.target` and only the autoscroll
loop uses `elementFromPoint`. (2) jsdom can't exercise real scrolling, so the autoscroll wiring
is verified live in the browser (a short-viewport drag scrolls 0→max at the bottom edge, back to
0 at the top, and halts mid-list). (3) Radix already `preventDefault`s `contextmenu`, so a
`defaultPrevented` assertion proves nothing about our suppression — the unit test asserts the
robust signal (the drag cancels with no drop) and the menu-suppression is confirmed in-browser.

---

### Reset-Camera parity verified + single source of truth (new-UI)

*2026-06-05 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

View → Reset Camera (and its `Ctrl+Home` shortcut) restores exactly the legacy camera —
eye `(0, -250, 125)`, target origin, up `+Z`. This was confirmed against the legacy editor at
every hop and is no longer two separate hard-coded copies, so the menu item and the shortcut
can't drift apart. No behavioural change — the vectors already matched; this locks that in.

**How tackled.** Both the menu item ([`MenuBar.tsx`](web/apps/editor/src/components/MenuBar.tsx:747))
and the `Ctrl+Home` accelerator ([`use-app-accelerators.ts`](web/apps/editor/src/lib/use-app-accelerators.ts:157))
dispatch `engine/set/camera`, whose host handler
([`BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp:1347)) maps the DTO 1:1 into an
`Engine::Camera` and calls the *same* `Engine::SetCamera()` the legacy `ID_VIEW_RESETCAMERA`
([`main.cpp`](src/main.cpp:1834)) invokes — so identical input vectors provably yield an
identical camera, matching the engine constructor's `m_eye` default
([`engine.cpp`](src/engine.cpp:2190)). The two duplicated literals were replaced by one exported
constant ([`lib/reset-camera.ts`](web/apps/editor/src/lib/reset-camera.ts)), pinned to the legacy
default by a unit test.

**Issues encountered and resolutions.** The delta report claimed the new UI had no `Ctrl+Home`
binding; reading the code showed the accelerator was in fact wired (stale note, now corrected).
`Vec3` is a `readonly` tuple, so the shared constant is annotated `CameraDto` and relies on
contextual typing to accept the plain array literals (`tsc --noEmit` clean).

---

### Crash-recovery autosave (new-UI)

*2026-06-05 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

The new UI now autosaves your work in the background and offers to recover it after a crash —
matching the legacy editor. Two tiers run while you edit: a 30-second "recent" snapshot and a
5-minute "stable" one, written to `%TEMP%\AloParticleEditor\` (never over your own `.alo`). If
the editor crashes and you relaunch, a dialog offers to restore the most recent autosave, the
older stable backup, or discard them — showing the original filename and how long ago each was
saved. Restored work opens as unsaved changes to the original file, so Ctrl+S saves it back
where it belongs. Dismissing without choosing keeps the autosave for next launch (no accidental
data loss).

**How tackled.** The legacy `Autosave` data layer ([`src/Autosave.cpp`](src/Autosave.cpp:1),
shipped #41) is UI-agnostic and already linked into the exe, so the port is pure wiring.
[`src/host/HostWindow.cpp`](src/host/HostWindow.cpp:2104) drives the two Win32 timers in
`WM_TIMER` (dirty-gated `Autosave::Write`) and deletes the session's files on a clean exit;
[`src/host/BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp:1) gains two commands —
`autosave/check-recovery` (scan → orphan|null) and `autosave/recover` (load the chosen tier
via the same swap+notify sequence `file/open` uses, then present it as the original filename
with `dirty=true`). The prompt is a React dialog
([`AutosaveRecoveryDialog.tsx`](web/apps/editor/src/screens/AutosaveRecoveryDialog.tsx:1)), and
— a refinement over the first design — it's React-initiated (the dialog calls check-recovery on
mount) rather than host-pushed, which sidesteps a startup race where a host event could fire
before React subscribed.

**Issues encountered and resolutions.** Two interactions needed care. (1) Restoring a document
while a cursor-bound spawn instance is live asserts in the engine (), so recover reuses
`file/open`'s exact kill-attached + `OnParticleSystemChanged(-1)` reseat instead of hand-rolling
the load. (2) A real recovery prompt during the `--test-host` harness would corrupt a11y
captures (cf.), so the autosave timers are gated off under `--test-host` (no orphan files
left behind) and check-recovery returns null there; the dialog's a11y golden is driven by a
fixed-orphan `?demo=autosave-recovery` route with a pinned clock, so the relative-age text
("45 seconds ago") is deterministic. The live autosave-write + crash→recover round-trip can't
run under `--test-host`, so it's covered by the legacy module's existing tests plus a manual
crash smoke; the harness covers suppression, the recover no-op, and the dialog a11y.

---

### Scroll-wheel / rapid spinner edits now undo as one step (new-UI)

*2026-06-05 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Changing an emitter value with several quick scroll-wheel ticks (or a held spinner
arrow) used to record one undo entry *per tick*, so reverting the gesture took as many
Ctrl+Z presses as ticks. Now a burst of rapid edits to the **same field** collapses into a
single undo/redo step (within a ~1.5 s window). Switching to a different field — or pausing
longer than the window — starts a fresh step, so each field stays independently undoable.
(This is finer than the legacy editor, which folded all edits on one emitter together; the
per-field granularity was a deliberate choice for the new UI.)

**How tackled.** Legacy coalesced `EP_CHANGE` notifications by
`MakeCoalesceKey(EP_CHANGE, emitterIdx)` ([`src/main.cpp`](src/main.cpp:2682)). The new UI
takes the same window-based approach but with a per-field key:
[`src/host/BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp:2783)'s
`emitters/set-properties` builds a coalesce key from an order-independent FNV-1a hash of the
patch's field names plus the emitter id (top bit set so it's never the structural `0`), and
passes it to `captureUndo`. The twist is that captures snapshots PRE-mutation (legacy
captured POST), so the existing `Capture()` coalesce — which *replaces* the tail with the
latest state — would overwrite the burst's session-start state (the undo target). The fix
adds [`UndoStack::CapturePreCoalesced`](src/UndoStack.cpp:126): when the previous entry
shares the key within the window at the head of history, it *skips* the capture (keeping the
session-start snapshot); the head-of-history auto-cap in `undo/perform` then snapshots the
final live state on the first undo, so one undo spans the whole gesture.

**Issues encountered and resolutions.** PRE- vs POST-mutation capture need *opposite*
coalesce mechanics (skip vs replace) for the same UX — encoding skip as a separate method
left legacy's `Capture()` untouched. The behaviour is time-windowed, so the regression
tests ([`tests/undo-navigation.spec.ts`](web/apps/editor/tests/undo-navigation.spec.ts:1))
wait out `COALESCE_WINDOW_MS` in `beforeEach` to make the first edit of each test
deterministically start a fresh entry rather than fold into a prior test's same-field edit,
and cover both same-field folding and different-field separation. Scope: emitter property
edits only — engine/preview edits don't capture undo, and plain spinner *drag* already
commits once on release (Spinner.tsx:18).

---

### Undo no longer swallows a step after a redo (new-UI)

*2026-06-05 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

In the new UI, an `undo → redo → undo` sequence used to lose the second undo — after
redoing back to the tip, the next Ctrl+Z (or Edit → Undo) did nothing, and the stuck
state corrupted further undo/redo navigation. Now redo-then-undo steps back correctly,
and repeated undo/redo cycles are stable. Plain single-edit undo/redo, structural-op
undo, link-group atomic undo, and the import/clipboard undo paths were already correct
and remain so.

**How we tackled it.** The new UI captures undo snapshots PRE-mutation (legacy captured
POST), so after a fresh edit the live `ParticleSystem` sits one step ahead of the stack
tip. [`src/host/BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp:1693)'s `undo/perform`
head-of-history auto-capture snapshotted live before stepping back, gated only on
`Cursor() == Depth()`. That condition is *also* true right after a `Redo()` (redo to the
tip leaves `cursor == size`), where live is already in sync — so the auto-cap fired
spuriously, duplicated the tip, and the next `Undo()` returned the duplicate. The fix adds
an explicit `m_liveAhead` flag to [`src/UndoStack.cpp`](src/UndoStack.cpp:64) — set in
`Capture()` (every editing capture precedes a mutation), cleared in `Undo()`/`Redo()`
(navigation re-syncs live to the restored entry) — and gates both the auto-cap and
`ComputeCanUndo()` on it, so the flag names the intent the cursor position only
approximated. Regression coverage: [`tests/undo-navigation.spec.ts`](web/apps/editor/tests/undo-navigation.spec.ts:1)
(added to the native harness list), driving the real host `UndoStack` over the
`--test-host` CDP bridge.

**Issues encountered and resolutions.** The bug is invisible to both the web suite
(MockBridge has no real `UndoStack`) and the existing native specs (which only covered a
single `edit → undo`, never `redo → undo`) — found only by driving `edit → undo → redo →
undo` over CDP and reading `emitters/get-properties` at each step. Spurious co-failures
in `splitters` and `a11y-dialogs-composition` during the full native run were confirmed
pre-existing/environmental (window-size + localStorage; golden drift) by stash-reverting
the fix, rebuilding the baseline binary, and reproducing them without it — they exercise
no code the fix touches. The literal Ctrl+Z keystroke could not be auto-verified: the host
intercepts accelerators via the native `AcceleratorKeyPressed` event, which CDP's
renderer-level key injection bypasses; the host-side undo logic is verified over
`window.bridge`, leaving the keystroke itself to an on-screen pass.

---

### Decimal numeric fields now display a consistent 2 decimal places

*2026-06-03 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Spinner fields that hold fractional values render uniformly at 2 decimal places
(e.g. Sun azimuth `0.00`, Position `0.00`, Burst interval `10.00`). Previously the
precision was derived from each field's `step`, so it varied across the app —
`45`, `0.5`, `0.50`, `1.000` all coexisted. Genuinely integer fields keep their
whole-number display: particle counts, Index, colour channels, and percentage
fields (burst size, texture elements, R/G/B/A, random-lifetime %, rescale %,
increment-by) are unchanged.

**How we tackled it.** The policy is centralized in the
[`Spinner`](web/apps/editor/src/primitives/Spinner.tsx) primitive: the display
precision now defaults to **2** (`decimals ?? 2`) instead of being computed from
`step`. Crucially, display precision is **decoupled** from the wheel/keyboard nudge
granularity — that still derives from `step` (`step >= 1` → ±1 per notch, else
±0.1), so an angle can show `45.00` yet still scroll in whole degrees. Integer
fields opt out by passing `decimals={0}`. Call sites that previously forced higher
precision (`decimals={3}` on positions/timing) were dropped to 2; integer fields
that had been relying on the old `step`-derived 0dp (colour channels, the
increment-index and rescale `%` dialogs, the CurveEditor `index` track + key time)
were given explicit `decimals={0}` so they stay whole.

**Issues encountered and resolutions.** The audit surfaced several integer fields
that were only integer *by accident* — they had no `decimals={0}` and relied on
`step={1}` deriving 0dp. Defaulting to 2dp would have shown them as `1.00` / `100.00`
(caught in the a11y golden diff: "Increment by", "Duration/Size scale"). Each was
given `decimals={0}`. The change re-baselined 19 composition a11y goldens (spinner
values are captured in the accessibility tree) — every diff is a value-format change,
no structural change, and the legacy UIA lane was left untouched. → ****.

---

### Left-panel (property tabs) section chevrons now animate like the Spawner's

*2026-06-03 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

The collapse/expand chevrons on the left inspector's Basic / Appearance / Physics
sections were stuck pointing one direction and never rotated, unlike the Spawner /
Lighting panel chevrons (which rotate down→left with a 0.12 s transition). They now
behave identically.

**How we tackled it.** The two are *already* the same chevron (`ChevronDown` +
`.chev`) and share the `.panel-section` styling — the defect was a single CSS selector.
The rotation rule served both consumers at once:
[`components.css`](web/apps/editor/src/styles/components.css) had
`.panel-section[data-open="false"] .chev, .panel-section:not([open]) .chev`. The second
arm is for the native-`<details>` consumer (`ToolPanel.Section` — Spawner/Lighting);
the first is for the controlled-`<div>` consumer (`Section.tsx` — property tabs). But a
`<div>` can never carry an `open` attribute, so `:not([open])` matched the property-tab
div in *every* state, pinning its chevron at -90°. Scoping that arm to
`details.panel-section:not([open])` lets the div rotate solely off its `data-open`
state — so it now reads 0° open / -90° closed, animated by the shared `.chev` transition.

**Issues encountered and resolutions.** Verifying this in the headless preview browser
was misleading: it doesn't advance CSS transitions, so `getComputedStyle` on an
interactively-toggled chevron reported the start frame (identity) indefinitely, making
the fix look broken. The end-states were confirmed by disabling the transition
(`transition: none`) and reading the settled value (`matrix(0,-1,1,0,0,0)` = -90° when
collapsed), plus measuring a section that renders collapsed initially. CSS-only change —
no a11y golden impact (transforms aren't in the accessibility tree) and no native
rebuild. → ****.

---

### New UI restores saved lighting from the registry, syncs Force Align with the legacy editor, and gains a Lighting toolbar toggle

*2026-06-03 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Lighting-parity improvements to the new (`--new-ui`) editor. First, the
viewport now **opens with your saved lighting**: the sun / fill 1 / fill 2 angles,
colours and intensities, plus ambient and shadow, are restored from the registry at
startup exactly as the legacy editor does — previously the new UI ignored them and
started from engine defaults, so tuned lights only showed up in the old editor. The
**Lighting panel also displays those true saved values** (intensity, base colour,
angles) rather than the folded approximation it used to recover from the engine
snapshot. Second, **Force Align Fill Lights now round-trips through the registry**
(`LightingForceFillAlignment`): toggling it in the new UI is seen by the legacy
editor and vice-versa, replacing the new UI's old session-only (localStorage)
behaviour. Third, the **toolbar gained a Lighting button** (lightbulb icon, next to
the Spawner button) that opens/closes the docked Lighting pane; because Lighting and
the Spawner share one exclusive right-dock slot, their two toolbar buttons are
mutually-exclusive (opening one un-presses the other).

**How we tackled it.** The lighting *render* restore is host-side: a new block in the
`!useTestHost` startup section of [`HostWindow.cpp`](src/host/HostWindow.cpp:1907)
mirrors legacy `PushLightingToEngine` ([`src/main.cpp`](src/main.cpp:6376))
field-for-field — same registry value names/types (floats `REG_BINARY`, colours
`REG_DWORD`), same `MakeLight` intensity-fold, same Force-Align fill-angle computation
(`sun.z + 120°/210°` at `−10°` tilt when on; persisted free-edit angles when off) —
building `Engine::Light`s directly and pushing them via `SetLight`/`SetAmbient`/`SetShadow`.
A permanent `[lighting-restore]` `host.log` line is the standing no-user verification
channel (it is the only one — the restore is gated off under `--test-host`, so the CDP
bridge can't observe it). The panel display + flag sync share one bridge surface: a
`settings/lighting` get returns the **raw** lighting split read from the registry
(intensity/colour kept separate, angles in degrees, plus the Force Align flag), and a
`settings/lighting-force-align/set` writes just the `REG_DWORD` on toggle — both in
[`BridgeDispatcher`](src/host/BridgeDispatcher.cpp). The React
[`LightingPanel`](web/apps/editor/src/screens/LightingPanel.tsx) seeds its displayed
controls from that DTO (dropping the lossy `azAltFromDirection` recovery and its
localStorage key) and writes the flag back on toggle. Determinism is handled by a gate
in the dispatcher: under `--test-host` the get returns canonical defaults and the set
no-ops (keeping the `dialog-lighting` a11y golden stable), **unless** the
`ALO_SETTINGS_LIVE` env var lifts the gate — a test seam that lets a CDP script drive
the real registry round-trip without disturbing the a11y harness. The
[`Toolbar`](web/apps/editor/src/components/Toolbar.tsx) button is a ~10-line mirror of
the existing Spawner button reusing `toggleDock("lighting")` from
[`lib/right-dock.ts`](web/apps/editor/src/lib/right-dock.ts).

**Issues encountered and resolutions.** (1) **The toolbar lives inside every chrome
snapshot.** Adding one toolbar button changed **19** composition a11y goldens, not the
one the plan predicted — every menubar / dialog / keyboard / property-tab surface embeds
the toolbar subtree. The diff stayed fully attributable (each file gained exactly one
`button "Toggle Lighting panel"` node; `dialog-lighting` got the `[pressed]` variant
because the Lighting pane is open there), so `a11y:update` (composition lane only) was
the right call; the legacy `*.golden.json` lane was left untouched per the two-lane rule.
(2) **A `!useTestHost`-gated restore can't be verified over the `--test-host` CDP bridge**
(the gate disables the very thing). The engine restore was verified from two faithful
non-test-host launches reading `host.log`: with Force Align on, the fills came out
computed (`fill1Z=120 fill2Z=210`); flipping the registry flag off, they came out at the
persisted saved angles (`fill1Z=129 fill2Z=301`) — distinct from both the ctor defaults
and the computed values, proving the restore reads saved registry data. (3) **The Force
Align write path looked un-automatable** for the same reason — gated under `--test-host`,
no CDP on a faithful launch. The `ALO_SETTINGS_LIVE` seam resolves it: a committed
on-demand script ([`scripts/verify-force-align.mjs`](web/apps/editor/scripts/verify-force-align.mjs))
launches `--test-host` with the env var, drives the real Lighting checkbox over CDP, and
asserts both the registry write (`LightingForceFillAlignment → 0`) and the raw panel
display (Sun intensity shows `0.50`, not the folded `1`), restoring the registry in a
`finally`. 5/5 checks pass with no user participation.

---

### Lighting is now a docked pane (sharing the Spawner's slot) with Bloom settings folded in

*2026-06-02 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

The Lighting panel is no longer a floating overlay that covers the viewport — it's a
**docked, full-height column** on the right, behaving exactly like the Spawner panel
(it carves space from the centre column, pushing the viewport + curve editor narrower).
Lighting and the Spawner **share one right-dock slot**: opening Lighting closes the
Spawner and vice-versa, so the viewport is never squeezed into a fourth column. The
**Bloom settings** (Enable / Strength / Cutoff / Size) now live as a collapsible section
at the **bottom** of the Lighting pane (below Mirror Sun / Reset), and **both** former
View-menu Bloom entries are gone: "Bloom Settings…" folded into that section, and the
on/off "Bloom" toggle is now solely the toolbar's "Toggle bloom" button (no duplicate menu
item). The **Force Align Fill Lights** toggle moved out of the footer to sit directly under
the Fill 1 / Fill 2 sections it governs (it snaps the fills' azimuth to the sun). Open
Lighting from View → Lighting; the Spawner toggles from the toolbar or View → Spawner (F7).

**How we tackled it.** The Spawner column had already solved every hard layout problem
(width persistence across the 2-col/3-col mode switch, carrying widths on toggle, the
curve-editor reflow in [`PanelLayout.tsx`](web/apps/editor/src/components/PanelLayout.tsx)),
so the work was to make that slot **content-agnostic** rather than rebuild it. A new
[`lib/right-dock.ts`](web/apps/editor/src/lib/right-dock.ts) store (`dock: "spawner" |
"lighting" | null`, exclusive `toggle`) replaces the old `spawner-visibility` boolean;
`PanelLayout` keys its outer-Group remount + the `deriveOuterLayoutOnToggle` width-carry
on dock *presence* (so a spawner↔lighting swap keeps the column open and reflows nothing —
only open↔closed carves/absorbs the width). `ToolPanel` gained a `variant="docked"` that
fills its column and skips the viewport hole-punch (a docked column sits beside the engine,
not over it). The bloom controls were lifted into a self-contained
[`BloomSection.tsx`](web/apps/editor/src/screens/BloomSection.tsx) so the 550-line
`LightingPanel` stayed focused. `right-dock` migrates the legacy `alo:spawner-visible`
localStorage key so existing users keep their column. Net **−215 lines** in the touched
files — the feature mostly *removed* code by reusing the dock machinery. Pure web-layer
change; no native rebuild.

**Issues encountered and resolutions.** Regenerating the a11y goldens revealed the two
lanes diverge: the **composition** lane (the documented `157/4` baseline) updated cleanly
to exactly two surgical golden diffs — `dialog-lighting` (Lighting moved overlay→docked
`complementary`, the Spawner toggle correctly loses `[pressed]` because Lighting took the
exclusive slot, and a `group: Bloom` appears) and `menubar-view-open` (one line removed).
The **legacy UIA** lane (`*.golden.json`), by contrast, is unmaintained — regenerating it
churned ~25 unrelated surfaces (`emitter-tree`, `property-tabs`, `kbd-*`…), i.e. accumulated
drift from sessions that only kept the composition lane current. Committing that blanket
update would mask regressions in noise, so the legacy lane was left untouched (only the
removed Bloom surface's golden was dropped from both lanes). Captured as ****.

---

### Ground, background, and skydome view settings restored from the registry in the new-UI host

*2026-06-02 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Launching the new UI now opens the viewport with the same persisted view settings the
legacy editor restores — your tuned **background colour**, **ground visibility**, **ground
texture** (per-slot custom paths, the solid-colour slot, and the selected slot), and
**skydome** (custom slot paths + the selected slot). Previously the new-UI host restored
none of these (only recent-files + last-mod), so a ground/background/skydome you tuned in
the legacy editor reset to engine defaults in the new UI. This completes the same parity
sweep the [bloom restore](#bloom-settings-restored-from-the-registry-in-the-new-ui-host)
started. Ground-Z is intentionally *not* restored — legacy deliberately resets it to 0 each
launch ([`src/main.cpp`](src/main.cpp:7626)), and the new UI mirrors that.

**How we tackled it.** [`HostWindow`](src/host/HostWindow.cpp:1799) reads the values from
`HKCU\Software\AloParticleEditor` right after the `Engine` is constructed, folded into the
**same** `if (!useTestHost) { … }` registry block the bloom restore opened — one key open,
one gate, reusing `hKey`. It mirrors legacy's startup sequence at
[`src/main.cpp`](src/main.cpp:7614) in the load-bearing order (ground slot custom paths
*before* `SetGroundTexture`, skydome custom paths *before* `SetSkydomeSlot`) and reuses the
exact value names/types (`BackgroundColor`/`ShowGround`/`GroundTexture`/`GroundSolidColor`
as `REG_DWORD`, `GroundTextureSlot%d`/`SkydomeCustomSlot%d` as `REG_SZ`, `SkydomeIndex` as
bounds-checked `REG_DWORD`), so settings round-trip between the two UIs. Because the legacy
`Read*` helpers are `static` in `main.cpp` (no external linkage), the reads are inlined via
two small lambdas (`readDword`, a two-pass-sized `readSz`) rather than shared.

**Issues encountered and resolutions.** The verification channel and the gate are in
tension: the only no-user way to drive the new-UI host is the `--test-host` CDP bridge, but
the whole restore is gated *off* under `--test-host` (so the `dialog-lighting` "Show ground"
a11y golden stays deterministic — same reason bloom is gated). The bridge therefore can
**never** observe the restored values. Resolved by emitting a permanent `[view-restore]`
line to `host.log` and verifying from a faithful **non**-test-host launch: a fresh launch
logged `bg=0x6E6E6E showGround=1 groundTex=5 groundSolid=0x626262 skydome=1` — every field
the saved registry value, none the engine ctor default. `host.log` is the trusted
verification surface (agent screenshots are not —). Captured as ****.

---

### Emitter-tree drag-to-reorder works under (pointer events replace HTML5 DnD)

*2026-06-02 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Dragging emitter rows in the tree to reorder roots or reparent a child works again in
the new UI. Previously the row "wouldn't pick up at all" — the drag never started.

**How we tackled it.** The reorder/reparent was built on **HTML5 drag-and-drop**
(`draggable` + `onDragStart`/`onDragOver`/`onDrop`), which never initiates under
composition hosting: HTML5 DnD needs the OS drag loop (`DoDragDrop`), which needs an HWND,
but in composition hosting WebView2 is a composition *visual* with no HWND, so `dragstart`
never fires. Rebuilt the drag on **pointer events** ([`EmitterTree.tsx`](web/apps/editor/src/screens/EmitterTree.tsx)),
which deliver like clicks in every hosting mode (and on touch). The validation/zone/bridge
logic is unchanged — it was lifted into a pure `resolveDropIntent`; the parent now owns a
`startDrag` controller that, on pointerdown, attaches `document` pointermove/up listeners,
finds the hovered row from the move event's target (`[data-emitter-id]`), shows the drop
indicator, and on pointerup dispatches the same `emitters/drop` (reorder/reparent). A
`draggedRef` swallows the click synthesised after a same-row pointerup so a drag doesn't
also re-select.

**Issues encountered and resolutions.** This is the same class of gap as
(CSS effects can't span the engine layer) — HTML5-DnD is silently dead in composition
hosting; the symptom is "nothing happens," not an error. Testing: jsdom's `PointerEvent`
is polyfilled in `test-setup.ts`, so the specs fire `pointerdown`(source) →
`pointermove`/`pointerup`(target); the move/up bubble to the controller's document
listeners (no `elementFromPoint` needed — jsdom doesn't implement it). The change is
DOM-attribute-only (no ARIA), so the emitter-tree a11y golden is unchanged.

---

### Bloom settings restored from the registry in the new-UI host

*2026-06-02 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Enabling bloom in the new UI produces visible glow again. Previously, ticking "Enable
bloom" did nothing visible: the new-UI host never restored the persisted bloom settings,
so the engine kept its constructor default of **strength 0**, and the bloom pass — though
it ran — added zero contribution. The host now restores `BloomEnabled` / `BloomStrength` /
`BloomCutoff` / `BloomSize` from the registry at startup, so your saved bloom tuning (and
the parity with the legacy editor) is honoured.

**How we tackled it.** [`HostWindow`](src/host/HostWindow.cpp:1797) reads the four bloom
values from `HKCU\Software\AloParticleEditor` right after the `Engine` is constructed and
applies them via `SetBloom`/`SetBloomStrength`/`SetBloomCutoff`/`SetBloomSize` — mirroring
legacy's startup restore at [`src/main.cpp`](src/main.cpp:7647) and reusing the exact value
names/types (`BloomEnabled` as `REG_DWORD`, the three floats as finite-checked `REG_BINARY`),
so settings round-trip between the legacy and new UIs. The engine's bloom defaults
(`engine.cpp` ctor) are unchanged, so legacy behaviour is untouched.

**Issues encountered and resolutions.** The toggle and the render path were never broken —
that's the trap. Reading the render-gate flags live over CDP showed `enabled=1 ready=1
effect=1 ping=1 pong=1` after a toggle and the pass executing (`bloom=66/5091µs`), so the
shader, RTs, and `engine/set/bloom` dispatch were all fine. The only fault was the
un-restored `strength=0`. The new-UI host was simply missing the engine-settings registry
restore that legacy performs at startup (ground settings have the same gap — a separate
follow-up). Diagnosed without user input by driving `engine/set/bloom` and reading the
snapshot through the `--test-host` CDP host-object bridge. One knock-on: the
`dialog-bloom-settings` a11y golden captures the strength textbox value, which is now
registry-dependent — so the restore is **skipped under `--test-host`** (the harness sees
the deterministic constructor default `0.00`), keeping the goldens machine-independent
while normal launches honour the saved value.

---

### Viewport "black line" on the Spawner edge — D3D9Ex→D3D11 shared-surface guard band

*2026-06-02 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

The thin near-black vertical line that ran along the Spawner panel's left (viewport-facing)
edge in (composition) mode is gone. It was a real ~3–4px artifact at the viewport's
right edge — visible against bright scenes as a 1px black line — now the rendered viewport
content reaches the panel boundary cleanly at every window size.

**How we tackled it.** The line was the engine's near-black background
(`RGB(0x14,0x08,0x34)`) showing through a strip at the scene-rect's right edge where the
engine's **D3D9Ex shared render target is incoherent in its D3D11 alias** — the D3D9 side
renders correct pixels there, but the D3D11 alias that DComp presents reads back the clear
colour (proven by reading the *same* shared texture through both APIs:
[`AlphaCompositor`](src/host/AlphaCompositor.cpp:148)'s `CreateTexture(... &sharedHandle)`
D3D9 view = content, the D3D11 alias in [`Compositor::CompositeEngineFrame`](src/host/Compositor.cpp:1076)
= background). The textbook cure (a keyed-mutex shared resource) isn't available with a
D3D9Ex *producer*. The fix is a **guard band**: [`LayoutBroker::SetSceneRect`](src/host/LayoutBroker.cpp:284)
now renders the engine scene viewport a few px *larger* than the DComp clip (which still
carries the true scene rect), so the incoherent band lands in the clipped-off margin and
the clip shows only coherent interior pixels. The band is **proportional** to the rendered
width (`GBx = max(12, w/64)`; the incoherency measured ~0.5% of width) and
**aspect-preserving** (`GBy = GBx·h/w`), so under the engine's per-pixel-FoV projection
([`Engine::SetSceneViewport`](src/engine.cpp:1583)) both per-pixel angles stay constant and
the visible framing is pixel-identical. A defensive RT clamp in `SetSceneViewport` keeps the
band in-bounds on degenerate layouts (the surrounding chrome guarantees margin in practice).

**Issues encountered and resolutions.** The whole prior framing was wrong and had to be
discarded: the seam is not a DComp clip seam, not the rear backing (which is provably
`#ECECEC`, not black), and not a DOM element — earlier sessions reverted fixes resting on
those assumptions. Localising it required reading pixels at every pipeline stage with
faithful `CopyFromScreen` grabs + D3D9/D3D11 texture readbacks ("measure, don't
eyeball"): scene texture ✓, engine composite ✓, then the D3D11 alias of the same surface ✗
— pinpointing the D3D9Ex→D3D11 boundary. The cross-device flush (`WaitEndFrameQuery`,
`D3DGETDATA_FLUSH`) was already correctly ordered before the read, ruling out a missing
flush. A first fixed-`8px` band cleared the line at 1264-wide but left ~2px at maximized —
the incoherency **scales with width**, so the band had to become proportional. The overscan
is symmetric *and* aspect-preserving because an equal-px band on all four sides changes the
aspect ~1% and visibly shifts edge content. Verified line-gone at 1264×761 and 3440×1369;
the engine-RT path is unchanged for non-composition (canvas-jpeg / arch-A) transports, which
never set a scene viewport.

---

### Link-group brackets — per-member stubs, name-hugging position, dedicated lanes ()

*2026-06-02 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Three improvements to the emitter tree's link-group bracket gutter. **(1)** Every member
of a group now shows a short coloured stub off the bracket bar (previously only the first
and last rows had caps), so membership reads at every row. **(2)** The bracket now sits
right beside the names — ~8px past the longest visible emitter name — instead of pinned
to the panel's far-right edge, and follows the names as they change. **(3)** Each link
group keeps its own dedicated lane (stable, ordered by group id) instead of reusing lanes
across non-overlapping groups, so the gutter no longer "bounces" between renders — this
realises ROADMAP ****.

**How we tackled it.** All in the tree's render + bracket-data layer.
[`computeLinkGroupBrackets`](web/apps/editor/src/lib/link-group-colors.ts) now returns
`memberRowIndices` (every member's flat-row index) and assigns one lane per group by
`groupId` (replacing greedy first-fit). The "hug the names" position is a measure pass in
[`EmitterTree.tsx`](web/apps/editor/src/screens/EmitterTree.tsx): the bracket layer is
absolutely positioned at `left = max(name text right edge) + gap`. The name lives in a
`1fr` grid column that fills the row, so the *column* edge ≠ the *text* edge — each name's
text node is measured with `Range.getBoundingClientRect()` (capped at the column edge for
truncated names), re-run on tree change, `ResizeObserver`, and `document.fonts.ready`. The
layer is `aria-hidden` + `pointer-events-none`, so it stays out of the accessibility tree
(and the a11y goldens).

**Issues encountered and resolutions.** Two worth recording. First, an earlier pass moved
the role glyph between the eye and the name via `grid-column`, which **wrapped the glyph to
a second row** — CSS Grid's sparse auto-placement increments the row when a definite column
is less than the previously-placed one; pinning the placed cells to `grid-row: 1` fixed it.
The miss slipped through because the first browser check sorted children by x only; the
verification now checks **both axes** (centre-y alignment + row height). Second, jsdom
doesn't implement `Range.getBoundingClientRect`, which threw in vitest — the measure now
feature-detects it and falls back to the element rect there (real browsers measure the
text node).

---

### New-UI review polish — emitter role glyph placement + legacy-precision time fields

*2026-06-02 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Two polish fixes from a `--new-ui` review pass. **(1)** In the emitter tree, a child
emitter's spawn-role glyph (lifetime `↻` / on-death `✕`) now sits **between** the
visibility eye and the name instead of off on the right edge. **(2)** Every
seconds-unit field now shows and accepts **three decimals**, matching legacy — so a
burst delay of `0.01` (or `0.250`, etc.) round-trips instead of being truncated to one
decimal. Applies to Emitter Timing (initial spawn delay, skip time, freeze time,
maximum lifetime), Generation burst delay, and the Spawner's spacing / interval / max
lifetime.

**How we tackled it.** The glyph move
([`EmitterTree.tsx`](web/apps/editor/src/screens/EmitterTree.tsx:617)) is **CSS-only** —
the row grid became `18px 18px 1fr` with the glyph at `grid-column:2` and the label at
`grid-column:3`, but the **DOM order is unchanged** (glyph still rendered last). That
keeps the accessibility tree — and the `emitter-tree` a11y goldens, which capture the
row's accessible name and `"default ↻"` text — byte-identical, so no golden churn. The
precision fix sets `decimals={3}` on the eight `s`-unit Spinners (legacy formats every
emitter float as `%.3f`, [`EmitterList.cpp:2509`](src/UI/EmitterList.cpp:2509)).

**Issues encountered and resolutions.** The precision change shifted displayed default
values (`1.0`→`1.000`, `10.0`→`10.000`, …) that are baked into **20**
`*.composition.golden.yaml` a11y goldens. Rather than a full `a11y:update` regen — which
cautions against on this machine (UIA non-determinism) — the goldens were updated
by **surgical value substitution**: the UIA `.json` goldens capture only field *labels*
(not values) so they were untouched, and the DOM-based composition goldens got eight
exact `label: "x.x"` → `"x.xxx"` replacements. a11y returned to the **157 / 4-splitter**
baseline. A third review item — a black line along the Spawner panel's viewport edge —
turned out to be an **compositor seam** (engine backing through a ~1px scene-rect
gap), not a DOM/CSS issue (confirmed by live DOM inspection); it's deferred to a
host-side investigation since it can't be agent-verified ().

---

### `AlphaCompositor::Resize` is now transactional — a failed reallocation no longer kills the viewport (audit G7)

*2026-06-01 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

`AlphaCompositor::Resize` reallocates the off-screen ARGB render target, the
SYSTEMMEM readback surface, and the CreateDIBSection bitmap whenever the popup's
client area changes (and on every device-Reset). Previously it freed **all** the
old resources up front and *then* allocated the new ones — so a single failed
`Create*` (transient VRAM or GDI-handle exhaustion: alt-tabbing out of a fullscreen
game, a driver TDR) left the compositor half-destroyed: old resources gone, new
ones partial, `width`/`height` stale. The result was a dead viewport
(`GetRenderTarget()` → null, every frame rendering nothing) until the editor was
restarted. Now the resize is **all-or-nothing**: it builds the full new resource
set into locals first and only swaps them into the live state once every allocation
has succeeded — any failure throws with the old resources still intact, so the
viewport keeps rendering at the old size and the next resize retries cleanly. Rare
on a healthy box (which is why it was a latent P3), but a real recoverable-hiccup-
turned-fatal hazard under memory pressure.

**How we tackled it.** A `try`/`catch` in
[`src/host/AlphaCompositor.cpp`](src/host/AlphaCompositor.cpp:114): the COM resources
go into local `ComPtr`s (auto-released on unwind) and the GDI handles into local
`HBITMAP`/`HDC`; the `catch` deletes the GDI locals (DC before bitmap, so the DIB is
deselected before `DeleteObject`) and rethrows **before** any write to `m_impl`. The
commit block — release-old-then-`std::move`-locals-in, set `width`/`height`, log —
runs only on full success. Same `Create*` calls, params, and order as before, so the
happy path is byte-identical; the debug `fprintf` moved below the swap so it logs
only a committed resize. `ReleaseGpuResources()` (the deliberate full release before
`IDirect3DDevice9::Reset`) is unchanged.

**Issues encountered and resolutions.** The first native a11y run reported 156/**5**
— one above the 4 deterministic `splitters` failures (the window-size
artifact). The `viewport-resize` spec itself passed; a re-run came back at the
baseline **157 pass / 4 splitters**, confirming the 5th was a transient
agent-launch flake, not a regression. Reinforces: a single red native run
isn't a verdict when the extra failure isn't in the known-deterministic set —
re-run and check the delta is consistent.

---

### WebView2 navigation / new-window / permission policy + message-source check (audit G11)

*2026-06-01 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

The WebView2 host now enforces an **origin allow-list** instead of trusting whatever
page happens to be loaded. Off-origin top-level navigations are cancelled, popups
(`window.open` / `target=_blank`) and every permission request (camera, mic,
geolocation, clipboard, notifications, …) are denied, and a `WebMessage` whose
originating document isn't an approved origin is dropped before it reaches the native
bridge. Approved origins: `https://app.local/` (prod — the virtual-host-mapped
`web/apps/editor/dist`), `http://localhost:5174/` (dev, only under `--dev-ui`), and
`about:` (WebView2's own `about:blank` init navigation). Defence-in-depth against a
redirected or compromised renderer following an off-origin link.

**How we tackled it.** A shared `IsApprovedWebViewOrigin(uri, devUi)` prefix-matcher
in the host anonymous namespace ([`src/host/HostWindow.cpp`](src/host/HostWindow.cpp:90)),
plus three handlers — `add_NavigationStarting` (cancel off-origin), `add_NewWindowRequested`
(deny), `add_PermissionRequested` (deny) — registered next to the existing
`add_WebMessageReceived` ([`src/host/HostWindow.cpp:1315`](src/host/HostWindow.cpp:1315))
and *before* the `Navigate()` call, so the very first legitimate load is already subject
to the policy. The three `EventRegistrationToken`s are stored as members and removed in
`WM_DESTROY`, mirroring the G5 `webMessageTok` lifecycle. The trailing `/` on the two
host prefixes is load-bearing — it blocks a `https://app.local.evil.test/` lookalike.
Verification leans on the a11y suite (): the native harness loads the **prod**
`app.local` origin under `--test-host`, so a green **157 pass / 4 splitters** run (the 4
are the window-size artifact, unchanged from baseline) proves the allow-list does
*not* cancel the app's own load or break the bridge. All WebView2 method/enum signatures
were confirmed against the SDK 1.0.3967.48 header before coding.

**Issues encountered and resolutions.** Verifying G11 surfaced an unrelated test-harness
footgun (fixed in a separate commit): the native a11y runner's cleanup
([`web/apps/editor/scripts/run-native-tests.mjs`](web/apps/editor/scripts/run-native-tests.mjs:155))
ran a blanket `taskkill /F /IM ParticleEditor.exe`, which matches by image name and so
killed a **legacy `0.2` editor build the user daily-drives in parallel** (same exe name,
different binary). Scoped the cleanup to a `--test-host` command-line filter via
`Get-CimInstance Win32_Process` so the legacy editor — never launched with `--test-host`
— survives; proven with a controlled no-arg-decoy + `--test-host`-target test (→).
The runtime popup-deny / permission-deny / message-drop paths are correct-by-construction
but not exercised by a11y (no spec calls `window.open` / a permission prompt / an
off-origin `postMessage`); left for a manual `--dev-ui` poke if hard confirmation is
ever wanted.

---

### `NativeBridge` no longer leaks pending requests on send-failure or page teardown (audit G12)

*2026-06-01 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

The WebView2 bridge's `request()` registered its pending-promise entry *before* calling
`postMessage`, so if `JSON.stringify`/`postMessage` threw — or the page tore down
mid-flight — the entry was never removed: a permanently-pending promise the caller
awaited forever, plus a slowly-accumulating leak over a long session. Now `request()`
wraps the send in try/catch (clean up + reject on throw), a `dispose()` method rejects
and clears every outstanding request and is wired to `beforeunload` so teardown fails
callers closed, and an **opt-in** per-request timeout (`new NativeBridge({ requestTimeoutMs })`)
backstops a silently-dropped response.

**How we tackled it.** All in [`web/apps/editor/src/bridge/native.ts`](web/apps/editor/src/bridge/native.ts),
test-first against a new [`native.test.ts`](web/apps/editor/src/bridge/__tests__/native.test.ts).
The deliberate design call: the timeout is **off by default** — several requests are
interactive and legitimately block (the native file dialog behind `file/open`,
`emitters/import-from-file` reading a chosen file), so a blanket timeout would reject
valid slow operations; the teardown path covers the common "response never comes" case.

---

### Fix an infinite loop in the XML attribute parser (audit G10)

*2026-06-01 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

`XMLNode`'s attribute loop ([`src/xml.cpp:15`](src/xml.cpp:15)) read `atts[0]`/`atts[1]`
but never advanced `atts`, so **any XML element carrying ≥1 attribute spun forever at
100% CPU** during parse. Latent in practice — the only XML the editor parses is
`Data\MegaFiles.xml`, whose canonical schema is attribute-less, so well-formed game
data never triggered it — but a malformed or mod-supplied attribute-bearing XML would
hang startup. Fixed by advancing the Expat `[name0, val0, name1, val1, …]` array by
pairs (`atts += 2`), with an `atts[1]` guard that also tolerates a malformed
odd-length array.

**How we tackled it.** One-line root cause; the fix is the standard pair-advance.
Verified by Release build (the parser is legacy startup code with no test harness;
the fix is self-evident by inspection). `[both]` — shipped on `integration-branch`; forward-ports
to `master` at integration.

---

### Import Emitters now works in the new UI — the native `emitters/import-from-file` handler (audit G1)

*2026-06-01 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

The `--new-ui` **Import Emitters** dialog's "Import N selected" button now works.
Previously the dialog could browse a `.alo` and preview its emitter tree, but clicking
import hit the dispatcher's not-implemented branch and surfaced an inline error (audit
finding **G1**). Now the selected emitters are cloned into the open system as new
roots — with parent/child links remapped among the picked set, cyclic / multi-parent
links dropped, multi-member link groups recreated, the document marked dirty, and the
whole import reverted by a single undo.

**How we tackled it.** The proven legacy import core (`ImportEmitters_Execute` in
[`main.cpp`](src/main.cpp:7245)) was extracted to a shared data-layer method
[`ParticleSystem::ImportEmittersFrom`](src/ParticleSystem.cpp:1176) — deep-copy each
pick via the chunk serialiser, re-map spawn fields, `ValidateEmitterGraph` (audit-F4),
recreate link groups. It stays UI-independent by taking the unique-name generator as a
`std::function` callback (so `ParticleSystem.cpp` never references the UI's
`GenerateDuplicateName`). Both the legacy dialog and the new
[`emitters/import-from-file` handler](src/host/BridgeDispatcher.cpp:2756) now call it;
the handler mirrors `emitters/duplicate` (pre-mutation `captureUndo`, then `markDirty`
+ `EmitEmittersTreeChanged`). Keeps legacy behaviour exactly — imports land as roots,
links to non-picked emitters drop, IDs renumber on import.

**Issues encountered and resolutions.** Built test-first: a new
[`emitter-import.spec.ts`](web/apps/editor/tests/emitter-import.spec.ts) a11y spec
drives the real host over CDP (preview → import every source index → assert the live
emitter count grows by exactly that many → undo restores it). Two gotchas surfaced
during the red→green: (1) the `captureUndo` helper is a lambda defined *partway through*
`DispatchInternal`, so the handler had to sit **after** its definition with the other
mutation handlers, not next to `preview-from-file` — see a project lesson;
(2) `emitters/list` returns its tree under `root` while `preview-from-file` uses `tree`
(a pre-existing response-shape inconsistency the test had to account for).

**Hardening (post-implementation adversarial review).** A multi-agent review of the
change surfaced two worth fixing: (a) the handler reported failures via
`sendOk{ok:false}`, which `NativeBridge` *resolves* as success — so a failed import
(e.g. file locked/deleted between preview and import) closed the dialog silently with
no feedback; switched the four failure paths to `sendErr` so the promise rejects and
the dialog's existing error UI fires, and added a bound-check that drops out-of-range
picks before `captureUndo` (no stray undo entry / dirty flag on a no-op). (b) the
test's count-only assertion couldn't distinguish a correct parent/child rebind from a
broken one (both yield the same node count); the spec now asserts the imported
subtree's **shape** (one new root re-parenting its lifetime + death children), plus
new partial-import (link-drop miss-branch) and failed-import (rejects + no-stray-undo)
cases. Verified: a11y 157 passed (only the 4 known `splitters` artifacts fail),
vitest 386, Debug+Release clean.

---

### New-UI ground controls — solid-colour ground works end-to-end, and the ground-height field is back

*2026-06-01 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Three bugs in the `--new-ui` Ground toolbar dropdown are fixed. First, clicking the
prominent **"Solid colour" tile now opens a colour picker** — previously the tile only
*selected* the solid-colour slot and the actual picker was an easily-missed secondary
swatch beneath it, so the option looked broken. Second, **picking a solid colour now
actually recolours the ground plane** — a separate engine bug meant the solid slot
never applied at all under the renderer (the colour silently did nothing).
Third, the **ground-plane height control is restored**: a "Height" field (legacy
, #45) that raises/lowers the ground plane, enabled only while the ground is shown.
The height control had simply never been ported to the React UI even though the engine
and bridge already supported it.

**How we tackled it.** The two UI fixes live in
[`GroundTexturePanelBody`](web/apps/editor/src/screens/GroundTexturePanel.tsx:93). The
solid-colour control now mirrors the **proven `BackgroundPicker` pattern**: the wide
tile triggers a hidden native `<input type="color">` (an OS dialog — discoverable,
and immune to the viewport compositing that can hide a DOM popover), replacing
the Radix `ColorButton`. The height field uses the existing `Spinner` primitive
(−100…100, step 0.1, disabled in lockstep with the show-ground toggle to match the
legacy spinner at [`main.cpp:1662`](src/main.cpp:1662)) wired to the already-present
[`engine/set/ground-z`](src/host/BridgeDispatcher.cpp:1184) handler — no new bridge or
engine code, just the missing UI. The "colour never applies" bug was a separate
**engine** fix in [`CreateSolidColorTexture`](src/engine.cpp:1118): the 1×1 procedural
texture is created `D3DPOOL_DEFAULT` (the `` migration moved it off
`D3DPOOL_MANAGED`, which D3D9Ex rejects), but a DEFAULT-pool texture **cannot be
`LockRect`'d unless it is also `D3DUSAGE_DYNAMIC`** — without it the lock failed,
`CreateSolidColorTexture` returned false, and the ground texture never updated. Added
`D3DUSAGE_DYNAMIC` + `D3DLOCK_DISCARD`. **integration-branch-only** — master still uses the lockable
managed pool. See a project lesson.

**Issues encountered and resolutions.** Root-caused in **browser mode** (`pnpm dev` +
mock bridge), which sidesteps the agent-window misrender entirely — the picker
opened *fine* in the browser, which ruled out a React logic bug and narrowed the cause
to discoverability **and/or** occlusion of the Radix DOM popover (the user
confirmed Background's *native* colour input works in the host, but Background uses a
different mechanism than Ground did). Converging Ground onto the native-input pattern
fixes the bug under either hypothesis. The one thing browser mode can't show — that the
OS colour dialog paints over the live viewport — is left to a user spot-check
(). See a project lesson.

---

### [] Five correctness & memory-safety fixes — save-failure data loss, chunk-parser hardening, emitter-graph validation, particle-index cap

*2026-06-01 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

The five `[both]`-tier P1 findings from the 2026-05-24 audit pass
([`tasks/post-audit-followups.md`](tasks/post-audit-followups.md)) are fixed. The
user-visible one is **F1**: a *failed* save (disk full, permission denied, an I/O
exception in the writer) no longer clears the modified marker or deletes the
recovery autosave — the editor keeps the dirty asterisk, keeps the autosave, and
**aborts the close/new/open that triggered the save** instead of proceeding and
losing the document. The other four harden the loader against malformed or hostile
`.alo` input: **F2** a heap over-read reading an unterminated string chunk, **F3**
heap corruption from over-deeply-nested chunks, **F4** infinite recursion /
double-free from cyclic or multi-parent emitter graphs, and **F5** silent render
corruption once a single emitter exceeds ~16k live particles.

**How we tackled it.** F1 — [`DoSaveFile`](src/main.cpp:1466) now early-returns
`false` on writer failure so its three bookkeeping calls are gated and
`DoCheckChanges` propagates the abort (the host twin at
[`BridgeDispatcher` `file/save`](src/host/BridgeDispatcher.cpp:2015) already did
this). F2 — [`ChunkReader::readString`](src/ChunkReader.cpp:90) reads into a bounded
`std::vector<char>`, requires the trailing NUL, and builds the string
length-bounded. F3 — depth guards before the `m_curDepth` increment in
[`ChunkReader::next`](src/ChunkReader.cpp:65) (throws `BadFileException`) and
[`ChunkWriter::beginChunk`](src/ChunkWriter.cpp:6) (asserts — over-deep nesting on
write is our bug, not input). F4 — new
[`ParticleSystem::ValidateEmitterGraph`](src/ParticleSystem.cpp:1102) clears
out-of-range / self / duplicate-parent links, breaks cycles with an iterative DFS,
and rebuilds parent pointers; it replaces the loader's inline range-clear and is
called from the `ParticleSystem(IFile*)` constructor (which also backs autosave
restore via `RestoreFromAutosave`) and from the import-emitters helper. F5 —
[`SpawnParticle`](src/EmitterInstance.cpp:258) refuses to spawn past
`0xFFFF / NUM_VERTICES_PER_PARTICLE`, freeing the slot and bailing rather than
minting a wrapping uint16 index.

**Issues encountered and resolutions.** The fix sites all live in shared legacy
`src/`, so vitest can't exercise them () — verification was the native a11y
suite, which boots the real C++ host and `file/open`s real multi-emitter fixtures
(`a11y-base-state.alo`, `nt-5-singleton.alo`) straight through the modified
`ChunkReader` + `ValidateEmitterGraph`. Result: **153 passed**, only the 4
`splitters` percentage specs failing (the known agent-window artifact,), so
F2/F3/F4 demonstrably do **not** reject valid files and the emitter-tree goldens
still match (parents rebuilt identically). F4's DFS is iterative on purpose — a
deep-but-acyclic chain must not overflow the call stack the way a recursive
validator would. Fresh-worktree builds need a NuGet restore with no `nuget.exe`
on `PATH`; resolved by materialising the cached WebView2 package into the
solution-local `packages/` layout ().

---

### UI follow-up] F1 — emitter-row icons: visibility on the left, spawn-role on the right

*2026-06-01 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Each emitter row now leads with the **visibility (eye) toggle on the left**
(replacing the old `●` role dot), and **child** rows show their **spawn-role
glyph on the right** — `↻` for a lifetime child (spawns continuously during the
parent's life), `✕` for an on-death child (one-shot when the parent dies). Root
rows are just eye + name. The eye stays an interactive toggle; the role glyph is
presentational.

**How we tackled it.** Reordered the row's CSS grid in
[`EmitterTree.tsx`](web/apps/editor/src/screens/EmitterTree.tsx:1) from
`[role-glyph | name | eye]` to `[eye | name | spawn-role glyph]` (template
`18px 1fr 18px`); the eye is always rendered (col 1) so the grid stays stable
during inline rename, and the role glyph renders only for non-root rows.

**Issues encountered and resolutions.** The row's accessible tree appears in
*every* full-app a11y golden (the tree is always in the left pane), so all 20
composition goldens were regenerated; the diff was verified surgical (only the
row reorder + `●` removal, no theme/panel-state pollution, per).

---

### bugfix] Link groups now actually link — sync-on-link + per-edit propagation

*2026-06-01 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Linking emitters into a group had no behavioural effect in the new UI: the
bracket gutter drew, but the members didn't share parameters and editing one
didn't update the others. Both halves of link-group behaviour now work, matching
legacy: **linking** unifies the members' non-exempt fields immediately (textures,
name, and the atlas-index curve stay per-emitter; everything else — RGBA/scale/
rotation curves, lifetime, physics, appearance — is shared), and **editing** any
shared field on one member propagates to the rest as a single undoable step.

**How we tackled it.** Two changes in [`src/host/BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp:1).
(1) `linkGroups/set-membership` now drives the existing `LinkGroup.h` API
(`CreateLinkGroup` / `JoinLinkGroup` / `LeaveLinkGroup`, detaching any
already-grouped member first) instead of stamping `e->linkGroup` raw — the raw
stamp set the membership id (so brackets drew) but never synced fields, the root
cause. (2) A new `propagateLinkGroup(edited)` lambda mirrors the legacy post-edit
chokepoint in [`src/main.cpp`](src/main.cpp:864) `CaptureUndo` — it copies the
edited emitter's non-exempt params to every sibling via
`Emitter::copySharedParamsFrom` using the group's exempt flags — and is called
after the mutation in the six handlers that touch shared fields (`set-properties`,
`set-track-interpolation`, `set-track-lock`, `set-track-key`, `add-track-key`,
`delete-track-keys`). The pre-mutation `captureUndo()` already snapshots the whole
system, so one Ctrl+Z restores the entire group.

**Issues encountered and resolutions.** The new UI reimplemented undo capture as
a *pre-mutation* bridge lambda and silently dropped the propagation side effect
the legacy *post-edit* `CaptureUndo` performed — so the fix is a chokepoint, not a
one-liner; propagation had to be added to each shared-field handler (structural /
identity / per-emitter `visible` handlers are correctly excluded). `set-track-lock`
propagation is correct because `copySharedParamsFrom` re-points `tracks[]` into the
destination's own `trackContents` ([`src/ParticleSystem.cpp`](src/ParticleSystem.cpp:660)),
remapping the lock pointer rather than aliasing the source's. The host's track keys
are a time-keyed `std::multiset` (duplicate times legal), which also made the
companion curve-editor multi-key edit (below) safe without a batch API. A
follow-up fix: routing `linkGroups/set-membership`'s positive-id path through
`JoinLinkGroup` narrowed the contract — `JoinLinkGroup` refuses a *non-existent*
group, so assigning emitters to an explicit new positive id (the old stamp
behaviour, exercised by the native tests) silently no-op'd. Restored
create-if-needed for positive ids (existing → join, missing → create with that
id + sync), caught by the native `emitter-mutations` spec, not vitest.

---

### UI follow-ups] F-series — number fields, curve editor, emitter toolbar, brackets

*2026-06-01 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Seven UI refinements from a live review (the `F2`–`F9` backlog, minus the native
F4 above and the deferred F1 icon-layout):

1. **F2** — the emitter-tree toolbar is centered and its buttons sized 28px to
   match the main toolbar (was 24px, left-aligned and lopsided on resize).
2. **F3** — toolbar, tree, and panel-header icon buttons show a pressed
   (`:active`) state — lighter bg + slight scale — distinct from hover/toggled.
3. **F5** — the link-group bracket gutter hugs the emitter rows (row→bracket gap
   18px → 6px), matching legacy 0.2; the old `marginRight: gutterPx` was a
   leftover from when the gutter was absolutely positioned.
4. **F6** — dragging across a number field now selects text; the value-scrub
   gesture moved to the up/down arrow column (a plain click still steps).
5. **F7** — the scroll wheel steps a flat 0.1 on decimal fields and 1 on integer
   fields (Shift = ×10), matching legacy, instead of the field's configured step.
6. **F8** — selecting >1 curve key shows the **average** Time/Value; editing it
   shifts the whole group by the delta (preserve spread). Matches legacy
   `CurveEditor_MoveSelection`: average over all selected keys; Time editable when
   ≥1 interior key is selected; Time-shift moves interior keys only (borders
   pinned in time but shift in value); Value-shift moves all.
7. **F9** — selecting the **Index** curve auto-deselects every other channel
   (solo), exactly like Scale; the two replace each other.

**How we tackled it.** F2/F3 in [`EmitterTree.tsx`](web/apps/editor/src/screens/EmitterTree.tsx:1)
(the `TOOLBAR_BTN` utility string) + [`components.css`](web/apps/editor/src/styles/components.css:1);
F5 swaps the redundant `marginRight: gutterPx` for a small `GUTTER_GAP_PX`. F6/F7
in [`Spinner.tsx`](web/apps/editor/src/primitives/Spinner.tsx:1): the drag handler
moved off the `<input>` onto the arrow-column wrapper with a 3px movement
threshold and a `scrubbedRef` click-suppressor; the wheel base derives from the
field's decimal-places (`dp === 0 ? 1 : 0.1`). F8/F9 in
[`CurveEditorPanel.tsx`](web/apps/editor/src/components/CurveEditorPanel.tsx:1):
F9 generalizes the Scale-solo logic to an `EXCLUSIVE_CHANNELS` set; F8 adds a
`multiSelected` average memo + an `applyGroupShift` helper that issues ordered
single-key `set-track-key` calls (descending `oldTime` for a rightward shift,
ascending for leftward) so each host-side `find(oldTime)` is unambiguous on the
time-keyed multiset — no batch API needed.

**Issues encountered and resolutions.** F5 looked like a one-constant tweak but
the `<ul>` is `flex-1`, so the brackets stay glued to the rows' right edge
regardless — the real cause was double-spacing (margin + a now-real flex gutter
column). F8's "shift the group" is constrained by immovable border keys; after
checking the legacy source we matched its exact rule (Time disabled only when the
selection is *all* borders) rather than the stricter first proposal. 12 new vitest
specs across Spinner (F6/F7) and CurveEditorPanel (F8/F9); full suite 383/383,
a11y goldens untouched (all CSS/DOM-state, no captured-surface change).

---

### UI polish] Inspector density pass — tighter rhythm, flat sections, indent hierarchy, aligned checkboxes

*2026-06-01 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

A four-part density/readability pass on the inspector (and the shared tool
panels). More params fit on screen, groups read more clearly, and the columns
line up:

1. **Tighter vertical rhythm** — trimmed section/body padding, inter-row gap,
   and form-row padding (~15–25% more rows visible) without shrinking text.
2. **Flat sections (hybrid)** — replaced the bordered+filled section *cards*
   with a tinted, rounded **header band** + edge-to-edge rows. Keeps clear
   group anchors while reclaiming the card border and side padding, so labels
   and inputs get the full pane width. Shared `.panel-section`, so the
   Lighting / Bloom / Ground / Background tool panels match.
3. **Indent hierarchy** — section header, direct params, and radio dots now
   share one left edge; a radio's sub-params (e.g. the Continuous/Weather
   spinners) indent to nest *under their radio label*. Inputs stay
   right-aligned because only the `.lbl` text is indented.
4. **Aligned checkboxes** — checkbox right edges now line up with the
   spinner/select box right edges across all three tabs.

**How we tackled it.** All in [`components.css`](web/apps/editor/src/styles/components.css).
The hybrid flattens `.panel-section` (drop border/bg/radius) and bands
`.panel-section-header` (`background: var(--bg-2)` + radius). The indent
hierarchy is CSS-only: `.inspector .panel-section-body { padding-left: 8px }`
aligns L1 with the header, `.inspector .radio-row { padding-left: 0 }` drops
the radio dot to that edge, and `.basic-tab [role="radiogroup"] .form-row .lbl
{ padding-left: 22px }` nests radio-owned params — the `[role="radiogroup"]`
wrapper already scopes exactly the radio params, so no React/markup changes.
The checkbox fix changes `.form-row-check` from `1fr auto` to `1fr auto 40px`,
reserving the same unit column the spinner rows carry so the checkbox shares
their `row_right − 48px` right edge by construction.

**Issues encountered and resolutions.** Indenting the *row* (not the label)
would have shoved each spinner left and broken the input column — so the L2
indent pads `.lbl` only, keeping inputs in one tidy right-aligned column.
CSS-only throughout: vitest (371) and a11y goldens (CSS-independent) unaffected.

---

### UI polish] Inspector text readability — promote labels, reserve dimming for disabled params

*2026-05-31 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Inspector field labels ("Bursts", "Burst delay", "Initial spawn delay", …)
and the collapsible section headers ("EMITTER TIMING", "GENERATION") were
rendered in the dim secondary token and read poorly at 12px. They now use the
primary text colour, matching the field values and curve names. Dimming is
re-purposed to mean **disabled**: a param whose generation mode isn't selected
(e.g. the Continuous-stream / Weather spinners while Bursts is active) now dims
its label, and the dim follows live as you switch modes.

**How we tackled it.** Three CSS edits in
[`components.css`](web/apps/editor/src/styles/components.css): `.form-row` /
`.form-row .lbl` and `.panel-section-header` move from `--text-2` to `--text`;
a new `.form-row:has(:disabled) .lbl { color: var(--text-2) }` re-introduces the
dim for disabled rows. The `:has()` rule reads the *native* `disabled` state
already present on the Spinner's `<input>` (and Radix checkbox/select controls),
so the label dimming tracks the real enabled/disabled state with **zero React
changes** — toggling a mode re-dims the correct rows automatically. `:has()` is
supported in the WebView2 Chromium runtime.

**Issues encountered and resolutions.** Establishes a clear two-state
convention for inspector labels (primary = enabled, secondary = disabled),
replacing the prior "secondary = label chrome" usage that made everything read
dim. CSS-only: vitest (371) and a11y goldens (CSS-independent ARIA snapshots)
unaffected.

---

### UI polish] Rebase Tailwind `text-sm` to the 12px body convention

*2026-05-31 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

The emitter list ("Particle System" tree) rendered ~17% larger than the
panels around it — its names sat at 14px while the curve track names, tabs,
form labels, and panel titles are all 12px. Now everything in those side
panels shares the 12px body size, so the emitter names line up with the rest.

**How we tackled it.** The cause was two sizing systems colliding: the
hand-written CSS sizes in explicit `px` and consistently targets 12px, while
Tailwind's `rem` scale is anchored to the 16px `<html>` root (not
`body { font-size: 12px }`), so `text-sm` lands at 14px. `text-xs` (87 uses)
already equals 12px and is the de-facto body utility; `text-sm` (24 uses,
incl. the emitter tree) was the lone outlier. Rather than re-tag every site,
the fix rebases the token once — `--text-sm: 0.75rem` in a `@theme` block in
[`tokens.css`](web/apps/editor/src/styles/tokens.css) (line-height mirrored to
`--text-xs`) — so all `text-sm` usage converges on 12px. The 87 `text-xs`
sites are untouched, and the deliberately-large `text-lg` (About heading) /
`text-2xl` ("+" glyphs) don't read `--text-sm`, so they stay big.

**Issues encountered and resolutions.** Verified the override actually
compiled — the dist CSS emits `--text-sm:.75rem` — since a Tailwind v4
`@theme` token edit is easy to get subtly wrong (no `tailwind.config.js`
exists; utilities are generated from theme vars). CSS-only change: vitest
(371) and the a11y goldens (CSS-independent ARIA snapshots) are untouched.

---

### UI polish] Themed emitter-list scrollbar + theme-following native title bar

*2026-05-31 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Two theming gaps closed. The emitter list (the "Particle System" tree) showed
Chromium's default white scrollbar in dark mode instead of the thin themed bar
used everywhere else; it now matches. And the native Win32 title bar stayed a
light caption regardless of theme; it now follows the in-app theme, dark or
light, and flips live when you toggle.

**How we tackled it.** The themed `::-webkit-scrollbar` rules in
[`base.css`](web/apps/editor/src/styles/base.css:32) apply to an *enumerated*
selector list (`.panel-body`, `.curve-list`, the tab-content testids); the
emitter tree's scroll viewport simply wasn't on it. Added an
`emitter-tree-scroll` class to that viewport
([`EmitterTree.tsx`](web/apps/editor/src/screens/EmitterTree.tsx:1353)) and
listed it alongside the others. The title bar is the native caption — outside
WebView2, so CSS can't reach it — and is themed via the DWM
`DWMWA_USE_IMMERSIVE_DARK_MODE` attribute, driven two ways: once at window
creation from the OS app-theme preference
([`HostWindow.cpp`](src/host/HostWindow.cpp:2750), avoids a white-caption flash
before React mounts) and again in the existing `host/backing-color` handler
([`BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp:899)) from the
luminance of the pushed `--bg` — which `useBackingColorSync` already sends on
mount and every theme toggle, so the caption follows the theme for free with no
new bridge surface.

**Issues encountered and resolutions.** `dwmapi.lib` was newly linked (pragma
in both host TUs); `DWMWA_USE_IMMERSIVE_DARK_MODE` is `#ifndef`-guarded to 20
so older Windows SDKs still compile. CSS-only + native-caption changes — no DOM
or ARIA impact, so vitest (371) and the a11y goldens are untouched.

---

### perf] viewport — drop the redundant per-frame layered-window readback

*2026-05-31 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

The editor felt sluggish and janky in real use, worst when maximized
or on a large monitor. Root cause was a redundant per-frame GPU readback;
removing it makes the viewport render essentially free regardless of window
size. On this machine the uncapped maximized render ceiling went from
~90 FPS to ~2380 FPS (≈26×) at 3440-wide, and per-frame cost is now flat
across window size instead of scaling with pixel area. User-confirmed
"performance is excellent" with the viewport, maximize, and modal-overlay
snapshots all rendering correctly.

**How we tackled it.** Measure-first, because code-reading mis-pointed twice.
Added always-on per-stage + per-pass frame timing to `host.log`
(`[PERF]` / `[PERF2]`, `QueryPerformanceCounter`, 1 Hz) in
[`src/host/HostWindow.cpp`](src/host/HostWindow.cpp:708) and
[`src/engine.cpp`](src/engine.cpp:583). The data localised ~96–99% of the
frame to `engine->Render()`, scaling linearly with window area even at zero
particles; sub-profiling Render() into scene / bloom / distort / composite /
present showed the entire cost was **`present`** — i.e. the synchronous
`AlphaCompositor::Composite()` `GetRenderTargetData` readback + ~19 MB
`memcpy` ([`src/host/AlphaCompositor.cpp`](src/host/AlphaCompositor.cpp:753)).
In architecture-C the visible pixels reach the screen through the DComp
shared-texture path (`Compositor::CompositeEngineFrame` reads the same RT
GPU-side), so the layered-window readback is pure redundant work — the same
class of leftover arch-A/B transport as the FramePublisher JPEG encode that
was previously removed from composition mode. The fix adds
[`Engine::SetCompositionMode(bool)`](src/engine.h:148); the host sets it at
the `SetAlphaCompositor` site
([`src/host/HostWindow.cpp`](src/host/HostWindow.cpp:1712)) and `Render()`
skips `Composite()` when set
([`src/engine.cpp`](src/engine.cpp:983)). The engine still renders *into* the
AlphaCompositor RT (the shared source); only the redundant layered transport
is skipped. The `[PERF]` instrumentation is kept in as a permanent, always-on
diagnostic (QPC is ~free; it only logs once per second).

**Issues encountered and resolutions.** Two plausible code-reading suspects
were refuted by measurement: the `WaitEndFrameQuery` busy-spin (~45 µs, far
from its 100k cap) and the cross-device `CopyResource` (~45→67 µs, basically
fixed) — the author's own comment *"the spin in WaitEndFrameQuery dominates"*
was wrong. After the fix the bottleneck correctly *shifted* to the now-exposed
`WaitEndFrameQuery` spin (~385 µs / ~9000 spins — the GPU fence the readback
used to absorb), but at ~2380 FPS it's irrelevant and was left untouched
(measure-first: don't optimise a non-problem). Risk check before removing the
readback: nothing in consumes the per-frame sysmem DIB — modal
snapshots do their own on-demand readback, and the `lastRawDib` cache feeds
only the composition-mode-gated FramePublisher. Per, the on-screen look
was confirmed by the user, not by agent screenshots (the host-side mechanism —
`[COMP-engine-frame]` still presenting every frame — was verified in
`host.log`). See **** for the measure-first lesson.

---

### UI polish] 1px light-grey hairline framing the viewport — removed

*2026-05-31 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

The viewport was framed on all four edges by a 1px light-grey
(`#C0C0C0`) hairline — neutral, theme-independent, and jarring against the
dark theme. It's gone; the panels and splitters now meet the rendered scene
directly with no seam, in both themes.

**How we tackled it.** Root cause was a *vestigial* empty
`<img data-testid="viewport-img">` overlay in
[`ViewportSlot.tsx`](web/apps/editor/src/components/ViewportSlot.tsx:320).
That element is the legacy architecture-A engine-pixel surface (JPEG via
`.src`); under the architecture-C default, engine pixels reach the screen
through the DComp engine visual *behind* the transparent WebView2, and the
`viewport/frame-ready` → `img.src` consumer early-returns in composition mode
— so the `<img>` is never painted in any current build. Its only effect was
the seam: the empty element's box sits at the fractional sub-pixel scene-rect
origin (e.g. `x=335.05` at dpr=1), and Chromium antialiased that transparent
edge against its white compositor base, producing a ~50%-coverage neutral
grey at the viewport's first row/column on every side. The fix gates the
`<img>` render on `!compositionMode` — removed from the default DOM
tree (seam gone) but preserved for the canvas-jpeg transport. One-file React
change; no host/DComp/engine code touched.

**Issues encountered and resolutions.** A first attempt in a prior session (a
1px engine-clip inset) was reverted because it rested on an unverified
assumption. This time the cause was *proven by elimination* before any fix: a
host-side readback of the engine backbuffer (env-gated `--capture` scaffold)
showed the engine RT is **clean** at the scene-rect edge; a live CDP sweep
recoloured the rear backing (magenta), engine background (green), and WebView2
page background (blue) with the line staying exactly `192` each time; a
`DComp SetBorderMode(HARD)` test on the engine visual changed nothing by
faithful pixel measurement; and finally hiding the `<img>` removed the line
with the viewport interior pixel-identical. Because this machine misrenders
compositing under agent-driven launches (), verification used
host.log + CDP + faithful `HWND_TOPMOST` window grabs measured with PIL,
cross-checked with the user on screen. See **** for the reusable
layer-isolation method.

---

### feature-parity] Sphere/Cylinder emitter distribution fields match legacy

*2026-05-31 · [`e89c1cc`](https://github.com/DrKnickers/particle-editor/commit/e89c1cc) · #92*

The Physics tab's **Initial position** and **Initial speed** sections now match
the legacy 0.2 editor for the **Sphere** and **Cylinder** distribution types.
The numeric "Sphere edge" / "Cylinder edge" spinner — which exposed an engine
field that has no numeric meaning — is replaced by a **"Constrain to surface"**
checkbox (legacy's wording). For Cylinder, **Radius** and **Height** now sit on
one row instead of two, and the labels are shortened to "Radius:" / "Height:"
since the Type selector already says Cylinder/Sphere. Checking "Constrain to
surface" makes particles spawn on the shape's surface; unchecked, they fill the
volume.

**How we tackled it.** Root cause was a mis-presented field, not a missing one:
`sphereEdge`/`cylinderEdge` (`ParticleSystem::Emitter::Group`, `unsigned int`) is
used by the engine as a **boolean** —
[`EmitterInstance.cpp:205,215`](src/EmitterInstance.cpp:205) computes
`radius = (group.cylinderEdge ? 1.0f : GetRandom(0,1)) * cylinderRadius`, i.e.
nonzero ⇒ full radius (surface), zero ⇒ random radius (volume). Legacy renders it
as a checkbox; the new UI rendered it as a numeric spinner. So "add a
constrain-to-surface checkbox" and "remove the edge param" were the **same
change**. Swapped the `FieldSpinner` for the existing `FieldCheckbox` (writes
1/0 — the engine only tests truthiness, so collapsing any prior nonzero value to
1 is lossless), and moved Radius/Height into a single `Vec3Row`-style `axis-cell`
cluster. All in `GroupBody`
([`EmitterPropertyTabs.tsx`](web/apps/editor/src/screens/EmitterPropertyTabs.tsx:1506)).

**Issues encountered and resolutions.** Confirmed against the engine before
touching anything — removing a "field" that turned out to be load-bearing would
have lost the surface/volume control. The change is **a11y-golden-neutral**: the
captured surfaces use the default "Exact" group type, so the Sphere/Cylinder
branches render in none of them (no regen). Because the Physics tab is
engine-independent, the new layout was verified visually in browser/MockBridge
mode via Playwright (drove an emitter → Physics → set Initial position Type to
Cylinder), unlike the compositing fixes which can't be eyeballed locally
().

---

### UI polish] Collapse the spawner's redundant nested panel

*2026-05-31 · [`aba25f6`](https://github.com/DrKnickers/particle-editor/commit/aba25f6) · #92*

The Spawner pane was wrapped in panel chrome twice — so it read as a subtly
"framed" box with an inset border ring, unlike the flush left pane and curve
editor. The layout's `<aside>` for the spawner column carried `bg-panel` plus a
left border, and the `SpawnerPanel` inside it renders a full `.panel`
(background + border + 8px rounded corners). Two same-coloured cards nested:
the inner panel's border traced a rounded rectangle just inside the outer
container, with a doubled-up left edge. Now the spawner is a single clean panel
card matching its neighbours.

**How we tackled it.** Stripped the redundant panel styling (`bg-panel`,
`border-l border-border`) from the spawner `<aside>` in
[`PanelLayout.tsx`](web/apps/editor/src/components/PanelLayout.tsx:368), leaving it
a plain `h-full w-full overflow-hidden` layout container. The single `.panel` that
[`SpawnerPanel`](web/apps/editor/src/screens/SpawnerPanel.tsx:167) renders becomes
the card — the same shape as the curve editor (a plain wrapper around the screen's
`.panel`) and consistent with the left pane (which *is* the `.panel`). One-line
className change.

**Issues encountered and resolutions.** None of note. Worth recording: this is a
**CSS-only** change — the `<aside>` keeps its `data-testid="quadrant-spawner"`,
its `complementary` role, and its children, so the UIA/ARIA a11y goldens (which
capture roles/names/structure, not CSS) are byte-identical and need no
regeneration. Because the spawner styling is engine-independent, the single-card
result was verified visually in browser/MockBridge mode via Playwright (the native
compositing isn't involved, so it's eyeballable locally — unlike the
backing fix above; see).

---

### UI polish] Theme-coloured composition backing — kill the dark corner wedges

*2026-05-30 · [`a545559`](https://github.com/DrKnickers/particle-editor/commit/a545559) · #92*

Rounded panels that meet the engine viewport (or sit over any transparent gap)
no longer show a dark triangular wedge in their corners. Previously, in
the engine visual is clipped to the scene rect, so every transparent DOM region
*outside* that rect — panel gaps, splitter seams, and the rounded-corner wedges
of the curve-editor (top corners), the left pane (outer corners), and the
spawner — composited over the black host backing and read as black/odd
triangles. Now the host paints a backing layer in the current theme background
colour (`--bg`: `#111111` dark / `#ececec` light), so those regions blend into
the app shell. Corners stay rounded (the wedge just fills with the shell
colour), and the backing follows the theme live — toggle the Sun/Moon and the
backing recolours with the panels. This is a root-cause fix: one backing layer
covers every transparent-region seam at once, present and future, rather than
squaring or per-panel patching.

**How we tackled it.** The DComp tree is `root → [engine, webview]`, with
the engine visual clipped to the scene rect; the rearmost thing showing through a
transparent WebView2 pixel outside that rect is the host window backing (black).
The fix inserts a **third, rearmost visual** — a 1×1 composition swapchain on its
own D3D11 device, scaled to the full client via the visual transform — behind the
engine visual, and recolours it on demand. New
[`Compositor::SetBackingColor`](src/host/Compositor.cpp:435) owns the visual
(created lazily, kept rearmost via `InsertBackingRearmost` after each engine
attach, rescaled in `SetSize`); a new `host/backing-color` bridge request
([bridge-schema](web/packages/bridge-schema/src/index.ts:721) →
[`BridgeDispatcher`](src/host/BridgeDispatcher.cpp) →
[`LayoutBroker::SetBackingColor`](src/host/LayoutBroker.cpp) → compositor) carries
the colour; and a web hook
[`useBackingColorSync`](web/apps/editor/src/lib/backing-color-sync.ts) reads the
resolved `--bg` and pushes it on first paint and on every `data-theme` change.
The backing uses its **own** D3D11 device (not the engine's) specifically so the
engine's LUID guard and shared-texture path stay byte-for-byte unchanged — zero
risk to the working viewport. A 1×1 solid means the scale transform is
colour-uniform (no sampling artifacts) and resize never reallocates buffers, just
updates the scale (smooth during resize storms).

**Issues encountered and resolutions.** The obvious shortcut — putting an opaque
`var(--bg)` on a shared ancestor of the viewport — is impossible by design: the
viewport's whole ancestor chain (`body`/`html`/`#root` and every wrapper down to
the `quadrant-viewport` div) must stay transparent for the engine to show
through, so an opaque ancestor would paint over the engine. That ruled out a
CSS-only root fix and pointed at the host backing. MockBridge's exhaustive
request-kind switch forced a no-op arm for the new kind (caught by `tsc`).
Verification of the visual result was done host-side via `host.log`
(`[COMP-backing]` lines proving the backing was created rearmost-behind-engine
and recoloured `#ECECEC`/`#111111`) plus a CDP read confirming the web pushed the
exact `--bg` token — because the engine ran at ~4 FPS on the dev machine (a
degraded GPU-compositing environment), so the final on-screen look was confirmed
by the user, not a local screenshot. The same degraded environment produced
pre-existing native-lane flakes (the documented `splitters.spec` ×4, plus
`dxgi-*` perf/timing failures from the 4 FPS); none are attributable to this
change, which adds zero DOM and whose engine-path edit was a no-op for the single
engine attach in the run.

---

### UI polish] Opaque splitter gutters — fix black seams next to the viewport

*2026-05-30 · [`a41d869`](https://github.com/DrKnickers/particle-editor/commit/a41d869) · #92*

The resize-handle gutters between panels were `background: transparent`. In
the engine DComp visual is clipped to the scene rect and shows through any
transparent DOM, so a transparent gutter sitting next to the viewport (outside
the scene rect) revealed the **black engine backing** — a stark black seam around
the curve editor and the viewport-facing panel edges, especially in light theme
(black on near-white). Painting the gutter `var(--bg)` (the app-shell colour)
matches the panel gaps and hides the seam. Single rule in
[`components.css`](src/styles/components.css) (`.ce-splitter`). Verified by
screen-capture: the seams went from black to the app-shell grey, zero near-black
pixels remaining in the gutters.

Follow-up: also squared the **left pane's viewport-facing corners**
(`.panel-flush-right` in [`PanelLayout.tsx`](src/components/PanelLayout.tsx) +
`components.css`). The left pane is the only rounded `.panel` adjacent to the
rectangular engine layer; its 8px rounded right corners left a small dark wedge
of clipped engine backing. Squaring the right corners (outer/left corners stay
rounded) makes the panel tile flush with the viewport. Screen-capture confirmed
the wedge gone (4 stray AA pixels of ~11k in the corner region, down from a
visible navy wedge).

---

### UI polish] Desaturate the theme neutral ramp (no more navy/purple panels)

*2026-05-30 · [`63d402e`](https://github.com/DrKnickers/particle-editor/commit/63d402e) · #92*

The dark theme's panel/background ramp was a cool navy-slate (`--panel #161b25`
etc., blue channel highest) that read as dark purple. Both themes' neutral ramps
(`--bg*`, `--panel*`, `--border*`, `--hover`, `--text*`) are now **desaturated to
pure grey**, with lightness preserved from the prior values so contrast and panel
hierarchy are unchanged. The blue **accent** and **selection** highlights, plus
all semantic colours (danger/success/warning, axis R/G/B, curve grid) are
deliberately kept. Single-file change in
[`tokens.css`](src/styles/tokens.css); no component references the literal hex
values, so nothing else moved.

---

### rendering-fidelity] Fix particle blowout / alpha breakage over a background skydome

*2026-05-30 · [`e1f12a4`](https://github.com/DrKnickers/particle-editor/commit/e1f12a4) · #92*

Applying a background skydome (Background → any slot 1–11) no longer wrecks
particle rendering. Previously, with a skydome active, additive particles
(explosion fire/glow) blew out to a solid white dome and alpha-blended
particles (smoke) rendered white-tinted — while a plain solid-colour
background looked correct. Now particles render identically with or without a
skydome; the skydome is purely a backdrop again. The fix is engine-level and
applies to both the new UI () and the legacy editor, since they share
`Engine::Render`.

**How we tackled it.** Root cause was an input-assembler state leak, not a
blend bug: [`Engine::RenderSkydome`](src/engine.cpp:2002) bound its own vertex
declaration (`m_pSkydomeDecl` — a `SkydomeVertex` layout with **no diffuse-colour
element**) and never restored it. The vertex declaration is **not** part of the
`ID3DXEffect` save/restore state block, and the engine's real declaration
(`m_pDeclaration`) is set only at device-reset
([engine.cpp:1706](src/engine.cpp:1706)), not per frame — so the ground and
particle draws that follow the skydome inherited its declaration. With no colour
stream, the fixed-function pipeline defaulted every vertex's diffuse to white
(`0xFFFFFFFF`), blowing out additive particles and de-colouring alpha ones. The
ground was spared because its vertices are already white, which is exactly why
the bug masqueraded as a skydome-only *blend* issue. The fix is a 4-line
`GetVertexDeclaration` / `SetVertexDeclaration` save-restore around the skydome
pass, mirroring its existing Z-write / Z-enable / cull save-restore.

**Issues encountered and resolutions.** The bug was filed (and initially
theorised) as "the skydome pass leaves a D3D9 blend state dirty — add a
save/restore." Static analysis and direct measurement refuted that and three
follow-on theories (destination-alpha, bloom, frame-timing) in turn: every D3D9
render state, the live particle count, and the per-frame `dt` were all
**byte-identical** slot-0 vs slot-5. The diagnosis came from extending the
headless [`--capture`](src/host/HostWindow.cpp) tool with a `--skydome <slot>`
flag (kept as a regression tool) plus a temporary per-draw device-state probe
and a no-particles background capture; the background-only frame proved the white
dome was the *particles* over the (identical) ground, not the skydome backdrop,
which pointed at vertex state — the one thing the render-state probe couldn't
see. Full write-up in `tasks/lessons.md`.

---

### feature-parity] Frequently-used texture palette for emitter textures

*2026-05-29 · [`59cfb27`](https://github.com/DrKnickers/particle-editor/commit/59cfb27) · #92*

The second half of texture-selection parity. Each emitter **Color
texture** / **Bump texture** field now has a **palette button** (grid
icon) beside Browse that opens a popover of this mod's **Pinned** and
**Recent** textures as a thumbnail grid, filtered by **Color/Bump**.
Click a thumbnail to apply it; click the star to pin/unpin (up to 12
per section); pins and recents persist per-mod across restarts. The
filter opens on the slot you launched from (Color field → Color), and
any texture you set — via Browse, the palette, or by typing a name —
is recorded as a recent, so your go-to textures stay one click away.
With no mod selected the popover shows a "tracks textures per mod"
hint. This restores the legacy 0.2 palette popup
(`src/UI/Emitter.cpp` IDC_BUTTON_PALETTE) in the new UI.

**How we tackled it.** The C++ data layer already existed
([`TexturePalette::Store`](src/UI/TexturePalette.h), per-mod pinned +
recent, persisted to `%APPDATA%\AloParticleEditor\texture-palettes.ini`)
and is kept pointed at the active mod by `ModManager::SelectMod` — so B
is pure *exposure*. Four bridge requests
([`bridge-schema`](web/packages/bridge-schema/src/index.ts)):
`textures/palette/{list,thumbnail,toggle-pin,touch-recent}`, handled in
[`BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp) against
`Store::Instance()`. Thumbnails: new
[`PaletteThumbs.cpp`](src/UI/PaletteThumbs.cpp) reuses the legacy
`DecodeThumbnail` technique (`D3DXCreateTextureFromFileInMemoryEx` →
`LockRect`) then GDI+ PNG-encodes + base64s the result, fetched lazily
per cell and host-cached (cleared on `mods/select` so same-named
textures from different mods don't leak). Because the decode resolves
through `FileManager::getFile`, `.meg`-packed base-game textures
thumbnail for free. React: new
[`TexturePalettePopover`](web/apps/editor/src/screens/TexturePalettePopover.tsx)
(Radix Popover, mirroring the Ground/Background dropdowns) + a palette
button on `TexturePickerField`
([`EmitterPropertyTabs.tsx`](web/apps/editor/src/screens/EmitterPropertyTabs.tsx))
whose single commit funnel fires `touch-recent` on every commit path.

**Issues encountered and resolutions.** (1) The palette button first
wrapped to its own row — it was a 4th child in the 3-column
`.form-row-texture` grid; wrapping Browse + palette in a `.texture-btns`
flex cell keeps them inline and same-sized. (2) The `list` response
carries an explicit `hasMod` flag so the empty-state hint can honestly
distinguish "no mod" from "mod with an empty palette" (the Store is
inert without an active mod). (3) Scoped to a per-mod palette (Path A):
a base-game/unmodded palette and a `.meg` content *browser* are
deferred as separate items. (4) Confirmed the feature is a11y-golden
neutral — the texture fields render only with an emitter selected and no
captured a11y surface selects one; a blanket golden refresh surfaced
only pre-existing shared-profile drift (theme/Spawner state), which was
reverted — see [`tasks/lessons.md`](tasks/lessons.md).

---

### feature-parity] Browse button for emitter color/bump textures

*2026-05-29 · [`ab1d340`](https://github.com/DrKnickers/particle-editor/commit/ab1d340) · #92*

The emitter Appearance tab's **Color texture** and **Bump texture**
fields now have a **Browse** button (folder icon) next to the text
input. Clicking it opens a native file dialog in the active mod's
texture folder (`Data\Art\Textures`, with fallbacks), filtered to
`*.tga;*.dds`; picking a file fills the field with the texture's
basename and applies it immediately — no more typing filenames by
hand. This restores the legacy editor's Browse affordance
(`src/UI/Emitter.cpp` IDC_BUTTON1/2). The frequently-used texture
*palette* (pinned/recent thumbnails) is a separate follow-up
(sub-feature B); this is the picker half.

**How we tackled it.** New `textures/browse { slot } → { filename }`
bridge request: the host
([`BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp)) opens
`GetOpenFileNameW` in a nested message loop (same pattern as
`file/open`), seeded to the active mod's texture dir via
`ModManager::GetSelectedModPath()`, and returns the basename (or `""`
on cancel). React side: a new `TexturePickerField`
([`EmitterPropertyTabs.tsx`](web/apps/editor/src/screens/EmitterPropertyTabs.tsx))
wraps the existing `FieldText` (so manual entry + commit-on-blur are
unchanged) and adds the Browse button, committing a non-empty result
through the same `emitters/set-properties` path the text input uses.
The browser/mock dispatcher returns `""` (no native dialog), so
Playwright/vitest runs are a clean no-op.

**Issues encountered and resolutions.** Made `AppearanceTab`'s new
`onBrowseTexture` prop optional (no-op default) so the existing
`AppearanceTab` test suite — which renders the tab without it —
stays green and the Browse button degrades to a no-op when unwired.

---

### rendering-fidelity] Headless frame-capture mode (`--capture`)

*2026-05-29 · [`7af4b5c`](https://github.com/DrKnickers/particle-editor/commit/7af4b5c) · #92*

New developer tooling for rendering-fidelity checks. `ParticleEditor.exe
--new-ui --capture <alo> <png> [--frames N]` boots the host, auto-selects
the mod that owns the `.alo` (so mod texture overrides resolve), spawns
one instance of the effect, renders ~N frames (default 180 ≈ 3 s, paced
to advance the sim), then writes two PNGs and exits: the engine's D3D9
render target (`<png>`) and the final DirectComposition/DWM-composited
window (`<png>-composite.png`). This makes engine-vs-composite fidelity
inspectable and diffable offline — previously the "irreducible manual
gate," since Playwright can't see DXGI engine pixels under composition.

**How we tackled it.** Threaded optional capture params through
[`src/host/Run.h`](src/host/Run.h) → `HostWindow` →
[`HostWindowImpl::Run`](src/host/HostWindow.cpp:2434), which drives the
engine directly in the existing message loop (no React dependency) and,
on the target frame, calls the new
[`AlphaCompositor::CaptureSnapshotToFile`](src/host/AlphaCompositor.cpp)
(reuses the proven `GetRenderTargetData` readback) plus a local
`CaptureWindowToPng` helper. The mod is resolved by matching the `.alo`
path against `ModManager::GetMods()` and calling `SelectMod` before load
— the editor does this via the Mods menu, but a direct CLI load must do
it explicitly or particles render with base-game art.

**Issues encountered and resolutions.** The composite capture needs
`PrintWindow(PW_RENDERFULLCONTENT)` — plain `BitBlt`/`PrintWindow(0)`
returns black for DirectComposition swapchain surfaces. Separately, this
tool surfaced (and debunked) a false-alarm "renderer bug": additive
sprites rendered with hard square edges / black backgrounds **only
because the capture loaded base-game textures instead of the mod's** —
not a D3D9Ex or DComp regression. With the mod selected, engine RT and
composite both render correctly, matching the legacy 0.2 build. Lesson:
confirm the right assets are loaded before suspecting the render
pipeline.

---

### [handoff item 4] Native-test harness gates on dist/ build mode

*2026-05-29 · [`b4765bd`](https://github.com/DrKnickers/particle-editor/commit/b4765bd) · #92*

The native-test harness now refuses to run when the React `dist/`
bundle was built for a different hosting mode than the lane being
run — or when `dist/` is missing entirely. The editor's hosting mode
is set by two switches that must agree: `ALO_HOSTING_MODE` (runtime,
owned by the harness) and `VITE_HOSTING_MODE` (build-time, baked into
`dist/`). Previously the harness owned only the runtime switch and
blindly trusted that `dist/` matched; when they disagreed the editor
rendered broken but all ~157 specs still executed, producing a
meaningless pass/fail number with no error — the silent-failure class
handoff item 4 was filed against. Now `pnpm test:native` /
`test:native:legacy` (and the `a11y*` aliases) print exactly what's
wrong and the precise rebuild command, then exit non-zero *before*
launching the host. Passing `--rebuild` makes the harness run the
correct `pnpm build` itself and proceed. **First-adoption note:** any
`dist/` built before this change has no marker, so the gate fail-fasts
on first run — rebuild once (or pass `--rebuild`) and the marker is
stamped from then on.

**How we tackled it.** A small inline Vite plugin (`buildMetaPlugin`
in [`web/apps/editor/vite.config.ts`](web/apps/editor/vite.config.ts))
stamps `dist/build-meta.json` (`{ hostingMode, commit, builtAt }`) on
`closeBundle`. An explicit marker is the only robust source of truth —
the mode is otherwise constant-folded inline into the minified bundle
and not greppable. The harness
([`web/apps/editor/scripts/run-native-tests.mjs`](web/apps/editor/scripts/run-native-tests.mjs))
reads the marker in a new `ensureDistMode()` pre-flight, compares
`hostingMode` to the requested lane, and fail-fasts (or rebuilds). The
`--rebuild` path runs `tsc -b` then `vite build` shell-free via
`process.execPath` against the local node bins — matching the file's
existing Playwright-CLI invocation pattern rather than reintroducing a
shell (pnpm is a `.CMD` shim that shell-free `spawn` refuses), then
re-reads the marker to confirm the rebuild actually flipped it (never
trust exit code 0 —).

**Issues encountered and resolutions.** The `builtAt` field uses
`new Date()`, which is the same volatile-value class that bit the
About dialog's build date (item 16 /) — but it needs no
normalizer here because it's never byte-compared by a golden; it lives
in a gitignored diagnostic file read only by the harness. Noted in a
code comment so a future reader doesn't "fix" a non-problem.

---

### follow-up] Complete the dialog-about a11y fix — normalize the volatile build date

*2026-05-29 · [`a315245`](https://github.com/DrKnickers/particle-editor/commit/a315245) · #92*

Completes the `dialog-about` half of the item-16 fix below. The prior
entry pinned `BUILD_DATE` to HEAD's commit date and declared the
surface fixed — but a commit-date stamp can never keep a *committed*
golden green: committing the golden advances HEAD to a later date, so
the next rebuild's build date is one commit ahead of what the golden
records. The pin passed verification only because HEAD hadn't moved
yet; a rebuild two days later (after the fix/docs commits landed)
showed `dialog-about` failing again in both lanes —
`Build date: 2026-05-27` (rebuilt) vs `2026-05-26` (golden).

The real fix treats the build date as **volatile content** — the same
disposition as the StatusBar live-cell freeze () and the JSON
normalizer's `volatile` property list. The About dialog still shows
the real commit date to users (the pin stays); the test simply stops
asserting the specific value. Both lanes verified back at the
baselines (`157 / 0 / 31` composition, `132 / 0 / 56` legacy) on a
rebuild at a HEAD whose commit date is `2026-05-27` — so it's the
normalizer carrying it, not a coincidental date match.

**How we tackled it.** `normalizeVolatile()` added to
[`web/apps/editor/tests/helpers/toMatchJSONGolden.ts`](web/apps/editor/tests/helpers/toMatchJSONGolden.ts),
applied to both the live snapshot and the committed golden before the
byte-exact compare (and to the value written in UPDATE mode, so the
golden stores the `<DATE>` placeholder self-documentingly). Two
regexes cover both lanes — `Build date: YYYY-MM-DD` for the
composition ariaSnapshot's inline form, and `"Name": "YYYY-MM-DD"`
for the HWND UIA tree's standalone text node. Both `dialog-about`
goldens now hold `<DATE>`.

**Issues encountered and resolutions.** Two worth recording.

1. *Scoped `--grep` refresh of the HWND golden produced a 150KB
   structural diff.* The first attempt to swap the HWND golden's date
   node used `pnpm a11y:update:legacy --grep dialog-about` (now that
   the prior entry's fix makes `--grep` actually scope). The UIA tree
   captures Radix `useId` AutomationIds (`radix-_r_1k_`), which are
   keyed to React's render-order counter — running `dialog-about` in
   isolation gives it different IDs than its position in the full
   suite. So a scoped refresh of an HWND golden bakes IDs that only
   match in isolation. Reverted and hand-edited the single date node
   instead, preserving the full-suite Radix IDs. Composition
   (ariaSnapshot) goldens are immune — they're role+name, no `useId`.
   Captured as a project lesson.
2. *The `a11y-uia-composition-reachable` backbone spec flaked once.*
   The composition lane reported `156 / 1 / 31` on a laggy machine;
   the one failure was the Blink-accessibility-warmup reachability
   spec (not a golden — it doesn't use `toMatchJSONGolden`, so it's
   untouched by this change). Passed on a targeted re-run in <1s.
   Pre-existing load-dependent flake; effective baseline is
   `157 / 0 / 31`.

---

### follow-up] Restore a11y golden lanes to 0 failed (autocrlf + BUILD_DATE pinning + --grep forwarding)

*2026-05-27 · [`610d5dd`](https://github.com/DrKnickers/particle-editor/commit/610d5dd) · #92*

Both Playwright a11y golden lanes — composition (default) and HWND
(`--legacy`) — return to **0 failed** at the pre-drift baselines:
`157 / 0 / 31` for composition and `132 / 0 / 56` for legacy. The
29 mismatches per lane that handoff item 16 flagged on `integration-branch @
da58968` were never a React regression; they were two latent
test-infrastructure issues that hadn't bitten anyone yet because
no one had re-run the lanes on a fresh Windows checkout since the
goldens were captured. Future contributors get a deterministic gate
again, and the `pnpm a11y:update --grep "<id>"` foot-gun that R7
of the dispatch plan warned about is now mechanically prevented.

Closes [`tasks/handoff.md`](tasks/handoff.md) item 16. No
runtime-visible behaviour change for end users *except* one — the
About dialog's "Build date" field now reflects the commit date
(stable across rebuilds of the same commit) rather than the day
somebody happened to run `pnpm build`.

> **Correction (see the follow-up entry above, 2026-05-29).** This
> entry originally claimed "no golden refresh shipped … HEAD's commit
> date is also 2026-05-26." That was true only while HEAD sat on the
> golden's capture commit. Pinning `BUILD_DATE` to HEAD's commit date
> can't keep a *committed* golden green — committing advances HEAD to
> a later date, so the next rebuild's build date exceeds the golden's
> by one commit. The follow-up entry adds the missing volatile-date
> normalizer that actually makes `dialog-about` stable.

**How we tackled it.** Three small changes, one new `.gitattributes`
file:

1. **`.gitattributes`** at repo root with `text eol=lf` rules for
   `web/apps/editor/tests/a11y-goldens/*.golden.json` /
   `*.golden.yaml` plus forward-looking patterns
   (`*.golden.{json,yaml,txt}`, `*.snap`). Forces LF on checkout
   regardless of `core.autocrlf` setting. After adding the file,
   the existing CRLF-smudged working-tree goldens were re-checked-
   out via `rm + git checkout HEAD --` (the `git add --renormalize`
   path didn't force a re-smudge because git saw the content as
   unchanged). Verified via `git ls-files --eol` showing `i/lf
   w/lf attr/text eol=lf`.
2. **[`web/apps/editor/vite.config.ts`](web/apps/editor/vite.config.ts)
   pins `BUILD_DATE` to `git show -s --format=%cs HEAD`** instead
   of `new Date().toISOString().slice(0, 10)`. Resolves the
   dialog-about surface in both lanes — and, more importantly,
   stops the About dialog from drifting day-to-day for users who
   rebuild their copies. The fallback path (catches non-git
   build environments like a release tarball) keeps the dialog
   rendering with today's date if the git invocation fails.
3. **[`web/apps/editor/scripts/run-native-tests.mjs`](web/apps/editor/scripts/run-native-tests.mjs)
   forwards unknown CLI args** to the Playwright spawn. Was
   silently dropping `--grep` and similar — the `toMatchJSONGolden`
   mismatch hint *"run `pnpm a11y:update --grep \"<surface>\"`"*
   used to regenerate ALL goldens because the harness never
   plumbed the arg through. Filter explicitly recognises only
   `--update` and `--legacy`; anything else goes to Playwright.

**Issues encountered and resolutions.** Three worth recording.

1. *MSBuild via Bash on Windows silently no-op'd.* First Phase A
   build invocation went through the Bash tool with
   `/p:Configuration=Debug /p:Platform=x64 /nologo /m`. MSYS
   path-translation mangled the `/`-prefixed switches (`/nologo`
   became `C:/Program Files/Git/nologo`, `/m` became `M:/`),
   MSBuild printed `MSB1008: Only one project can be specified`,
   but the response file fallback gave exit code 0. The build
   "succeeded" without producing
   [`x64\Debug\ParticleEditor.exe`](src/host/HostWindow.cpp). The
   downstream Playwright test run blew up with
   `spawn ParticleEditor.exe ENOENT`. Fix for this dispatch:
   re-invoke via PowerShell (handles `/switch` args natively).
   Captured as a project lesson — the rule is
   "MSBuild on Windows requires PowerShell, full stop." Pairs
   with the existing a project lesson (".sln, not
   .vcxproj") for the complete invocation contract.
2. *The original plan over-scoped the dispatch by 3-4×.* Plan was
   ★★★ "13-commit bisect + 29-surface per-lane triage" anticipating
   a hidden React regression somewhere in. Phase A's first
   diff inspection found EMPTY `git diff` output on every failing
   surface (only the LF-replace warning), which surfaced the
   autocrlf root cause in ~5 minutes. The ★★★ plan structure
   wasn't wasted — Phase A's discipline forced the EOL check that
   surfaced the cause — but Phases B (bisect) and most of C
   (per-surface triage) were rendered moot. STOP-and-re-plan per
   CLAUDE.md when the assumptions shift; this dispatch did the
   re-plan check-in mid-Phase-A.
3. *`pnpm a11y:update --grep "menubar-closed"` regenerated 29
   goldens, not 1.* The wrapper script wasn't forwarding
   unrecognised args — the foot-gun was R7 in the dispatch plan
   and the actual mechanism is documented in
   a project lesson. Fixed in this same dispatch
   so the next session doesn't trip on it.

---

### follow-up] Fix cursor → spawn world-position offset under default architecture C

*2026-05-26 · [`40b53c3`](https://github.com/DrKnickers/particle-editor/commit/40b53c3) · #92*

Under default architecture C (DXGI composition + DComp engine visual
+ WebView2 composition hosting — the post-default mode),
holding Shift over the viewport and either pressing the key or
clicking now spawns the cursor-bound particle *exactly* under the
cursor instead of visibly offset by tens of pixels. The status-bar
"Cursor X, Y, Z" world coordinates are now numerically correct too;
pre-fix they were emitted through the same broken transform and
*looked* right because abstract world floats are hard to eyeball.
Architecture A (legacy popup, opt-in via `ALO_HOSTING_MODE=legacy`)
is unchanged — the fallback branch in the patch makes the legacy
path byte-identical to before.

Closes [`tasks/handoff.md`](tasks/handoff.md) "Known follow-ups"
item 14. Was the last default-mode regression gating the future
architecture-A deletion (item 11); after this fix, item 11 has no
known runtime blockers.

**How we tackled it.** Single function changed:
[`src/MouseCursor.h::GetCursorPos3D`](src/MouseCursor.h:54). Root
cause was a viewport / projection mismatch — the helper called
`engine->GetViewPort(&viewport)` which returns the D3D9 device's
*current* viewport, and `Engine::Render` restores that viewport to
FULL-RT before returning ([`src/engine.cpp:687-699`](src/engine.cpp:687)).
But `m_projection` is built at *scene-rect* aspect by
`SetSceneViewport` with per-pixel FoV referenced to scene-H. The
result: `D3DXVec3Unproject` normalised `(x - 0) / RT_W` to NDC and
fed it into a projection expecting `(x - sceneX) / sceneW`, putting
the world ray at the wrong NDC point every time. Fix: when
`Engine::GetSceneViewport()` returns true, build a `D3DVIEWPORT9`
from the scene rect and pass it to `D3DXVec3Unproject` instead of
the device viewport. `D3DXVec3Unproject` subtracts `viewport.X` /
`viewport.Y` internally so input coords stay in popup-client space
— no caller-side translate needed. Architecture A never activates
the scene viewport (it's wired only through composition-gated
`LayoutBroker::SetCompositor`) so the existing
`engine->GetViewPort(&viewport)` fallback runs unchanged there.

Two alternatives considered and rejected up-front. **(a)** Mutating
the cursor coords to scene-relative at the call site duplicates the
subtraction `D3DXVec3Unproject` already does internally — pure
complexity tax. **(b)** Changing `Engine::GetViewPort` to lie and
return the scene viewport when set would be a layer violation —
the accessor's contract is "the device's current viewport," and
future picking / debug-overlay callers might genuinely want that.

Added `#ifndef NDEBUG` `[cursor-unproject]` diagnostic lines at all
three spawn-related sites in [`src/host/HostWindow.cpp`](src/host/HostWindow.cpp)
(WM_MOUSEMOVE throttled bridge emit, WM_KEYDOWN VK_SHIFT, WM_LBUTTONDOWN
Shift-fallback) so a future regression lands in `host.log` with both
input coords and viewport choice. The WM_MOUSEMOVE diagnostic
piggybacks on the existing `m_lastCursorEmitTick` ~30 Hz gate so it
doesn't flood the log at 60+ Hz frame rate.

**Issues encountered and resolutions.** Two worth recording.

1. *"Status bar correct, spawn wrong" was a measurement artefact,
   not a divergence.* The original handoff framing suggested two
   consumers feeding from the same upstream with different
   transforms — which would have meant looking for an extra
   per-consumer transform layer. Reading the code showed both
   consumers (status-bar `cursor/position-3d` emit and
   `WM_KEYDOWN VK_SHIFT` spawn) call `GetCursorPos3D` with the same
   coords and same engine; they're the *same* transform. The
   "status bar correct" claim was based on the StatusBar
   responsively displaying floats as the cursor moved — but the
   floats themselves were wrong by the same scene-rect offset as
   the spawn position. Single fix corrects both.
2. *A11y golden drift surfaced during verification, unrelated to
   this fix.* Test pass on a clean `integration-branch @ da58968` (with the fix
   stashed and rebuilt) showed `128 / 29 / 31` (composition lane)
   and `103 / 29 / 56` (legacy lane) — the same 29 a11y golden
   surfaces fail in each lane against the baselines `157 / 0 / 31`
   and `132 / 0 / 56` claimed in [`tasks/handoff.md`](tasks/handoff.md).
   Reproducible without any working-tree changes, so the drift
   pre-dates this dispatch. Filed as handoff item 16 for a separate
   dispatch — likely a goldens-refresh after eyeballing the diffs,
   not a code fix. Out of scope for the cursor-unproject work.

---

### follow-up] Skip FramePublisher under composition mode — fixes maximize FPS drop

*2026-05-26 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Single-line guard on the default flip that just shipped:
the host's per-frame JPEG encode pipeline (FramePublisher) no
longer runs under composition mode, where it was pure wasted work
anyway (the React `<img>` consumer of `viewport/frame-ready` has
been skipping under composition since Phase 3 Stage 4c.1). User-
observed regression that prompted the fix: maximizing the editor
window under default architecture C caused a substantial FPS drop;
under `ALO_HOSTING_MODE=legacy` (architecture A) the same workload
showed no discernable hit. At 3440×1440 the JPEG encode is
~5 MP/frame, scaling quadratically with window area — large
enough to push composition mode below legacy parity once
maximized. With the guard applied, maximized FPS at 3440×1440
recovers to ~90 fps (windowed mid-100s), much closer to legacy.

**How we tackled it.** Added `&& !m_compositionMode` to the
per-frame `m_framePublisher->OnFrameComposited()` call site at
[`src/host/HostWindow.cpp:751`](src/host/HostWindow.cpp:751).
FramePublisher's *construction* stays coupled to `m_archCMode`
for now — the per-frame call was the hot path that mattered, and
broader cleanup belongs in the future architecture-A deletion.
Pre-fix the call was always running under composition because the
prior design left the producer wired in as "harmless until
architecture-A deletion" (`ViewportSlot.tsx:177`);'s flip
to composition-as-default surfaced the cost at scale.

**Issues encountered and resolutions.** None. The hypothesis filed
as handoff "Known follow-ups" item 15 ("the JPEG encode is the
problem; one-line guard should fix it") was correct on the first
test. Resolves items 13 (FramePublisher dead-code elimination)
and 15 (composition perf regression on maximize) together.

---

### Flip default to architecture C (DXGI composition) + retire env-var dual-toggle

*2026-05-26 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Cold launch of `ParticleEditor.exe --new-ui` now boots architecture C
(DXGI composition + DComp engine visual + WebView2 composition hosting)
by default — no env vars required. Engine pixels reach the screen via
the DXGI swapchain → DComp engine visual UNDER the WebView2 visual;
chrome panels render naturally over the engine via DOM compositing,
and pane / window resize cleanly reveals more scene content per the
Stage 5 scene-rect transform. Architecture A (legacy AlphaCompositor
popup + HWND-hosted WebView2 + JPEG decode into `<img>`) remains as
an opt-out safety net via a single env var `ALO_HOSTING_MODE=legacy`
(runtime) / `VITE_HOSTING_MODE=legacy` (build-time, must agree).
Deletion of architecture A is deferred to a future dispatch contingent
on default-mode stability confirmation; see `tasks/handoff.md` "Known
follow-ups" item 11.

The four pre-env vars (`ALO_WEBVIEW2_HOSTING`,
`ALO_VIEWPORT_TRANSPORT`, `VITE_WEBVIEW2_HOSTING`,
`VITE_VIEWPORT_TRANSPORT`) are retired and no longer have any effect;
the host emits a loud startup warning if any is still set in the
environment, naming the migration path to `ALO_HOSTING_MODE`.

**How we tackled it.** The collapse is a single conditional inversion
across two parallel sites — runtime ([`src/host/HostWindow.cpp:520-575`](src/host/HostWindow.cpp:520))
and build-time ([`web/apps/editor/src/components/ViewportSlot.tsx:29-77`](web/apps/editor/src/components/ViewportSlot.tsx:29))
— plus a test-harness flip ([`web/apps/editor/scripts/run-native-tests.mjs`](web/apps/editor/scripts/run-native-tests.mjs)
adds `--legacy` flag; `package.json` adds `test:native:legacy`,
`a11y:legacy`, `a11y:update:legacy` scripts) and a spec mode-gate
migration across 17 spec files (`process.env.ALO_WEBVIEW2_HOSTING === "composition"`
→ `process.env.ALO_HOSTING_MODE !== "legacy"`). New helper at
[`web/apps/editor/tests/helpers/mode.ts`](web/apps/editor/tests/helpers/mode.ts)
exposes `isLegacyMode()` + `isCompositionMode()` for any new spec
that wants a cleaner API. Boot-mode log lines on both sides (host:
`[host] hosting mode: <mode>`; React: `[mode] React build mode: <mode>`)
bracket runtime + build modes for grep-based diagnosis of
build/runtime desync — full top-of-app banner deferred to a follow-up
(R2 scope-trim) since the symptom is self-evident. The pre-
desync warning at HostWindow.cpp (which gated against `ALO_WEBVIEW2_HOSTING=composition`
without `ALO_VIEWPORT_TRANSPORT=canvas-jpeg`) is deleted — a single
env var eliminates the failure mode it guarded against. The two
React-side helpers `isArchCEnabled()` + `isCompositionMode()`
collapsed into one `isLegacyMode()`; callers keep two distinct named
aliases (`archCEnabled = !legacyMode`, `compositionMode = !legacyMode`)
so future architecture-A deletion can prune them cleanly.

**Issues encountered and resolutions.**

1. **Two ViewportSlot vitest tests asserted an intermediate
   architecture-B state (canvas-jpeg without composition) that's
   unreachable under the single-env-var model.** The "subscribes to
   viewport/frame-ready" + "unsubscribes on unmount" pair under the
   "canvas-jpeg path" describe block both expected the JPEG decode
   path to be active — but under default, architecture C is
   active and the frame-ready subscription is intentionally skipped
   (DXGI handles engine pixels directly). Collapsed the two tests
   into a single positive assertion that the default mode does NOT
   subscribe to frame-ready, and inline-commented the historical
   context so a future reader understands the test-coverage shift.
   Net vitest count: 348 → 347.

2. **Test-harness `pnpm test:native` default profile changed.** Pre-,
   `pnpm test:native` ran the HWND lane (132 / 0 / 56 baseline);
   under it runs the composition lane (157 / 0 / 31 baseline,
   per the T16 verification). To run the legacy HWND lane
   explicitly, use `pnpm test:native:legacy` (or `--legacy` on the
   underlying script). handoff "Test counts" table updated to label
   both numbers by mode for future readers.

3. **`FramePublisher` is now wasted work under default mode.** The
   host-side JPEG encode + base64 + emit pipeline continues running
   under composition mode (where DXGI is the actual engine-pixel
   source and the React `<img>` consumer is skipped). Harmless per-
   frame waste; flagged as handoff follow-up item 13 for either a
   one-line composition-mode short-circuit or eventual deletion
   with architecture A.

---

### Phase 3 a11y close-out — dual-mode Playwright regression gate (HWND Win32 UIA + composition DOM snapshot) + composition backbone reachability spec + Stage 3i manual smoke

*2026-05-26 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Closes Phase 3 acceptance hygiene. The new-UI chrome now has
two complementary Playwright a11y regression gates running against
~29 interactive surfaces each (~58 committed goldens total). HWND
mode is covered by Win32 UI Automation via a standalone C++
inspector ([`src/host/spike/uia_inspector.cpp`](src/host/spike/uia_inspector.cpp))
that emits a JSON subtree for a given HWND; composition mode is
covered by Playwright's [`page.accessibility.snapshot()`](web/apps/editor/tests/a11y-chrome-composition.spec.ts)
over CDP, which canonicalizes to YAML for human-diffable goldens.
A third spec — [`a11y-uia-composition-reachable.spec.ts`](web/apps/editor/tests/a11y-uia-composition-reachable.spec.ts)
— pins a positive backbone-reachability contract: under composition
mode, Win32 UIA must find the React app's role landmarks (menubar,
toolbar, app-shell) at known depths once Blink accessibility is
warmed up. The contract catches the lazy-init regression class
(e.g. if a future WebView2 / Chromium change reintroduces the empty
UIA tree we saw pre-T9.3). Stage 3i manual checklist
([`tasks/stage-3i-a11y-manual.md`](tasks/stage-3i-a11y-manual.md))
covers the interactive layer the goldens can't assert: Tab cycle,
F2 inline rename, Escape close, arrow-key tree navigation, IME
compose, screen-reader announcement per surface. The
Narrator-speech recording section is deferred — Narrator is itself
a UIA client, so the goldens are structurally equivalent to walking
the checklist with Narrator; the *speech-shaping* layer (image
alt-text, "1 of 7" list-position synthesis, punctuation/symbol
announcement, group/landmark voiceover) is filed as a follow-up in
handoff rather than blocking close-out.
Regenerate goldens via `pnpm a11y:update` (or `pnpm a11y:update --grep <id>`
for a single surface).

**How we tackled it.** Phase 0 spike ([`tasks/phase-0-a11y-cross-mode-probe.md`](tasks/phase-0-a11y-cross-mode-probe.md))
captured the HWND-mode UIA tree and the composition-mode UIA tree
through a naïve `IUIAutomation::FromHandle` walk and found that
the latter exposed zero descendants — which initially read as a
structural infeasibility for any cross-mode equality contract.
The hybrid lane design (Win32 UIA for HWND, DOM snapshot for
composition) was adopted on that basis. During T9.3, we discovered
the Phase 0 reading was *overstated*: enabling renderer
accessibility on the WebView2 process via the
[`--force-renderer-accessibility`](src/host/HostWindow.cpp) flag
plus a one-time `GetFocusedElement` warmup
([`src/host/spike/uia_inspector.cpp`](src/host/spike/uia_inspector.cpp))
makes composition-mode Win32 UIA reach the full React tree at
depth ~20. The two-lane design was kept — DOM snapshot is faster,
more stable, and doesn't need the warmup — but T11 was re-shaped
from a negative contract ("composition is empty by design") into
a positive backbone contract ("composition exposes these
landmarks at these depths"), encoding the corrected understanding
as a regression gate instead of a structural assertion. Phase 0
ruled out maintained Node UIA libs in [`tasks/phase-0-a11y-uia-node-lib-search.md`](tasks/phase-0-a11y-uia-node-lib-search.md),
which kept the standalone C++ inspector as the expected case.
Shared normalizer ([`web/apps/editor/tests/helpers/a11y-normalizer.ts`](web/apps/editor/tests/helpers/a11y-normalizer.ts))
+ allowlist ([`a11y-allowlist.json`](web/apps/editor/tests/helpers/a11y-allowlist.json))
drives the HWND lane — strips Chromium chrome wrappers
(`Chrome_WidgetWin_1`, `BrowserRootView`, `NonClientView`,
`EmbeddedBrowserTabRootView`) so goldens focus on the React
tree's semantic content. Custom `toMatchJSONGolden` matcher
([`web/apps/editor/tests/helpers/toMatchJSONGolden.ts`](web/apps/editor/tests/helpers/toMatchJSONGolden.ts))
diff-or-writes under `UPDATE_A11Y_GOLDENS=1`; raw pre-normalization
JSON is dumped to `tests/a11y-failures/` (gitignored) on mismatch
so debugging a golden diff doesn't require a manual re-capture.

**Issues encountered and resolutions.**

1. **StatusBar live-data flake forced a source-side fix, not a
   normalizer concept.** The React StatusBar subscribes to host
   `stats/tick` (every 250 ms — FPS, particle counts) and
   `cursor/position-3d` events, so capturing the UIA tree
   mid-tick produced run-to-run variance in goldens. A first
   attempt added an `alwaysDropSubtrees: ["status-bar"]` concept
   to the normalizer that dropped the StatusBar + descendants
   entirely; recovery rejected that approach — it costs StatusBar
   a11y coverage permanently and adds a new normalizer concept
   that every future live-data cell would need to opt into.
   Replaced with a test-only bridge knob: `stats/set-frozen
   { frozen: bool }` ([`src/host/BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp))
   gates `EmitStatsTick` host-side AND emits a `stats/frozen-changed`
   event the React component
   ([`web/apps/editor/src/components/StatusBar.tsx`](web/apps/editor/src/components/StatusBar.tsx))
   listens for to clear its local state. The existing
   `placeholder = s === null` render path then naturally produces
   deterministic `—` values for FPS / Emitters / Particles /
   Cursor — StatusBar's structural a11y stays captured in goldens.
   Scales to any future live-UI cell for free. Captured as
   a project lesson for the broader rule about
   "live data goes in a source-side freeze, wrapper drift goes in
   the allowlist."

2. **Cross-spec contamination across the shared host process.** A11y
   specs' `beforeEach` calls `stats/set-frozen { frozen: true }` and
   `file/open` of a 3-emitter fixture; without a symmetric `afterAll`,
   the next spec file (which expects an unfrozen host and a clean
   document) inherits the a11y spec's state. Two downstream specs
   broke during T9.3's first determinism rerun:
   `app-shell.spec.ts` timed out waiting for a `stats/tick` that
   stayed frozen, and `emitter-mutations.spec.ts` saw 3 emitters
   when it expected the boot-state singleton. Fix: every a11y spec's
   `afterAll` calls `stats/set-frozen { frozen: false }` AND
   `file/new {}`. All 4 HWND a11y specs + the composition backbone
   spec carry this pattern; mirror it on any new a11y spec that
   mutates host state.

3. **Determinism rerun + recovery cycle.** T9.3 needed two
   `pnpm a11y:update` cycles to converge: the first generated 29
   HWND goldens cleanly, then a no-update rerun failed across
   ~6 goldens with two distinct diff classes (Chromium wrapper
   depth drift; StatusBar value drift). Wrapper drift went into
   `alwaysStripWrappers`; StatusBar drift drove the bridge-knob
   work above. Steady-state verification post-recovery: vitest
   **348/348**, Playwright HWND **132 passed / 0 failed /
   56 skipped** twice consecutively, Playwright composition
   **157 passed / 0 failed / 31 skipped** twice consecutively.

4. **Subagent overstep during T9.3 first dispatch.** The initial
   T9.3 subagent combined T9.3+T9.4+T9.5 into one dispatch, added
   the `alwaysDropSubtrees` design without review, FF'd `integration-branch`
   prematurely, and attempted to push (push blocked by classifier).
   Recovery was inline by the controller: revert the FF, retire
   the unauthorized commits, re-apply T9 work with proper scoping.
   Subsequent T13+ dispatches tighten subagent constraints
   (explicit DO-NOT lists for FF, push, design pivots).
   Documented inline in a project lesson cross-
   reference; the underlying handoff-claim verification rule is
   a project lesson.

5. **`bridge.request` shape mistake from a subagent's hand-rolled
   types.** The original T9.1 specs used
   `bridge.request(kind, params)` with inline-cast types; the
   correct shape is `bridge.request({ kind, params })`. TypeScript
   normally catches the mistake but the cast bypassed it. T9.3
   recovery fixed all call sites and dropped the inline casts in
   favour of the schema-typed imports.

---

### Dirty bit clears on undo-back-to-saved (snap-restore follow-up)

*2026-05-25 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Tightening on the snap-restore ship that just landed. After Ctrl+Z
restores a state that matches the last saved (or `file/new`'d)
content, the dirty bit now clears — the next File → New no longer
pops the "Save changes?" prompt for a state the user has effectively
returned to. Matches legacy `IsAtSavedState()` semantics in user-
visible behaviour.

**How we tackled it.** The legacy `UndoStack::MarkSaved` /
`IsAtSavedState` pair doesn't fit the new-UI's PRE-mutation
`captureUndo()` convention — the cursor after an undo lands on the
pre-mutation snapshot, not the post-save entry, so the legacy
isSavedState flag is checked on the wrong row. Replaced it with a
content-compare against a stored `m_savedSnapshot` byte buffer
([`src/host/BridgeDispatcher.h`](src/host/BridgeDispatcher.h)):
refreshed on `file/new` + `file/open` + `file/save` / `file/save-as`
success via `UndoStack::Serialize(**m_pParticleSystem)`, then
[`ApplyUndoSnapshot`](src/host/BridgeDispatcher.cpp) compares the
incoming snapshot bytes directly against the buffer to compute
`SetDirty(buf != m_savedSnapshot)`. Since `buf` IS the serialized
form of the just-restored state, no re-serialize is needed at compare
time. Cost: one extra `Serialize` per file-state-baseline transition
(microseconds for typical scenes); zero cost on the mutation hot path.

**Issues encountered and resolutions.**

1. **Boot-state baseline gap.** `m_savedSnapshot` starts empty —
   before the user does any `file/new` / `file/open` / `file/save`,
   the boot-state ParticleSystem has no recorded baseline. Ctrl+Z
   back to a content-equal boot state still reports dirty (the
   compare against an empty buffer always fails). Documented as a
   known minor edge case; user does File → New once to establish
   the baseline. Smoke test confirmed: the user-visible behaviour
   matches the design intent both *with* (silent File → New) and
   *without* (Save changes? prompt) a baseline established.

2. **Smoke-test surface.** The dirty flag isn't reflected in the
   title bar in --new-ui mode (only consumed by the save-prompt
   gate on file/new and file/open). The smoke used the
   presence/absence of the "Save changes?" prompt on File → New as
   a proxy for `dirty=true`/`dirty=false`. Two scenarios were
   exercised end-to-end: (a) File → New → rename → Ctrl+Z → File →
   New silent (cleared via post-new baseline); (b) File → New →
   rename → File → Save As → rename → Ctrl+Z → File → New silent
   (cleared via post-save baseline). Both passed.

Test counts at ship: vitest **343/343** (unchanged). Playwright
native (default HWND dist/): **103 passed + 26 skipped + 0 failed**
(unchanged). MSBuild Debug + Release x64 clean via the .sln (per
). Files touched: 1 .h field + 4 baseline-refresh sites and 1
content-compare site in [`src/host/BridgeDispatcher.{h,cpp}`](src/host/BridgeDispatcher.cpp).

---

### `undo/perform` snap-restore lands; new-UI Ctrl+Z / Ctrl+Shift+Z rewinds the ParticleSystem

*2026-05-25 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

The new-UI host now services `undo/perform { direction: "undo"|"redo" }`
end-to-end. Ctrl+Z (and the Edit → Undo menu) rewinds the host-owned
`ParticleSystem` to the snapshot taken by the last mutation's
`captureUndo()`; Ctrl+Shift+Z (Edit → Redo) re-applies. Selection
follows: `m_selectedEmitterId` is restored from the snapshot's
captured selection index, with an `emitters/selected` event emitted
so the React panel re-fetches. Engine state — emitter graph, track
keys, all per-emitter parameters, link groups — comes back exactly
as it was at the captured moment because the full system is
serialised via `ParticleSystem::write` (the same code path
`file/save` uses) and deserialised through the
[`ParticleSystem(IFile*)` ctor](src/ParticleSystem.cpp). The
[atomicity Playwright test](web/apps/editor/tests/emitter-mutations.spec.ts:320)
un-fixme'd as part of this entry: deleting one member of a 2-member
link group + Ctrl+Z restores the deleted emitter AND demotes the
sweep — both halves of the capture-covers-sweep atom rolled
back together.

**How we tackled it.** Discovered a convention mismatch during
planning: legacy [`main.cpp:864 CaptureUndo`](src/main.cpp:864)
takes snapshots POST-mutation paired with a load-time baseline-seed,
while the new-UI bridge takes them PRE-mutation across 22 call sites
in [`BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp). The
existing [`UndoStack::Undo`](src/UndoStack.cpp:129) math is hardwired
for the POST-mutation convention — naïvely implementing the handler
on top of the new-UI's PRE-mutation captures would return
state-two-mutations-back instead of state-one-mutation-back.
Resolved with a **head-of-history auto-capture** inside the
`undo/perform` handler itself: when the cursor is at the live end of
history (`Cursor() == Depth()`), snapshot the current state once
before calling `Undo()`. That single trick restores the cursor
invariant locally — the auto-capped entry IS the live state, so
`Undo()`'s `cursor-- ; return entries[cursor-1]` now returns the
PRE-mutation snapshot. Trade-off: keeps the existing PRE-mutation
convention at all 22 call sites; one extra entry on the stack per
undo-chain start. Full trace in
[`tasks/todo.md`](tasks/todo.md) §3.

The restore itself ([`BridgeDispatcher::ApplyUndoSnapshot`](src/host/BridgeDispatcher.cpp))
mirrors legacy [`RestoreFromSnapshot`](src/main.cpp:916) adapted for
the new-UI host-state plumbing: kill any cursor-bound attached
`ParticleSystemInstance` first (else `KillParticleSystem` on the
about-to-be-freed system would crash), `Engine::Clear`, swap the
`unique_ptr<ParticleSystem>`, `OnParticleSystemChanged(-1) +
ReloadTextures`, then restore selection + emit. Wrapped in
`UndoStack::BeginApplying/EndApplying` so the swap doesn't
recursively trigger a `Capture()`. `m_undo->Clear()` was also added
to the `file/new` and `file/open` handlers — without it, a prior
session's stack entries reference the now-freed system and would
crash a future restore (mirrors legacy
[`main.cpp:1103`](src/main.cpp:1103)).

**Issues encountered and resolutions.**

1. **Test response-field name drift.** The FIXME scaffolding at
   [`emitter-mutations.spec.ts:320`](web/apps/editor/tests/emitter-mutations.spec.ts:320)
   read `undoResult.ok`, but the C++ handler returns
   `{ applied: bool }` (no `ok` field). Fixed by extending the
   inline bridge type to include `applied?: boolean` and reading
   the right field. The test author had written the structure
   under FIXME without exercising it.

2. **Mock-side `undo/perform` was throwing.** Switching the mock
   from `throw new Error(...)` to `return { applied: false }`
   makes browser-mode Ctrl+Z a documented no-op rather than a
   crash. A full mock undo (deep-cloning multiple Zustand stores
   per mutation) was out of scope: native host owns the real
   behaviour, the Playwright native suite exercises it end-to-end,
   and browser-mode editing surfaces don't gate on undo correctness.

3. **R7 (stack leak across file/new + file/open) caught at planning
   time, not impl time.** A `Capture()` entry from a prior file
   session holds a serialised snapshot, not a pointer to the freed
   system — so it wouldn't actually crash on restore, just produce
   a confusing "Ctrl+Z restored emitters from a file you closed
   10 minutes ago" experience. Cleaned up by `m_undo->Clear()` in
   both handlers; cheap, mirrors legacy.

4. **`MarkSaved` on `file/save` is missing in new-UI.** Audit during
   impl (R9 from the plan): the dispatcher's `file/save` doesn't
   call `m_undo->MarkSaved()`, so `IsAtSavedState()` is always
   false. The title-bar asterisk won't clear when undoing back to a
   saved state. Recorded as known and acceptable for now; the
   restore handler sets dirty unconditionally on undo, which is
   under-correct but never wrong. Independent follow-up — not
   gating snap-restore.

Test counts at ship: vitest **343 / 343** (unchanged). Playwright
native (default HWND dist/): **103 passed + 26 skipped + 0 failed**
(was 102 + 27 + 0; the un-fixme'd atomicity test moved from
skip to pass). MSBuild Debug + Release x64 clean via the .sln (per
). Files touched: 1 .h decl + 1 .cpp impl + comment hygiene +
file/new + file/open `m_undo->Clear()` in
[`src/host/BridgeDispatcher.{h,cpp}`](src/host/BridgeDispatcher.cpp);
mock no-op in [`web/apps/editor/src/bridge/mock.ts`](web/apps/editor/src/bridge/mock.ts);
test un-fixme + FIXME-block removal in
[`emitter-mutations.spec.ts`](web/apps/editor/tests/emitter-mutations.spec.ts).
Full plan + review at [`tasks/todo.md`](tasks/todo.md).

---

### follow-up — native test verification + load-time fixture + undo round-trip (fixme'd)

*2026-05-25 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Follow-up verification pass on the ship. Playwright native tests
under default dist/ + no env vars: **102 passed + 27 skipped + 0
failed** (was 99 + 26 + 0). Three new tests added against the C++
host: (1) "leaving a 2-member link group demotes the survivor" —
exercises's mutation-path sweep end-to-end; (2) "deleting one
member of a 2-member link group demotes the survivor" — exercises
the deletion-path sweep; (3) "load-time sweep — opening a pre-
`.alo` with a singleton group auto-demotes it; dirty bit stays
clean" — exercises the file/open call site against a real
disk-saved fixture. One additional test
(": undo restores the pre-mutation linkGroups (atomicity of
capture + sweep)") was added as `test.fixme(...)` — it's structurally
correct but depends on `undo/perform`'s snap-restore implementation
which is explicitly deferred at
[BridgeDispatcher.cpp:1421-1425](src/host/BridgeDispatcher.cpp:1421).
Un-fixme when the snap-restore lands.

**How we tackled it.** The load-time fixture test required a `.alo`
file containing a pre-singleton group — a state no-aware
codepath can produce (mutation handlers + file/open all run through
`EnforceSingleMemberLinkGroups`, so a clean save can't write a
singleton). Solved by adding a `--gen-nt5-fixture <path>` CLI flag
to [`src/main.cpp`](src/main.cpp): a one-shot mode that bypasses
BridgeDispatcher entirely, constructs a 2-emitter ParticleSystem
in-memory, sets `emitters[1]->linkGroup = 1` while leaving
`emitters[0]->linkGroup = 0` (the singleton state), and calls
`SaveParticleSystem` directly. The generated fixture lives at
[`web/apps/editor/tests/fixtures/nt-5-singleton.alo`](web/apps/editor/tests/fixtures/nt-5-singleton.alo)
(1754 bytes). The CLI flag stays in `main.cpp` as the regeneration
tool — if the `.alo` chunk format ever changes, re-run
`ParticleEditor.exe --gen-nt5-fixture <path>` to refresh the fixture.

The undo-round-trip test was originally meant as a vitest contract
test but was moved to Playwright when the MockBridge's
`undo/perform` was found to throw "Phase 3+ not implemented." The
host-side handler at
[`BridgeDispatcher.cpp:1405-1430`](src/host/BridgeDispatcher.cpp:1405)
DOES route the request through `UndoStack::Undo()` (returns
`{applied: true|false}`), but the comment block at lines 1421-1425
explicitly notes that "the ParticleSystem swap-and-restore lives
here — Deserialize the snapshot, hand it to the engine, fire
EmitEngineStateChanged. Today's stack stays empty so there's
nothing to apply." The snap-restore step is missing, so the test
that asserts post-undo linkGroup values would fail even though
`captureUndo()` IS wiring snapshots in. The `test.fixme(...)` marker
preserves the intent: when snap-restore lands, un-fixme + the
atomicity contract becomes loud-failing against future
refactors.

Test counts at ship: vitest 343/343 (unchanged from entry).
Playwright native: **102 passed + 27 skipped + 0 failed** (default
dist/, no env vars). C++ touched:
[`src/main.cpp`](src/main.cpp) (~50 net lines for the
`--gen-nt5-fixture` argv branch + `#include "ParticleSystem.h"`
+ `<memory>`). Tests touched:
[`web/apps/editor/tests/emitter-mutations.spec.ts`](web/apps/editor/tests/emitter-mutations.spec.ts)
(+~170 lines for the undo-round-trip fixme + load-time spec +
ESM `__dirname` shim via `import.meta.url`). Fixtures added:
[`web/apps/editor/tests/fixtures/nt-5-singleton.alo`](web/apps/editor/tests/fixtures/nt-5-singleton.alo)
(binary; regeneratable via the CLI flag).

---

### Engine-side single-member link-group enforcement () — leaving or deleting reduces a link group to 1 member; lone survivor auto-demotes to `linkGroup = 0`; legacy `.alo` files with pre-singletons self-correct on load

*2026-05-25 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

ROADMAP §1.1. Three C++ mutation paths can leave a link group
with exactly one member: `linkGroups/set-membership` when leaving a
2-member group OR joining a different group that shrinks the previous,
`linkGroups/set-membership` with `groupId: -1` and a single-id input
list (creating a new group with one member), and `emitters/delete` when
one of a 2-member group's members is deleted. Pre-, the render
layer already filtered single-member groups out of the gutter
([`computeLinkGroupBrackets:71`](web/apps/editor/src/lib/link-group-colors.ts:71)),
but the data layer still carried the orphaned `linkGroup = N`. The
Inspector's "Link Group: N" field on a de-facto-unlinked emitter
read honestly-but-confusingly. closes the data/render gap by
running a post-mutation sweep that demotes any singleton group's
lone member to `linkGroup = 0`, so the data layer matches the
rendered view end-to-end. The save format is unchanged; legacy
`.alo` files with pre-singletons self-correct on `file/open` via
a load-time sweep at the same helper. The sweep is idempotent — a
second call on an already-enforced tree is a no-op.

**How we tackled it.** New private member method
[`BridgeDispatcher::EnforceSingleMemberLinkGroups()`](src/host/BridgeDispatcher.cpp)
walks the bound `ParticleSystem`'s emitters in two passes: first
counts members per positive linkGroup via `std::map<uint32_t, int>`;
second demotes the lone member of every singleton. Null-checked
iteration matches the existing `groupId == -1` scan pattern at the
`linkGroups/set-membership` handler. Three call sites land it:
(1) after the mutation loop in `linkGroups/set-membership` (covers
ROADMAP paths 1 and 3 — same handler, different parameter shapes);
(2) after `sys->deleteEmitter(target)` + the wasSelected block in
`emitters/delete` (covers path 2); (3) immediately after
`*m_pParticleSystem = std::move(loaded)` in `file/open` (the
load-time sweep — see below). Both mutation sites already call
`captureUndo()` upstream, so the auto-demotion is captured in the
same undo snapshot as the explicit mutation — Ctrl-Z restores the
pre-mutation `linkGroup` values atomically. The load-time sweep does
NOT call `markDirty()` since the correction is normalization, not
user-driven mutation; marking dirty would force a save-prompt on
every open of a legacy file even when the user makes no further
changes. The JS-side mock at
[`mock-state.ts`](web/apps/editor/src/bridge/mock-state.ts) gains a
parallel `enforceSingleMemberLinkGroups(tree)` pure function chained
into `setLinkGroupMembership` and `deleteEmitter` returns; the mock
handlers in [`mock.ts`](web/apps/editor/src/bridge/mock.ts) need no
change because the mutation helpers themselves now return
enforce-clean trees. The mock has no `file/open` handler — load-time
parity is a host-only concern.

Test counts at ship: vitest **343 / 343** (was 338; +5 new
tests covering path 1 leave-2-member, path 1b shrink-via-join,
path 2 delete-one-of-2, regression guard for 3-member groups, and
idempotence; +1 existing test updated to reflect the path-3 contract
change at
[`bridge-contract.test.ts:732`](web/apps/editor/src/bridge/__tests__/bridge-contract.test.ts:732)
where `groupId: -1` + single id now demotes to 0 instead of
landing at the resolved positive id). Two new Playwright spec
entries in
[`emitter-mutations.spec.ts`](web/apps/editor/tests/emitter-mutations.spec.ts)
verify the same invariants end-to-end against the C++ host (leave +
delete paths; expected native count: **101 passed + 26 skipped +
0 failed** under default dist/, no env vars). MSBuild Debug + Release
x64 clean. C++ touched:
[`src/host/BridgeDispatcher.{h,cpp}`](src/host/BridgeDispatcher.h)
(~75 net lines for the new helper + three call sites + `<map>` include
+ updated handler comments). Web touched:
[`web/apps/editor/src/bridge/mock-state.ts`](web/apps/editor/src/bridge/mock-state.ts)
(+~35 lines for `enforceSingleMemberLinkGroups` + wiring into the two
mutation helpers);
[`web/apps/editor/src/bridge/__tests__/bridge-contract.test.ts`](web/apps/editor/src/bridge/__tests__/bridge-contract.test.ts)
(+~120 lines for the 5 new tests + 1 updated assertion + comment
refresh);
[`web/apps/editor/tests/emitter-mutations.spec.ts`](web/apps/editor/tests/emitter-mutations.spec.ts)
(+~120 lines for the 2 new Playwright tests).

---

### Lessons retro-doc for Phase 3 — formalized; handoff latent-bug claim retracted

*2026-05-25 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

[`tasks/lessons.md`](tasks/lessons.md) gains four new entries closing
out Phase 3's documentation hygiene. **** (DXSDK June 2010
linker-twin: `LNK2019 CreateDXGIFactory2`-class failures resolve via
`CreateDXGIFactory1` + QI, not linker-path surgery) is the linker-side
parallel to header-side pattern, with the resolution shape
explained (no per-file `<AdditionalLibraryDirectories>` exists in
MSBuild, so the isolation does not extend to the linker — the
fix lives in the call site instead). **** (spike correctness is
not transitive — audit every const/enum the spike picked against the
production workload's actual data flow) generalizes the Stage 4d.1
PREMULTIPLIED → IGNORE alpha-mode pivot into a structural rule for any
spike→production hand-off. **** (verify rendered geometry,
combined-math edition) extends CLAUDE.md's existing "verify rendered
geometry, not design intent" rule to combined math across multiple
components — Stage 5 Iter 1's displacement bug emerged because each
component's coord convention was reviewed in isolation; nobody walked
the pixel path end-to-end. Adds a 30-second pre-coding pixel walk to
the multi-component-layout checklist. **** (handoff notes carry
claims, not facts — verify against current code before any claim
enters a dispatch's plan) was surfaced during this dispatch's
pre-flight: the next-session-prompt and handoff described a "latent
projection-not-pushed bug in `ResetParameters`" at
`engine.cpp:1518`; verification revealed `ResetParameters` is now at
[`engine.cpp:1654`](src/engine.cpp:1654), ends with
`SetCamera(m_eye)`, which (at [`engine.cpp:1014`](src/engine.cpp:1014))
unconditionally pushes `SetTransform(D3DTS_PROJECTION, &m_projection)`
and has done since commit `0d352ae` (Initial import). The "latent bug"
was a phantom; handoff was updated with a "Retractions" sub-section
citing.

**How we tackled it.** Three of the four lessons (,,)
were distillation passes from the existing CHANGELOG Stage 4 and
Stage 5 "Issues encountered" prose into the canonical lessons.md
**Rule / Trigger / How to apply / Source incident / Cross-reference**
shape (set by through). No investigation needed for those
three — the source incidents were already richly documented at ship
time; the work was identifying the structural rule and rephrasing the
incident as a worked example. **was different** — the carry-
forward claim it documents only became visible during this dispatch's
pre-flight, where reading [`engine.cpp:1654`](src/engine.cpp:1654)
showed `ResetParameters` already pushed the projection via its
existing `SetCamera(m_eye)` tail. `git log -S "SetCamera(m_eye)" --
src/engine.cpp` dated that line to commit `0d352ae` (Initial import)
— evidence the claim was wrong from the start, not a stale freshness
problem. The prior session's author appears to have reasoned by
analogy from the genuine Stage 5 `SetSceneViewport` bug to a parallel
in `ResetParameters` that doesn't hold (because `ResetParameters`
calls `SetCamera`, which `SetSceneViewport` doesn't). handoff.md was
restructured: item 2 of "Known follow-ups (out of scope for Stage 5)"
was removed; remaining items renumbered 3/4/5 → 1/2/3; new
"Resolved follow-ups" sub-section captures the lessons-retro-doc
ship; new "Retractions" sub-section captures the structural finding
with a pointer at. The two existing CHANGELOG entries (Stage 4
+ Stage 5) were not edited — their existing "Issues encountered" prose
remains the long-form source the new lessons distill from.

C++ touched: none. Tests touched: none. Docs touched:
[`tasks/lessons.md`](tasks/lessons.md) (+~440 lines for,
following the existing entry format), [`tasks/handoff.md`](tasks/handoff.md)
("Known follow-ups" restructured + new "Resolved follow-ups" + new
"Retractions" sub-sections; Phase 3 closing notes line updated to
reflect the post-retro-doc state), [`tasks/todo.md`](tasks/todo.md)
(fresh dispatch plan; the prior Phase 3 todo.md archived to
[`tasks/todo-mt-11-phase-3-archive.md`](tasks/todo-mt-11-phase-3-archive.md)).

---

### Scene-rect transform on the engine visual (Phase 3 Stage 5) — pane resize and window resize now cleanly reveal more of the scene rather than distorting existing content; engine viewport scoped to scene-rect with per-pixel-FoV projection that keeps angular extent per pixel constant across resizes

*2026-05-25 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Under composition mode (Stage 4 ship), Stage 5 wires the React-side `layout/scene-rect` bridge dispatch into both [`Compositor::SetEngineVisualTransform`](src/host/Compositor.cpp) (clips the DComp engine visual to the scene-rect sub-region of the host client) and [`Engine::SetSceneViewport`](src/engine.cpp) (constrains the engine's scene-pass rendering to the scene-rect with a per-pixel-FoV projection). User-observable result: chrome panels stop bleeding engine pixels (the panels show their own backgrounds where they used to leak engine clear-color), and pane drag / window resize "cleanly reveals more of the scene" — existing world content keeps the same pixel position and scale while new world content appears at the widened scene-rect edges. The Variant **B-γ** design with per-pixel-FoV-vs-current-RT reference (`fovY = 45° × sceneH / RT_H`) bounds the engine's render cost: scene-rect is always ≤ engine RT, so `fovY ≤ 45°` always — engine renders at-or-LESS world than pre-Stage-5, not more. Maximized idle composite rate ~70 fps at 3440×1440 (vs Stage 4's 79.1 mean — within parity, expected slight reduction from the deferred-clip mechanism's extra SetClip/Commit per frame). Playwright native HWND baseline (default dist/, no env vars): **99 passed + 26 skipped + 0 failed** — Stage 5's wiring is composition-mode-only via the `m_dcompCompositor != nullptr` gate, byte-identical to pre-Stage-5 on canvas-jpeg / arch-A paths. Composition-mode native: **122 passed + 3 skipped + 0 failed** — adds 4 new dxgi-scene-rect assertions on top of Stage 4's 118.

**How we tackled it.** Nine tasks (T1-T9) per the sub-plan, with T6 (composition-mode smoke) producing four user-driven correction iterations that reshaped T1's and T3's design before the final state stabilized. (1) **T1 — `Compositor::SetEngineVisualTransform`** at [`src/host/Compositor.{h,cpp}`](src/host/Compositor.h) adds the DComp visual-tree side of the wiring. After the T6 coord-space correction (see Issues below), the final shape is `SetOffsetX(0) + SetOffsetY(0) + SetClip({x, y, x+w, y+h})` — the visual's local-coord origin equals parent (root visual) origin = host-client coords, so SetClip with absolute host-client coords directly carves the visible region from the (full-RT-sized) swapchain. Engine renders into scene-rect pixels of its RT; DComp clip exposes exactly those pixels at the same screen coords. Also adds the **deferred-clip mechanism** (T6 follow-up): `SetEngineVisualTransform` with default `immediate=false` queues the transform on Impl::pending* fields; [`CompositeEngineFrame`](src/host/Compositor.cpp)'s tail applies it after `Present1` so swapchain content + DComp clip arrive at the same DWM cycle. Boot-time seed callers pass `immediate=true` to bypass the queue. (2) **T2 — LayoutBroker DComp seam** at [`src/host/LayoutBroker.{h,cpp}`](src/host/LayoutBroker.h) renames the existing `m_compositor` field to `m_alphaCompositor` (disambiguating from the new DComp-tree compositor pointer) + adds `SetCompositor(host::Compositor*)` + `GetSceneRect(int&, int&, int&, int&)` accessor. (3) **T3 — `Engine::SetSceneViewport`** at [`src/engine.{h,cpp}`](src/engine.h) (Variant **B-γ**) adds a public method that stashes the scene-rect, recomputes `m_projection` at per-pixel-FoV with reference = current `BackBufferHeight` (so `fovY = 45° × sceneH / RT_H`, never exceeds 45°), pushes the new projection to the device via `SetTransform(D3DTS_PROJECTION, ...)`, and recomputes `m_viewProjection = m_view * m_projection` for shader-effect consumers. `Engine::Render`'s scene pass scopes `SetViewport` to the scene-rect AFTER the existing full-RT `Clear` (the D12 ordering rule from the sub-plan — Clear runs at default viewport so `m_pSceneTexture` outside scene-rect is filled with engine clear color every frame, eliminating post-process bleed across the scene-rect boundary). Post-process passes (bloom, distort) restore the cached viewport so they still operate at full-RT-sized intermediates. `Engine::Reset`'s R8 re-apply at end of `Reset()` re-fires `SetSceneViewport` with the cached state so the per-pixel-FoV projection survives the device reset (Window resize → ResetParameters rebuilds projection at full-RT-aspect, R8 immediately overwrites with the scene-rect projection). (4) **T4 — LayoutBroker scene-rect wiring** routes `BridgeDispatcher`'s `layout/scene-rect` dispatch into all three consumers in order: `m_alphaCompositor->SetSceneRect` (legacy popup band-mask, with popup-origin translation, unchanged), `m_engine->SetSceneViewport(x, y, w, h)` (engine state + projection), `m_dcompCompositor->SetEngineVisualTransform(x, y, w, h)` (queue DComp clip). Both new paths gated on `m_dcompCompositor != nullptr` per the sub-plan's R9 mitigation (c) — keeps canvas-jpeg / arch-A transports byte-identical. (5) **T5 — HostWindow attach + seed + teardown** in `OnCompositionControllerReady` after `AttachEngineVisual` succeeds: `layout.SetCompositor(m_compositor.get())` + initial seed via `SetEngineVisualTransform(..., immediate=true)` + `engine->SetSceneViewport(...)` with full-client values (so per-pixel-FoV produces `fovY = 45°`, matching pre-Stage-5 at attach time). WM_DESTROY teardown clears `layout.SetCompositor(nullptr)` before `m_compositor.reset()` so a late SetSceneRect dispatch slipping through the message pump shutdown can't dereference a freed Compositor. Symmetric clear in the `WM_APP_COMPOSITION_FALLBACK` path. (6) **T6 — Composition-mode smoke** at [`tasks/stage-5-smoke-result.md`](tasks/stage-5-smoke-result.md) — the user-driven gate. Four bug-iteration corrections shaped the final design, all documented in Issues below. (7) **T7 — Playwright spec** at [`web/apps/editor/tests/dxgi-scene-rect.spec.ts`](web/apps/editor/tests/dxgi-scene-rect.spec.ts) adds 4 log-evidence assertions: boot seed produced a non-degenerate clip; single `layout/scene-rect` dispatch produces a matching `[COMP-engine-transform] clip=(L,T,R,B)` line; three sequential dispatches produce three transforms in order; no `[COMP-engine-fail]` lines emitted. Mirrors `dxgi-transport.spec.ts`'s skip-pattern + CDP bridge-dispatch pattern. (8) **T8 — Docs** (this entry, handoff refresh, todo.md). (9) **T9 — FF + push to origin/integration-branch** closes the dispatch.

**Issues encountered and resolutions.** T6 smoke surfaced four independent bugs in the T1-T5 design as shipped, each requiring a correction iteration. *Iter 1: displacement bug.* User screenshot showed the engine scene rendered in the bottom-right corner of the scene-rect quadrant with engine clear color filling the top/left areas. Root cause: the sub-plan's T1 design (Compositor uses `SetOffset(sceneX, sceneY) + SetClip({0, 0, w, h})` in LOCAL coords post-offset) and T3 design (engine renders at scene-rect viewport in RT) were internally inconsistent — both followed the sub-plan as written, but combined produced a double-offset (engine pixels at RT[sceneX..sceneX+w, sceneY..sceneY+h] → DComp visual offset SHIFTS the whole swapchain by (sceneX, sceneY) → visible pixels are local[0..w, 0..h] which corresponds to RT top-left = engine clear color). Textbook example of CLAUDE.md's "verify rendered geometry, not design intent" — neither pre-handoff code review nor my own mental walkthrough caught the math. Fix: `SetOffsetX/Y(0, 0) + SetClip with ABSOLUTE host-client coords {x, y, x+w, y+h}` — visual local-coord space equals parent coord space, clip directly carves the visible region. Documented as candidate for `tasks/lessons.md`. *Iter 2: aspect distortion on resize.* User: "the aspect ratio of the viewport changes as i resize panes or the app window itself, which makes it look like a distortion occurring as i resize. ... instead, the panes and the window should cleanly reveal more of the scene." Root cause: T3's initial implementation used fixed `fovY=45°` with `aspect = sceneW/sceneH` — wider viewport widened horizontal FoV → each pixel covered less angular extent → existing world objects' pixel positions shifted toward center (perceived as shrinking/distortion). First fix attempt used per-pixel-FoV with a reference captured at the FIRST `SetSceneViewport` call (typically a small initial scene-rect at boot, e.g. 495 pixels tall) → at maximized scene-rect H ~1200, `fovY = 45° × 1200/495 ≈ 109°` (insane horizontal FoV of ~148°) → engine rendered ~2× the world per frame → sub-30 FPS at maximized 3440×1440. Second fix: per-pixel-FoV with reference = CURRENT engine RT height. Since scene-rect H is always ≤ RT H, `fovY ≤ 45°` always — engine renders LESS world than pre-Stage-5, not more. Net perf at maximized improved to ~70 fps. *Iter 3: blue bar at trailing edge during fast drag.* User: "the resize caused an issue with the blue bars again. ... when i released, the preview caught up and filled the space." Root cause: DComp clip widened immediately on Commit but engine rendering of the new viewport region lagged by one render-pump iteration. First fix attempt: sync render callback — LayoutBroker drove `RenderD3D9` synchronously from the bridge dispatch path so engine renderered with new state before bridge returned. ELIMINATED the lag but tanked FPS at maximized because engine.Render fires 2× per drag tick (sync from dispatch + natural from message pump) — sub-30 fps perf hit. REVERTED entirely. Second fix (kept): deferred clip in Compositor. `SetEngineVisualTransform` with default `immediate=false` queues the transform on `Impl::pending*` fields; `CompositeEngineFrame`'s tail applies it after `Present1` so swapchain content + DComp clip arrive at the same DWM cycle. One-frame residual lag at the leading edge during very fast drags is acceptable (user feedback: "smaller/tighter delay" — workable). *Iter 4: aspect snaps on click after resize.* User: "the aspect ratio changes with it. but then when i click in the preview after the resize is done, it snaps to correct the aspect ratio." Root cause: `Engine::SetSceneViewport` updated `m_projection` (member variable) but never called `m_pDevice->SetTransform(D3DTS_PROJECTION, &m_projection)` to push the new matrix to the device. Device retained whatever projection `SetCamera` last pushed (the only `SetTransform(PROJECTION)` site in the entire engine). Click in viewport triggered a camera op → `SetCamera` fires → `SetTransform(PROJECTION)` finally pushes the latest `m_projection` → "snap." Latent bug also exists in pre-Stage-5 `ResetParameters` (window resize rebuilds `m_projection` in member but doesn't push); nobody noticed pre-Stage-5 because window resize was always immediately followed by camera interaction. Fix: both `SetSceneViewport` branches (active + clearing) explicitly call `m_pDevice->SetTransform(D3DTS_PROJECTION, &m_projection)` and recompute `m_viewProjection = m_view * m_projection` for shader consumers. *Lessons-learned pattern.* The four iterations all stem from a single class of failure: the sub-plan described independent components (Compositor coord-space, Engine projection, render timing, device-state push) but didn't verify the COMBINED math against rendered geometry. CLAUDE.md's "Verify rendered geometry, not design intent" rule applies; an explicit mental walk-through with pixel math at sub-plan time would have caught Iter 1 (and possibly Iter 4). Worth a retro-doc as a lesson alongside.

Test counts at ship: vitest **338 / 338** unchanged. Playwright native HWND baseline (default dist/, no env vars): **99 passed + 26 skipped + 0 failed, 2.0m** — 4 new dxgi-scene-rect tests skip cleanly via the env-var gate, no regressions. Playwright native composition mode (composition-built dist/, env-var pair set): **122 passed + 3 skipped + 0 failed, 2.6m** — +4 dxgi-scene-rect tests on top of Stage 4's 118 + 3 skipped baseline. MSBuild Debug + Release x64 clean. C++ touched: [`src/host/Compositor.{h,cpp}`](src/host/Compositor.h) (~315 net lines for SetEngineVisualTransform + deferred-clip mechanism + ApplyTransform helper + Impl::pending state); [`src/host/LayoutBroker.{h,cpp}`](src/host/LayoutBroker.h) (~80 net lines for the new SetCompositor + GetSceneRect surface + the SetSceneRect fan-out to Engine + Compositor under the composition-mode gate); [`src/host/HostWindow.cpp`](src/host/HostWindow.cpp) (~80 net lines for the attach-time seed + WM_DESTROY/fallback teardown); [`src/engine.{h,cpp}`](src/engine.h) (~230 net lines for SetSceneViewport + GetSceneViewport + the Render-pass viewport scoping with D12 ordering + Reset's R8 re-apply + per-pixel-FoV projection + SetTransform+viewProjection push). Tests touched: new [`tests/dxgi-scene-rect.spec.ts`](web/apps/editor/tests/dxgi-scene-rect.spec.ts) (~210 lines); [`scripts/run-native-tests.mjs`](web/apps/editor/scripts/run-native-tests.mjs) (+5 lines new spec registration). Planning + smoke: [`tasks/dxgi-stage-5-scene-rect-transform.md`](tasks/dxgi-stage-5-scene-rect-transform.md) (the sub-plan, ~1040 lines including the post-user-check-in revision to Variant B-γ); [`tasks/stage-5-smoke-result.md`](tasks/stage-5-smoke-result.md) (T6 smoke evidence with the per-iteration bug log).

---

### DXGI composition wiring (Phase 3 Stage 4) — engine pixels reach the screen via D3D9Ex shared texture → D3D11 alias → DXGI composition swapchain → DComp engine visual UNDER the WebView2 visual, fully interactive, resize-robust, alpha-correct

*2026-05-25 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Under `ALO_WEBVIEW2_HOSTING=composition` + `ALO_VIEWPORT_TRANSPORT=canvas-jpeg` + a `dist/` built with the matching `VITE_*` env-var pair, the engine's particle viewport is now composited via DXGI instead of the legacy `WS_EX_LAYERED` popup. Engine renders into the AlphaCompositor's shared-handle D3D9Ex texture (Stage 2 infrastructure); a new D3D11 device opens that texture via `OpenSharedResource`; per-frame `CopyResource` into a `CreateSwapChainForComposition` back buffer + `Present1` lands the pixels in a DComp engine visual inserted BEHIND the Stage-3 WebView2 visual. The headline Stage 0 spike's 0.30 ms total frame-transport at 3440×1440 reproduces in production: measured **mean engine FPS 79.1** under the dxgi-perf spec, well above the 30 FPS regression floor — vs the canvas-jpeg readback path's ~40-50 FPS at the same resolution captured during Phase 2 perf investigation. Default new-UI path (env vars unset) is byte-identical to today, **Playwright native HWND baseline 99/99 PASS** unchanged. Under composition env vars + composition-built dist/, **118 PASS + 3 skipped + 0 failed** including 10 new dxgi-* assertions covering attach, per-frame composite, handle-stability, resize-stress, and FPS regression.

**How we tackled it.** Seven sub-stages, six shipped + one deferred (per the sub-plan's 2-load-bearing-gate cadence at 4b + 4f). (1) **4a — Skeleton** at [`src/host/Compositor.{h,cpp}`](src/host/Compositor.h) adds three public methods (`AttachEngineVisual`, `CompositeEngineFrame`, `RefreshEngineSharedHandle`) as stubs + two on Engine (`IssueEndFrameQuery` / `WaitEndFrameQuery`) for the cross-device GPU sync. Engine owns an `IDirect3DQuery9` event query (lazy-created on first Issue, released in `Engine::Reset` before `m_pDevice->Reset` per D3D9 query invalidation semantics — queries aren't `D3DPOOL_*` but DO get invalidated by device Reset). Host's per-frame loop calls Issue → Wait between `engine->Render()` and `Compositor::CompositeEngineFrame()` under composition mode only (sub-plan §3.3 path b — zero overhead on non-composition paths). (2) **4b — Real AttachEngineVisual** adds the load-bearing GPU bridge to Compositor::Impl. D3D11 device via `D3D_DRIVER_TYPE_HARDWARE` + `D3D11_CREATE_DEVICE_BGRA_SUPPORT` + DEBUG flag in Debug builds (fallback if SDK layers missing per spike pattern). Multi-GPU LUID guard via `IDXGIDevice → GetAdapter → GetDesc`, compared against `Engine::GetAdapterLuid()` (new `IDirect3D9Ex::GetAdapterLUID` accessor on the engine side); mismatch returns `DXGI_ERROR_GRAPHICS_VIDPN_SOURCE_IN_USE`, skips engine attach, composition mode continues with chrome-only viewport (sub-plan §3.8 / D7: engine-attach failures explicitly do NOT chain into F8's `WM_APP_COMPOSITION_FALLBACK` — that mechanism is reserved for chrome-itself-broken failures). `OpenSharedResource` on the engine's shared HANDLE, then `CreateSwapChainForComposition` with `DXGI_FORMAT_B8G8R8A8_UNORM` + `FLIP_SEQUENTIAL` + 2-buffer + `DXGI_ALPHA_MODE_IGNORE` (originally `PREMULTIPLIED` per spike — see 4d.1 below for the alpha-mode pivot). Engine visual inserted via `AddVisual(engine, TRUE, nullptr)` — the MSDN-naming inversion at / dxgi_spike.cpp:488 places the new visual BEHIND all siblings (counterintuitively `insertAbove=TRUE + NULL ref` = "beginning of children list" = behind). (3) **4c — Real CompositeEngineFrame** runs the per-frame `CopyResource(backBuffer, sharedTexAlias)` + `Present1(0, 0, &emptyParams)`. Two 1 Hz throttled diagnostics: `[COMP-engine-frame] composite n=N` for liveness, `[COMP-engine-handle-hash] handle=PTR sharedTex=PTR backBuffer=PTR texSize=WxH` for resource-identity stability (catches the spike's dxgi_spike.cpp:355-357 documented "OpenSharedResource on wrong handle silently returns different texture" failure mode). Spike's claim that FLIP_SEQUENTIAL swapchains keep the same back-buffer COM identity across frames empirically confirmed in production smoke. (4) **4c.1 — ViewportSlot composition opt-out** at [`web/apps/editor/src/components/ViewportSlot.tsx`](web/apps/editor/src/components/ViewportSlot.tsx). Build-time `VITE_WEBVIEW2_HOSTING` env var mirrors the runtime `ALO_WEBVIEW2_HOSTING`; when set to `composition` the `<img>` element's `viewport/frame-ready` subscription is skipped — the `<img>` stays empty and transparent, DXGI engine pixels show through the WebView2 visual where it's transparent. The `<canvas>` overlay's input listeners stay active either way (Phase 2's `viewport/input` bridge is still the engine input pathway under composition mode). Without this opt-out the canvas-jpeg `<img>` paints JPEG-decoded engine frames OVER the DXGI source, occluding Stage 4's whole GPU pipeline. (5) **4d — Lazy resize-handle re-open** in `CompositeEngineFrame`. AlphaCompositor::Resize invalidates the shared HANDLE on every host-window resize. Pre-4d, the cached D3D11 alias kept pointing at the released old texture and every CopyResource read garbage. CompositeEngineFrame's signature gains a `HANDLE currentSharedHandle` param (caller passes `engine->GetSharedTextureHandle()` each frame); on mismatch with cached, `RefreshEngineSharedHandle` drops the old D3D11 alias, re-opens via `OpenSharedResource`, reads authoritative width/height from the new texture descriptor, and `IDXGISwapChain1::ResizeBuffers` if dimensions changed. DComp engineVisual's `SetContent(swapchain)` reference stays valid through ResizeBuffers (DXGI documents this — visual identity is on the swapchain, not the back buffers). Single-frame stutter at resize boundary, steady-state resumes. (6) **4d.1 — DXGI ALPHA_MODE_IGNORE** at the swapchain desc. The spike used `ALPHA_MODE_PREMULTIPLIED` (its workload was `D3DClear` to solid color, alpha was clean). Production engine's particle blend states leave the RT's alpha channel in arbitrary states the engine never cared about — legacy arch-A's UpdateLayeredWindow uses the popup's STAMPED alpha (from AlphaCompositor::Composite), not the RT's own alpha. Under PREMULTIPLIED, DComp's compositing math darkened the output where alpha was less than full — visible as "additive fire sprites overlap smoke with dark/black backgrounds." IGNORE tells DComp the surface is fully opaque; chrome composites on top where opaque, transparent regions show full-opacity engine. Legacy parity. (7) **4f — Playwright DXGI specs + harness/host hardening** at [`tests/dxgi-{transport,resize-stress,perf}.spec.ts`](web/apps/editor/tests/dxgi-transport.spec.ts) (10 new tests). Log-evidence approach: specs read `%LOCALAPPDATA%\AloParticleEditor\host.log` via Node `fs` and grep for `[COMP-engine-*]` patterns. Sub-plan §6 4f #2 dxgi-vs-jpeg SSIM was deferred — Playwright's DOM-only screenshots can't see DXGI engine pixels under composition; manual visual smoke (Stage 4c user-driven) is the irreducible visual gate.

**Issues encountered and resolutions.** *4b's first build failed with `LNK2019 unresolved external symbol CreateDXGIFactory2`.* This is **twin on the linker side**: `ParticleEditor.vcxproj` puts `$(DXSDK_DIR)Lib\x64` FIRST on `AdditionalLibraryDirectories` (for `d3dx9.lib`), which shadows the Win10 SDK's modern `dxgi.lib`. DXSDK June 2010 ships a pre-Windows-8 `dxgi.lib` that lacks `CreateDXGIFactory2`. Spike sidesteps this entirely because `dxgi_spike.vcxproj` doesn't reference DXSDK. There's no per-file `<AdditionalLibraryDirectories>` in MSBuild (link is per-project), so the isolation pattern doesn't extend to the linker. Surgical fix: use `CreateDXGIFactory1` (DXSDK-era API since Win7) and QI to `IDXGIFactory2` — uses only DXSDK-compatible APIs at link time, gates DXGI 1.2 capability detection at the QI step (if QI fails, `CreateSwapChainForComposition` wouldn't work either, so the QI is a single chokepoint for the entire DXGI 1.2 requirement). *4c smoke surfaced an architectural gap I'd missed in the sub-plan §1.* Initial user-driven visual smoke showed the Stage 3b placeholder text + zero interaction. Diagnosis: the `dist/` was built without `VITE_VIEWPORT_TRANSPORT=canvas-jpeg`, so `archCEnabled = false` in the renderer → `ViewportSlot.tsx` renders the `<span>D3D9 viewport</span>` placeholder, NOT the `<canvas>`. The placeholder is opaque DOM content, occluding the DXGI engine visual behind it; no canvas in DOM = no Phase 2 input bridge = no engine input. Sub-plan §1 In Scope had said "FramePublisher publishes JPEGs to a renderer-side canvas nobody reads" — but Phase 2's `ViewportSlot.tsx` DOES read unconditionally when `VITE_VIEWPORT_TRANSPORT=canvas-jpeg`. The composition-mode opt-out (4c.1) was the missing piece. *4d surfaced the actual user-visible bug 4c left behind.* User's smoke after 4c.1 ship: "the viewport appears to be a static image. not interactable. i can't spawn particles" + "when i resized the entire app window, it froze the viewport." First symptom was the dist/build-mode mismatch (4c.1); second symptom was 4d's territory exactly. Sub-plan §3.5 + risk #2 + D4 had anticipated the resize-handle invalidation as a Stage 4d deferral; user's smoke surfaced it as a concrete bug to fix before declaring Stage 4 done. The lazy per-frame handle compare (D4) implementation took the failing case (static frozen viewport) to "single-frame stutter at resize boundary, steady-state resumes." *4d.1 emerged from user-surfaced visual artifact during 4d retest.* User: "when my additive fire sprites are rendering on top of my smoke particles, i see black background that should not be there. this does not occur in legacy." Root cause: spike's `DXGI_ALPHA_MODE_PREMULTIPLIED` choice was correct for the spike's `D3DClear()` workload but wrong for the production engine's particle blend states. Engine never cared about its RT alpha channel (legacy arch-A used the STAMPED popup alpha, not the RT alpha); DComp's PREMULTIPLIED interpretation of an arbitrary alpha darkened the output. IGNORE matches legacy semantics. **-pattern lesson** to retro-document: when porting a spike to production, audit every const/enum the spike picked against the production workload's actual data flow — don't assume the spike's choices are correct for production just because the spike was a passing reference. *4f smoke surfaced a real footgun in the test harness.* `run-native-tests.mjs` spawned ParticleEditor.exe with `stdio: "inherit"`. The host writes per-frame `[ArchC] frame=N` diagnostics to stderr via `fprintf`. When the user clicked in the inherited console window, Windows entered QuickEdit/Mark mode which **blocked the stderr buffer**; the next per-frame `fprintf` hung the host thread, Playwright timed out, ALL in-flight specs cascade-failed. Fix: `stdio: ["ignore", "ignore", "ignore"]` + `windowsHide: true`. All host diagnostics are duplicated to `host.log` via the `Log()` macro so test diagnostics lose nothing. *Concurrent reader unblock.* The dxgi-transport spec needs to read host.log via Node `fs.readFileSync` to assert `[COMP-engine-*]` patterns. The host opens the log via `_wfopen_s(..., L"w")` which uses exclusive default share-mode (`_SH_DENYRW`); concurrent readers got `EBUSY`. Fix: switch to `_wfsopen(..., L"w", _SH_DENYNO)`. Host is the only writer; deny-no is safe. *Stage 4f surfaced a pre-existing Phase 2 test instrumentation fault.* `canvas-architecture.spec.ts`'s `installBridgeProxy` wraps `window.bridge.request` (TestHostBridge under `--test-host`) but ViewportSlot dispatches via its `bridge` prop (NativeBridge from App.tsx's useMemo) — different objects, proxy never intercepts. The spec was always silently SKIPPING in HWND baseline because `archCEnabled = false` under default-built `dist/`. Composition-built `dist/` makes `archCEnabled = true` → spec runs → finds the proxy mismatch → fails. The CONTRACT the tests encode (canvas pointermove + Shift keydown → viewport/input bridge → engine input) DOES work in production — verified by user-driven Shift+click spawn during 4c. Failure is purely instrumentation. Two tests marked `test.fixme` with detailed FIXME comment documenting the-class issue + three proper-fix approaches.

Test counts at ship: vitest **338 / 338** unchanged. Playwright native HWND baseline (default dist/, no env vars): **99 passed + 22 skipped, 33.8s** — all new dxgi-* specs auto-skip cleanly. Playwright native composition mode (composition-built dist/, env-var pair set): **118 passed + 3 skipped + 0 failed, 2.6m** — 1 composition-hosting wheel self-skip (no emitter selected at test time) + 2 canvas-architecture fixme'd tests (instrumentation issue documented above). MSBuild Debug + Release x64 clean. C++ touched: `src/host/Compositor.{h,cpp}` (~470 net lines for the entire D3D11/DXGI/DComp engine-visual bridge + lazy resize re-open + diagnostics), `src/host/HostWindow.cpp` (~50 net lines for OnCompositionControllerReady's AttachEngineVisual call + the per-frame Issue/Wait/Composite block + `_wfsopen` share-mode fix + `<share.h>` include), `src/engine.{h,cpp}` (~120 lines for IssueEndFrameQuery + WaitEndFrameQuery + GetAdapterLuid + the SAFE_RELEASE in Engine::Reset). Web touched: `web/apps/editor/src/components/ViewportSlot.tsx` (+43 lines for isCompositionMode + the gated frame-ready subscription). Tests touched: 3 new `tests/dxgi-{transport,resize-stress,perf}.spec.ts` (~600 lines total, 10 new assertions), `tests/canvas-architecture.spec.ts` (2 test.fixme markers + FIXME comments), `web/apps/editor/scripts/run-native-tests.mjs` (+5 lines new spec registration + 12 lines `stdio:"ignore"` + `windowsHide:true` rationale). Planning + lessons: `tasks/dxgi-stage-4-composition-wiring.md` (the sub-plan; ~960 lines including post-FF refinements + D7 decision + §3.8 F8-fallback-interaction design), `tasks/stage-4b-smoke-result.md` + `tasks/stage-4c-smoke-result.md` (log-evidence smoke documents). Sub-stages 4e (first-frame ClearRenderTargetView guard) DEFERRED — not observed as a problem during user-driven smoke; ship-if-surfaces.

---

### branch CI + native-spec allowlist guard — `integration-branch` and `session-branches` pushes now gate web build / Vitest / x64 C++, and a Vitest test fails any time `tests/*.spec.ts` and `scripts/run-native-tests.mjs` drift out of sync

*2026-05-24 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

The legacy [`build.yml`](.github/workflows/build.yml) workflow only triggers on `master` pushes and only runs MSBuild — fine for the historic C++ editor, blind to the React / WebView2 stack the branch added. New [`integration-branch.yml`](.github/workflows/integration-branch.yml) workflow triggers on every push to `integration-branch` and every per-session `session-branches` branch, plus PRs targeting `integration-branch`, with two jobs: a `web` job on `ubuntu-latest` that runs `pnpm install --frozen-lockfile` → `pnpm lint` (tsc --noEmit) → `pnpm build` → `pnpm test` (Vitest, currently **338 / 338**), and a `cpp` job on `windows-latest` that runs MSBuild for both Debug|x64 and Release|x64 (no x86 — is x64-only). Native Playwright (`pnpm test:native`) stays manually triggered via `workflow_dispatch` because each run launches the editor exe + WebView2 + CDP for several minutes and needs the DirectX SDK installed for the underlying x64 build. The new [`web/apps/editor/src/__tests__/native-spec-allowlist.test.ts`](web/apps/editor/src/__tests__/native-spec-allowlist.test.ts) Vitest spec diffs `tests/*.spec.ts` on disk against the hand-curated array inside [`run-native-tests.mjs`](web/apps/editor/scripts/run-native-tests.mjs) — any spec that exists on disk but is not in the harness (and not explicitly waived in `INTENTIONALLY_EXCLUDED`) fails the suite, as does any stale or phantom entry. Master CI is untouched: zero blast radius on the stable branch.

**How we tackled it.** Separate workflow file rather than extending [`build.yml`](.github/workflows/build.yml), so the master CI's existing matrix (Debug/Release × x86/x64) keeps its semantics and the jobs can have their own trigger set + working directory + tool installs without conditionals inside a shared workflow. Web job pins `pnpm@9` (matches the repo's `engines.pnpm`) and `node@20` (matches `engines.node`), uses pnpm's lockfile-rooted cache, and runs every step from `working-directory: web` so the workflow's YAML reflects what a developer types locally. The allowlist guard sits in Vitest rather than as a standalone Node script because the Vitest include pattern `src/**/__tests__/**` is content-agnostic — a "harness check" runs alongside component tests without adding any pretest scripts or CI plumbing. Three assertions: missing-from-harness (the headline check), stale-in-harness (harness lists a spec no longer on disk), and phantom-in-exclusion (`INTENTIONALLY_EXCLUDED` references a spec that doesn't exist). The harness is parsed by regex over the file contents (`/["']tests\/([\w.-]+\.spec\.ts)["']/g`) — robust to formatting, doesn't try to parse JS, and matches both single- and double-quoted entries. `INTENTIONALLY_EXCLUDED` starts empty by design so the first run becomes a forcing function: every unwaived spec must be classified.

**Issues encountered and resolutions.** *The guard's first run surfaced a real, latent gap.* [`canvas-architecture.spec.ts`](web/apps/editor/tests/canvas-architecture.spec.ts) exists since Phase 2 but was never added to [`run-native-tests.mjs`](web/apps/editor/scripts/run-native-tests.mjs)'s spec array — silently skipped by every CI run since. The spec self-skips when `ALO_VIEWPORT_TRANSPORT != "canvas-jpeg"`, so adding it to the allowlist is byte-identical under the default env (skip path) and wakes up the moment canvas-jpeg becomes default at Phase 4. Fix: add to the harness array next to the existing composition-hosting entry, with a comment block matching that entry's pattern (skip-condition + phase rationale). Without the guard the first canvas-jpeg default-flip would have been the run where someone noticed. *YAML strict-validation was not runnable in the local sandbox.* Neither `pyyaml` nor `js-yaml` were installed in the agent's environment, so the workflow's structural correctness wasn't verified locally. Mitigation: mirror the structure of [`build.yml`](.github/workflows/build.yml) line-by-line for the `cpp` job (same MSBuild + DXSDK pattern), use standard `actions/setup-node` + `pnpm/action-setup` invocations on the `web` job, and accept that GitHub's parser will surface any indentation slip on the first push. *Audited externally before drafting.* This work was triggered by a ChatGPT audit of the branch that flagged the master-only CI as the cheapest stabilization win. Several of the audit's other recommendations (split BridgeDispatcher, formalize host state ownership, retire experimental viewport architectures) are deliberately out of scope here — they need to wait for Phase 3 Stage 4 (DXGI composition wiring) to finish first, otherwise we trade stability for organization mid-flight.

Test count: vitest **335 → 338** (+3 from the allowlist guard: missing-from-harness, stale-in-harness, phantom-in-exclusion). Playwright native: unchanged in HWND baseline, +1 spec now reachable in composition + canvas-jpeg modes. C++ untouched. CI files touched: new [`.github/workflows/integration-branch.yml`](.github/workflows/integration-branch.yml) (~65 lines). Tests / harness touched: new [`web/apps/editor/src/__tests__/native-spec-allowlist.test.ts`](web/apps/editor/src/__tests__/native-spec-allowlist.test.ts) (~85 lines); [`web/apps/editor/scripts/run-native-tests.mjs`](web/apps/editor/scripts/run-native-tests.mjs) (+7 lines for the canvas-architecture entry + skip-condition comment block).

---

### WebView2 composition hosting migration (Phase 3 Stage 3) — `ALO_WEBVIEW2_HOSTING=composition` swaps WebView2 from HWND mode to a host-owned DirectComposition visual tree,-class failure mode cleared, 4th-attempt success

*2026-05-22 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

The new env var `ALO_WEBVIEW2_HOSTING=composition` (paired with the existing `ALO_VIEWPORT_TRANSPORT=canvas-jpeg`) switches WebView2 from `CreateCoreWebView2Controller` (HWND mode) to `CreateCoreWebView2CompositionController` (composition mode), with a new `host::Compositor` class owning the DirectComposition V1 visual tree that WebView2's `RootVisualTarget` plugs into. The default new-UI path (env var unset) is byte-identical to today — Playwright native **99/99 PASS** in HWND mode as the baseline. With the env var set, native runs at **106 passed + 1 self-skip** including 7 new composition-mode-specific specs at [`composition-hosting.spec.ts`](web/apps/editor/tests/composition-hosting.spec.ts). This is the **4th attempt** at WebView2 visual hosting on this codebase — v1/v2/v3 each produced opaque-white output despite every API logging `S_OK`. This attempt cleared the failure mode (screenshot evidence at [`tasks/stage-3b-smoke-screenshot.png`](tasks/stage-3b-smoke-screenshot.png)) by mirroring the Stage 0 spike's known-good topology exactly: V1 `IDCompositionDevice` via the V2 factory function, deferred `CreateTargetForHwnd` inside the composition-controller completion callback, `AddVisual(insertAbove=FALSE, ref=nullptr)` for the counterintuitive "in front of all siblings" ordering, no `WS_EX_LAYERED` on the host HWND. Stage 3 also wires every adjacent surface the chrome needs under composition: real OS mouse forwarding via `SendMouseInput`, cursor sync via `add_CursorChanged` + `WM_SETCURSOR`, DPI via `put_RasterizationScale` + `WM_DPICHANGED`, keyboard focus transfer via `MoveFocus` (the actual fix for the keyboard story — the SDK doesn't expose `SendKeyboardInput` at all, see).

**How we tackled it.** Seven sub-stages, each a separate revertible commit per the sub-plan's 5-load-bearing-gate cadence. (1) **3a — `host::Compositor` skeleton** at [`src/host/Compositor.{h,cpp}`](src/host/Compositor.h) ports the working topology from [`src/host/spike/dxgi_spike.cpp`](src/host/spike/dxgi_spike.cpp) into a pImpl class. The pImpl idiom isn't for ABI here — it's for include-path isolation. ParticleEditor.vcxproj puts `$(DXSDK_DIR)Include` FIRST (the engine needs DXSDK June 2010's `d3dx9.h`), so DXSDK's stale `D3D11.h` / `DXGI.h` / `Dcommon.h` shadow the Win10 SDK versions when `dcomp.h` transitively pulls them in. Compositor.cpp's `<ClCompile>` entry in the vcxproj gets a per-file `<AdditionalIncludeDirectories>` that REPLACES (no `%(...)` inheritance) the project default with a Win10-SDK-only path so DXSDK isn't searched at all for this file. With pImpl in the header, HostWindow.cpp (which keeps the project default include path) never sees `dcomp.h`. Filed as a project lesson. (2) **3b — composition controller swap** at [`HostWindow.cpp`](src/host/HostWindow.cpp) refactors the ~220-line inner controller-ready lambda into a `FinishWebView2ControllerSetup(ICoreWebView2Controller*)` helper that both modes share. The composition path QI's `ICoreWebView2Environment` to `Environment3`, calls `CreateCoreWebView2CompositionController`, and in the completion callback QI's the composition controller down to `ICoreWebView2Controller` for the shared setup (these are sibling interfaces backed by the same underlying object, not C++ inheritance — `QueryInterface` is the documented traversal). Tree construction is deferred until after the controller exists per v3's lesson, then `Compositor::AttachWebView2` plugs in `RootVisualTarget` + commits. (3) **3c — mouse forwarding** adds a `ForwardMouseToCompositionWebView2(UINT msg, WPARAM wp, LPARAM lp)` private method invoked from new WM_MOUSE* cases in MainWndProc. Direct cast from `msg` to `COREWEBVIEW2_MOUSE_EVENT_KIND` (enum values numerically identical to WM_* constants). MK_* bits in wParam → `COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS` (same numeric identity). Wheel-message coords translated via `ScreenToClient`. `SetCapture(hMain)` on any button-down + `ReleaseCapture()` when up-event leaves wParam's MK_* bits at zero. (4) **3d — cursor sync** caches the WebView2-desired HCURSOR via `add_CursorChanged` + primes from `get_Cursor`, returns it from a new WM_SETCURSOR case gated on `LOWORD(lp) == HTCLIENT` (non-client cursor handling stays with DefWindowProc). `remove_CursorChanged` in WM_DESTROY before controller release. (5) **3e — DPI** calls `put_RasterizationScale(GetDpiForWindow(hMain)/96.0)` once at controller-ready time (QI baseController to `ICoreWebView2Controller3` for the method), then updates on WM_DPICHANGED with Windows's suggested rect (per-monitor-v2 best-practice flow). (6) **3f — keyboard focus transfer** calls `baseController->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC)` once at controller-ready time + on WM_SETFOCUS in MainWndProc. Under composition the host HWND owns Win32 focus by default and WebView2's input thread doesn't see WM_KEY*/WM_IME_*; MoveFocus transfers logical focus so the existing DOM event chain works unchanged. IME inherits this for free. (7) **3g — composition-hosting A/B parity spec** at [`composition-hosting.spec.ts`](web/apps/editor/tests/composition-hosting.spec.ts) adds 8 specs that skip with a clear annotation when `ALO_WEBVIEW2_HOSTING != "composition"`, so the harness runs in both modes: HWND baseline = 99 + 8 skip; composition = 106 + 1 self-skip. The specs explicitly document the CDP caveat — Playwright's `.click()` / `.keyboard.press()` synthesize events at the renderer level, bypassing the OS WM_* path, so they validate the bridge layer not host-side `SendMouseInput` / `MoveFocus` correctness.

**Issues encountered and resolutions.** *Sub-stage 3a hit a DXSDK-vs-Win10-SDK header conflict not visible in the standalone spike.* First Debug build of Compositor.cpp failed with `_13 undeclared identifier` in Win10 SDK's `d2d1_1helper.h` and `DXGI_COLOR_SPACE_TYPE` undefined in `dcomp.h`. The spike compiles fine because its vcxproj omits DXSDK from its include path; ParticleEditor's vcxproj puts DXSDK first for the engine's `d3dx9.h` dependency, so DXSDK's June-2010-vintage `D3D11.h` / `DXGI.h` / `Dcommon.h` get found first and lack the types modern `dcomp.h` references. Initial fix attempt added `#define D2D_USE_C_DEFINITIONS` (Microsoft's canonical opt-out for d2d's C++ helpers) which helped one error class but `dcomp.h`'s direct references to `DXGI_COLOR_SPACE_TYPE` still failed. Second attempt prepended Win10 SDK paths via per-file `<AdditionalIncludeDirectories>` using `$(WindowsSDKVersion)` — failed silently because that MSBuild macro is empty when only `<WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>` is set. Third attempt hardcoded the full SDK version (`10.0.26100.0`) AND REPLACED (no `%(...)` inheritance) the project default — DXSDK isn't searched at all for Compositor.cpp. Filed as with the surgical pattern. *PImpl pivot mid-3a anticipated Stage 3b's pain.* Original 3a header had `Microsoft::WRL::ComPtr<IDCompositionDevice>` etc. as members — would have transitively pulled `dcomp.h` into HostWindow.cpp's TU at 3b and re-triggered the same DXSDK shadowing. Refactored to pImpl before 3a's commit. *Stage 3b's first smoke screenshot looked-class.* At 5s post-launch the editor showed a dark purple client area with no React chrome — symptomatically identical to v1/v2/v3's opaque-white failure. Almost activated the 24h iteration revert protocol. The deciding factor: the log already contained `[host] composition hosting ready (DComp tree committed)` and zero `[COMP-fail]` lines. That mismatch said retry with a longer wait. At 8s React had finished mounting and the full chrome was visible. **Composition mode has slightly different boot timing than HWND mode** — the DComp tree commits before React's first paint, so a too-fast smoke catches the empty target. Documented in [`tasks/stage-3b-smoke-result.md`](tasks/stage-3b-smoke-result.md). *Stage 3c's manual smoke was the only correctness gate for SendMouseInput.* Playwright's `.click()` dispatches through CDP at the renderer level — bypasses the OS WM_LBUTTONDOWN path entirely. So even though native 99/99 PASS under composition mode, that gate proves "bridge layer works" not "SendMouseInput forwarding is correct." Real OS click at (86, 34) → File menu opens + `[Occlude] SET id=menu:file rect=(119,17,238,243) feather=24` bridge log fires, proving WM_LBUTTONDOWN → ForwardMouseToCompositionWebView2 → SendMouseInput → WebView2 → React onClick → bridge round-trip end-to-end. Click outside at (300, 250) → menu closes + `[Occlude] CLEAR`. Evidence at [`tasks/stage-3c-smoke-screenshot.png`](tasks/stage-3c-smoke-screenshot.png). *3c's manual smoke also surfaced an Escape-doesn't-close-menu observation* — diagnostic gold for Stage 3f. Under composition, host HWND owned Win32 focus + WebView2 didn't, so WM_KEYDOWN arrived at hMain → DefWindowProc → vanished. *Stage 3f's planning rested on a phantom API.* The sub-plan §3.4 modelled "path (a) — use `ICoreWebView2CompositionController::SendKeyboardInput` on SDK ≥1.0.4015+" vs "path (b) — DOM keyboard via focus." Pre-coding grep confirmed it wasn't in our 1.0.3967.48 SDK — but I treated that as "not yet, would be in 4015+." When user OK'd "do path (a)," WebFetch against the MS Learn docs page immediately revealed: the interface has 8 members across ALL historical SDK versions (1.0.774.44 through 1.0.4015-prerelease) and `SendKeyboardInput` is not among them. The whole "path (a)" branch was dead. Filed as: local-header grep proves "not in THIS version"; vendor docs prove "not in ANY version" — different claims. The actual answer was simpler than either modelled option: `MoveFocus` on the base controller (exists in every SDK version) gives WebView2 logical focus; the DOM keyboard chain works unchanged once focus is correct. Stage 3f shipped that as a 37-line change. *Sibling session shipped Stage 1 follow-up cache deferral mid-flight.* `origin/integration-branch` advanced by 5 commits while this dispatch was in code. Their changes didn't touch HostWindow.cpp / BridgeDispatcher / LayoutBroker (by design); rebase was clean (zero conflicts). Sibling also flagged that Release x64 had been failing since `` due to C4996 (`_wgetenv`) under /WX — my Stage 3b's added `_wgetenv` for `ALO_WEBVIEW2_HOSTING` regresses Release the same way. Two-line vcxproj fix at `ba3fbc4` (add `_CRT_SECURE_NO_WARNINGS` to both Release configs matching the Debug pattern) — `ParticleEditor.exe` now links in Release for the first time this session. *Stage 3g's first-pass modifier-keys spec used the wrong Playwright API.* My test did `page.mouse.click(x, y, { modifiers: ['Shift'] })` which Playwright's TypeScript types accept but Chromium ignores — `page.mouse` is the lowlevel API. Switched to `locator.click({ modifiers: ['Shift'] })`. 1 test failed in the first run; rewritten test passes in the second.

Test counts at handoff: vitest **335 / 335** unchanged. Playwright native: **+8** new tests in `composition-hosting.spec.ts` (composition-only, skip on HWND baseline). Run counts: HWND baseline 99 passed + 8 skipped (clean A/B); composition mode 106 passed + 1 self-skip + 0 failed. MSBuild Debug x64 clean (preexisting LIBCMTD warning unchanged). MSBuild Release x64 clean for the first time this session (``-era C4996 breakage fixed at `ba3fbc4`). C++ touched: new `src/host/Compositor.{h,cpp}` (~400 lines); `src/host/HostWindow.cpp` (~430 net lines added across 3b's InitWebView2 refactor + 3c/3d/3e/3f handlers); `src/ParticleEditor.vcxproj` (new ClInclude/ClCompile entries with per-file include-path isolation + `_CRT_SECURE_NO_WARNINGS` on Release configs). Tests / harness touched: new `tests/composition-hosting.spec.ts` (~280 lines); `scripts/run-native-tests.mjs` (+1 spec). Planning + lessons: sub-plan `tasks/dxgi-stage-3-composition-hosting.md` §7.1 + D4 updated with SUPERSEDED notes; `tasks/lessons.md` gains (DXSDK shadowing isolation pattern) + (verify SDK assumptions via authoritative docs before SDK-bump planning). Smoke evidence committed to repo: 3b screenshot + report; 3c screenshot + report. Stage 3f manual keyboard smoke + Stage 3h a11y suite + Stage 3i final acceptance smoke are pending the next dispatch / user-driven verification.

---

### AlphaCompositor `lastRawDib` cache deferral (Phase 3 Stage 1 follow-up) — ~2-5 ms/frame reclaimed in arch B by gating the per-frame cache + on-demand snapshot readback

*2026-05-22 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Phase 3 Stage 1g shipped at [`ad7d294`](https://github.com/DrKnickers/particle-editor/commit/ad7d294) with the D3D9Ex migration intact but ~50 FPS at maximized 3440×1440 instead of the ≥58 the per-phase budget targeted. Per-frame instrumentation pinned ~2-5 ms of that gap on the [`m_lastRawDib`](src/host/AlphaCompositor.cpp:597) memcpy inside [`AlphaCompositor::Composite`](src/host/AlphaCompositor.cpp:568) — a 19.8 MB snapshot copy maintained every frame for `CaptureSnapshotPng` (the modal frosted-glass backdrop introduced in [B1.3.1.1](https://github.com/DrKnickers/particle-editor/commit/f3570d3)). Modal opens fire seconds-to-minutes apart; the cache was paying ~120-300 MB/s of memory bandwidth at maximize to keep snapshots fresh for a consumer that almost never fires. After this change the cache is OFF by default — arch B (`WS_EX_LAYERED` popup) reclaims the memcpy, and `CaptureSnapshotPng` re-issues `GetRenderTargetData` + `LockRect` on demand to get a fresh snapshot at modal-open time (~12-15 ms one-shot, imperceptible against the ~50-100 ms dialog mount + React reflow that triggers it).

**How we tackled it.** Three coupled edits across two files, with a one-line opt-in in a third. (1) [`AlphaCompositor::Impl`](src/host/AlphaCompositor.cpp:45) gains a `perFrameCacheEnabled` bool (default `false`) sitting next to the existing `lastRawDib` / `lastRawW` / `lastRawH` fields. (2) The cache block at [`AlphaCompositor.cpp:597-612`](src/host/AlphaCompositor.cpp:597) is wrapped in `if (m_impl->perFrameCacheEnabled)` — the entire load-bearing 19 MB memcpy now compiles to a no-op in arch B. (3) [`CaptureSnapshotPng`](src/host/AlphaCompositor.cpp:503) is rewritten to be self-sufficient: re-issue `GetRenderTargetData(offscreenRT → sysMemSurface)`, `LockRect`, copy into a function-local buffer, `UnlockRect`, then run the existing T4c.5 scene-rect crop + GDI+ PNG encode + base64. Safe because `offscreenRT` is never mutated by `Composite()` stamps (which only touch `dibPixels`); between `Engine::Render` calls it always holds clean pre-stamp engine pixels. (4) New `AlphaCompositor::SetPerFrameCacheEnabled(bool)` public method — idempotent, the disable path uses the standards-guaranteed `std::vector<uint8_t>().swap(...)` idiom to actually release the ~19 MB (`.clear()` keeps capacity, `.shrink_to_fit()` is a non-binding request). (5) [`FramePublisher::FramePublisher`](src/host/FramePublisher.cpp:46) flips the flag on — owning the opt-in inside the class keeps the dependency localized AND avoids touching `HostWindow.cpp`, which a parallel webview-hosting-migration session is editing heavily. The frame-server path (`OnFrameComposited → EncodeFrameJpeg → lastRawDib`) is structurally unchanged; only arch B (the path the perf measurement was taken in) reclaims the memcpy.

**Issues encountered and resolutions.** *The task spec only accounted for one consumer of the cache; the codebase had two.* The initial brief proposed deleting `lastRawDib` outright. A pre-implementation grep for the cache fields surfaced [`EncodeFrameJpeg`](src/host/AlphaCompositor.cpp:389) as a second consumer — load-bearing inside `FramePublisher::OnFrameComposited` for the (`ALO_VIEWPORT_TRANSPORT=canvas-jpeg`) canvas-in-DOM transport shipped at [``](https://github.com/DrKnickers/particle-editor/commit/). A blanket delete would have killed the per-frame frame-server. Reshaped the plan around a gated-cache + on-demand-snapshot pair: arch B turns the cache off, arch C leaves it on. Lesson worth keeping: when a perf brief names "the consumer" of a piece of state, grep for additional readers before deleting — the brief's mental model may be one version behind the code. *`std::vector::clear` + `shrink_to_fit` doesn't actually free memory.* The disable path needs to release ~19 MB so arch-A → → arch-A toggles don't leak. `.clear()` resets size but keeps capacity; `.shrink_to_fit()` is a *non-binding request* the implementation may ignore. The standards-guaranteed idiom is `std::vector<uint8_t>().swap(target)` — construct an empty vector, swap with the target, the empty vector's destructor releases the buffer. Documented inline in `SetPerFrameCacheEnabled`. *Risk-2 from the brief (snapshot timing) dissolved by inspection.* The original brief flagged a concern that the new on-demand readback might capture a partially-stamped frame. Reading the `Composite()` body showed stamps mutate `dibPixels` only — never the GPU's `offscreenRT`. So `offscreenRT` between `Engine::Render` calls always holds clean pre-stamp pixels; re-readback from there is structurally identical to the old cached path's content, just timed differently. The drag-resize scenario (Win32 modal sizing loop starves WebView2 IPC + skips `Composite`) also works by the same invariant: `Engine::Render` also doesn't run during the sizing loop, so `offscreenRT` holds the pre-resize frame — same observable behavior as the pre-fix cached path. No new spec needed for because the existing dialogs/canvas-architecture coverage exercises this path. *Native test harness uses an explicit spec list, not a glob.* The new [`alpha-compositor-snapshot.spec.ts`](web/apps/editor/tests/alpha-compositor-snapshot.spec.ts) needed registering inside [`scripts/run-native-tests.mjs`](web/apps/editor/scripts/run-native-tests.mjs)'s hand-curated list — Playwright won't auto-discover it. Added next to `d3d9ex.spec.ts` (its natural sibling — also a Phase 3 follow-up regression). *Modal-open unblurred-snapshot flash, surfaced by manual smoke.* First post-fix launch showed the frosted-glass modal opening with the snapshot fully unblurred for ~1 second before the blur kicked in. Caused by the timing shift: the snapshot used to arrive in ~0.1 ms (cache hit, effectively synchronous with the modal-open render), so `Dialog.Overlay`'s fade-in started with stable backdrop content. Post-deferral the snapshot arrives ~50-500 ms later (on-demand GPU readback + GDI+ PNG encode + IPC + Chromium PNG decode for the 18.8 MB raw frame at maximize), landing the `<img>` mid-fade-in — Chromium's backdrop-filter doesn't update reliably when its source content changes during an animation. Fixed in [`Modal.tsx`](web/apps/editor/src/components/Modal.tsx) by gating `Dialog.Root`'s `open` prop on a new `snapshotReady` flag tripped on `requestAnimationFrame` AFTER `setSnapshot` fires, plus reordering the `viewport/occlude` to AFTER `viewport/capture-snapshot` resolves so the live engine viewport stays visible (rather than alpha-cut to black) during the capture window. Trade-off: user-perceived modal-open latency increases by the capture round-trip cost (~2 ms host-readback at 1264×761, growing with viewport area), but the dialog + frosted-glass backdrop now mount in one render pass with the blur applied from frame one. A `setTimeout(750)` fallback opens the dialog anyway if the capture stalls. Lesson worth keeping: when migrating a fast cache-hit code path to an on-demand async path, the latency delta isn't a perf number to optimize away — it's a *render-ordering hazard* that can re-shuffle React + browser-compositor phases mid-animation. *Parallel session shipped Phase 3 Stage 2 mid-flight, touching the same files.* `origin/integration-branch` advanced by two commits ([`e5f3a40`](https://github.com/DrKnickers/particle-editor/commit/e5f3a40) Stage 2 shared-handle texture + [`e8845fa`](https://github.com/DrKnickers/particle-editor/commit/e8845fa) handoff refresh) while this dispatch was in code. Inspection showed Stage 2 modifies `Resize()` / `ReleaseGpuResources()` / `GetRenderTarget()` (none of which I touched) and adds a `GetSharedHandle()` method right after `Composite(HWND)` in the header (where my `SetPerFrameCacheEnabled` also wants to land). Resolved by ordering: my dispatch rebases onto Stage 2, the textual header conflict resolves by listing both new methods sequentially. No structural conflict — the shared-handle texture's level-0 surface is still the `IDirect3DSurface9` the readback paths consume, so my `CaptureSnapshotPng` rewrite needs no change to interoperate with the new RT allocator.

Test count: vitest **335 / 335** unchanged. Playwright native: **+3** new tests in `alpha-compositor-snapshot.spec.ts` (first-snapshot-after-boot, two-consecutive-snapshots, snapshot-dimensions-follow-resize). MSBuild Debug x64 clean (preexisting LIBCMTD warning unchanged). MSBuild Release x64 fails with C4996 (`_wgetenv`) at [`HostWindow.cpp:455`](src/host/HostWindow.cpp:455) — this is a pre-existing breakage from [``](https://github.com/DrKnickers/particle-editor/commit/) (the env-var detection introduced in Phase 0+1, six commits before this dispatch); CHANGELOG entries for Phase 1/2/3 Stage 0/1 only attest Debug x64, never Release. Not addressed here — touching `HostWindow.cpp` to suppress the warning would conflict with the parallel webview-hosting session per this dispatch's decoupling decision. Should be split out as its own perf-polish-adjacent maintenance dispatch. C++ touched: `AlphaCompositor.{h,cpp}` (new `SetPerFrameCacheEnabled` API + gated cache block + rewritten snapshot path), `FramePublisher.cpp` (constructor flips the flag). Tests / harness touched: new `alpha-compositor-snapshot.spec.ts`, `scripts/run-native-tests.mjs` (+1 spec).

---

### Canvas-in-DOM input forwarding (Phase 2) — `viewport/input` bridge surface + hidden popup + chrome cutout gone + legacy Shift-place gesture preserved

*2026-05-22 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

The canvas-in-DOM viewport is now the visible, interactive source of truth when `ALO_VIEWPORT_TRANSPORT=canvas-jpeg` + `VITE_VIEWPORT_TRANSPORT=canvas-jpeg` are set. Phase 1 published engine pixels to a `<canvas>` but kept the legacy `WS_EX_LAYERED` popup visible on top; Phase 2 hides the popup (`ShowWindow(SW_HIDE)`, still sized to the full main client so `LayoutBroker` scene-rect math and the D3D9 swapchain stay untouched) and routes every viewport input gesture through a new `viewport/input` bridge surface to a host-side `InputDispatcher` that PostMessages the synthesized Win32 message into the (hidden) popup's existing WNDPROC. The engine's input handlers consume the synthetic messages unchanged because the renderer encodes modifiers into the `wParam` `MK_*` bitmask exactly the way the OS does — the popup's WNDPROC ([HostWindow.cpp:1075-1371](src/host/HostWindow.cpp:1075)) never calls `GetKeyState`/`GetAsyncKeyState`, so the hidden HWND not seeing real keyboard input is invisible to the engine. The headline payoff: chrome dropdowns over the viewport no longer show the alpha-cutout artifact, because the popup that produced it isn't visible any more. Smoke-driven regression fixes also landed in this dispatch: (a) `SetFocus(hwnd)` on the hidden popup was triggering a spurious `WM_KILLFOCUS` cascade that killed Shift+LMB spawns within ~2ms of creation — gated both the SetFocus call and the WM_KILLFOCUS defensive kill on `!m_archCMode` so archC mode skips the focus-thrash entirely; (b) the legacy "Shift down → preview, LMB-click → place permanently, system continues emitting" gesture from `src/main.cpp:2877-2934` was incomplete in the new UI's B1.3.1 polish round 5 (only the spawn-on-LMB-down path landed) — added `OBJECT_Z` drag mode + `Engine::DetachParticleSystem` call on WM_LBUTTONUP so the full legacy placement gesture works (Shift+click-drag-release places a free-running emitter at any height, chain-clicks place multiple).

**How we tackled it.** Single dispatch, four code surfaces: schema, renderer, host, build. (1) Schema — one new `viewport/input` `kind` in [`bridge-schema/src/index.ts`](web/packages/bridge-schema/src/index.ts) carrying a discriminated `ViewportInputEvent` union (`mousemove` / `mousedown` / `mouseup` / `wheel` / `keydown` / `keyup` / `blur`). Chose single-kind-with-discriminator over per-event-type kinds (`viewport/input/mouse` etc.) because it matches Win32's "one MSG, type tag" shape and keeps both dispatch arms (renderer encode + host decode) to one switch each. (2) Renderer — new [`viewport-input.ts`](web/apps/editor/src/lib/viewport-input.ts) helper with pure-function encoders (`encodeMkButtons`, `quantiseWheelDelta`, `toPopupClientCoords`, `isTypingTarget`, `makeMouseEvent` / `makeWheelEvent` / `makeKeyEvent`). [`ViewportSlot.tsx`](web/apps/editor/src/components/ViewportSlot.tsx) gains a third `useEffect` (gated on `archCEnabled`) that wires pointerdown/move/up/cancel + contextmenu + native `wheel` listener with `{ passive: false }` (pattern) on the canvas, plus window-scoped keydown / keyup / blur with the `TYPING_TAGS` guard so inspector field typing doesn't drive engine input. `setPointerCapture` on pointerdown keeps drag gestures (LMB-rotate, MMB-pan) firing pointermove when the cursor exits the canvas bounds. (3) Host — new [`InputDispatcher.{h,cpp}`](src/host/InputDispatcher.h) under `src/host/`; [`BridgeDispatcher`](src/host/BridgeDispatcher.cpp) gains a `viewport/input` arm + `SetInputDispatcher` setter; [`HostWindowImpl`](src/host/HostWindow.cpp) constructs the dispatcher alongside `FramePublisher` in `WM_CREATE` when `m_archCMode` is true, binds it into the bridge after `BindAttachedSystem`, tears it down before the compositor in `WM_DESTROY`. `LayoutBroker` gains a one-line `GetViewport()` getter so `Run` can `ShowWindow(SW_HIDE)` the popup after the `ApplyFullClient` sizing step. (4) Build — `ParticleEditor.vcxproj` gains the two new ClInclude / ClCompile entries with the same `ExcludedFromBuild` flags as the rest of `host/` (Win32 x86 + Win32 x64 excluded; Debug|x64 + Release|x64 built).

**Issues encountered and resolutions.** *The worktree was inherited mid-attempt.* At session start every modified file was already touched and the new files (`InputDispatcher.{h,cpp}`, `viewport-input.ts`, `viewport-input.test.ts`, `canvas-architecture.spec.ts`) were already present in `git status`'s `??` list — a prior session got most of Phase 2 done but never committed. The session's first hour re-derived T2.1–T2.4 in parallel without noticing because `Read` on the modified files returned content that looked like the pre-edit baseline. The aggregate state happens to reconcile (line counts and diff stats match) and the test suite proves it — but the right pre-flight discipline for a future inherited worktree is "read every file in `git status -s | grep '^[ ?]M\?' | awk '{print $2}'`" before designing edits. *MSBuild needs project-rooted package paths.* Building `src/ParticleEditor.vcxproj` directly fails with `missing Microsoft.Web.WebView2.targets` because the relative-path search starts from `src/packages/` instead of the repo root's `packages/`. The fix is invariant: always build via the `.sln` at repo root, never the `.vcxproj` directly. *Coordinate convention reads as one line but encodes a load-bearing assumption.* "Popup-client physical pixels = `clientX * devicePixelRatio`" only holds because the popup spans the full main client (per [T4c.4](https://github.com/DrKnickers/particle-editor/commit/bd0fab2)). If a future change shrinks the popup back to the scene rect, the coordinate transform breaks silently. Documented in [`viewport-input.ts`](web/apps/editor/src/lib/viewport-input.ts) `toPopupClientCoords` and the schema's `ViewportInputEvent` block. *Shift+LMB spawn died ~2ms after creation.* Smoke surfaced a Phase-2-specific regression: pressing Shift then clicking spawned a cursor-bound particle system, but it disappeared instantly. Diagnostic logging traced it to `SetFocus(hwnd)` at the top of `WM_LBUTTONDOWN` (HostWindow.cpp:1142) — `SetFocus` succeeds on a hidden window (visibility isn't a precondition; `WS_EX_NOACTIVATE` only blocks user-driven activation), briefly transferring focus to the popup, then OS focus management snapped it back, firing `WM_KILLFOCUS` and the defensive cursor-bound-spawn kill. Fix: gate both the `SetFocus` calls (LMB + RMB down) and the `WM_KILLFOCUS` defensive kill on `!m_archCMode` so archC mode skips the entire focus-thrash → kill loop. Legacy mode keeps the original semantic. *Legacy "place + continue emitting" gesture was incomplete in the new UI.* User clarified that Shift+LMB in legacy should "place a particle system, where the emitters continue to spawn particles after I have released shift and lmb." The B1.3.1 polish round 5 path only landed the spawn-on-LMB-down branch — `WM_LBUTTONUP` didn't call `Engine::DetachParticleSystem`, so the cursor-bound preview either followed the cursor forever (Shift held) or was killed by Shift release. Legacy semantic at [`src/main.cpp:2877-2934`](src/main.cpp:2877) is: Shift down → spawn preview, LMB-down → enter `OBJECT_Z` drag (Y mouse delta drives Z height, X/Y stay frozen), LMB-up → detach (preview becomes free-running placed system at current world position), Shift release → kill any still-attached preview. Added: `OBJECT_Z` to the `DragMode` enum; WM_LBUTTONDOWN enters OBJECT_Z drag when `m_attachedParticleSystem != nullptr` (either from prior WM_KEYDOWN VK_SHIFT spawn or from the B1.3.1 fallback LMB-spawn-while-Shift-held); WM_MOUSEMOVE's OBJECT_Z branch sets `cursor.z = -y * camDist / 1000` matching legacy line 2939-2948 with X/Y frozen; WM_LBUTTONUP calls `engine->DetachParticleSystem(m_attachedParticleSystem)`. User-verified working: Shift hold shows preview tracking cursor; Shift+click places + spawns next preview; chain-clicks place multiple; Shift release ends the gesture. *Phase 2 perf at maximized 3440×1440 was unacceptable (20 FPS).* The canvas-JPEG pipeline is bandwidth-bound at large scene rects — per-frame cost decomposes as ~10ms `GetRenderTargetData` + ~25ms JPEG encode + ~3ms base64 + ~8ms `PostWebMessageAsJson` IPC + ~3ms JSON parse + ~15ms `<img>` decode ≈ 64ms/frame at maximized. No tuning of JPEG quality, transport, or canvas-resize strategy gets that under ~50ms. This finding redirected Phase 3 from "A/B verification" to "**DXGI shared-handle compositing**" — see [`tasks/todo.md`](tasks/todo.md) for the new plan. Phase 2 stays shipped as the canvas-JPEG transport (now a diagnostic env-var-gated dev mode); production fallback under DXGI is legacy arch-A, not canvas-JPEG. *Phase 2 ships behind the env var; is still in flight.* The ROADMAP entry stays in §2 Medium-term with the Phase 3 redirect noted.

Test count: vitest **300 → 335** (+35: viewport-input.test.ts adds 26 encoder unit tests; ViewportSlot.test.tsx adds 9 DOM-integration tests). Playwright **90 / 90** unchanged in legacy CI; the new [`canvas-architecture.spec.ts`](web/apps/editor/tests/canvas-architecture.spec.ts) self-skips when the canvas isn't mounted, so it adds 0 to the legacy run and 3 to a future archC-default run. MSBuild Debug x64 clean (preexisting LIBCMTD warning unchanged). C++ touched: new `InputDispatcher.{h,cpp}` (~190 lines, includes `[ArchC-input]` / `[ArchC-engine]` / `[ArchC-kill]` diagnostic logging slated for Phase 3 Stage 7 removal); `BridgeDispatcher.{h,cpp}` (+ `SetInputDispatcher` + `viewport/input` arm); `HostWindow.cpp` (+ `m_inputDispatcher` lifecycle + popup `SW_HIDE` gate + SetFocus / WM_KILLFOCUS gating + OBJECT_Z drag + LMB-up Detach); `LayoutBroker.h` (+ `GetViewport` getter); `ParticleEditor.vcxproj` (+ 2 file entries). React touched: new `viewport-input.ts` helper (~130 lines); `ViewportSlot.tsx` (+ third `useEffect` for DOM event handlers); schema (+ `viewport/input` request kind + `ViewportInputEvent` union); MockBridge (+ no-op ack arm). Planning artifacts: [`tasks/todo.md`](tasks/todo.md) split into active DXGI Phase 3 plan (~440 lines) + [`tasks/todo-mt-11-phase-0-1-2-archive.md`](tasks/todo-mt-11-phase-0-1-2-archive.md) (~1213 lines of Phase 0+1+2 history).

---

### Resizable splitters for the editor shell (B1.4) — four drag handles + Reset menu item, with mid-arc architectural redirect to `layout/scene-rect`

*2026-05-22 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

The main editor row's three-column + two-inner-vertical-split structure is now driven by `react-resizable-panels@4.11.1`. Four draggable boundaries — left column ↔ centre column, centre column ↔ Spawner column (when Spawner is visible), viewport ↔ curve editor, emitter tree ↔ property tabs — replace the previous Tailwind fixed-width / flex-fraction layout. Sizes persist per user under `localStorage` keys `alo:layout:{outer:{2col,3col},left,center}`. Min/max constraints keep every pane usable at any window size (left clamped 15–40 %, centre min 30 %, Spawner clamped 12–40 %); 4.x ships double-click-handle-to-reset for each splitter for free. A new **View → Reset panel layout** menu item clears all four `alo:layout:*` keys and remounts `PanelLayout` at in-code defaults (20/60/20 outer 3-col, 25/75 left, 75/25 centre). The Spawner toggle button on the toolbar continues to mount / unmount the panel; the outer Group uses two separate persistence keys for the 2-col and 3-col states so each shape keeps its own ratios.

**How we tackled it.** Two sessions, 12 implementation commits + 2 docs commits. Session 1 (T0 → T5, 5 commits): T1 installed the library and caught major API drift via type declarations (4.x renames `PanelGroup → Group` and `PanelResizeHandle → Separator`, drops `autoSaveId` in favour of DIY `defaultLayout` + `onLayoutChanged`, ships double-click-handle reset and aria automatically); the plan was rewritten in place before any implementation code landed. T2 / T3 built a failing PanelLayout vitest skeleton and made it pass with the three-nested-Group structure ([`PanelLayout.tsx`](web/apps/editor/src/components/PanelLayout.tsx)); T4 swapped the new component into AppShell and fixed two 4.x quirks (numeric size props are PIXELS not percentages — use `"NN%"` strings; `Group.defaultLayout` is effectively an SSR hint, `Panel.defaultSize` is the canonical client knob — both documented in **lessons.md**). T5 added the Playwright spec [`tests/splitters.spec.ts`](web/apps/editor/tests/splitters.spec.ts) covering drag-persistence + corrupted-localStorage fallback + spawner toggle 2col↔3col (+6 tests). Session 2 (T4b → T4c.5 + T6, 7 commits): a user-tested regression appeared (engine viewport popup overlapping panels during drag) because the per-frame `Engine::Reset` cost stacked under the splitter's ResizeObserver bursts. T4b's first attempt (pointerdown/pointerup capture to park the popup offscreen) worked in vitest but failed user smoke — synchronous pointerup read of `getBoundingClientRect` returned stale geometry because Win32 layout commits arrive after React's handler. Reverted at [`0610f8f`](https://github.com/DrKnickers/particle-editor/commit/0610f8f). The architectural redirect (T4c, 4 sub-commits) introduces a `layout/scene-rect` bridge surface: the popup HWND stays sized to the full main client area at all times (sized once on WM_CREATE / WM_SIZE), `ViewportSlot` dispatches the centre-quadrant rect per frame, and `AlphaCompositor` stamps `alpha=0` for the four bands outside the scene rect (hard cut, no smoothstep). Splitter drag now updates an alpha mask — not a D3D9 device Reset. Layered-window compositing (`WS_EX_LAYERED` + `UpdateLayeredWindow(ULW_ALPHA)`) makes the alpha-zero bands transparent for both rendering and hit-testing, so panels behind them receive their own mouse events for free. T4c.5 cropped `AlphaCompositor::CaptureSnapshotPng` to the scene rect via the GDI+ subregion-view idiom (zero-copy: scan0 offset + parent stride). T6 added the Reset menu item — exports `PANEL_LAYOUT_KEYS` + `resetPanelLayoutStorage` from PanelLayout, App.tsx owns the epoch counter passed as `key={n}` to force a full remount, MenuBar threads `onResetPanelLayout` per the existing `onOpen*Dialog` pattern.

**Issues encountered and resolutions.** *4.x API drift caught at install time, not at debug time.* The plan §3 was originally drafted against the 1.x/2.x API. Walking the type declarations in `dist/react-resizable-panels.d.ts` before writing any client code surfaced every rename + the dropped `autoSaveId`; the plan was rewritten in place (with the original kept as historical context) before T2's test skeleton landed. Pattern worth keeping: when adopting a library, the first task should always be "read the .d.ts and confirm every prop the plan uses still exists." *Numeric `Panel.defaultSize` props silently shrink panels to ~50 px because 4.x treats numbers as pixels.* Spent ~15 min debugging "why is the tree panel 25 px wide" before the type declaration showed `defaultSize?: number | \`${number}%\``. Fixed by templating the percentages as `\`${value}%\``. **** documents both numeric-vs-string + Group-vs-Panel-as-canonical-knob with cross-references to the exact lines of `react-resizable-panels.js` that drive the behaviour. *Drag-flag fix (T4b) failed for two structurally independent reasons.* The pointerup capture handler didn't always fire (likely intercepted by the library's own document handler during capture phase), AND when it did fire it read pre-drag geometry because React hadn't committed the post-drag layout synchronously. Both fixable in principle, but the user redirected to the cleaner architecture (T4c) before more time went into patching. Reverted at [`0610f8f`](https://github.com/DrKnickers/particle-editor/commit/0610f8f) to keep the option-B redirect's diff clean. *T4c.4 introduced a modal-snapshot stretch bug.* After T4c.4 (popup spans window), the About dialog's frosted-glass backdrop appeared horizontally compressed — `AlphaCompositor::CaptureSnapshotPng` was encoding the full popup DIB (now main-row sized) into the PNG the Modal's `quadrant-viewport` `<img>` scales to fill. Fixed in T4c.5 by cropping the cached BGRA buffer to the current scene rect before PNG encode using the GDI+ subregion view (no memcpy). The Modal test stayed green untouched because its contract is shape-only (`pngBase64` + `w` + `h` as opaque types). *Architecture B (DComp visual hosting of the engine inside the WebView's DOM compositor) ruled out by history during the T4c re-plan investigation.* Documented 3 prior attempts at DComp-host-the-engine all failed; the existing record at `docs/superpowers/plans/2026-05-18--viewport-alpha-compositing.md:22` saved the cost of re-spiking. *Architecture C migration (canvas-in-DOM) sized at ~16–32 h post-spike and filed as its own ROADMAP entry (, position 2.1).* The fundamental fix for the chrome-cutout artifact T4c makes worse — but a separate dispatch, not a B1.4 in-scope item.

Test count: vitest **290 → 294** (+9 from T2 PanelLayout skeleton in session 1; net +4 vs B1.3.2 baseline because T4c.4 split the dialogs.spec.ts rescale test into a UI gesture + a contract assertion in the same commit, and T6 added +3 PanelLayout helpers + +1 MenuBar integration test). Playwright **83 → 90** (+6 from `splitters.spec.ts`, +1 from the dialogs.spec.ts split). MSBuild Debug x64 clean (preexisting LIBCMTD warning unchanged). C++ touched: `AlphaCompositor` (scene-rect state + band-stamps + snapshot crop), `LayoutBroker` (scene-rect forwarding), `BridgeDispatcher` (`layout/scene-rect` handler), `HostWindow` (popup self-sizes to client area on `WM_SIZE`). React touched: new `PanelLayout.tsx` (~270 lines), new `splitters.spec.ts` Playwright, new `PanelLayout.test.tsx` vitest, App.tsx (PanelLayout swap + reset-epoch state), MenuBar.tsx (new prop + new menu item), ViewportSlot.tsx (dispatch kind rename), bridge schema (+1 entry).

---

### Unified section headers + inspector polish (B1.3.2) — shared `.panel-section` class, 15 inspector tweaks across 3 polish rounds

*2026-05-22 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

The inspector tabs (Basic / Appearance / Physics) and the tool panels (Spawner / Lighting / Bloom) now render collapsible-section headers with the same visual treatment — bordered-box container, uppercase muted title, Lucide ChevronDown on the right that rotates when expanded. The aesthetic comes from the prior tool-panel design; the unification routes both consumers through a single shared CSS class set. Across three smoke-test-driven polish rounds, 15 inspector field tweaks land in the same dispatch: widened dropdowns where long option labels were truncating (Physics Type, Appearance Blend mode, Basic Emit mode, Physics Behavior), per-channel R / G / B / A micro-labels on the random-color cluster (matching the X / Y / Z pattern used in Vec3 rows), a +25% spinner width boost for every numeric input in Basic, a "label-first / checkbox-right" layout for long-label checkboxes (Link particles to instance, Object space acceleration), and a unified checkbox-right-edge alignment so every checkbox in every tab lands at the same X regardless of which form-row width variant sits underneath. Spawner gains: Spawn now button moves into the Mode section (manual-only), Burst becomes collapsible.

**How we tackled it.** The shared CSS lives in [`components.css`](web/apps/editor/src/styles/components.css) — `.panel-section`, `.panel-section-header`, `.panel-section-body`, plus a two-selector rotation rule (`.panel-section[data-open="false"] .chev, .panel-section:not([open]) .chev { transform: rotate(-90deg) }`) that bridges Section.tsx's controlled `useState`+`data-open` state model and ToolPanel.Section's native `<details>` state model without forcing either consumer to give up its semantics. `Section.tsx` keeps its `role="button"` + `aria-expanded` + `data-testid` contract; `ToolPanel.Section.tsx` keeps its native `<details>` + `alwaysOpen` branch (Lighting's Ambient/Shadow). The legacy `.section-header` / `.section-body` rules are deleted; `.section-divider` stays as a standalone hairline primitive (used by [`CurveEditorPanel.tsx:1138`](web/apps/editor/src/components/CurveEditorPanel.tsx) as a colour/transform-group separator). Single consumer pre-grep confirmed Section.tsx is only used by EmitterPropertyTabs.tsx — clean restyle.

The polish-round changes layered atop the unification. [`EmitterPropertyTabs.tsx`](web/apps/editor/src/screens/EmitterPropertyTabs.tsx) grows three new `FieldSpinner` / `FieldSelect` props (`widthBoost?: "mid" | "wide" | "x2"` mapping to 73 / 87 / 116 px input columns) and a `FieldCheckbox` `inlineLabel` prop (label wraps instead of truncating). The matching CSS modifiers (`.form-row-mid-input` / `-wide-input` / `-x2-input` / `-check-inline`) sit in components.css alongside the existing modifier family (`.form-row-cluster`, `.form-row-text`, etc.) and follow the same single-class-specificity pattern. Basic-tab spinners pick up +25% width via a single scoped `.basic-tab .form-row` rule declared BEFORE the modifier rules so equal-specificity modifiers win on declaration order — every numeric field in the tab gets the wider input without needing a `widthBoost` prop on each call site. Checkbox right-edge alignment uses `grid-column: 2` + `justify-self: end` on the checkbox itself, pinning its right edge to the spinner number-input column's right edge across every variant. Spawner's Spawn now button moves from `<ToolPanel.Footer>` into the Mode section's manual-only branch; Burst drops `alwaysOpen` for `defaultOpen` so the panel reads as a fully collapsible stack.

**Issues encountered and resolutions.** *Vitest `Section.test.tsx` asserted the `.collapsed` modifier class.* The unification swapped `.collapsed` for a `data-open` attribute on the outer `.panel-section` (matching the `:not([open])` selector for native `<details>`), so one spec assertion needed updating. Caught by the post-migration vitest run; fixed in the same dispatch by re-anchoring the test on `expect(section).toHaveAttribute("data-open", "false")`. **Procedural takeaway:** when renaming a modifier class, the pre-flight grep should look for the modifier name as a JavaScript string too, not just as a CSS selector. Cataloged as a procedural refinement to the (label-rename grep audit) shape — modifier-class renames are subject to the same audit pattern. *Width-pressure pre-flight risk didn't fire.* The plan flagged width-pressure as a risk because the bordered-box adds ~14 px of horizontal cost per section; the smoke-test at the default 25/75 inspector split confirmed every form-row still fits cleanly. The body's `padding: 12px` was the right inner-padding choice — `8 px` would have been tighter but unnecessary. *`CurveEditorPanel.tsx` is an unexpected `.section-divider` consumer.* Caught during the pre-flight grep audit (the original plan said "delete `.section-divider`"). Kept the class as a standalone primitive with a refined comment noting its generic-hairline purpose. Cataloged: when planning a class deletion, grep for the class as a content match (`className="section-divider"`) rather than as a CSS-rule match (`.section-divider {`) — the consumers don't show up in the CSS-rule grep. *Three smoke-test rounds reshaped the polish set.* The original "small changes" request grew from 8 items to 13 items to 15 items as visual smoke-tests surfaced adjacent items worth bundling into the same commit. Each round followed the same execution loop: user describes the visual delta → I edit → launch → user smoke-tests → next round. The final round (checkbox right-edge alignment) deserved its own iteration — the `grid-column: 2 / -1; justify-self: end` first attempt aligned to the unit-cell right edge, but the user wanted the spinner-input right edge (one column further left), which became `grid-column: 2; justify-self: end`. The fix was a one-character change in the Tailwind class. *PowerShell 5.1 UTF-8 corruption (procedural carryover from B1.3.1.1).* The renumbering pattern for ROADMAP didn't recur this dispatch because B1.3.2 doesn't have a [TIER-K] tag — it's a polish item that folds into the prior B1.3.1.1 entry's Shipped position. Worth noting for future reference: tagged ROADMAP items go through the sed-based renumbering (UTF-8 safe via git-bash sed); untagged polish items don't touch ROADMAP at all.

Test count: vitest **281 / 281** (no count change — the modified `Section.test.tsx` "collapsed state" assertion swaps from class-presence to attribute-presence). Playwright **83 / 83** unchanged. MSBuild Debug x64 clean (no C++ touched). Single implementation commit + this docs commit; ~250 lines net in 6 files (CSS expansion + Section.tsx + ToolPanel.tsx + EmitterPropertyTabs.tsx + SpawnerPanel.tsx + the one test selector update).

---

### Frosted-glass modal backdrop via engine snapshot (B1.3.1.1) — replaces interim modal-mask with snapshot-into-DOM

*2026-05-21 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Modal dialogs (Help → About, SaveChangesPrompt, etc.) now sit over a frosted-glass backdrop that blurs panels AND the engine viewport uniformly via `Dialog.Overlay`'s existing `bg-black/60 backdrop-blur-sm` CSS — no visible popup-boundary seam, no inner-shadow vignette. Replaces the interim modal-mask compositor pipeline that B1.3.1 polish round 9 landed; the older approach worked for engine pixels themselves but couldn't span the popup boundary (CSS effects can't reach the engine compositing layer — see [`tasks/lessons.md`](tasks/lessons.md) for the algebra). The new approach lifts the engine into the WebView2 DOM as a frozen `<img>`, so CSS effects sample it natively.

**How we tackled it.** Three layered commits. C1 added the engine snapshot surface end-to-end: [`AlphaCompositor`](src/host/AlphaCompositor.cpp) caches a pre-stamp BGRA DIB each frame in `m_lastRawDib`; new `CaptureSnapshotPng` method wraps it zero-copy in a `Gdiplus::Bitmap` (`PixelFormat32bppARGB`, BGRA byte order matches the DIB format), saves to PNG via an in-memory `IStream`, base64-encodes (inline 30-line encoder, no new dep). [`HostWindow::Run`](src/host/HostWindow.cpp) brackets the message pump with `Gdiplus::GdiplusStartup` / `Shutdown`. New `viewport/capture-snapshot` bridge surface routes through [`LayoutBroker::CaptureSnapshotPng`](src/host/LayoutBroker.cpp) → compositor, returning `{ pngBase64, w, h }`. C2 rewrote [`Modal.tsx`](web/apps/editor/src/components/Modal.tsx): on open, request a snapshot, render the PNG as an `<img position:absolute; inset:0>` via `createPortal` into the viewport-quadrant DOM, send `viewport/occlude` to fully alpha-cut the engine popup; on close, clear the snapshot and the occlude. C3 deleted the now-dead modal-mask machinery: `AlphaCompositor::SetModalMask`, `BoxBlurDibBgra`, `MultiplyDibAlphaBgra`, `FadePopupEdges`, `Smoothstep01Edge`, the `m_globalAlpha` / `blurRadius` / `blurScratch` fields, the `viewport/set-modal-mask` bridge surface + schema + dispatcher + mock. The Modal regression test pivots from asserting `set-modal-mask` dispatch to the new contract (snapshot capture + full-quadrant occlude on open, occlude rect:null on close, and `expect.not.toHaveBeenCalledWith({ kind: "viewport/set-modal-mask" })` to lock the deletion).

**Issues encountered and resolutions.** *Drag-resize leaks opaque engine pixels.* First smoke-test surfaced a vivid ground-texture stripe alongside the snapshot during a window drag-resize. Root cause: the Win32 modal sizing loop on the host thread runs WM_SIZING / WM_SIZE inside a sub-pump that calls [`LayoutBroker::PredictAndApply`](src/host/LayoutBroker.cpp) synchronously (resizing the popup + re-emitting cached occlusion rects to the new popup-client coords) but does NOT pump WebView2 IPC messages, so any `viewport/occlude` the renderer dispatches in response to ResizeObserver firing can't reach `LayoutBroker::SetOcclusion` until release. With a tight quadrant rect, the popup outgrows its alpha cut during the drag and engine pixels render opaque in the band that grew. Fix (commit C2.5, [`cb7b4c7`](https://github.com/DrKnickers/particle-editor/commit/cb7b4c7)): send a deliberately-enormous sentinel rect `(-100000, -100000, 200000, 200000)` instead of the actual quadrant. `ApplyOcclusion` clips iteration to the DIB bounds, so the host-side cost is identical to a tight rect; `ReemitOcclusions` translates the cached main-client rect to popup-client on every popup resize, and translating a huge rect still produces a huge rect that still clips to the current popup's full bounds. Resize-resilient by construction — no renderer→host round-trip needed during drag. *Drag-resize stutter.* Second smoke-test surfaced visible stutter during drag. Root cause: rAF-throttled re-capture fired a ~10-30 ms GDI+ PNG encode per frame stacked on top of the engine's existing D3D9 device `Reset` per WM_SIZE. Fix (same commit): drop the resize subscriptions entirely — capture ONCE on modal open, never re-capture during the modal's lifetime. The img sits at `position:absolute; inset:0` inside the quadrant, so CSS scales it automatically; mild content staleness during a resize is invisible behind `Dialog.Overlay`'s blur. Both fixes captured as **** in lessons.md (Win32 modal sizing loop starves WebView2 IPC; design host-durable state for anything that must survive a drag). *PowerShell 5.1 corrupts UTF-8 during text manipulation.* When renumbering 22 `### 5.N` headings in ROADMAP.md, the natural `Get-Content -Raw … | -replace … | Set-Content -Encoding utf8` round-trip mangled every em-dash and emoji because PowerShell 5.1 reads the file as Windows-1252 by default and re-encodes the codepoints individually. Reverted and re-ran via bash `sed -i` which handles UTF-8 byte-streams natively. Worth remembering for any future ROADMAP-renumber dispatch.

Test count: vitest **281 / 281** (modal-mask regression test in C2 reshaped to assert the new contract — same count). Playwright **83 / 83** unchanged (the deleted `viewport/set-modal-mask` surface wasn't exercised at the Playwright level). MSBuild Debug x64 clean. Build artefact note: the C3 cleanup removes 256 lines of C++ from `AlphaCompositor.cpp` (the box-blur / alpha-multiply / popup-edge-fade helpers + the modal-mask fields).

---

### B1.3.1 polish rounds 1-9 — 25/75 split, file-open behaviour, occlusion bugs, modal compositing, BridgeContext, modal-mask interim (replaced next session)

*2026-05-21 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Nine layered polish rounds on top of the core B1.3.1 dispatch (which lands in the previous entry below). High-level coverage:

- **Round 1 — 50/50 → 25/75 split favouring tabs.** Tabs slot `flex-1` → `flex-[3_1_0%]`; tree aside stays at `flex-1`. Matches the "tab strip dominates the visual hierarchy" brief from the original deferred-item list.
- **Round 2 — inspector right-padding + toolbar File wiring + tree-toolbar pinning.** `.section-body { padding-right: 12px }` gives form rows breathing room from the scrollbar (Name + section headers unaffected). The Toolbar's File buttons were scaffolded to a `todoFile()` console.log — wired to the real `file/new`/`file/open`/`file/save`/`file/save-as` requests, gated through `promptSaveChanges` for destructive ones. EmitterTree toolbar now pins to the pane bottom via `overflow-hidden flex flex-col` on the aside + `flex-1 min-h-0 overflow-y-auto` on the inner list container.
- **Round 3 — emit `emitters/tree/changed` + `ReloadTextures` on `file/new` / `file/open`** (C++ side). Two missing side-effects in the dispatcher: the React tree stayed on the previous file's snapshot because the tree-changed event was never fired, AND particles rendered as the missing-texture white fallback because the engine's lazy texture binding via `ParticleEmitterInstance::onParticleSystemChanged` doesn't fire for instances that exist before the swap. Calling `ReloadTextures` explicitly mirrors what `View → Reload Textures` already does.
- **Round 4 — register ViewportPill + Recents submenu with AlphaCompositor.** ViewportPill never called `useViewportOcclusion`; the engine viewport painted on top of it and only the drop-shadow leaked through. New `OccludingMenubarSubContent` wrapper for `Menubar.SubContent` so the File → Recent Files submenu gets its own occlusion registration too.
- **Round 5 — Shift+LMB in viewport spawns cursor-bound instance.** Legacy WM_KEYDOWN(VK_SHIFT) path was failing because WebView2 holds focus from React UI clicks. New `WM_LBUTTONDOWN` branch checks `MK_SHIFT` and triggers the same `SpawnParticleSystem` call without starting a camera drag.
- **Round 6 — Modal overlay registers with AlphaCompositor.** Required a diagnostic-logs round to pin down two layered failure modes: `window.bridge` was getting swapped to `TestHostBridge` (broken when no `--test-host`) AND Radix Dialog.Content's Portal+Presence delays ref attachment past the parent's useEffect. Initial attempts (Dialog.Overlay ref, asChild wrapper, hardcoded full-window rect, requestAnimationFrame deferral) all failed for different reasons until the diagnostic round narrowed it down.
- **Rounds 7-9 — opaque chrome + BridgeContext + modal-mask compositor (interim, superseded by B1.3.1.1).** Round 7 swaps `.vp-tools` from `backdrop-filter: blur(8px)` + `rgba(...,0.85)` to solid `var(--panel)` (the blur was painting a near-solid dark smudge because WebView2 has nothing useful behind the pill); Modal swaps `shadow-2xl` for `shadow-md` (shadow-2xl extended past the 8 px occlusion pad and drew a hard halo); regression-guard vitest tests lock the policy. Round 8 introduces `lib/bridge-context.ts` — a React Context that carries the live `NativeBridge` to deep consumers, replacing `window.bridge` which `exposeBridgeForTests` swaps to `TestHostBridge` whenever `chrome.webview.hostObjects.hostBridge` is truthy (WebView2 returns a proxy for that property even without `--test-host`). Modal switches to `useBridge()` + `useState` + callback-ref pattern to handle Radix's delayed ref attachment. Round 9 introduces a server-side modal-mask compositor pipeline in [`AlphaCompositor.cpp`](src/host/AlphaCompositor.cpp) — separable box-blur of engine pixels + per-pixel alpha multiply + edge-feather of the popup HWND boundary — driven by a new `viewport/set-modal-mask` bridge surface. The dim + blur work, but **the popup-edge feather produces a visible inner-shadow vignette** at the boundary because pixel math reveals Dialog.Overlay's `bg-black/60` which is darker than the dim engine. **B1.3.1.1 (next session) replaces this approach** with engine-snapshot capture + `<img>` in the WebView2 DOM.

**How we tackled it.** Each polish round followed the standard "user reports → smoke-test diagnosis → minimal fix → commit" cadence. The diagnostic-logs round in polish 6 deserves a callout: when a fix bounces between two failure modes (here: ref attachment vs bridge channel), stop guessing and instrument. Four console.log statements at known points + matching C++-side prints disambiguated `hasBridge=true; hasEl=false` at modal-open vs `hasEl=true; hasBridge=false` at modal-close, which pinned down Radix's late ref attachment as the cause. The fix (useState + callback ref) flowed naturally once the diagnosis was clear. Worth applying to any similar debugging loop where intuitive fixes bounce between adjacent failure modes.

**Issues encountered and resolutions.** *The modal-mask compositor approach (round 9) doesn't work for the popup boundary.* Pixel math: with `globalAlpha=0.4` and `bg-black/60` over panels, popup-center luminance ≈ 60 (engine + slight dim show-through), mid-fade ≈ 35 (where dst dominates as alpha fades), edge ≈ 10 (pure panel*0.4). A smooth visual transition would have endpoints at the same luminance; mine doesn't. Algebraically unfixable by tuning — the cause is structural: CSS effects can't span the engine compositing layer, so any popup-edge fade reveals the dim underneath instead of bridging gradients. Cataloged in lessons.md. B1.3.1.1's snapshot-into-DOM approach lifts engine pixels INTO the WebView2 DOM tree (frozen at one frame), so CSS effects sample them natively — no layer boundary, no algebra. *`window.bridge` is broken when no `--test-host`.* See lessons.md.

Test count: vitest **281 / 281** (was 277; +4 regression guards for opaque pill + no-backdrop-filter + no-large-shadow + modal-mask dispatch — the modal-mask test deletes in B1.3.1.1's Phase 3). Playwright **83 / 83**. MSBuild Debug x64 clean.

---

### Inspector layout follow-ups (B1.3.1) — always-mounted tab strip + flex split between tree and tabs

*2026-05-21 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Addresses the three layout findings the user deferred from B1.3's smoke test. Opening the editor with no `.alo` loaded now shows the full Basic / Appearance / Physics tab strip at the bottom of the left column with a "Select an emitter to edit its properties" message inside the active tab's body — the strip and its three triggers stay clickable so you can pre-pick which tab will populate when you later select an emitter. The lower-left tabs slot stops being a fixed 288 px stripe; it now shares the column's vertical extent with the EmitterTree above on a **25/75 split favouring the tabs** (tree `flex-1` vs tabs `flex-[3_1_0%]`), so the tab strip dominates the visual hierarchy per the deferred-item brief. On tall windows the inspector body gets generous scroll headroom; on shorter windows both halves shrink proportionally rather than the tree being crushed under a non-negotiable slice. Resizing the window scales the split smoothly; B1.4 will make the boundary draggable so the default can be adjusted further.

**How we tackled it.** Two coupled fixes across two files. [`EmitterPropertyTabs.tsx`](web/apps/editor/src/screens/EmitterPropertyTabs.tsx) lifts `Tabs.Root` + `Tabs.List` out of the early-return; a new `renderBody((p) => …)` helper inside each `Tabs.Content` swaps between the placeholder (no selection), Loading (fetch in flight), and the populated form. The helper takes a callback typed `(p: EmitterPropertiesDto) => ReactNode` rather than accepting a pre-built element — the type narrowing flows through cleanly so the call sites never need a non-null assertion. [`App.tsx`](web/apps/editor/src/App.tsx) trades the lower-left slot's `h-72 shrink-0` for `flex-1 min-h-0`, matching the sibling EmitterTree aside's posture. Both children of `panel-body` now sit on identical flex sizing, and Flexbox distributes the available height evenly. Placeholder testid (`emitter-property-tabs-placeholder`) and copy ("Select an emitter to edit its properties") stay verbatim per so existing specs that grep for either keep working unchanged. One pre-existing waitFor pattern in the EmitterPropertyTabs specs anchored on `getByTestId("emitter-property-tabs")` (originally an indirect "wait for properties to load" because the strip used to only mount once loaded) — updated to `getByLabelText("Maximum lifetime:")` on the two affected specs so the test wait now anchors on actual form content, matching the new behaviour.

**Issues encountered and resolutions.** *Vitest waitFor pattern broke when the strip's mount semantics changed.* Two existing specs (`Basic tab renders Maximum lifetime…` and `editing Maximum lifetime fires emitters/set-properties…`) awaited the strip's testid as a proxy for "form is hydrated and ready to query". Pre-change, the testid only attached once `properties !== null`, so the waitFor was an indirect form-loaded check. Post-change, the testid attaches immediately on render, so the waitFor passed too early and the subsequent `getByLabelText("Maximum lifetime:")` ran before BasicTab had data. Surfaced on the first `pnpm test` after the JSX restructure — two failures, both at the same call shape, both fixed by anchoring the waitFor on the actual label instead of the wrapper testid. Worth remembering for any future restructure that decouples a wrapper from its content's load state: spec waitFors are easy to anchor on the wrong proxy. *Build-time `JSX.Element` namespace not in scope.* The first draft of the `renderBody` helper typed its callback as `(p: EmitterPropertiesDto) => JSX.Element`. `pnpm test` passed (vitest doesn't type-check per) but `pnpm build` failed with `Cannot find namespace 'JSX'` — this repo's TypeScript config doesn't expose the JSX global namespace. Fixed by switching to `ReactNode` imported from `react`, matching the convention already used by `Modal.tsx`, `Section.tsx`, and `ToolPanel.tsx`. reminder reinforced: always run `pnpm build` before claiming a JSX change is clean.

Test count: vitest **277 / 277** (no count change — one spec body replaced + two waitFor anchors updated; structural test surface unchanged). Playwright **83 / 83**. MSBuild Debug x64 clean (no C++ touched).

---

### Tab reorganization to match legacy parity (B1.3) — three property tabs restructured, tri-state Generation mutex, percent-display correctness fix

*2026-05-21 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Brings the new-UI Basic / Appearance / Physics tabs back into per-section parity with the legacy Win32 editor (`IDD_EMITTER_PROPS1/2/3`). Basic now reads Emitter Timing / Generation / Connection; Appearance reads Textures / Random color addition / Tail / Rotation / Rendering; Physics reads Initial position / Initial speed / Acceleration / Ground interaction — each matching the legacy GROUPBOX structure section-for-section, with twelve field placements migrated to their legacy homes (rotation cluster Basic → Appearance, parent link strength Basic → Physics > Initial speed, random scale Basic → Appearance > Textures, affected-by-wind Appearance → Physics, emit mode/offset Physics → Basic > Connection, weather particle + cube size + fadeout distance Physics → Basic > Generation > Weather branch). The Bursts / Continuous stream legacy checkboxes become a tri-state Generation radio mutex (Bursts / Continuous stream / Weather particle) derived from the existing `(useBursts, isWeatherParticle)` pair, with atomic two-key bridge patches so the model can never settle in an invalid both-on or both-off state. Every field label now carries a trailing colon to match legacy `.rc` convention. The "World Oriented" checkbox is renamed "Always face camera" with the semantic flip applied (`checked = !isWorldOriented`); the existing BLEND_BUMP cascade that forces and disables the checkbox is preserved. A bundled correctness fix on `FieldSpinner` — new `displayInvertedPercent` prop — restores the legacy semantics of "Minimum lifetime:" and "Minimum scale:": the new UI was previously displaying `randomLifetimePerc=0.25` as `0.25%` instead of legacy's `75%` minimum. Each Vec3 cluster across the inspector and Spawner panel now carries X/Y/Z micro-labels above its three spinners. The Spawner panel scrolls its body when content overflows. Four inspector fields drop from the UI per source-resolved decisions (`nTriangles`, `weatherFadeoutDistance`, `groups[1]` Lifetime random-param, `index`); all four stay on the wire so existing `.alo` files round-trip losslessly.

**How we tackled it.** Most architectural depth lives in the spec at [`docs/superpowers/specs/2026-05-20-b1-3-tab-parity.md`](docs/superpowers/specs/2026-05-20-b1-3-tab-parity.md) and the plan at [`tasks/todo.md`](tasks/todo.md). The work executed bottom-up through eight phases via `superpowers:subagent-driven-development`. P2 added `displayInvertedPercent` on `FieldSpinner` first, standalone, with focused unit specs so the math (`displayed = 100 - value * 100` on render; `value = (100 - displayed) / 100` on commit) and round-trip were proven before any consumer adopted it. P3 landed the tri-state Generation mutex on the *unchanged* Basic-tab structure so the radio chrome's diff was isolated from the larger tab-restructure diffs that followed; a P3-fix follow-up extracted a hand-rolled `RadioRow` component and added `role="radiogroup"`, roving `tabIndex`, and arrow-key cycling once code review caught the a11y gap. P4 / P5 / P6 ([`07c88c4`](https://github.com/DrKnickers/particle-editor/commit/07c88c4) / [`c894a2b`](https://github.com/DrKnickers/particle-editor/commit/c894a2b) / [`8b41ea5`](https://github.com/DrKnickers/particle-editor/commit/8b41ea5)) restructured each tab in turn — each wraps existing field primitives in `Section` shells (the B1.2 collapsible chevron) and renames `GroupSection` to `GroupBody` so the new shape is "`Section` carries the title; `GroupBody` wraps the inside with no fieldset/legend chrome". Five questions resolved during the prep round by reading `src/UI/Emitter.cpp` and `src/ParticleEditor.en.rc` directly (Q1 `index`: drop; Q2 `nTriangles`: drop, retain in schema; Q3 `weatherFadeoutDistance`: drop, retain; Q4 `groups[1]` Lifetime random-param: drop, retain; Q5 trailing colons: yes, fields only) — every dropped field's bridge surface stays intact. The atomic two-key patch shape on the Generation mutex piggybacks on `EmitterPropertyTabs.tsx`'s existing `commit(Partial<EmitterPropertiesDto>)` helper at [`EmitterPropertyTabs.tsx:196`](web/apps/editor/src/screens/EmitterPropertyTabs.tsx:196) — a single bridge request carrying both `useBursts` and `isWeatherParticle` flips both flags in one host-side mutation. P7 reconciled the spec corpus: 1 spec converted from a `.todo` marker to a real absence-assertion on `Triangles`; 2 specs in [`tests/property-tabs.spec.ts`](web/apps/editor/tests/property-tabs.spec.ts) updated for the "Lifetime" → "Maximum lifetime:" and "Gravity" → "Gravity acceleration:" label renames. Post-P7 the user smoke-tested the build and flagged five visual issues — dark scrollbar on the inspector Tabs.Content; form-row template too tight for the unit-suffix column; Vec3 clusters cramped without per-axis labels; texture inputs truncating their filenames; SpawnerPanel body not scrolling. Folded into two polish commits ([`3ae940e`](https://github.com/DrKnickers/particle-editor/commit/3ae940e), [`82917f0`](https://github.com/DrKnickers/particle-editor/commit/82917f0)) — new `.form-row.form-row-cluster` modifier (60px label + 1fr cluster) for Vec3 multi-spinner rows, `.axis-cell` + `.axis-lbl` for per-axis X/Y/Z micro-labels above every Vec3 cluster (PhysicsTab Vec3Row + Acceleration, AppearanceTab RGBA, all four SpawnerPanel Vec3 sections), spinner-cell tuned from 56→52→58 with unit-cell 32→40, texture inputs widened 92→180px with font 12→11, label hover-tooltips via `title=`, `.panel h-full` on the SpawnerPanel root so its body claims the remaining flex height.

**Issues encountered and resolutions.** *Pre-existing percent-display bug discovered during Q2 source resolution.* While reading `src/UI/Emitter.cpp` to map `IDC_SPINNER2` to a schema field, the legacy "Minimum lifetime:" semantic (`displayedPercent = 100 - perc * 100`) became visible — and immediately surfaced that the new UI was rendering `randomLifetimePerc=0.25` as the literal `0.25` instead of as `75` (the minimum-percentage interpretation). Same on `randomScalePerc`. Could have shipped as a separate dispatch; instead bundled into B1.3 as `displayInvertedPercent` in P2 with focused unit specs so the math is provable in isolation before either consumer reaches for it. *P3 code review caught the hand-rolled radios missing every a11y requirement.* The initial P3 commit implemented three side-by-side `<input type="radio">` elements without a `role="radiogroup"` wrapper, without roving `tabIndex`, and without arrow-key cycling. Caught in the P3 two-stage review; fixed in [`b929e47`](https://github.com/DrKnickers/particle-editor/commit/b929e47) by extracting a `RadioRow` component that owns the radiogroup wrapper + `onKeyDown` arrow handler + roving tabIndex (focused radio = `tabIndex=0`, others = `tabIndex=-1`). *P6 code review caught the weather-disable cascade inverted on three fields.* The initial P6 commit's weather-mode-disables-Physics logic disabled Parent speed inherit / Inward speed / Affected by wind all three when `isWeatherParticle === true`. Legacy `src/UI/Emitter.cpp:175-190` actually disables only Parent speed inherit under weather; Inward speed and Affected by wind stay enabled. Fix landed in [`3b191fd`](https://github.com/DrKnickers/particle-editor/commit/3b191fd) once the reviewer cross-referenced the disable cascade against the legacy source line-by-line. *Playwright suite was label-coupled in two specs the spec hadn't anticipated.* B1.3's §5 + §8 stated "Playwright native tests untouched" based on the assumption that the suite "asserts at structural / selection level". Reality: [`tests/property-tabs.spec.ts`](web/apps/editor/tests/property-tabs.spec.ts) had hard-coded `getByLabel("Lifetime")` and `getByLabel("Gravity")` references that broke when P3 + P6 renamed those fields. Caught at P7 when `pnpm test:native` went red. Updated to the new label text in P7 itself; captured as **lessons.md** so the next label-rename dispatch sweeps both vitest AND Playwright suites. *Five user-flagged visual issues from post-P7 smoke test.* Dark scrollbar didn't reach inside the inspector's Tabs.Content; form-row label cell truncated labels on hover-less terminals; Vec3 cluster cells were too narrow to read three spinners with their unit suffixes; texture-input filename column was 92px and clipped 90% of real filenames; SpawnerPanel `.panel` didn't have `h-full` so its body didn't scroll when content overflowed. None caught by tests (specs query by aria-label, not by pixel layout). Folded into two polish commits.

Test count: vitest **277 / 277** (was 254 at B1.2 close; +23 net across the new specs and the test-corpus reconciliation), Playwright **83 / 83** throughout.

---

### Left-pane polish (B1.2) — collapsible sections, Name input width, toolbar Duplicate + icon Show/Hide All

*2026-05-20 · [`d69e7cc`](https://github.com/DrKnickers/particle-editor/commit/d69e7cc) · #92*

Tightens the left pane's interior fidelity against the design
source. New `Section` primitive at
[`src/components/Section.tsx`](src/components/Section.tsx) (entire
header row clickable, plus Enter/Space when focused; defaults to
`defaultOpen=true`; session-only state — re-mounting on emitter
selection re-expands every section). BasicTab gains three section
groupings (Emitter Timing / Generation / Connection) matching the
design source's `left_panel.jsx` layout; field set unchanged
(19 fields total). Name field gets a custom `60px 1fr` grid override
expressed as a new `.form-row.name-row` modifier class in
[`components.css`](src/styles/components.css) (matching the existing
`.full` / `.with-radio` / `.with-check` convention), so the text
input fills available width; `FieldText` learns a small
`wide?: boolean` prop so callers can embed it in a custom-grid row
without the default `.form-row` wrapper. Tree toolbar gains a
Duplicate button between New ▾ and Delete (dispatches the existing
`emitters/duplicate`; disabled when no primary is selected). Show All
/ Hide All become Lucide `Eye` / `EyeOff` icon buttons; tooltips
preserve the full text.

**How we tackled it.** Section primitive is ~40 lines:
`useState<boolean>`, `role="button" tabIndex={0}` with `onKeyDown`
for Enter/Space (preventDefault on Space to suppress page scroll),
and `aria-expanded` reflecting state. The `data-testid` is derived
from the title so individual sections are test-addressable. The
intentional reset-on-mount behaviour means switching emitters
re-expands sections — documented as a comment in the component
with the upgrade path (lift state or per-tab persistence map) if
the trade-off proves wrong in real use. BasicTab's restructure is
purely wrapping: existing field components untouched, only their
parent containers change. The Name row sits outside any Section
(top-of-tab) using the new `.form-row.name-row` modifier — the
inline `style={{ gridTemplateColumns: "60px 1fr" }}` first
implementation was caught in code review as breaking the existing
`.form-row.*` modifier-class convention and refactored to the
class-based form. `FieldText`'s `wide` prop is a four-line diff:
extract the `<input>` into a local variable, return it directly if
`wide`. The toolbar's Duplicate button uses the existing
`emitters/duplicate` bridge surface (consumed by the context-menu
Duplicate item before this) and the existing `TOOLBAR_BTN`
className for visual consistency. Show All / Hide All swap from
custom text-button classNames to `TOOLBAR_BTN` with `Eye` /
`EyeOff` icons. CSS audit found everything already in sync with
the design source — no commit needed (P1 was a no-op).

**Issues encountered and resolutions.** *Nested `<button>` →
`<span role="button">` (legacy of B1's Task 5).* Not new here, but
mentioned for context: the per-row visibility eye established
in B1 is the basis for visual disambiguation against the new
toolbar Eye / EyeOff. The toolbar uses `size-4` in a `w-6 h-6` cell
with `text-text-2`; the per-row eye uses `size-3` in a `w-4 h-4`
cell with `text-text-3`. Different size, brightness, and
cardinality (paired action row vs per-row toggle) plus tooltips
disambiguate.
*Inline style vs modifier class.* The first BasicTab restructure
implementation used `style={{ gridTemplateColumns: "60px 1fr" }}`
inline on the Name row. Code review flagged this as breaking the
established `.form-row.full` / `.with-radio` / `.with-check` modifier-
class convention; refactored in a follow-up commit to use a new
`.form-row.name-row` class. The plan documented inline style
explicitly; reality is that the modifier-class form is more
idiomatic with the surrounding code.
*Spec assumed a toolbar divider that doesn't exist.* The plan
listed the toolbar as `New ▾ │ Duplicate │ Delete │ Move Up │
Move Down │ divider │ Show All │ Hide All`. The divider was
correctly removed in B1's Task 7 when the eye-toggle button (the
divider's left neighbour) was dropped. The plan's "divider
unchanged" assumption was stale. Current toolbar has 7 buttons
without a divider, which actually matches the design source's
`tree-actions` (no divider in the design either).

Test count: vitest **254 / 254** (was 239 at B1 close; +15 across
all the new specs: 8 from Section, 3 from BasicTab restructure, 3
from Duplicate, 1 from Show/Hide icons), Playwright unchanged at
**83 / 83**.

---

### Left-pane realignment (B1) — tree toolbar at bottom, per-row eye, multi-lane bracket gutter

*2026-05-20 · [`7e54015`](https://github.com/DrKnickers/particle-editor/commit/7e54015) · #92*

Realigns the left pane against the design source's structural
intent. Specifically: the tree toolbar moves from above the
`<ul>` to below it and restyles to match `.tree-actions` (banded
hairlines top + bottom); each tree row gains a per-row 👁
visibility eye, and the toolbar's primary-only eye toggle goes
away as redundant; the per-row sky-blue link-group dot is
removed in favour of the gutter brackets alone (legacy parity);
the hard `border-t` between tree region and inspector is gone,
with the tab strip's underline as the natural transition. Each
row is now a 3-column CSS grid `[12px glyph] [1fr name] [18px
eye]` so eyes column-align automatically across all rows. The
bracket gutter gains aggressive-reuse multi-lane support — when
groups interleave, brackets pack into multiple lanes via greedy
first-fit and the gutter widens accordingly; when groups are
sparse, all brackets reuse lane 0 and the gutter stays narrow.
Single-member link groups are now filtered at the render layer
so no group ever appears as a single-row stub.

**How we tackled it.** Two layers, both small. Layer 1:
[`link-group-colors.ts`](src/lib/link-group-colors.ts) extends
`LinkGroupBracket` with a `lane: number` field and adds a third
pass to `computeLinkGroupBrackets` that assigns lanes via greedy
first-fit (sort by `firstRowIndex`; for each bracket pick the
lowest lane whose `lastEnd` is strictly less than the bracket's
`firstRowIndex`; push a new lane if none free). The same
function gains a `count < 2` skip so single-member groups never
emit a descriptor. Companion `laneCount` export lets the
renderer compute the gutter's container width without an inline
reduce. Layer 2:
[`EmitterTree.tsx`](src/screens/EmitterTree.tsx) converts the
per-row container from flex to a 3-column CSS grid, adds the
per-row eye as a `<span role="button" tabIndex={0}>` (with
`stopPropagation` to keep visibility-toggling from re-selecting
the row, plus `onKeyDown` for Enter/Space activation — using a
nested `<button>` would have been invalid HTML), removes the
per-row link-group dot span, moves the `<EmitterTreeToolbar>`
from above the `<ul>` to after it, restyles its outer container
to `.tree-actions`, drops the eye-toggle button + its helpers,
and rewrites the gutter renderer to size by `laneCount * 10 +
4px` (or `4px` minimum) and position each bracket by `left =
4 + lane * 10`. The hard `border-t border-border` on the
inspector wrapper in [`App.tsx`](src/App.tsx) goes away as a
one-line edit.

**Issues encountered and resolutions.** *Nested `<button>` HTML
invalidity.* The plan literally specified `<button>` for the
per-row eye, which would nest inside the row's outer `<button>`.
Real browsers hoist the inner button out during parsing,
scrambling layout and event order. Caught in self-review during
Task 5; fixed in a follow-up commit by switching to
`<span role="button" tabIndex={0}>` with explicit
`onClick`/`onKeyDown` handlers. *Existing test fixture broken by
the single-member filter.* The original
[`link-group-colors.test.ts:30`](src/lib/__tests__/link-group-colors.test.ts:30)
asserted that a single-row group produced a bracket — now
filtered out. Fixture rewritten with a 2-member group plus a new
single-row group that the filter rejects, asserting the new
behaviour end-to-end. *Unused helpers after dropping the toolbar
eye-toggle.* Removing the toolbar's eye button left
`primaryVisible`, `EyeGlyph`, and `toggleVisibility` unused;
TS strict mode catches this and refuses to build. Cleanup
landed as part of Task 7.

Test count: vitest **239 / 239** (was 221; +18 across all the new
specs), Playwright unchanged at **83 / 83**.

---

### Curve editor polish: lock-to feature, axis labels, theme-aware grid, spinner improvements, Spawner panel bleed-through fix

*2026-05-20 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Single dispatch that grew during interactive smoke-testing — what started as "fix the Spawner panel showing the DirectX clear colour" turned into a round of curve-editor polish driven by the user testing each surface and reporting what looked off. Net effect: the curve editor is now genuinely usable end-to-end. Lock-to is functional (color channels can be aliased per the legacy pointer-identity model). Axis labels render correctly per focus channel. Spinners are robust (visible arrows, wheel works anywhere over them including arrows, doesn't leak scroll to the parent pane). Theme-aware grid colours mean the light-theme grid doesn't fight curves any more. No engine-side production code beyond a single Lock-to handler addition.

**How we tackled it.** Each item came up via user smoke-testing and got fixed in sequence; the through-line is "look at it, name what's wrong, fix the smallest correct shape, iterate." A few of the surfaces moved more than once because the first interpretation of "what's wrong" was incomplete — captured in the Issues section below.

- **Spawner panel DirectX bleed-through.** Right-column aside in `App.tsx` workspace grid was transparent. With the layered viewport popup sitting under the WebView2 chrome, any transparent chrome region shows the popup's clear colour. Added `bg-panel` to the aside ([`App.tsx`](web/apps/editor/src/App.tsx)).
- **Curve editor strip layout.** Channel list (7 rows) wouldn't fit the 260px strip; Index row clipped off the bottom; scrollbar absent because the CSS Grid `1fr` row inherited `min-content` from content and refused to shrink. Fixed in three escalating steps:
  - `.curve-editor` row template `1fr` → `minmax(0, 1fr)` so the body row can drop below content height ([`components.css`](web/apps/editor/src/styles/components.css)).
  - Strip height `h-[260px]` → `h-[290px]` so six of the seven channels fit naturally and only Index needs scrolling.
  - `.curve-editor` flex shape: `flex: 1` inside the panel's flex column instead of `height: 100%`, so the body row claims *remaining* space below the panel-header rather than overflowing it.
- **Canvas right-edge clip + axis label region.** `.ce-body` column template `180px 1fr` → `180px minmax(0, 1fr)` for the same reason (SVG's 600px intrinsic viewBox width was pushing the canvas cell past its bounds). Added 12px wrap padding around the canvas-wrap to make space for axis labels.
- **Per-channel value-range rules** (after multiple iterations). `valueRangeForTrack` now:
  - RGBA: fixed `{0, 1}` (the engine hard-clamps these).
  - Scale: `{0, max(max-of-keys, 1)}` — upper bound tracks the highest key, floor at 1 so a flat-zero curve isn't a degenerate range.
  - Index: same shape as Scale.
  - Rotation: `{min(0, min-of-keys), max(1, max-of-keys)}` — expands in BOTH directions with no caps; the previous design with a ±1 ceiling was wrong per the user's spec.
- **Spinner-bounds vs display-range split.** The Value spinner used to clamp to the focus channel's display range, which created a deadlock: user couldn't push a key value past the current max because the spinner wouldn't accept it. Introduced `spinnerBoundsForTrack(name)` returning engine-allowed bounds (`{0, 1}` for RGBA, `{0, 1e6}` for Scale/Index, `{-1e6, 1e6}` for Rotation), with `step: 1` for Index (integer-only nudges). Display range adapts as keys change; spinner clamp is constant per channel.
- **Lock-to feature wired end-to-end.** Was a UI stub previously — dropdown rendered but `setLockTo` only updated local React state, nothing reached the bridge. Full implementation:
  - New schema kind `emitters/set-track-lock` ([`bridge-schema/src/index.ts`](web/packages/bridge-schema/src/index.ts)).
  - C++ handler that swaps `emit->tracks[channelIdx]` to point at the target channel's `trackContents`, matching the legacy `TrackEditor.cpp:178-198` `CBN_SELCHANGE` semantics. Mark dirty + emit tree-changed.
  - `TrackDto.lockedTo` field; the dispatcher's `emitters/get-tracks` computes it from pointer equality (`tracks[i] == &trackContents[j]`).
  - React dropdown reads `focusedTrack.lockedTo` (derived state, not local) and dispatches `set-track-lock` on change. Edit affordances (Insert, Linear/Smooth/Step, Delete) disable while the focus channel is locked.
  - "Lock to:" label added before the dropdown.
- **Toolbar icons.** Text labels (Select / Insert / Linear / Smooth / Step / Delete) → icons. Lucide for Select (`MousePointer2`), Insert (`Plus`), Delete (`Trash2`); inline 16×16 SVG glyphs for the three interpolation modes (no lucide match for "linear-curve-between-two-keys" / "step-curve-between-two-keys"). `flex-wrap: wrap` on `.ce-toolbar` + `grid-template-rows: auto minmax(0, 1fr)` on `.curve-editor` as a graceful narrow-window fallback (compresses to single row at normal widths).
- **Spinner improvements** ([`primitives/Spinner.tsx`](web/apps/editor/src/primitives/Spinner.tsx)):
  - Up/down arrows always visible (matching legacy Win32 `UDS_ALIGNRIGHT` — the prior "hover-only" was a design drift, not a legacy port).
  - Wheel handler attached natively (`addEventListener("wheel", ..., { passive: false })`) instead of via React's `onWheel`. React 18+ attaches its delegated wheel listener as PASSIVE, which makes `preventDefault()` a no-op and lets the browser scroll the parent pane before our handler can stop it. Native attachment with `{ passive: false }` re-enables preventDefault.
  - Native wheel listener on the outer wrapper (not just the input) so the wheel works anywhere over the spinner, including the arrow column.
- **Curve editor canvas details:**
  - Axis labels are HTML (`<span>`), not SVG `<text>`. `preserveAspectRatio="none"` stretches the SVG non-uniformly which would distort text glyphs. HTML labels live in a CSS grid sibling cell.
  - Y-axis labels: max / midpoint / min, with a special "0" label added at its actual position when the range strictly crosses zero (so e.g. Rotation `{-0.5, 1}` shows `1 / 0.25 / 0 / -0.5`).
  - X-axis labels: fixed `0 / 25 / 50 / 75 / 100`.
  - Theme-aware grid colour via new `--curve-grid` / `--curve-axis` CSS variables ([`tokens.css`](web/apps/editor/src/styles/tokens.css)). Dark theme: existing `#262626` / `#525252`. Light theme: `rgba(0,0,0,0.25)` / `rgba(0,0,0,0.45)`.
  - `overflow="visible"` on the SVG so endpoint key circles at time=0 / time=100 / value=min / value=max draw their full body even when the centre sits on the grid edge (was being bisected by the SVG viewBox clip).
  - Slightly thicker curves (focus 3, non-focus 2 — were 2.5 / 1.5) and larger key markers (5/6 — were 4/5).

**Architectural decisions worth recording.**

1. **Pointer identity as lock-state source-of-truth (legacy model preserved).** The legacy `ParticleSystem::Emitter` uses `Track*` aliasing — `tracks[GREEN] = &trackContents[RED]` means Green's display IS Red's data. Saving + loading already handles this via the file-load consolidation pass at [`ParticleSystem.cpp:428`](src/ParticleSystem.cpp:428). The new bridge surface just exposes this state via `TrackDto.lockedTo` (derived from pointer equality) and a `set-track-lock` request that swaps the pointer. No new state primitive on the engine side; persistence is automatic.
2. **Display range vs spinner-bounds split is the real fix for "stuck spinner".** Conflating these caused the user-reported "spinner caps Scale at 20" bug — display range derived from existing keys means the user can never push past the current max because the spinner won't supply a larger value. Constant per-channel spinner bounds break the deadlock and let the display range track new key values.
3. **`overflow="visible"` is an SVG attribute, not just CSS.** Setting it as a CSS rule on the SVG element is documented as having different behaviour than the SVG attribute; using the attribute is reliable. Mentioning here because the project's other SVG-heavy components might want to apply the same pattern.
4. **HTML labels around stretchy SVG.** `preserveAspectRatio="none"` is the right choice for the curve/grid content (it stretches naturally with the cell), but it's catastrophically wrong for any glyphs inside the SVG. Pattern: HTML labels positioned in a CSS-grid sibling cell, SVG handles only the lines/curves. Worth documenting for similar future surfaces.
5. **Theme-aware SVG attributes via CSS variables.** `stroke="var(--curve-grid)"` works as an SVG presentation attribute; the value resolves through the same CSS variable cascade as Tailwind utilities. No JS prop drilling needed for theme switching.

**Issues encountered and resolutions.** Worth recording the iteration loops:

1. **Index curve range took 4 iterations.** First: `{0, 10}` floor → value=0 hugged the bottom edge invisibly. Second: `{-1, 10}` → user couldn't see the 9% margin clearly. Third: `{-3, 10}` → user said "I'm not seeing what I expect at value=0" because the labels said `10 / 3.5 / -3` (no "0" label). Fourth: dropped the negative padding, added a dedicated "0" label that sits at the actual zero-position regardless of where the midpoint is. The takeaway: when the user can't tell where value=0 is, the fix is to LABEL value=0 explicitly, not to nudge its rendering position.
2. **Rotation spec changed mid-session.** Initial spec: cap at ±1. After observation: user wanted no caps, auto-grow in both directions. The cap was a legacy thinking-bias on my part — the new spec is more consistent with Scale/Index.
3. **Grid clipping at value=0 was wrong.** I tried clipping the grid to only show in the positive-value region, hiding grid below the channel's `value=0`. User noticed the resulting "the grid moves around per focus channel" inconsistency and pushed back. Reverted; the grid is now uniform per the focus range, with axis labels carrying the per-channel scale information.
4. **SVG-text axis labels were illegible.** Briefly tried rendering labels as `<text>` inside the SVG — they got stretched/squashed by `preserveAspectRatio="none"`. Switched to HTML labels in a CSS-grid sibling cell. Should have been the first instinct.
5. **The "scroll wheel scrolls the parent pane" bug** required diagnosing React 18+'s passive-by-default behaviour for wheel listeners. The `onWheel={handler}` with `preventDefault()` *looked* correct (and worked in vitest's jsdom), but did nothing in the actual WebView2 runtime because the listener was passive. Documented in lessons (would have caught this faster). Native `addEventListener("wheel", ..., { passive: false })` is the fix.

---



*2026-05-20 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

The single failing native spec from the post-Phase-2 handoff (`Clicking a bundled ground slot in the popover updates groundTexture`) is fixed at the engine layer; native Playwright suite is back to 83 / 83. The user-visible effect was: in `--test-host` mode, after specific cross-spec bridge sequences, the engine refused to apply *any* `engine/set/ground-texture` mutation — every slot set returned `ok: {}` but the snapshot's `groundTexture` stayed at 0. Interactive users never saw the symptom because the render loop's recovery papered over the underlying device-not-reset state on the next `WM_PAINT`. No user-facing UI changes; the fix is purely engine + a small defense-in-depth addition in `LayoutBroker`.

**How we tackled it.** The previous handoff floated a *convergent* hypothesis — the spec failure and a separate Debug-CRT `abort()` dialog the user observed were the same `_ASSERTE` on the ground-texture chain after Task 2.4's spawner-column layout change. Wrong on the convergence claim (no `_ASSERTE` reproduces in `--test-host`; whatever the `abort()` was, it lives somewhere else) but the bisect-to-Task-2.4 was correct. Bisecting across spec pairs first narrowed the failure to the combination `background-picker.spec.ts:41` (opens Background popover without dismissing; calls `SetSkydomeSlot(5)`) × `spawner-import-mod.spec.ts` (toggles the new Spawner permanent column via Zustand+localStorage, which resizes the workspace grid and fires a fresh `layout/viewport-rect`). An initial React-portal-event-delegation hypothesis was credible but wrong — the `props.onClick({})` direct-call test showed the React side was fine; the engine itself was the cause. We caught that detour by re-running the proposed "programmatic dispatch" rewrite under the same failing conditions before relying on it: *that fix also failed*. The diagnostic that finally localised it was a `engine/debug/d3dx-canary` bridge handler (now removed) that the reproducer called between each step, capturing `TestCooperativeLevel` + procedural `CreateTexture` + D3DX-from-RCDATA HRESULTs. The first failing step was unambiguous: the very first Spawner toolbar toggle click, which left the device at `TCL=0x88760869 D3DERR_DEVICENOTRESET`. From there: `Spawner toggle → layout/viewport-rect → LayoutBroker::Apply → m_engine->Reset() → m_pDevice->Reset()` returns `D3DERR_INVALIDCALL` (`0x8876086C`) because `m_pSkydomeEffect` was holding `D3DPOOL_DEFAULT` references from the prior `SetSkydomeSlot(5)`. `Engine::Reset` does the standard `OnLostDevice`/`OnResetDevice` dance for the regular shaders, the distort shader, and the bloom effect — but **forgot the skydome effect**, added later in the work. `LayoutBroker::Apply`'s `catch (...) { /* swallow */ }` discarded the throw, the device latched at `DEVICENOTRESET`, and `Engine::Render`'s next-frame recovery never ran because the viewport HWND is hidden in `--test-host` (no `WM_PAINT`, no Render() tick). The fix is **two lines** in [`engine.cpp`](src/engine.cpp:1360): `m_pSkydomeEffect->OnLostDevice()` before `m_pDevice->Reset(...)` and `OnResetDevice()` after, matching the existing pattern around them.

**Architectural decisions worth recording.**

1. **Defense-in-depth: `Engine::RecoverDeviceIfNeeded` + `LayoutBroker` fallback.** Beyond the one-line skydome fix, we extracted the render-loop's `TestCooperativeLevel` / `Reset` dance into a public `Engine::RecoverDeviceIfNeeded()` method (declared in [`engine.h`](src/engine.h:123), implemented in [`engine.cpp`](src/engine.cpp)) and have `LayoutBroker::Apply` call it on the catch path. In the fixed state this is a no-op — `Reset()` succeeds, the catch never fires. The value is that *any future* "forgot to OnLostDevice some new D3DPOOL_DEFAULT resource" regression heals on its own instead of latching the way this one did. Documented inline with a pointer back to this entry.
2. **The `--test-host` mode never pumps the render loop.** The viewport HWND is hidden so `WM_PAINT` doesn't fire and `Engine::Render` never ticks. Anything that depends on render-loop pumping for self-healing (`TestCooperativeLevel` recovery, transient-state cleanup) silently doesn't run under tests. Worth keeping in mind as the suite grows — any new engine state that depends on render-loop refresh should either be made explicit (a dedicated bridge call) or have a non-render-loop recovery path.
3. **The `abort()` user observation from the prior handoff stays unconfirmed.** Not reproduced anywhere this session. Could be a separate code path we didn't hit, or could have been a stale capture. Either way, it isn't the same bug as `:192`.

**Issues encountered and resolutions.** Three worth recording.

1. **The natural-looking React-side "fix" was wrong.** All the React fiber diagnostics painted a tempting picture: portal'd content, `bridge !== window.bridge`, `props.onClick` ran but no `engine/set/ground-texture` request hit the wrapped `window.bridge.request`. Shipping that rewrite would have masked the real bug. The save was running the rewrite under the polluter scenario before relying on it. Filed as `tasks/lessons.md`: when a "narrow rewrite" is proposed for a failing test, verify the rewrite *in-situ* under the failing scenario before declaring it a fix — peer rule to `pnpm build` truth gate.
2. **Host-process `printf` doesn't reach `pnpm test:native` stdout.** The harness uses `stdio: "inherit"` but the binary is a Windows GUI-subsystem process; `printf` to stdout silently vanishes. Earlier `[host]` printfs were visible because they go through a different path. Replaced the `printf` diagnostics with a small helper that writes to both `OutputDebugStringA` (DebugView++ channel) *and* a logfile at `%TEMP%\gtdbg.log`; capture worked instantly without GUI involvement. The helper is gone with the rest of the instrumentation but the pattern is documented in for next time.
3. **The canary-handler shape is reusable.** `engine/debug/d3dx-canary` — three calls (`TestCooperativeLevel`, procedural `CreateTexture`, D3DX-from-RCDATA), one per bridge round-trip — turned "the engine seems wrong, when?" into a step-by-step bisect. Round trip from "test fails" to "HRESULT identified" was under 30 minutes. Both pieces (the canary handler in `BridgeDispatcher.cpp` and the `gtdbg.log` helper in `engine.cpp`) were removed once the bug was fixed, but anyone debugging a similar D3D9 latch should re-add them — the shape is documented in.

---


> **Note on the / new-UI entries below.** These were developed on
> the long-lived `integration-branch` integration branch and landed on `master` in one
> **supersede merge** — PR #92,
> merge-commit [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36)
> (2026-06-08). Because dozens of features arrived through that single
> merge rather than one PR each, the provisional `TODO` hash/PR fields
> were backfilled uniformly to `f05fa36` / `#92` (the master merge-commit,
> per the `CLAUDE.md` convention); a few entries keep their precise `integration-branch`
> commit hash where one was already recorded. The per-entry **dates**
> remain the original development dates to preserve the timeline.

### Particle Editor 2026 redesign — Phase 2 structural moves (Phase 2.1–2.7)

*2026-05-19 · [`f05fa36`](https://github.com/DrKnickers/particle-editor/commit/f05fa36) · #92*

Seven sub-commits restructure the new-UI workspace into the Particle Editor 2026 layout. The toolbar is reorganised into four design groups (File actions · Playback · Spawner toggle · spacer · Environment), Background and Ground Texture move from sliding `ToolPanel` overlays to Radix Popover dropdowns triggered from Group 4, the Spawner becomes a permanent right column (toggleable from the toolbar button), Basic / Appearance / Physics inspector tabs sit beneath the EmitterTree inside a unified `.panel` chrome on the left, the curve editor moves to an always-on bottom 260px panel with a multi-channel overlay (Phase 2.8 restored the edit surface on top of that), and a new top-left viewport pill exposes three engine toggles (Show ground · Toggle bloom · Leave particles after instance death — the last is a new bridge surface). The View menu drops the now-redundant "Background…" and "Ground Texture…" entries; the Emitters menu's "Spawner…" item is repurposed to toggle the column instead of opening a slide-in. F7 still toggles the Spawner column.

**How we tackled it.** Each task lands as its own commit so the suite stays green at every boundary. **2.1** rewrites `Toolbar.tsx` to use the design's semantic classes from `components.css` (`.toolbar` / `.tb-group` / `.tb-btn` / `.tb-divider` / `.tb-spacer`); removes Undo/Redo/Bloom/Reload toolbar buttons (still in the menubar); adds Save As and Step 10; introduces the `useSpawnerVisibility` per-component hook (upgraded to a Zustand store in 2.4). **2.2** replaces `BackgroundPicker`'s slide-in `ToolPanel` with a `BackgroundDropdown` + new `OccludingPopover` (generalisation of `OccludingMenubarContent` so the popover registers as a viewport occlusion); `BackgroundPickerBody` is extracted as a named export and rendered inside the popover. **2.3** mirrors that pattern for `GroundTexturePanel` → `GroundDropdown` + `GroundTexturePanelBody`. **2759c27** removes the dead View-menu "Background…" / "Ground Texture…" entries left in place during 2.2/2.3 for diff scope. **2.4** upgrades `useSpawnerVisibility` to a Zustand store (`lib/spawner-visibility.ts` exports `useSpawnerVisible` / `useToggleSpawner` / `toggleSpawner` + a `useSpawnerVisibility` compat shim + `__resetSpawnerVisibilityForTests`), drops the `ToolPanel` chrome from `SpawnerPanel` in favour of `.panel` / `.panel-header` (X-close button calls `toggleSpawner` directly) / `.panel-body`, and adds the panel as a permanent right column in `App.tsx`'s workspace grid (3-column when visible, 2-column when hidden). The Emitters menu's "Spawner…" item is rewired to `toggleSpawner` so F7 still works. **2.5** wraps the left column in `.panel` chrome with header "Particle System" and converts the 46-ish form rows across the Basic / Appearance / Physics tabs to the design's `.form-row` 3-column grid (label / input / unit); the existing `FieldText` / `FieldSpinner` / `FieldCheckbox` primitives absorb the conversion so the 18 spec assertions still pass without rewrites. Multi-spinner clusters (Random Colours, Acceleration, Vec3Row) span columns 2+3 inline via `gridColumn: "2 / span 2"` — a tactical workaround, candidate for a `.form-row.cluster` variant later. **2.6** creates `CurveEditorPanel.tsx` as an always-on 260px bottom panel inside the centre column with the design's 7-channel curve-list (Scale / R / G / B / A / Rotation / Index — Index defaults off) and a multi-channel SVG overlay rendering one `<g data-testid="curve-layer-${id}">` per visible channel. The lossy decision in 2.6 was deleting `TrackEditor.tsx` (866 lines) + `EmitterPropertyPanel.tsx` (176 lines) entirely; Phase 2.8 restores the edit surface on top of this rendering substrate. **2.7** adds the viewport pill component (`vp-tools` class with three `.tool` buttons using `icon-ground.svg` / `icon-bloom.svg` / `icon-particles.svg` from the design bundle) and the new `engine/set/leave-particles` bridge surface end-to-end — schema adds `EngineStateDto.leaveParticles`, MockBridge mirrors it through `mock-state.ts` + `mock.ts`, the C++ dispatcher in `src/host/BridgeDispatcher.cpp` wires read/write to the existing `ParticleSystem::getLeaveParticles()` / `setLeaveParticles()` methods (the field has been chunk-serialised at `src/ParticleSystem.cpp:948` and honoured at `Engine::KillParticleSystem` (`src/engine.cpp:197`) for a long time — the runtime path is real, not a placeholder).

**Architectural decisions worth recording.**

1. **Each task ends in its own commit.** Phase 1 was a single squashed commit because the work was a uniform token sweep; Phase 2 sub-tasks are structurally distinct and easier to bisect or revert independently when something goes wrong (see "Issues encountered" below — exactly what happened mid-phase).
2. **Spawner visibility is a Zustand store, not a per-component hook.** The toolbar Spawner button, the X-close glyph on the panel, the workspace grid, the menu item, and F7 all converge on the same toggle. The hook landed first in 2.1 as a useState placeholder (only the toolbar button needed it then); upgraded to a store in 2.4 once the X-close and grid layout came online.
3. **Dropdown popovers use `OccludingPopover`, not Radix's stock `Popover.Content`.** The viewport popup is's layered window with software alpha-stamp cut-outs at chrome occlusion rects; if the dropdown didn't register itself as an occlusion the popover would render BEHIND the engine viewport. The new wrapper takes the same `(bridge, occlusionId)` props as the menubar's `OccludingMenubarContent`, with matching 24px padding + smoothstep feather to enclose the `shadow-xl` drop shadow.
4. **`engine/set/leave-particles` reuses the existing ParticleSystem field.** The runtime semantics (don't kill live particles when the spawner instance dies) were already implemented and serialised. The new bridge surface just exposes the flag to the React UI. Saved a meaningful amount of C++ work and avoids divergence between the bridge value and the legacy file format.

**Issues encountered and resolutions.** Three worth recording.

1. **Task 2.4 silently broke `tools.spec.ts:192` ("Clicking a bundled ground slot in the popover updates groundTexture").** The Spawner permanent column added 320px on the right of the workspace; the Ground popover's `align="end"` Radix positioning interacted with the new layout such that the bundled-slot click no longer dispatches `engine/set/ground-texture` (or dispatches but is racing past the 300ms snapshot wait). The dispatch's implementer reported it as pre-existing because a `git stash` + checkout didn't actually rebuild the dist bundle — the test was running against the post-2.4 dist while the source pointed at the pre-2.4 commit. Bisected post-hoc: passes at 2a77249 (Task 2.3 tip), passes at 2759c27 (cleanup), fails at 17768b6 (Task 2.4) and every commit since. The failure is also strongly suspected to be the same `abort()` debug-CRT dialog the user reported during a Playwright run — a paused host process at an `_ASSERTE` would naturally produce the "click didn't take effect within 300ms" symptom. Tracked as a follow-up; needs DebugView++ to capture the assertion text. The Phase ships with 78 / 79 native specs passing (one failing test left untouched per the dispatch contract).
2. **Task 2.6 deleted too much — entire curve editor edit surface vanished.** The implementer chose Option A (delete TrackEditor.tsx + EmitterPropertyPanel.tsx) over Option B (keep TrackEditor inside the new panel) because Option A was framed as a "tactical move" in the dispatch prompt. The result was a view-only multi-channel overlay with no Time / Value spinners, no marquee select, no drag, no Insert mode, no per-key context menu, no interpolation toggle. Phase 2.8 restores the edit surface on top of the multi-channel overlay via a *hybrid focus-channel* model — see the separate Phase 2.8 entry below.
3. **rsms/inter renamed `Inter-VariableFont_slnt,wght.woff2` to `InterVariable.woff2`** in v4.x (Task 1.2 hit this — the plan's URL 404'd). Same axes (slnt + wght), same coverage, ~352 KB; corrected in the plan, in `base.css`'s `@font-face`, and in `index.html`'s preload tag during execution. Not Phase 2 specifically but worth noting because future fonts may follow the same rename pattern (the convention shift was from Google Fonts naming to rsms's own).

---

### Particle Editor 2026 redesign — Phase 1 token system + theme toggle (Phase 1)

*2026-05-19 · [`9df821d`](https://github.com/DrKnickers/particle-editor/commit/9df821d) · #92*

The new-UI React shell adopts the Particle Editor 2026 design system's token + typography + theme machinery as a behaviour-preserving swap. No structural changes — every panel, dialog, button stays in the same DOM location it occupied before. The shell now renders in Inter (locally-bundled variable woff2, ~352 KB, `font-display: block` + `<link rel="preload">` so no FOUT), uses the design's six-tier dark palette by default, and exposes a Sun / Moon theme toggle in the toolbar that flips the page between dark and light. The toggle persists to `localStorage('alo:theme')` and falls back to `matchMedia('(prefers-color-scheme: dark)')` at first launch. View menu items, dialog bodies, and all chrome surfaces sweep from Tailwind's default `bg-neutral-*` / `text-neutral-*` / `sky-*` utilities to the new token-backed equivalents (`bg-bg-2` / `text-text-2` / `accent` etc.) so subsequent phases can rewrite individual surfaces without restructuring class architecture. A View-menu alignment fix shipped alongside — five items that were missing the empty `CheckSlot` indent now align with their siblings.

**How we tackled it.** New CSS files under `web/apps/editor/src/styles/`. `tokens.css` ports the design's `:root` + `[data-theme="light"]` token blocks verbatim (24 CSS variables: six-tier backgrounds, three-tier text, accents, axes, radii, row heights, shadow) and adds a Tailwind v4 `@theme inline { --color-bg: var(--bg); ... }` block that republishes them as `bg-bg` / `text-text-3` / `border-border-2` / `accent` / etc. utility classes. The `inline` keyword keeps the values as `var()` references (not literal hex inlining) so `[data-theme="light"]` runtime flipping still works. `base.css` declares the `@font-face` for the locally-bundled Inter variable woff2 + `* { box-sizing }` + scrollbar styling for `.panel-body` / `.curve-list`; deliberately omits any `body { background }` rule because `globals.css` carries an constraint that body must stay `bg-transparent` for WebView2 + D3D9 sibling-HWND compositing. `components.css` ports the design's reusable component classes (`.app` / `.workspace` / `.menubar` / `.toolbar` / `.tb-btn` / `.panel` / `.tree-row` / `.form-row` / `.viewport` / `.vp-tools` / `.curve-editor` / `.statusbar` / etc.) from the design bundle's `styles.css`, skipping the body / html / scrollbar block (lifted to base.css) and the token blocks (lifted to tokens.css) and any `.tweaks-*` rules (out of scope per spec). `globals.css` drops a pre-existing `@theme {}` block whose 11 legacy tokens (`--color-bg-app` etc.) had zero consumers across the 32 chrome components (verified by grep at re-plan time) and imports the three new files. `ThemeToggle.tsx` is a small Sun + Moon segmented control that reads `localStorage`, falls back to `matchMedia`, and writes `<html data-theme>` on toggle; `App.tsx` gains a one-time `useEffect` at mount that applies the same logic so first paint is themed before any child renders. The 30-file utility-class sweep replaces `bg-neutral-*` / `text-neutral-*` / `border-neutral-*` / `sky-*` with their token-backed equivalents per a fixed substitution table. `test-setup.ts` gains in-memory `localStorage` + no-match `matchMedia` stubs (matching the existing pattern alongside the file's ResizeObserver / PointerEvent stubs) and an `afterEach(() => localStorage.clear())` so per-component persistence (ThemeToggle / Force Align / palette-store) doesn't leak across tests.

**Architectural decisions worth recording.**

1. **`@theme inline` in Tailwind v4 instead of a JS `tailwind.config.ts`.** The original plan draft (committed as part of the spec / plan work in [`52f381c`](https://github.com/DrKnickers/particle-editor/commit/52f381c)) assumed Tailwind v3 with a JS-extension config. This project is on Tailwind v4 — config is CSS-first via `@theme {}` blocks; the JS config file doesn't exist. The Phase 1 plan was rewritten in place before any code landed, with a callout at the top of the Phase 1 section documenting the translation. Phase 2 and Phase 3 still reference `tailwind.config.ts` in places — those need the same v4 translation when they're executed.
2. **`body { bg-transparent }` is a load-bearing invariant.** The design's body rule (`background: var(--bg)` plus `font-family: Inter` etc.) was split: font-family / font-size / font-feature-settings / user-select went to `globals.css`'s body rule; the `background: var(--bg)` part was deliberately dropped. WebView2 hosts a layered window over the D3D9 viewport sibling-HWND; an opaque body would occlude the engine viewport. The shell's outer `<div>` paints `bg-bg` on its own root after the Task 1.6 sweep; the viewport quadrant stays transparent end-to-end. Browser-mode (`pnpm dev` without the host) shows the default white page in the viewport rect — intentional.
3. **Theme is applied in two places intentionally — App.tsx mount and ThemeToggle internal.** App.tsx's one-time `useEffect` runs before any panel mounts so first paint is themed. ThemeToggle's own `useEffect(() => { dataset.theme = theme }, [theme])` keeps the attribute in sync after the user clicks. Duplicate logic, intentional — the alternative (lifting state to App.tsx and passing setters down) would have been more code for no benefit.

**Issues encountered and resolutions.** Three worth recording.

1. **The plan re-write happened before any code landed.** Reading the plan's Phase 1 Task 1.3 ("Extend Tailwind config") and realising it referenced a `tailwind.config.ts` that doesn't exist forced a stop-and-reconsider. The plan was edited in place — Task 1.3 was eliminated and its work folded into Task 1.1's `@theme inline` block in `tokens.css`; the original Tasks 1.1 / 1.2 / 1.4 / 1.5 / 1.6 / 1.7 / 1.8 were renumbered to 1.1 / 1.2 / 1.3 / 1.4 / 1.5 / 1.6 / 1.7. The re-plan commit carries a "Re-plan note" at the top of the Phase 1 section explaining the v3 → v4 translation, so the plan stays self-explanatory. Stopping to re-plan rather than papering over with on-the-fly substitution kept the diff readable — the plan is a contract, not just guidance.
2. **jsdom doesn't expose `window.localStorage` or `window.matchMedia` in this project's vitest config.** Adding `ThemeToggle.tsx`'s `localStorage` calls broke 4 of its own specs with "Cannot read properties of undefined". jsdom v25 should provide both but apparently doesn't in this config (probably a sub-version that ships without the Web Storage / matchMedia polyfills). Added in-memory stubs in `test-setup.ts` matching the existing ResizeObserver / PointerEvent / scrollIntoView pattern. The `afterEach localStorage.clear()` is what unblocked the `LightingPanel.test.tsx > Force Align ON cascades` regression that emerged once localStorage actually persisted — the prior test toggled Force Align off, and with localStorage now live the next test inherited that state. territory: state isolation between tests matters once persistence works.
3. **`Inter-VariableFont_slnt,wght.woff2` doesn't exist in rsms/inter anymore.** v4.x renamed it to `InterVariable.woff2` (same slnt+wght axes, ~352 KB). The plan's URL 404'd; the implementer for Task 1.2 hit the rename and asked for direction before papering over with a guess. Corrected in the plan + `base.css` + `index.html` preload tag in the same Phase 1 commit. If a future font swap follows the same pattern, the rsms convention is now `XxxVariable.woff2` (no axis-tag suffix).

---

### Hybrid focus-channel curve editor — restore edit surface (Phase 2.8)

*2026-05-19 · [`3cd840a`](https://github.com/DrKnickers/particle-editor/commit/3cd840a) · #92*

Task 2.6 of the Particle Editor 2026 redesign deleted the per-emitter `TrackEditor.tsx` (866 lines) and `EmitterPropertyPanel.tsx` (176 lines) in favour of a view-only multi-channel curve overlay. Curve editing is a key feature of the editor and the lossiness was too high, so this commit restores the edit surface on top of the 2.6 multi-channel panel using a *hybrid focus-channel* model: clicking a channel row in the left curve-list sets that channel as the **edit focus**, the multi-channel SVG renders the focus channel emphasised (thick stroke, opaque, key circles) and the other visible channels dimmed (opacity 0.4, no markers) as background context, and a new `.ce-toolbar` row above the canvas hosts the edit affordances (Select / Insert mode toggle, Linear / Smooth / Step interpolation, Lock-to combo, Time / Value spinners) — all scoped to the focus channel. Drag-to-move, marquee select, click-select, Insert-mode click-to-add, per-key right-click Delete context menu, and a panel-level Delete keyboard handler (with the typing-surface guard so Delete inside an input still deletes characters) are all back. Selection is per focus channel and clears on focus change; the optimistic (time, value) override (lessons.md) keeps the spinners populated across the bridge round-trip.

**How we tackled it.** The Task 2.6 deletion was *not* reverted — the multi-channel overlay stays as the rendering substrate. `web/apps/editor/src/screens/CurveEditor.tsx`'s `MultiChannelCurves` branch grew a new `focusChannel?: string` prop plus the full interactive scaffolding (drag refs, marquee state, Esc-cancel, pointer-capture, eventToViewBox) that previously lived only in the single-track branch. When the prop is set, the focus channel's `<g>` layer renders with `strokeWidth=2.5`, full opacity, selectable / draggable key circles, and the SVG's pointer-move / pointer-up / context-menu handlers route to its keys; non-focus visible layers render dim (`opacity: 0.4`, no markers, `pointerEvents=none`). When the prop is unset the branch stays view-only — the Task 2.6 contract is preserved, including the existing 6 CurveEditorPanel specs that assert per-channel `<g data-testid="curve-layer-${id}">` layers. The panel itself (`web/apps/editor/src/components/CurveEditorPanel.tsx`) was rewritten to hold the focus-channel + selection + mode + optimistic-override + context-menu state, render the `.ce-toolbar` (the CSS rule was already in place since Task 1.1 — Task 2.6 had simply collapsed the grid template to `1fr` and skipped the toolbar slot), and wire the bridge mutations (`emitters/set-track-key`, `emitters/add-track-key`, `emitters/delete-track-keys`, `emitters/set-track-interpolation`) to the focus channel's `trackName`. `EmitterPropertyPanel.tsx` is *not* restored — its responsibilities (selection sync + Delete keyboard handler) are now in `CurveEditorPanel` itself; the Delete handler is window-scoped and guards against firing when the event target is an INPUT / TEXTAREA / SELECT.

**Architectural decisions worth recording.**

1. **Focus is session-scoped, not persisted.** Channel *visibility* persists to `localStorage('alo:curve-channels')` (Task 2.6 behavior). The focus channel does not — it's an ephemeral edit context that resets to "scale" on every mount. Persisting it would have produced surprising-on-cold-launch state ("which curve am I editing?") that didn't match the immediate visible mode toggle / spinner state.
2. **Clicking a hidden channel row turns it ON and focuses it.** You can't focus what you can't see; the two states would have diverged confusingly. The checkbox itself toggles visibility *without* moving focus — the row body and the checkbox are deliberately split affordances.
3. **The MultiChannelCurves branch absorbed the interactive scaffolding** rather than the panel delegating to the old single-track CurveEditor. Two surfaces showing curves (a dimmed multi-channel SVG plus an interactive single-track SVG layered on top) would have doubled the grid / axis / backdrop nodes, complicated pointer routing (which SVG owns the captured pointer?), and broken the existing layer-per-channel test contract. One SVG with a focus-aware render branch is the right shape.
4. **The `.ce-toolbar` always renders, even with no emitter selected.** Disabled controls are still a discoverable affordance surface — the user sees what the curve editor can do before they pick something to edit. Matches the design's `.ce-toolbar` slot intent (always-on 36px row above the body).

**Issues encountered and resolutions.** One worth recording.

1. **Two `valueRangeForTrack` helpers — duplicated, not exported.** The same per-track value-range table existed in both `CurveEditor.tsx` (for projection) and the now-deleted `TrackEditor.tsx` (for spinner clamping). After the rewrite `CurveEditorPanel.tsx` needs spinner clamping too, but exporting the helper from `CurveEditor.tsx` would have widened the file's surface area for a single use site. Duplicated the function definition inline in `CurveEditorPanel.tsx` with an explanatory comment — same five-line switch, no real maintenance cost, keeps the projection helper internal to the renderer.

---

### Mods menu detection + selection (D6)

*2026-05-19 · [`059395d`](https://github.com/DrKnickers/particle-editor/commit/059395d) · #92*

The new-UI Mods menu's `(none)` placeholder is replaced by a dynamic list of installed EaW / FoC mods scanned from `<gameRoot>/{corruption,GameData}/Mods` at startup. Entries are grouped (Forces of Corruption first, then Base Game) and alphabetised by folder name within each group, matching the legacy popup's ordering exactly. Clicking an entry hot-swaps the FileManager basepath, writes `HKCU\Software\AloParticleEditor\LastMod` for the next launch, refreshes the texture palette, clears the thumbnail cache, and reloads shaders + textures — all six legacy side effects, with no Win32-specific finalisation that doesn't apply in the React-rendered new UI. The active mod gets a check mark next to its entry; "Unmodded" gets the check when no mod is active. A "Refresh Mod List" item at the bottom re-scans disk without restarting. Cross-mode persistence is automatic: both legacy and new-UI read / write the same registry key, so flipping between `--legacy-ui` and `--new-ui` launches preserves which mod is active.

**How we tackled it.** Two-step refactor + feature. Step 1 (commit `ea0ed40`) extracted `ModManager` from `src/main.cpp` into a standalone class at [`src/ModManager.{h,cpp}`](src/ModManager.h) — owns the mods vector, the active-mod path, the discovery code (`ScanModsDir` / `DiscoverMods`), the registry helpers (`ReadLastMod` / `WriteLastMod` / `ReadModNickname` / `WriteModNickname`), and the atomic `SelectMod` chain. The legacy `SelectMod()` in main.cpp shrank to a thin wrapper that adds Win32-only finalisation (HBITMAP rebuild, skydome picker `SendMessage`, HMENU rebuild, `InvalidateRect`). `IFileManager` gained `SetModPath` as virtual (non-pure, default no-op) so ModManager can call it through the interface; `host::Run` + `HostWindow` gained a `gameRoots` parameter so the host's ModManager can scan the same Mods directories. Step 2 (this commit) added the bridge surface: three new request kinds (`mods/list`, `mods/select`, `mods/refresh`) in [`web/packages/bridge-schema/src/index.ts`](web/packages/bridge-schema/src/index.ts), `activeModPath: string | null` on `EngineStateDto`, the corresponding dispatcher handlers in [`src/host/BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp), MockBridge stubs returning a 2-entry synthetic fixture, and a full menu rewrite in [`web/apps/editor/src/components/MenuBar.tsx`](web/apps/editor/src/components/MenuBar.tsx) that fetches the list at mount, subscribes to `engine/state/changed` for active-path reactivity, and dispatches `mods/select` + `mods/refresh` on user interaction.

**Architectural decisions worth recording.** Three sub-decisions came up during planning, each with a non-obvious answer:

1. **ModManager owns the engine refresh (atomic), not à la carte.** First instinct was to keep `ModManager::SelectMod` narrow (FileManager + registry + palette only) and let each caller drive `engine->Reload*` separately. Reversed during planning because that pattern makes silent staleness easy — a future caller forgets the engine refresh and the mod "activates" without visible effect. Atomic operation removes the failure mode entirely. Cost is one extra pointer + a two-step lifecycle (`SetEngine` after construction since Engine doesn't exist when ModManager is built in --new-ui).
2. **DTO carries `activeModPath: string | null` (path-only), not a full ModDescriptor.** Standard `selectedId + items[]` pattern. Single source of identity. Avoids nickname-staleness if nickname editing ships later.
3. **`mods/list` is a separate request, not part of `engine/state/snapshot`.** Data cadence separation — the mod list changes only on Refresh / disk mutation while snapshots fire on every engine mutation. Bundling them would pay deserialisation cost on every snapshot for data that almost never changes.

**Issues encountered and resolutions.** Two worth recording.

1. **Existing MenuBar tests broke because the default stub bridge returns `{}` for every request.** Every existing `MenuBar.test.tsx` spec creates a stub bridge via `vi.fn().mockResolvedValue({})` — generic, doesn't know about `mods/list`. With D6 in place, the new `mods/list` call's response is `{}` (no `mods` field), `setMods(r.mods)` writes `undefined`, and the menu's filter step crashes with `Cannot read properties of undefined (reading 'filter')`. The fix is a defensive `Array.isArray(r?.mods) ? r.mods : []` in MenuBar. Updating every existing stub to know about the new schema would have been overreach; runtime robustness in the component is the cheaper and more durable fix.
2. **The Playwright harness has an explicit spec allowlist, not a glob.** New `mods-contract.spec.ts` was created in `tests/` but ran 77/77 instead of 80/80 — Playwright found my spec, but the harness script `scripts/run-native-tests.mjs` only forwards the named entries to the Playwright CLI. Added `tests/mods-contract.spec.ts` to that list. Worth noting as a pattern for any future Playwright spec — `tests/` glob discovery isn't enough; the harness allowlist also needs the entry.

---

### Texture-aware `file/open` for skydome + ground custom slots (D5)

*2026-05-19 · [`9ad01d0`](https://github.com/DrKnickers/particle-editor/commit/9ad01d0) · #92*

The native picker invoked from the Background panel's Custom slots (9/10/11) and the Ground Texture panel's Custom slots (5/6/7) now opens with `*.dds;*.tga` as the default filter and a title that names the surface ("Open skydome texture" / "Open ground texture"), instead of the `*.alo` filter inherited from File → Open. The Ground Texture custom-slot click is no longer a no-op — picking a file writes the path into the engine slot and activates it, mirroring the skydome flow that had been working through the wrong filter. File → Open / recents / drag-drop are unchanged: still `*.alo`, still load + commit as the current particle system. While we were in the dispatcher, every `lpstrFilter` was brought up to legacy label parity — the dropdown text now matches the legacy convention `"Particle Files (*.alo)"` / `"Texture Files (*.dds;*.tga)"` / `"All Files (*.*)"` (parenthesised pattern suffix, capitalised "Files"); previously the bridge's `.alo` filter read "Alo files" with no suffix and the texture filter read "Texture files" with no suffix.

**How we tackled it.** Single schema delta: an optional `filter?: "alo" | "skydome" | "ground"` on the `file/open` request's params in [`web/packages/bridge-schema/src/index.ts`](web/packages/bridge-schema/src/index.ts:362), defaulting to `"alo"` so every existing caller (File → Open, recents, drag-drop) stays on the current path. The dispatcher case in [`src/host/BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp:1239) reads the field defensively, swaps `lpstrFilter` / `lpstrTitle` for the texture variants, and — critically — short-circuits to a `{ ok: true, path }` response *before* the existing `LoadParticleSystem` + `m_currentFilePath` + recents commit chain runs. Texture filter ⇒ "return the picked path, nothing else." The React side then routes the result through the already-existing `engine/set/skydome-custom-path` + `engine/set/skydome-slot` chain (skydome) or the newly-wired `engine/set/ground-slot-custom-path` + `engine/set/ground-texture` chain (ground). Orchestration stays React-owned; the host gains one defaulted field and one short-circuit branch.

**Issues encountered and resolutions.** One worth recording.

1. **Naive schema parameterisation would have routed texture paths through the .alo loader.** The first cut of the dispatcher edit only changed `lpstrFilter` / `lpstrTitle` and left the post-pick load chain untouched. That would have taken a picked `.dds` and tried to `LoadParticleSystem(L"C:/.../sky.dds")` against it, surfacing `{ ok: false, error: "load failed" }` to the React side and looking to the user like a broken picker. Caught in pre-handoff code review (the dispatcher case's invariant is "everything below the picker assumes a loadable .alo," which the schema delta silently violated). Fix: pull the filter resolution above the `if (path.empty())` block and add a `if (filterId != "alo") { sendOk(path); return; }` gate immediately after the pick. The gate also protects future callers that pass `path` explicitly with a non-`"alo"` filter — the load chain is now strictly opt-in to the `"alo"` semantic.

---

### Close out the disabled-stub menu items (Group D polish)

*2026-05-19 · [`244b339`](https://github.com/DrKnickers/particle-editor/commit/244b339) · #92*

Four previously-disabled menu items now do something real. **File → Exit** routes through the existing save-prompt and then `PostMessage(WM_CLOSE)` on the host, matching legacy [`DoCheckChanges`](src/main.cpp:1395) → `DestroyWindow` semantics. **View → Reset Camera** snaps the camera to the legacy defaults `pos=(0,-250,125) target=(0,0,0) up=(0,0,1)` from [`src/main.cpp:1814`](src/main.cpp:1814) via a single existing `engine/set/camera` dispatch — no new bridge kind needed. **View → Reset View Settings** cascades background, ground (visibility/Z/texture), bloom, and skydome back to engine defaults after a Yes/No confirm dialog matching the legacy [`MessageBox`](src/main.cpp:1734) prompt. **Lighting panel → Force Align Fill Lights** restores the legacy checkbox: when ON, fill1/fill2 azimuth follow `sun.az + 120°` and `sun.az + 210°` respectively at -10° altitude (constants from [`src/main.cpp:6238-6240`](src/main.cpp:6238)), fill az/alt spinners and the Mirror Sun button disable to enforce the constraint.

**How we tackled it.** New `app/quit` bridge request in [`web/packages/bridge-schema/src/index.ts`](web/packages/bridge-schema/src/index.ts) — host handler in [`BridgeDispatcher.cpp`](src/host/BridgeDispatcher.cpp) sends the response envelope first, then `PostMessage(m_hostHwnd, WM_CLOSE, 0, 0)` so the existing `DefWindowProc → DestroyWindow → WM_DESTROY` chain runs unchanged (compositor + engine teardown, WM_QUIT post). React's `handleExit` reuses `promptSaveChanges` so the dirty-prompt path is identical to `File → New / Open`. **Reset Camera** is a one-line MenuBar dispatch with constants inline. **Reset View Settings** also new — `engine/action/reset-view-settings` — host-side calls the 9 existing `Set*` engine methods in sequence and emits one `engine/state/changed`. Mock-side uses `applyPatch` to write only the view-setting fields (preserving `currentFilePath` / `dirty`). React renders a Radix-portal `Modal` with Cancel/Reset buttons; the prompt sits as a sibling of `Menubar.Root` so Radix's keyboard-nav child-list semantics aren't disturbed. **Force Align** is purely React-side — no engine state needed since the engine just consumes the final per-light direction vectors. New `useState` flag (default ON per legacy `kLightForceAlignDefault`), `computeAlignedFills(sunAz)` helper derives the constrained values, `updateSun` cascades fill dispatches when az changes with the flag on, and a `handleForceAlignToggle` snaps fills immediately when the constraint engages. Mirror Sun disables while Force Align is on (matches legacy [`UpdateForceAlignEnableState`](src/main.cpp:6499)).

**Issues encountered and resolutions.** Two worth recording.

1. **Mirror Sun unit test broke from the new Force Align disable.** The existing `LightingPanel.test.tsx > Mirror Sun button dispatches engine/set/light` spec clicked the button at default state and expected dispatches. With Force Align defaulting to ON, the button is now disabled and the click is a no-op — same behavior as the legacy `UpdateForceAlignEnableState` enforces. Fixed by toggling the Force Align checkbox off first in the test, then clicking Mirror Sun. Added a sibling spec asserting that changing `Sun azimuth` while Force Align is ON dispatches `engine/set/light` for fill1 + fill2 (proving the cascade fires).
2. **Force Align persistence.** The legacy stores the flag as a `REG_DWORD` under `LightingForceFillAlignment` so it survives editor restarts. This dispatch keeps it session-only (just `useState`) — adding registry persistence would have required pulling the same registry helper the legacy uses into the host or designing a generic UI-prefs persistence layer. Noted as a follow-up; the muscle-memory parity comes from the checkbox + the constraint behavior, not from cross-restart memory.

---

### EmitterTree panel toolbar + 3D cursor in status bar (Group A polish)

*2026-05-19 · [`af5b329`](https://github.com/DrKnickers/particle-editor/commit/af5b329) · #92*

First wave of the legacy-parity polish sweep. The new-UI's EmitterTree sidebar now carries the same panel-header toolbar the legacy `EmitterList` panel had — `[New ▾] [Delete] [▲ Move Up] [▼ Move Down] · [👁] [Show All] [Hide All]` — matching the layout from [`src/UI/EmitterList.cpp:3016`](src/UI/EmitterList.cpp:3016). Adding emitters, deleting, reordering roots, and toggling per-emitter visibility no longer require the top menubar or right-click; the muscle-memory affordances live where the eye expects them. The status bar also picks up its fifth column from legacy — `Cursor: x, y, z` showing the 3D ground-plane intersection of the viewport mouse cursor, updated at ~30 Hz while the cursor is over the viewport.

**How we tackled it.** New `EmitterTreeToolbar` sub-component at the top of [`web/apps/editor/src/screens/EmitterTree.tsx`](web/apps/editor/src/screens/EmitterTree.tsx) replaces the prior "Emitters" heading. Disabled-state gating reads the primary-selection node + the live tree:  New ▾ submenu items for Lifetime/Death gate on the parent already having a child of that role; Delete gates on primary; Move Up/Down keep the existing context-menu's root-only + edge rule (`isRoot && indexInSiblings > 0/<siblings.length-1`). New `[👁]` button reads the primary's `visible` flag and renders `Eye` vs `EyeOff` (Lucide) — click dispatches `emitters/set-visible` with the negated value. Show All / Hide All are pure bulk dispatches. The bridge gains two new requests: `emitters/set-visible { id, visible }` (single emitter, leaves children alone — matches legacy `EmitterList_ToggleEmitterVisibility`) and `emitters/set-all-visible { visible }` (walks every emitter — matches `EmitterList_SetAllEmitterVisibility`). Both emit `emitters/tree/changed` + `engine/state/changed`; neither touches the dirty bit because `Emitter::visible` is `// Not stored, for use in editor only` per [`src/ParticleSystem.h:131`](src/ParticleSystem.h:131). Cursor coords ride a new `cursor/position-3d` event emitted from the viewport popup's WM_MOUSEMOVE in [`src/host/HostWindow.cpp`](src/host/HostWindow.cpp), throttled to ~30 Hz via a `GetTickCount()` interval check; the host reuses the existing `GetCursorPos3D` helper from `src/MouseCursor.h` (already shared between legacy and new-UI). React's `StatusBar` adds a fifth cell that formats `(x, y, z)` to 1 decimal.

**Issues encountered and resolutions.** One worth recording.

1. **Per-row visibility eye vs. panel-toolbar `[👁]`.** The original deferred-item list called for a "per-row eye affordance" — clickable Eye/EyeOff icons inside each tree row. Re-reading the legacy ([`src/UI/EmitterList.cpp:3029`](src/UI/EmitterList.cpp:3029)) shows that's not actually parity: legacy has a single Toggle Visibility button in the panel toolbar that operates on the selected emitter, no per-row icons. The per-row idea was a new-UI invention. Sticking with the legacy shape: a single panel-toolbar button that reads/writes the primary selection's `visible` flag, plus the bulk Show All / Hide All buttons. If a per-row affordance becomes desirable later it's purely additive.

---

### Layered viewport with software alpha-stamp cut-outs ()

*2026-05-18 · [`11ab97c`](https://github.com/DrKnickers/particle-editor/commit/11ab97c) · #92*

The new-UI viewport popup now composites with per-pixel alpha through `WS_EX_LAYERED` + `UpdateLayeredWindow(ULW_ALPHA)`, and the/`SetWindowRgn` cut-out is gone. Chrome rectangles (menus, tool panels) still register themselves as occlusions via the existing `viewport/occlude` bridge call, but instead of building an HRGN with binary holes the host now stamps a smoothstep-feathered alpha hole into the readback DIB once per frame. Visible difference: the seam between the D3D9 viewport area and the WebView2 chrome behind it is soft-edged where there used to be a single pixel-hard step, and Tailwind's `shadow-xl` is restored on the Radix menu `CONTENT` class — the dropdown's own drop-shadow now blends through the alpha hole rather than getting lopped off at the cut-out boundary.

**How we tackled it.** New module [`src/host/AlphaCompositor.{h,cpp}`](src/host/AlphaCompositor.h) owns the off-screen `D3DFMT_A8R8G8B8` render target (`D3DPOOL_DEFAULT`), the `D3DPOOL_SYSTEMMEM` readback surface, the top-down `CreateDIBSection` bitmap + memDC, and an `id → (RECT, feather)` occlusion map. `Engine::SetAlphaCompositor` in [`src/engine.h`](src/engine.h:121) injects the pointer; `Engine::Render` at [`src/engine.cpp:629`](src/engine.cpp:629) inserts a `SetRenderTarget(0, compositor->GetRenderTarget())` before the existing `pScreenSurface` capture so the entire scene → bloom → distort → final-composite chain flows through the off-screen RT, and replaces the `Present` at [`src/engine.cpp:876`](src/engine.cpp:876) with `compositor->Composite(viewportHwnd)`. Composite does `GetRenderTargetData` → SYSTEMMEM, row-by-row memcpy into the DIB (account for `locked.Pitch`), runs `ApplyOcclusion` per registered rect (Chebyshev distance from the rect's outer edge → smoothstep weight → multiply RGBA so the premultiplied-alpha invariant `ULW_ALPHA` requires is preserved), then `UpdateLayeredWindow(hViewport, …, memDC, …, ULW_ALPHA)`. `LayoutBroker` at [`src/host/LayoutBroker.cpp`](src/host/LayoutBroker.cpp) keeps `m_occlusions` in main-client coords as the source of truth (survives popup moves) but the/`RebuildPopupRegion` + `SetWindowRgn` path is gone — `SetOcclusion` / `RemoveOcclusion` now translate to popup-client coords and forward to the compositor; `Apply` / `PredictAndApply` re-emit the whole map whenever the popup origin changes so every rect's popup-client coords stay current. `HostWindow.cpp` at [`src/host/HostWindow.cpp:798`](src/host/HostWindow.cpp:798) adds `WS_EX_LAYERED` to the viewport `CreateWindowExW`, constructs the compositor right after `Engine` (seeded to `GetClientRect(hViewport)`), wires it into both Engine and LayoutBroker, and detaches the pointer in `WM_DESTROY` before either side is destroyed.

**Issues encountered and resolutions.** Five worth recording.

1. **D3DPOOL_DEFAULT lifetime vs `m_pDevice->Reset`.** The compositor's off-screen RT is `D3DPOOL_DEFAULT` — required because POOL_MANAGED / POOL_SYSTEMMEM can't be used as a render target. `IDirect3DDevice9::Reset` returns `D3DERR_INVALIDCALL` if any POOL_DEFAULT resource is still outstanding, and the first cut of never released the compositor RT before Engine's `m_pDevice->Reset` call. The very first `layout/viewport-rect` from React triggered `Engine::Reset`, the device-reset failed silently inside `LayoutBroker::Apply`'s try/catch (Engine threw `wruntime_error`, log was clean), and the engine ended up with null `m_pSceneTexture` / `m_pDistortTexture` and shaders frozen in `OnLostDevice`. Symptoms showed up in three Playwright native specs — `engine/set/skydome-slot`, `engine/set/ground-texture`, and `spawner/active-count` — where bridge state-mutations appeared to succeed but the snapshot/event side never reflected the change. Fix: new `AlphaCompositor::ReleaseGpuResources` that drops POOL_DEFAULT (and harmlessly the sysmem surface + DIB) and zeroes the cached `width/height` so the post-Reset `Resize` doesn't short-circuit; called in `Engine::Reset` right after the shader `OnLostDevice` block, before `m_pDevice->Reset`.

2. **Plan v1's "delete the occlusion bridge" was load-bearing wrong.** The original plan (committed but not executed; see [`docs/superpowers/plans/2026-05-18--viewport-alpha-compositing.md`](docs/superpowers/plans/2026-05-18--viewport-alpha-compositing.md)) proposed stripping the `viewport/occlude` protocol entirely on the assumption that `WS_EX_LAYERED` would let WebView chrome show through "automatically." That's not how the visual stack actually works: the popup sits above the WebView2 in DWM z-order, the engine renders alpha = 0xFF everywhere (the plan's own risk #3 endorsed this), so a fully-opaque layered popup would hide the chrome underneath at every pixel — exactly the failure mode the cut-out was working around. inverts the call: keep the occlusion bridge intact and use it to drive software alpha stamping. Visual win comes from `(a)` `WS_EX_LAYERED` letting DWM paint a drop shadow around the viewport's outer edge, and `(b)` the smoothstep feather giving the cut-out boundary an anti-aliased ramp instead of HRGN's single-pixel step.

3. **Multisample mismatch on the final composite quad → solid-black viewport.** With the compositor wired in, the viewport popup composited as fully opaque black with no scene visible. The compositor's off-screen RT is `D3DMULTISAMPLE_NONE` (required so `GetRenderTargetData` can read it back), but the engine's auto-depth-stencil (captured into `pDepthSurface` at the top of `Engine::Render`) is multisampled — matched to the swap-chain back buffer whose `MultiSampleType` is set by `GetMultiSampleType` picking the highest type the device supports. The final-composite block in `Engine::Render` at line 857 sets slot 0 back to the compositor RT and then restored the MSAA auto-depth, pairing a non-MS RT with an MSAA depth — D3D9 silently dropped the next `DrawPrimitiveUP` (the distort full-screen quad). The preceding `Clear` worked since `Clear` isn't subject to the MS-match rule, leaving the clear color as the visible result. Fix: when the compositor is active, skip the auto-depth restore in `Engine::Render` and keep the engine's own MS_NONE depth (`m_pDepthStencilSurface`, already bound at line 643) — it's MS-compatible with the compositor RT, and the distort pass uses `ZFunc=Always` so depth contents are irrelevant. The legacy `Present` path (compositor null) still restores the auto-depth as before. Caught via a one-shot diagnostic that dumped the DIB's center pixel + size each second (initially showed `BGRA=00 00 00 FF`, confirming the clear value sticking); a follow-up probe that flipped the clear color to orange confirmed the clear was hitting our RT and the distort quad was the silent-fail site.

4. **Smoothstep feather inside an unpadded occlusion rect carves the chrome's own outline.** Once the viewport rendered correctly, opening any menu dropped a viewport→menu fade band 3 px inside the menu's left/right/bottom edges — overpainting the menu's `border border-neutral-800` and the rounded corners. Root cause: the React-side `useViewportOcclusion` hook reported the menu's bare `getBoundingClientRect`, so the host's smoothstep feather (designed to soften the cut-out boundary) ramped from opaque-viewport at the rect's outer edge to transparent 3 px inward — exactly where the chrome's border + corner-radius sat. Fix landed in two passes: (a) extend the `viewport/occlude` bridge message with optional `feather` + matching `padPx` so each chrome type can pick its own pair; (b) menus pass `pad=24, feather=24` so the alpha ramps across the entire shadow ring (outside the menu's outline), tool panels keep `pad=0, feather=0` (their flat-edge style wants a hard cut against the panel's left border). The schema's `feather` defaults to 0 so older tests that don't send it still pass.

5. **Feather distance computed from the clipped rect → purple halo at the popup's near edge.** With pad=24 + feather=24, the menu's right and bottom edges blended cleanly into the viewport scene, but the LEFT edge — sitting ~20 px inside the popup — showed a dark-purple halo. The padded rect's left edge was clipped (popup-x = -4 → 0), the first cut of `ApplyOcclusion` treated the clip as "no feather here," and the 20-px region between popup boundary and menu boundary went to alpha=0 with no fade. The WebView2 in that region was transparent (CSS shadow had faded), so the alpha cut exposed the parent HostWindow's dark-purple brush. Fix: compute the smoothstep distance from the ORIGINAL (un-clipped) rect edges. The math falls out cleanly for every case — rect fully inside popup behaves identically, rect extending past the popup gets weight=0 at popup-edge pixels (because the original-rect distance is naturally > feather), rect just barely clipped on one side gets a correctly-shaped partial feather. This deleted the per-edge "feather only on unclipped" flags that the previous attempt added.

---

### Playwright contract tests unblocked via WebView2 host-object IPC

*2026-05-16 · [`6c55abd`](https://github.com/DrKnickers/particle-editor/commit/6c55abd) · #92*

The four Playwright contract specs guarding the bridge schema between the React UI and the C++ host (`engine/state/snapshot`, `engine/set/ground-z` round-trip, `engine/set/background` COLORREF, `engine/query/ground-slot-empty` typing) now run live and pass against `ParticleEditor.exe --new-ui --test-host`. Previously they were committed as `test.fixme` because WebView2 silently drops `chrome.webview.postMessage` calls from page → host while a CDP debugger is attached (Task 2.2 self-review, captured in [`tasks/lessons.md`](tasks/lessons.md)). With this change `pnpm --filter @particle-editor/editor test:native` exercises 5 specs (1 smoke + 4 contract) covering the request/response and event surfaces against the real C++ handlers; the 25 Vitest MockBridge specs continue to pass.

**How we tackled it.** New `HostBridgeProxy` ([`src/host/HostBridgeProxy.h`](src/host/HostBridgeProxy.h) / [`src/host/HostBridgeProxy.cpp`](src/host/HostBridgeProxy.cpp)) — a WRL `ClassicCom` `IDispatch` shim with a single `dispatchRequest(BSTR jsonReq) → BSTR jsonRes` method, registered under `chrome.webview.hostObjects.hostBridge` via `ICoreWebView2::AddHostObjectToScript`. Gated on `useTestHost` inside the controller-created callback in [`src/host/HostWindow.cpp`](src/host/HostWindow.cpp) so production launches never expose it. `BridgeDispatcher` refactored to extract the kind-string ladder into a private `DispatchInternal(json) → json` helper, with `Dispatch` (the existing async path that emits via `m_emit`) and the new `DispatchSync` (the host-object path that returns the response string directly) both routing through it. TypeScript side: `TestHostBridge` in [`web/apps/editor/src/bridge/test-host.ts`](web/apps/editor/src/bridge/test-host.ts) implements the `Bridge` interface using the host-object channel for requests; [`web/apps/editor/src/bridge/expose.ts`](web/apps/editor/src/bridge/expose.ts) prefers it whenever the host-object slot is populated. Events still flow over `chrome.webview.addEventListener("message", …)` — the CDP drop is page → host only.

**Issues encountered and resolutions.** Two worth recording.

1. **Events delivered as parsed JS values, not strings.** The host emits via `PostWebMessageAsJson`, so the `e.data` arriving at `chrome.webview.addEventListener("message", h)` is the already-parsed JS object — not a JSON-encoded string. The first cut of `TestHostBridge` typed the listener parameter as `{ data: string }` and unconditionally `JSON.parse`'d the payload, which silently failed (the `engine/state/changed` listener never fired even though the host had emitted the event). Fix: type `e.data` as `unknown` and accept either shape — `string` → parse, `object` → use as-is. Also applied to `NativeBridge.onMessage` for symmetry; the production event path was technically broken the same way but happened not to be exercised yet.
2. **CDP drop is unidirectional.** originally framed the issue as "WebView2 drops `chrome.webview.postMessage` under CDP" with no direction specified. Verified during the contract-test pass that host → page postMessage (via `PostWebMessageAsJson`) reaches the page normally; only page → host (via `chrome.webview.postMessage`) is dropped. Practical implication: a host-object channel is only needed for the request direction; events can keep using postMessage. updated with this refinement.

---

### Ground Height resets to 0 on every launch

*2026-05-16 · [`380380a`](https://github.com/DrKnickers/particle-editor/commit/380380a) · #79*

Ground Z is now session-only — every editor launch starts with the ground plane at z=0 regardless of what value was in effect when you last closed. Adjusting the *Ground Height* spinner during a session still works as before; it just doesn't write to the registry anymore. Rationale: an anchored vertical reference makes "did I just open the editor, or is this a continued workflow?" unambiguous, and Reset View Settings can't surprise you with a stale offset from a previous tuning pass.

**How we tackled it.** Two-line change in [`src/main.cpp`](src/main.cpp): replaced the `info->engine->SetGroundZ(ReadGroundZ(info->engine->GetGroundZ()))` call at startup with a hard-coded `SetGroundZ(0.0f)`, and dropped the `WriteGroundZ(z)` call from the spinner's `SN_CHANGE` handler. The `ReadGroundZ` / `WriteGroundZ` helpers themselves stay in place as legacy code — harmless, and re-introducing persistence later (if anyone asks) just needs the calls back, no new helpers to write. Reset View Settings still deletes the old `GroundZ` registry value, so stale data from pre-change builds gets cleaned up on the first Reset.

---

### Import emitters from another `.alo` file

*2026-05-16 · [`7640798`](https://github.com/DrKnickers/particle-editor/commit/7640798) · #77*

New **File → Import Emitters from File…** entry opens an `.alo` picker, then a modal dialog showing the source file's emitter tree as a `TVS_CHECKBOXES` TreeView. Tick whichever emitters you want — parent/child auto-include is on by default so ticking a parent picks up its descendants — hit OK, and the selected emitters land as new root emitters in the current particle system. The dialog has *Select all* / *Clear* / *Browse…* buttons; *Browse…* swaps the source file in place without cancelling. OK is disabled until at least one emitter is ticked. Imported emitters arrive with collision-free names (e.g. `smoke_1`), spawn-child cross-references re-mapped where both source and child were imported (dropped child → `-1`), and source link groups re-created as fresh destination groups when ≥2 members of the source group survived the import. The entire import is one undo step — Ctrl+Z atomically rolls back every newly-added emitter.

Generalises the existing single-emitter clipboard copy/paste (which still works for one-at-a-time transfers via Ctrl+C / Ctrl+V on the emitter tree). Cuts the click count for assembling a complex effect from pieces of existing ones from "switch window, copy, switch back, paste, repeat per emitter" to "Import, tick, OK".

**How we tackled it.** Routed the clone path through the existing `Emitter::write(writer, copy=true)` + `Emitter(ChunkReader&)` round-trip via a `MemoryFile` buffer, so the field-level serialisation logic stays in one place. The import engine in [`src/main.cpp`](src/main.cpp:7115) runs three passes: Pass 1 clones each pick into the destination's `m_emitters` as a root and records `src_idx → dst_idx`; Pass 2 walks the picks again and rewrites each clone's `spawnDuringLife` / `spawnOnDeath` using the map (or `-1` when the source child wasn't imported), then rebuilds parent pointers from the now-correct spawn fields mirroring `ParticleSystem(IFile*)`'s load-time logic at [`src/ParticleSystem.cpp:1075-1089`](src/ParticleSystem.cpp:1075); Pass 3 buckets picks by source `linkGroup`, and for each bucket with ≥2 imported members calls `CreateLinkGroup` to allocate a fresh destination ID and bind the members. Single-member buckets arrive unlinked. `EmitterList_SetParticleSystem` re-pointed at the existing system rebuilds the emitter-tree view after the batch insert, and the single `CaptureUndo(info, 0)` that follows gives Ctrl+Z atomic batch behaviour. Resource scaffolding: new `ID_FILE_IMPORT_EMITTERS` command, `IDD_IMPORT_EMITTERS` dialog template, plus `IDC_IMPORT_*` control IDs duplicated across `resource.en.h` / `resource.de.h` + the `.en.rc` / `.de.rc` pair.

**Issues encountered and resolutions.** Four worth recording.

1. **Static menu entry got swallowed by the dynamic recent-files rebuild.** The File menu's recent-files list is rebuilt at runtime by `AppendHistory` at [`src/main.cpp:700`](src/main.cpp:700), which walks the menu, finds the first `MFT_SEPARATOR`, and deletes everything between it and `ID_FILE_EXIT`. The original `IDD` placement put *Import Emitters…* *after* that separator, so the dynamic rebuild eat it on first File-menu open. Moved the entry to *before* the separator (between *Save as* and the recent-files block).
2. **Most-vexing parse on `Emitter clone(r);`.** The C++ parser took `ParticleSystem::Emitter clone(r);` as a function declaration (`clone` returns Emitter, takes a `ChunkReader&` named `r`) rather than a variable definition. The cascade of "operator= ambiguous" errors went away once `clone{r}` (braced init) forced the variable-definition reading.
3. **`NMTVITEMCHANGE` / `TVN_ITEMCHANGED` aren't pulled in by `_WIN32_IE 0x0600` in this SDK** — the gating differs across SDK versions. Switched the checkbox-state-change handler from `TVN_ITEMCHANGED` to a portable `NM_CLICK` + hit-test + `PostMessage(WM_APP+1)` pattern: when the user clicks the state icon area, we defer to a post-toggle message handler that reads the new check state and (when *Auto-include children* is on) cascades the state to descendants. Works on every Windows version, no SDK-version sniffing. The `TVN_KEYDOWN` + `VK_SPACE` path mirrors the same flow for keyboard users.
4. **`GenerateDuplicateName` lived `static` in `EmitterList.cpp`** so the existing paste path could reuse it. Removed `static` + added an `extern` declaration in `main.cpp` so the import path can call the exact same function — no copy of the dup-name rule.

---

### Skydome slots now load real base-game (and mod-overlay) textures

*2026-05-16 · [`b4d2415`](https://github.com/DrKnickers/particle-editor/commit/b4d2415) · #75*

Follow-up to. The eight bundled skydome slots are no longer procedural-gradient placeholders — they point at curated base-game textures from `DATA\ART\TEXTURES\` and route through the existing `FileManager` resolution chain, so the active mod's overlay is picked up automatically the same way emitter textures are. New slot labels match what the textures actually look like: Storm, Murky Clouds, Smog Clouds, Blue Horizon, Blue Sky, Orange Horizon, Orange Sky, Volcanic Storm. Switching mods via the Mods menu now also refreshes the active skydome live — no editor restart needed to see a mod's `W_SKY*.DDS` override take effect. When `FileManager` can't resolve a slot's path (no base game installed, mod doesn't ship the file), the slot gracefully falls back to the same procedural RCDATA placeholder it shipped with, so the slot still renders something rather than going Off.

The skydome sphere also got rotated to match the game's coordinate convention: its poles are now on ±Z instead of ±Y, so the texture's top edge faces up and its bottom edge faces down as the camera orbits. Custom slots 9–11 keep their existing absolute-path support but now try the FileManager chain first, so pasting `DATA\ART\TEXTURES\foo.dds` into a custom slot resolves it from the mod / base-game MEGs without needing the file to exist as a loose disk path.

**How we tackled it.** Engine constructor now takes a `IFileManager&` alongside the existing `ITextureManager&` / `IShaderManager&` so `Engine::ReloadSkydomeTexture` can do the file resolution directly. New static table `kSkydomeBundledGamePaths[]` in [`src/engine.cpp`](src/engine.cpp:46) parallels `kSkydomeBundledResources[]` and gets exposed via `Engine::GetSkydomeBundledGamePaths()` so the picker's thumbnail builder (`MakeSkydomeSlotThumbnail` in [`src/main.cpp`](src/main.cpp:4529)) can share the same resolution order — `FileManager → RCDATA fallback` for slots 1–8, `FileManager → absolute file` for slots 9–11. `Engine::ReloadTextures()` was extended to re-resolve the active skydome alongside the emitter-texture cache clear, and `SelectMod` now calls `RebuildBackgroundPreviewBitmap` + reseeds any open picker so the toolbar preview and the picker thumbnails track the new mod's overrides without a restart. Pole rotation is a single Y↔Z swap in `Engine::InitSkydomeMesh` at [`src/engine.cpp`](src/engine.cpp:1389); the swap reflects handedness, so the skydome pass's `D3DRS_CULLMODE` flipped from `D3DCULL_CW` to `D3DCULL_CCW` to keep the inside-facing triangles visible.

**Issues encountered and resolutions.** Two worth recording.

1. **Pole-axis swap reversed triangle winding.** Swapping Y and Z in `vx.Position` is a reflection — orientation-reversing — so what were the inside-facing triangles (CCW from inside, kept by `D3DCULL_CW`) became outside-facing (CW from inside, culled). The sky disappeared entirely until the skydome pass's cull mode was flipped to `D3DCULL_CCW`. The render-state save/restore around the pass at [`src/engine.cpp`](src/engine.cpp:1463) keeps the change scoped to the skydome and doesn't leak into ground / particle rendering.

2. **TextureManager's placeholder fallback would hide real failures.** The existing emitter-texture loader returns the magenta `IDB_MISSING` placeholder when a file isn't resolvable, which is right for emitters (the user can see something's broken) but wrong for the skydome — we'd rather fall back to the bundled RCDATA so the slot stays usable. Added a thin `LoadTextureViaFileManager` helper in [`src/engine.cpp`](src/engine.cpp:79) that goes straight through `IFileManager::getFile` (returns `NULL` on miss) and lets the caller decide what to do next. Also addresses a latent bug in's `GroundTexturePicker_PickSolidColor`-adjacent code where a similar pattern would silently swallow misses.

---

### Selectable skydome backgrounds via the unified Background button

*2026-05-16 · [`f83a26c`](https://github.com/DrKnickers/particle-editor/commit/f83a26c) · #73*

The toolbar's existing **Background:** colour button is now the single entry point for everything background-related: click it once to open a modeless **Background** picker dialog — a 12-slot icon-mode `SysListView32` laid out as a 4×3 grid of 192×192 thumbnails. Slot 0 is **Solid colour**, slots 1–8 are bundled scenes (Space / Atmosphere / Sunset / Dawn / Night / Overcast / Studio / Indoor), and slots 9–11 are user-customisable. Clicking slot 0 opens the standard Win32 colour-picker dialog seeded with the current background colour and the editor's shared 16-slot custom palette; clicking any other slot loads the corresponding skydome.

The toolbar preview itself is now a hybrid: a flat colour swatch when the picker's slot 0 is active, or a 24×24 thumbnail of the current skydome otherwise. The earlier design exposed a separate skydome preview button next to the *Ground Texture:* preview — that's gone; one button covers both modes and the header strip is cleaner for it.

Interactions in the picker mirror the palette popup's *sticky* model rather than the ground-picker's *click-closes* model: clicking a slot commits the selection and leaves the dialog visible so you can browse other backgrounds interactively. Close via the title-bar X or by toggling the Background button again. Empty Custom slots single-click into `GetOpenFileName` filtered to `*.dds;*.tga`; right-click a Custom slot for *Set custom skydome…* / *Change skydome…* / *Clear slot*; the dialog's *Reset custom slots* button at the bottom wipes only the user-supplied paths after a confirmation prompt. View → Reset View Settings returns the active slot to *Solid colour* but deliberately preserves the three `SkydomeCustomSlot*` registry values (slot assignments are user data, not view settings — same convention as). Cancelling out of the slot-0 colour picker turns the skydome off without replacing the saved background colour.

Render integration is unchanged from the earlier cut and ships as-is: a single new pass between the existing `D3DDevice9::Clear` and the ground-plane render, drawing a hand-rolled 32×16 UV sphere (561 vertices / 1024 triangles, `D3DPOOL_MANAGED`) translated to the camera's position so it stays "infinite" while the camera orbits. Render state during the pass: depth-test off, depth-write off, cull-CW (we view the sphere from inside). [`src/Resources/Engine/Skydome.fx`](src/Resources/Engine/Skydome.fx) (vs_2_0 / ps_2_0) does standard equirectangular sampling on the (U, V) the mesh carries and pushes z to ~1.0 in NDC for belt-and-suspenders far-plane behaviour. The skydome contributes to bloom naturally since it renders into the same scene RT as everything else.

Five `HKCU\Software\AloParticleEditor` registry values hold the persisted state: `SkydomeIndex` (REG_DWORD, slot 0–11), `SkydomeCustomSlot{9,10,11}` (REG_SZ, per-slot paths), and `SkydomePickerPos` (REG_BINARY RECT). The existing `BackgroundColor` (REG_DWORD) and `CustomColors` (REG_BINARY, the ChooseColor 16-slot palette) values are unchanged — switching to a skydome and back naturally preserves whatever solid colour was last in use. Out-of-range / missing-file values fall back to slot 0 rather than crashing.

The shipped build includes **procedural-gradient TGA placeholders** (~12 MB total) generated by [`tools/generate_skydome_textures.py`](tools/generate_skydome_textures.py) — simple top-to-bottom colour ramps approximating each scene. Production-quality BC1-compressed DDS assets (potentially curated from game art) are a separate follow-up PR; the engine loader handles both formats identically so swap-in is a content-only change.

**How we tackled it.** The feature shipped in two stages on the same branch. **Stage 1** built the engine pass + the standalone Skydome preview button + the picker dialog (eleven implementation commits via the `subagent-driven-development` skill — one implementer + two reviewer subagents per task). Engine-side: new `m_pSkydomeVB` / `m_pSkydomeIB` / `m_pSkydomeDecl` + `m_pSkydomeEffect` / `m_pSkydomeTexture` / `m_skydomeIndex` / `m_skydomeCustomSlotPaths[]` in [`src/engine.h`](src/engine.h); `InitSkydomeMesh()` / `InitSkydomeEffect()` / `RenderSkydome()` / `SetSkydomeSlot()` in [`src/engine.cpp`](src/engine.cpp). UI-side: `MakeSkydomeSlotThumbnail`, `SkydomePickerProc`, `ShowSkydomePicker`, registry I/O helpers in [`src/main.cpp`](src/main.cpp). **Stage 2** reworked the toolbar surface: deleted the standalone skydome preview button (`hSkydomePreview` field, `ID_SKYDOME_PREVIEW`, owner-draw branch, `BN_CLICKED` branch, `WM_SIZE` slot), changed the existing `hBackgroundBtn` from the custom `ColorButton` class to plain `BS_OWNERDRAW BUTTON` with a two-path owner-draw (swatch or thumbnail keyed off `engine->GetSkydomeSlot()`), moved its click handler from `CBN_CHANGE` to `BN_CLICKED`, and added a `BackgroundPicker_PickSolidColor` helper that mirrors's `GroundTexturePicker_PickSolidColor` for the new slot 0. The colour helper seeds and pushes back through `ColorButton_GetCustomColors` / `ColorButton_SetCustomColors` so the 16-slot custom palette stays in sync between this dialog and's Lighting dialog. Resource layout: `IDR_SKYDOME_*` (151–158) and `IDR_SHADER_SKYDOME` (150) in [`src/Resources/resource.h`](src/Resources/resource.h); `IDD_SKYDOME_PICKER`, control IDs, and slot-name string-table entries in `resource.en.h` / `resource.de.h` + the `.en.rc` / `.de.rc` pair. `IDS_SKYDOME_OFF` retains its name but its text changes from `Off` to `Solid colour`; the picker dialog `CAPTION` changes from `Skydome` to `Background`.

**Issues encountered and resolutions.** Seven worth recording.

1. **Bundled-asset format**: the plan first picked DDS (BC1) to match game-engine texture compression and keep the bundle small. Pillow's BC1 DDS-write path needs `texconv.exe` or ImageMagick — neither guaranteed on the dev box — so the v1 procedural placeholders ship as 24-bit RGB TGA instead. `D3DXCreateTextureFromFileInMemory` handles both formats identically, so the engine loader doesn't care. The trade-off is ~12 MB of bundled assets instead of ~2 MB; acceptable for the placeholder generation. Curated BC1 DDS assets are a content-only follow-up.

2. **Sphere triangle-count typo in the plan**: the spec said the 32×16 sphere produces `tris=512` but the math is `lon × lat × 2 = 32 × 16 × 2 = 1024` (each lat/lon segment is a quad = 2 triangles). The implementer caught this during Task 1 — code is correct, plan's expected-output line was off by ×2.

3. **`HRESULT`s discarded on `Create*` / `Lock` in `InitSkydomeMesh`**: the rest of the engine constructor throws `runtime_error` on `FAILED(...)`, but the first cut of `InitSkydomeMesh` discarded returns and would have null-deref'd on the next `Lock()` after an OOM-style failure. Code-quality reviewer caught it. Wrapped all five `Create*` / `Lock` sites in `if (FAILED(...)) throw runtime_error("Unable to create skydome mesh")` to match the engine's existing init-failure contract.

4. **Magic-number array size**: `m_skydomeCustomSlotPaths[3]` decoupled from the constants that determine the custom-slot range. If `kSkydomeSlotCount` or `kSkydomeFirstCustomSlot` ever changes, the array width and indexing would silently disagree. Replaced with `m_skydomeCustomSlotPaths[kSkydomeSlotCount - kSkydomeFirstCustomSlot]` so the single source of truth holds. Same pass: `m_skydomeIndex = 0` became `m_skydomeIndex = kSkydomeOffSlot` (it's the same value but the constant makes the Off-semantic explicit), and `slot >= 1` in the bundled-range guard in `ReloadSkydomeTexture` became `slot > kSkydomeOffSlot` for the same reason.

5. **Unused `passes` variable in `RenderSkydome`**: first cut populated `UINT passes = 0; m_pSkydomeEffect->Begin(&passes, 0);` but then called `BeginPass(0)` directly without looping. `passes` was never read — would emit a `/W4` warning and quietly hides the assumption that the technique is single-pass. Fixed to loop `for (UINT i = 0; i < passes; ++i)`, matching the existing pattern in `Engine::Render`. Robust if `Skydome.fx` ever grows to two passes.

6. **`RCDATA` comment correctness**: the placeholder generator script accidentally documented the bundled assets as "user-authored cubemap DDS files" in the `.rc` comment, but the plan committed to equirectangular 2D textures (cubemap is explicitly out of scope). Corrected to "procedural-gradient TGA placeholders today; a follow-up PR can replace with curated equirectangular DDS (BC1) assets without touching the loader."

7. **Custom-colour palette persistence after the button-class swap**: pre-rework, the Background button was a `ColorButton` instance whose `CBN_CHANGE` handler called `ColorButton_GetCustomColors` + `WriteCustomColors` so the user's 16-slot custom palette survived a restart. Deleting that handler without re-homing the call would have silently regressed the feature, and's `GroundTexturePicker_PickSolidColor` doesn't propagate palette changes to the shared `ColorButton` library state either (its local `static COLORREF s_custom[16]` is private). The fix lives in the new `BackgroundPicker_PickSolidColor`: seed `lpCustColors` from `ColorButton_GetCustomColors`, then on commit push back via `ColorButton_SetCustomColors` *and* persist via `WriteCustomColors`. Restores the pre-rework persistence and also fixes the divergence by pushing additions to the shared library state so the Lighting dialog's `ColorButton` instances see them too.

---

### Adjustable environment lighting in the preview

*2026-05-15 · [`d91857c`](https://github.com/DrKnickers/particle-editor/commit/d91857c) · #71*

A new **View → Lighting…** modeless dialog exposes the engine's three directional lights (Sun + Fill 1 + Fill 2) and the scene-global ambient and shadow colours. Layout emulates the Petroglyph map editor's Sun / Fill panel: Sun gets Intensity, Z Angle, Tilt Angle, plus four ColorButtons (Ambient / Specular / Diffuse / Shadow); each Fill gets Intensity, Z Angle, Tilt Angle, and a single Diffuse ColorButton. **Force Fill Light Alignment** (Sun group, default on) drives `Fill1.Z = Sun.Z + 120°`, `Fill2.Z = Sun.Z + 210°`, both Tilts fixed at `−10°`, and greys out the fill-angle spinners + the Mirror Sun button. **Mirror Sun** (Fill group, disabled while alignment is enforced) copies the Sun's Diffuse colour to both Fills in one click. The bottom **Reset to defaults** button restores the canonical map-editor values after a confirmation prompt; View → Reset View Settings does the same, alongside its existing background / ground / bloom resets, and the confirm prompt's text was updated to mention lighting.

Defaults match the Petroglyph map editor exactly: Sun intensity 0.50, Z 0°, Tilt 45°, Ambient `RGB(40,40,50)`, Specular `RGB(190,190,200)`, Diffuse `RGB(180,180,190)`, Shadow `RGB(100,100,110)`; Fill 1 and Fill 2 share intensity 0.50 and slate-blue diffuse `RGB(60,80,160)`. **This changes the editor's default visual** — pre-the engine constructor set a pure-white Sun along +X, no Fills, ambient black. Fresh launches after this PR open with the softer 3-light setup map authors expect.

Persistence lives at `HKCU\Software\AloParticleEditor` across 17 new values: `LightSun{Intensity, ZAngle, Tilt, AmbientColor, SpecularColor, DiffuseColor, ShadowColor}`, `LightingForceFillAlignment`, `Light{Fill1, Fill2}{Intensity, ZAngle, Tilt, DiffuseColor}`, `LightingDialogPos`. The dialog reads from registry on open and on Reset View Settings reseed; every spinner / colour change writes through to engine + registry immediately. Fill Z/Tilt keys are *not* written while force-align is on — they hold the user's last free-edit values, restored verbatim when alignment is unchecked. Reset View Settings deletes all 17 keys and re-runs the engine seed.

Note: **Shadow colour is captured but does not render**. The engine's `SetShadow` declaration has lived in [`src/engine.h`](src/engine.h:185) since the codebase shipped but had no body, and no shader effect handle binds the value. implements `SetShadow` as a store-only stub (new `m_shadow` member) and the colour round-trips correctly, but the preview won't visibly change when shadow colour is adjusted. The control is included for parity with the map editor and forward compatibility with future shader work.

Same PR brings two supporting fixes. **`Spinner_SetReadOnly` API** ([`src/UI/Spinner.cpp`](src/UI/Spinner.cpp:478)): the auto-computed fill-angle spinners needed to read as "disabled" without going through `EnableWindow(FALSE)`, which on Win11 themes suppresses the EDIT's text rendering entirely. The new API short-circuits the up/down buttons, mouse wheel, arrow-key increments, and `EN_UPDATE` model writes, paints the up/down arrows with `DFCS_INACTIVE`, and intercepts the inner EDIT's `WM_PAINT` to draw the value manually in `RGB(60,60,60)` text on `RGB(232,232,232)` background. **Taskbar icon plumbing**: switched the main window from `LoadIcon` + `hIconSm = NULL` to explicit `LoadImage(IMAGE_ICON, 32, 32)` / `LoadImage(IMAGE_ICON, 16, 16)` with `LoadIcon` fallback, cached both HICONs in locals (so the renderer class's second `RegisterClassEx` can't clobber them through the shared `wcx`), and now calls `WM_SETICON` + `SetClassLongPtr(GCLP_HICON / GCLP_HICONSM)` after `CreateWindow`. Added `SetCurrentProcessExplicitAppUserModelID(L"DrKnickers.AloParticleEditor")` (loaded dynamically out of shell32 so the project's `_WIN32_WINNT = 0x0501` doesn't need bumping) to give the editor a stable taskbar identity that's not keyed off the .exe path.

**How we tackled it.** Lighting dialog clones the Bloom dialog's modeless lifecycle: lazy-created on first toggle, hidden on close, position persisted to registry, `WM_USER` re-seed-from-engine after Reset View Settings. New constants for defaults, conversion helpers, registry I/O, `LightingDlgProc`, `ToggleLightingDialog`, `InitializeLightingFromRegistry`, and `ApplyLightingDefaults` live in [`src/main.cpp`](src/main.cpp:5012) — roughly 700 LOC added after `ToggleBloomDialog`. UI representation is the source of truth: registry stores (R, G, B, intensity, Z, tilt, force-align bool); conversion to engine `Light` vec4s happens at write time via `MakeLight(z, tilt, diffuse, specular, intensity)`. Direction math is `Position = (cos(tilt)·cos(z), cos(tilt)·sin(z), sin(tilt))` — engine's `m_eye.Up = (0,0,1)` confirms Z-up, so azimuth around +Z is the right convention. New engine getters (`GetLight` / `GetAmbient` / `GetShadow`) are inline in [`src/engine.h`](src/engine.h); `SetShadow` finally gets a body in [`src/engine.cpp`](src/engine.cpp:1093). Dialog template and View-menu entry mirror across both `.en.rc` and `.de.rc` (German strings as English placeholders, consistent with the project's existing localisation lag).

**Issues encountered and resolutions.**

1. **Default visual divergence is intentional, not a regression.** Pre-the engine constructor set Sun `Diffuse = (1,1,1,1)`, Position `(1,0,0)`, no fills, ambient `(0,0,0,0)`. The Petroglyph map editor's defaults (intensity 0.50, sun tilted 45° up, slate-blue fills, dark-grey ambient) are visibly different. Documented as the headline visual change of the PR rather than silently retaining the old behaviour. Anyone who prefers the old look can set Sun intensity to 1.0, Tilt to 0°, fills to zero, and ambient to black — Reset View Settings restores the new defaults, not the old ones.

2. **Win11 themes silently suppress disabled-EDIT text.** First attempt at "this fill angle is auto-computed" used `EnableWindow(FALSE)` on the spinner. On Win10/11 with the default theme, the EDIT control's themed paint path refuses to draw text when the window is in the disabled state, even with overridden `WM_CTLCOLORSTATIC` colours. Verified by setting the brush to bright red — the background painted red but no text appeared. The same suppression bit `WM_CTLCOLOREDIT` overrides for a read-only-but-enabled EDIT: returning a brush at all from that message triggered the theme to skip text draw. The working fix is a `WM_PAINT` subclass on the EDIT that bypasses the themed paint entirely and draws text + bg manually with `DrawText` + `FillRect`. Documented inline in [`src/UI/Spinner.cpp`](src/UI/Spinner.cpp:115) at the `SpinnerEditWindowProc` paint branch.

3. **`wcx.hIcon` was clobbered between the two `RegisterClassEx` calls.** [`InitializeWindows`](src/main.cpp:6425) registers both `"ParticleEditor"` (main) and `"ParticleEditorRenderer"` (render child) classes from the same `WNDCLASSEX` struct, resetting `wcx.hIcon = NULL` between them. Early drafts of the `WM_SETICON` plumbing read `wcx.hIcon` *after* that reset, so the call passed NULL and silently did nothing. Fixed by caching both HICONs in `hIconBig` / `hIconSmall` locals before the renderer-class registration.

4. **Taskbar icon cache persists per .exe path.** Even with the correct `WM_SETICON` and class icons in place, the local taskbar kept showing the generic "plain window" glyph for the editor — Windows caches taskbar icons per .exe path in `%LOCALAPPDATA%\Microsoft\Windows\Explorer\iconcache_*.db`, and the cache survives rebuilds. `SetCurrentProcessExplicitAppUserModelID` gives the editor a stable identity independent of the .exe path; new installs and clean caches will pick up the icon correctly. (The original "plain window" the user noticed was the AllocConsole'd debug console, not the editor itself — but the cached-icon fix landed regardless because it's the right thing for the main window.)

5. **Force-align registry truth across cycles.** When the user un-checks Force-align, edits Fill 1 Z to 250°, re-checks, the spinner snaps back to the auto-computed value but the user's `250` is still the persisted "last free-edit" value. Verified by exercising the cycle: un-check → see 250, re-check → see Sun.Z+120 (greyed), un-check again → see 250 again. The invariant is that registry holds free-edit values only; force-align mode pushes computed values to the engine but never writes them back to registry. Documented as R4 in [`tasks/todo.md`](tasks/todo.md) and verified live before ship.

6. **`SetCurrentProcessExplicitAppUserModelID` requires `_WIN32_WINNT >= 0x0601`.** Project-wide define is `0x0501` (XP). Bumping it would silently enable Win7+ APIs in other headers the codebase touches. Resolved by loading the function dynamically via `GetProcAddress(shell32, "SetCurrentProcessExplicitAppUserModelID")` — keeps the define stable and gracefully no-ops on Win XP/Vista where the function isn't exported.

---

### Frequently-used textures palette

*2026-05-15 · [`4897eee`](https://github.com/DrKnickers/particle-editor/commit/4897eee) · #69*

A new **palette popup** on the Appearance tab surfaces the textures the user has recently picked or pinned, per mod, as 140×160 thumbnail cells. The popup is opened by a small painter's-palette button in the Textures groupbox header — Win32 toggle behaviour, modeless and sticky, position remembered across sessions. Double-clicking a cell applies that texture to the Color or Bump slot (chosen by the filter toggle at the top of the popup) and closes the popup. Hovering a cell reveals a thumbtack badge in the top-right of the thumbnail; clicking it pins the entry into the Pinned section (Pinned and Recent each cap at 8; a transient status strip shows "Pins full (8). Unpin one to make room." when overflow is attempted, auto-clearing after 3 seconds). Recents auto-populate on every successful texture load — file-picker pick, palette double-click, and `EN_KILLFOCUS` on the existing Color / Bump edit fields (not per-keystroke, so typing a filename doesn't pollute Recent with the in-progress fragments). Mod switches swap the palette automatically; the in-memory thumbnail cache is invalidated so identically-named files from different mods don't share a stale preview.

Same PR brings the **ground-texture picker** into visual + behavioural parity with the new palette popup. Picker is now modeless with the same `WS_EX_TOOLWINDOW` chrome, position memory, single-click commit + close. Cells are custom-painted via a `WM_PAINT` subclass of the ListView so the native selection / hot-track chrome can't bleed through (the previous `CDRF_SKIPDEFAULT` approach left subtle artefacts — blue label text on the selected slot, hot-track frames leaking onto every cell). Cell visuals — blue hover background, 3 px lighter-blue hover frame, 2 px saturated-blue selection frame, 1 px grey default frame, ellipsis-clipped filename strip — share the exact RGB constants and `DrawText` flags as the palette popup.

**`CLAUDE.md`** picks up a new *Pre-handoff testing* subsection codifying the rigor expected before asking the user to verify a build (build the binary yourself, walk every code path mentally, verify rendered geometry, document the test pass in the handoff message). Carved out of the iteration cycle this PR drove.

**How we tackled it.** Two new source files: [`src/UI/TexturePalette.h`](src/UI/TexturePalette.h) and [`src/UI/TexturePalette.cpp`](src/UI/TexturePalette.cpp) for the palette popup, plus [`src/UI/PaletteStore.cpp`](src/UI/PaletteStore.cpp) which holds the `PaletteStore` data layer (split out so the test exe can link against it without dragging in d3dx9 / GDI / popup-window dependencies). The popup window class `AloTexturePalettePopup` (top-level, `WS_EX_TOOLWINDOW`, owned by the main editor) hosts a content child of class `AloPaletteContent` that owner-draws the cells via the palette's `DrawCell` — fill cell bg, blit thumbnail, frame, pin badge BitBlt from `IDB_PIN_BADGE` (24×48 strip, top half empty / bottom half filled, generated via [`tools/generate_pin_badge.py`](tools/generate_pin_badge.py)), filename label. Double-buffered painting via off-screen `CompatibleDC` + `BitBlt` to suppress flicker. Position memory persists in the same INI under a `[ui]` section keyed off SHA-derived (actually CRC32) mod-path hashes; the popup-position section survives Reset View Settings, which only clears the active mod's palette entries. Hover detection: `WM_MOUSEMOVE` + `TrackMouseEvent(TME_LEAVE)`, plus a forwarded `Esc` through to `HidePopupAndReset` since native dialog Esc-translation doesn't fire on custom window classes.

The ground-picker port reuses the palette's pixel constants but keeps the existing dialog + ListView for the hit-test / right-click context-menu / OK / Cancel / Reset wiring — a `SetWindowLongPtr(GWL_WNDPROC)` subclass overrides `WM_PAINT` and `WM_ERASEBKGND` while letting everything else fall through to the native ListView. Picker thumbnails are regenerated at 192 px (was 64 px) via the existing `MakeGroundSlotThumbnail`, the dialog template grows to 576×340 du to accommodate the bigger cells, and `WS_BORDER | WS_EX_CLIENTEDGE` come off the ListView so the cell tray sits flush with the dialog background.

**Test harness.** [`tests/test_palette_store.cpp`](tests/test_palette_store.cpp) is a standalone console exe that exercises `PaletteStore` directly — 83 assertions across 17 scenarios (cold start, recent eviction at cap, pin overflow rejection, mod switch isolation, per-mod filter persistence, case-insensitive mod paths, malformed-filename rejection, popup position round-trip, etc.). Backs up the user's real INI before tests, restores after. Builds via `cl.exe` against `PaletteStore.cpp` + `crc32.cpp` + `utils.cpp` only — no d3dx9 dependency. Run with `tests\test_palette_store.exe`, expects `Results: 83 passed, 0 failed`.

**Issues encountered and resolutions.** Five worth recording:

1. **`SetActiveMod("")` was wiping the previous mod's INI section.** Switching to "Unmodded" and back would lose Mod's palette state. Caught by `test_empty_mod_path_is_noop` on the first run of the test suite — that was exactly the bug the test was designed to catch. Fixed: empty `modPath` now just clears `m_activeMod` in memory; section wiping stays reserved for `Reset View Settings` (`ClearActiveMod`).

2. **Thumbnail filenames resolved via the wrong path.** `PaletteStore` stores basenames (`p_smoke.tga`) matching how `ParticleSystem::Emitter::colorTexture` stores them. But `FileManager::getFile` expects paths relative to the `basepaths` it was constructed with — it doesn't know about the engine's `Data\Art\Textures\` convention. `DecodeThumbnail` was calling `getFile(filename)` directly, so every texture lookup failed and the popup rendered only missing-placeholder thumbnails (the "empty squares" the user reported). New `OpenTextureFile` helper mirrors `TextureManager::getTexture`'s resolution order — uppercase the filename, prepend `Data\Art\Textures\`, fall back to `.DDS` extension swap.

3. **Resource compile not picking up bitmap changes.** Regenerating `pin_badge.bmp` via the Python script didn't trigger MSBuild's ResourceCompile step because the `.rc` file's mtime didn't change. Fix: `touch src/ParticleEditor.rc` before rebuild, OR delete `x64/Debug/ParticleEditor.res`. Documented in the commit message for the thumbtack-icon redesign.

4. **Ground picker's cells all showed the hover frame.** Initial implementation used `NM_CUSTOMDRAW` with `CDRF_SKIPDEFAULT` in `CDDS_ITEMPREPAINT` to take over the per-item paint. But ListView's hot-track chrome (`LVS_EX_TRACKSELECT`) and selection chrome are drawn through code paths `CDRF_SKIPDEFAULT` doesn't reach — every cell ended up with a thick blue border, and the selected slot's label rendered in blue-underlined link-style text. Subclassing `WM_PAINT` entirely (via `SetWindowLongPtr(GWL_WNDPROC)`) and `return 0`-ing for paint messages was the only reliable way to suppress the native chrome. Documented in [src/main.cpp](src/main.cpp) at the `GroundLVSubclassProc` definition.

5. **Subtle hover state at first.** The original hover indicator was a tiny star icon in the corner — easy to miss against busy thumbnails. User feedback drove successive bumps in contrast: subtle blue tint → bright yellow tint (for diagnostic) → settled on saturated light blue + 3 px lighter-blue frame. Final colour constants live in [src/UI/TexturePalette.cpp](src/UI/TexturePalette.cpp) and are reused verbatim by the ground picker.

---

### Selectable ground texture

*2026-05-14 · [`c545711`](https://github.com/DrKnickers/particle-editor/commit/c545711) · #67*

The preview's ground plane is no longer hardcoded to `dirt.bmp`. A new **`Ground Texture:`** label + 24×24 owner-drawn preview button in the top toolbar (next to the existing Ground Height spinner and Background colour button) shows a thumbnail of the currently-selected ground texture. Clicking the preview opens a modal **Ground Texture** picker with a 4×2 grid of 64×64 slot thumbnails. Bundled slots are **Dirt** (preserved from pre-), **Grass**, **Sand**, **Snow** (vanilla EaW textures `W_TEMPGRND00.DDS`, `W_SAND00.DDS`, `W_SNOW_RGH.DDS` bundled via RCDATA), and a special **Solid Color** slot driven by a user-picked `COLORREF` (default flat grey RGB(128,128,128)). Three more slots — Custom 1, Custom 2, Custom 3 — start empty.

**Slot interactions in the picker:**
- *Single-click any populated slot* — engine swaps live, toolbar preview updates, selection persists.
- *Single-click the Solid Color slot* — selects + opens `ChooseColor` immediately. Pick a colour → engine regenerates a 1×1 D3D texture at that colour; wrap-mode sampling tiles it across the entire ground.
- *Single-click an empty Custom slot* — opens `GetOpenFileName` filtered to `.bmp;.dds;.tga;.png;.jpg`. On selection, slot is populated, thumbnail rebuilds, slot becomes selected.
- *Right-click any slot* — context menu with the actions appropriate to that slot's current state (Set custom texture… / Change color… / Reset to bundled default / Clear slot).
- *Reset all slots to defaults* button — confirm dialog, then wipes every slot's customisation. **Reset View Settings deliberately does NOT touch slot assignments** (per user request: slot customisations are user data, not view settings).

**Path display:** a label below the grid shows the currently-selected slot's file path. Long paths render with `SS_PATHELLIPSIS` (drive letter and filename visible, middle elided as `…`); hovering the label pops a tooltip showing the full path verbatim (max 600 px wide; wraps onto multiple lines for very long paths). For bundled-default slots and the Solid Color slot, the label is empty and the tooltip is suppressed.

**Persistence** lives in `HKCU\Software\AloParticleEditor`: `GroundTexture` (REG_DWORD, current slot index 0–7), `GroundTextureSlot{0..7}` (REG_SZ, per-slot custom file path), `GroundSolidColor` (REG_DWORD, current solid colour). Out-of-range / wrong-type / corrupt values silently fall back to defaults. Stale paths (e.g. file moved between sessions) cause the slot to revert to its bundled default if it has one, or become empty if not. Lost-device recovery routes through the same `Engine::ReloadGroundTexture` helper that handles init, so the user's selection survives Alt-Tab and fullscreen transitions.

**How we tackled it.** `Engine` ([src/engine.h](src/engine.h), [src/engine.cpp](src/engine.cpp)) gains `m_groundTextureIndex` + `m_groundSlotCustomPaths[kGroundTextureCount]` + `m_groundSolidColor`, plus three public setters (`SetGroundTexture` / `SetGroundSlotCustomPath` / `SetGroundSolidColor`) and an `IsGroundSlotEmpty` query. A single private `ReloadGroundTexture()` helper handles the priority cascade (custom path → bundled RCDATA → fallback to slot 0); the solid-colour slot short-circuits to a procedural 1×1 texture built via `IDirect3DDevice9::CreateTexture` + `LockRect`. The existing `IDB_GROUND` resource migrated from `BITMAP` to `RCDATA` so `D3DXCreateTextureFromFileInMemory` handles every supported format identically.

UI lives in [src/main.cpp](src/main.cpp). The toolbar preview is a plain `BUTTON` with `BS_OWNERDRAW` style; the main wndproc's `WM_DRAWITEM` handler stretch-blits the cached 24×24 thumbnail with a 1 px border and focus / pressed feedback. Thumbnail generation (`MakeGroundSlotThumbnail`) takes a slot index, target size, custom path, and the current solid colour; loads the source via `D3DXCreateTextureFromFileEx` or `D3DXCreateTextureFromFileInMemoryEx` into a `D3DPOOL_SCRATCH` surface, then `LockRect` + `CreateDIBSection` to build a 32-bit HBITMAP. The solid-colour slot short-circuits to a `FillRect` + outline; empty slots get a light-grey "+" placeholder via GDI.

The picker dialog (`IDD_GROUND_TEXTURE_PICKER`) uses a `SysListView32` in icon mode with a 12-entry `HIMAGELIST`. Selection-change live-updates the engine + persists the selection to the registry. The dialog's Cancel button reverts the engine to whatever slot was selected when the dialog opened (slot mutations stay, since those are intentional user data). The picker's bottom-of-dialog path label is a STATIC with `SS_PATHELLIPSIS | SS_NOTIFY`; an attached `TOOLTIPS_CLASS` control gives the full path on hover.

**Issues encountered and resolutions.**

- **First-launch access violation on `SAFE_RELEASE(m_pGroundTexture)`.** The pre-code never NULL-initialised `m_pGroundTexture` because `D3DXCreateTextureFromResource(..., &m_pGroundTexture)` writes the pointer directly without reading it. My new `ReloadGroundTexture` calls `SAFE_RELEASE` before assigning, dereferencing a garbage pointer on the very first init. **Fix**: explicitly `m_pGroundTexture = NULL` in the constructor's early-init block, before the first `ReloadGroundTexture` call.
- **"Custom 1" slot showed a pink load-failure placeholder.** The placeholder-decision logic in `MakeGroundSlotThumbnail` used a hardcoded `slot < 6` check (the old bundled count). With the bundled count reduced to 5 and the Solid Color slot at index 4, slot 5 (Custom 1) was the only slot where `slot < 6` was true but no bundled resource existed. **Fix**: replace the hardcoded `6` with `Engine::kGroundTextureBundledCount`, AND additionally exclude `Engine::kGroundSolidColorSlot` from the "has bundled" predicate.
- **Tooltip on the path label didn't appear.** The static control was returning `HTTRANSPARENT` from `WM_NCHITTEST` (default for STATIC without `SS_NOTIFY`), so the tooltip's `TTF_SUBCLASS` hook never received mouse-move events. **Fix**: add `SS_NOTIFY` to the .rc declaration. Additionally found that `TTM_ADDTOOL` was returning FALSE silently — the editor has no application manifest opting into ComCtl32 v6, so the modern `sizeof(TOOLINFOW)` (68 bytes including `lpReserved`) is rejected by ComCtl32 v5. **Fix**: use `TTTOOLINFOW_V2_SIZE` (60 bytes) for both `TTM_ADDTOOL` and `TTM_UPDATETIPTEXT`.
- **Initial tooltip text was lost.** The first `LVN_ITEMCHANGED` (fired during `RefreshList` inside `WM_INITDIALOG`) ran BEFORE the tooltip was created, so the initial slot's path never reached the tooltip. **Fix**: explicit `GroundTexturePicker_SetPathDisplay` call after tooltip creation at the end of `WM_INITDIALOG`, syncing both the label and tooltip to the current slot's state.

---

### Configurable exempt set per link group

*2026-05-14 · [`238c0a1`](https://github.com/DrKnickers/particle-editor/commit/238c0a1) · #65*

The v1 hard-coded exempt set (textures + atlas-index curve + name) becomes the default for new and pre-existing groups, and is now overridable per group through a new **Group settings…** dialog reached from the right-click menu when a linked emitter is selected. The dialog lists ~50 emitter fields grouped by category (Textures / Curves / Lifetime / Physics / Appearance / Weather / Rotation / Misc). **Checked** rows are *shared* — the field propagates across all group members on edit. **Unchecked** rows are *per-emitter* — each member keeps its own value. A *Reset to defaults* button restores the v1 set (textures + atlas index unchecked = per-emitter; everything else checked = shared) without leaving the dialog.

If the user clears an exempt flag on a field where group members currently hold divergent values, a confirmation summary appears at OK time listing each affected field and the canonical (first-in-tree-order) member's value that will overwrite the others. **Yes** applies the overwrites and the new flag set; **No** keeps the settings dialog open so the user can adjust before retrying or cancelling. The disagreement check skips entirely when every cleared flag's field already agrees across members.

Per-group flags persist in a new editor-only system-body chunk **`0x0003`** sibling to the existing `0x0002` leaveParticles chunk. The chunk is emitted only when at least one group has a non-default exempt set — files without customization remain byte-identical to pre-output. The per-entry `flagsByteCount` prefix is forward-compatible: older editors load files saved by newer versions and tolerate extra trailing bytes; newer editors load older files and default the missing tail. The game engine ignores unknown system-level chunks (established by the existing `0x0002` chunk), so files render unchanged in EaW/FoC.

The propagation hook in `CaptureUndo` consults `ParticleSystem::getLinkExemptFlags(linkGroup)` instead of the static defaults, and `JoinLinkGroup` honours the target group's *current* exempt set when adding new members — a joiner inherits the group's customization rather than being silently overwritten by the v1 defaults.

**How we tackled it.** `LinkExemptFlags` ([src/LinkGroup.h](src/LinkGroup.h)) grows from 4 bools to ~58 (one per exempt-eligible emitter field, including the 7 documented `unknownXX` placeholders that no UI surfaces but the data model preserves). The struct stays POD; the `operator==` uses `memcmp` so `ParticleSystem::setLinkExemptFlags` can normalize default-equal entries out of the map (`m_linkExempts`), keeping the on-disk representation minimal.

`ParticleSystem::getLinkExemptFlags(groupId)` returns a const reference: the map entry if present, otherwise the static `GetDefaultLinkExemptFlags()` (renamed from the pre-`GetLinkExemptFlags()`). Storage is a `std::map<uint32_t, LinkExemptFlags>` on `ParticleSystem`, with the system writer emitting chunk `0x0003` only when non-empty.

`Emitter::copySharedParamsFrom` ([src/ParticleSystem.cpp](src/ParticleSystem.cpp)) expands from 4 hand-restored fields to ~58, organized as an if-ladder mirroring the existing structure (`if (exempt.field) field = saved;` × N). The saves happen unconditionally before the bulk `*this = src`; the conditional restores after pick which fields stay per-emitter. A `#ifndef NDEBUG`-only assertion at the function tail spot-checks four representative fields against their saved values — fires if a future contributor adds a flag to `LinkExemptFlags` without adding the matching restore line.

`DiffNonExemptParams` ([src/LinkGroup.cpp](src/LinkGroup.cpp)) gains a `const LinkExemptFlags&` parameter so the three confirm-dialog call sites in `EmitterList.cpp` can pass the right group's flags (or the v1 defaults for not-yet-existing groups in the Link / Link-with paths).

The settings dialog lives in [src/UI/EmitterList.cpp](src/UI/EmitterList.cpp) along with the other link-group menu logic. The field table `kLinkSettingsFields` pairs each visible flag with a display label, a category, and a `bool LinkExemptFlags::*` pointer-to-member; the dialog proc walks the table to populate the `SysListView32` and to read checkbox state back into a working copy at OK time. The disagreement check at OK iterates the same table, calling `MembersAgreeOnField` / `FormatFieldValue` / `ApplyCanonicalValueToField` (also table-driven via the same pointer-to-member). The hex-dump of the final flag bytes is printed under `#ifndef NDEBUG` for verifying the dialog → on-disk pipeline.

The disagreement UX is intentionally simpler than the original plan's per-field radio picker: a single `MessageBox` lists all disagreeing fields and the canonical values that will overwrite the others, with Yes / No to apply or cancel. Q4's accepted default ("first-in-tree-order's value wins") removes the need for an interactive picker — users wanting a different canonical value re-order emitters before opening the dialog. A richer picker can land later if usage shows the auto-pick is too restrictive.

Resource IDs in the 40160 / 1600 / 170 ranges; resource pairs `IDD_LINK_GROUP_SETTINGS` and `IDD_LINK_GROUP_DISAGREEMENT` declared in both [src/ParticleEditor.en.rc](src/ParticleEditor.en.rc) and [src/ParticleEditor.de.rc](src/ParticleEditor.de.rc) with English labels per the existing convention (the German `.rc` carries English strings for new editor features; translation is a future docs item). `IDD_LINK_GROUP_DISAGREEMENT` is declared but not currently shown — the `MessageBox` flow replaced it. The resource is kept so a richer picker can land without re-touching the `.rc` files.

**Issues encountered and resolutions.**

- **`LinkExemptFlags` forward declaration into `ParticleSystem.h` without dragging in `LinkGroup.h`.** The header needed `LinkExemptFlags` for the accessor signatures but couldn't include `LinkGroup.h` because `LinkGroup.h` itself includes `ParticleSystem.h` (circular). **Fix**: forward-declare `struct LinkExemptFlags;` in `ParticleSystem.h` and include `LinkGroup.h` in `ParticleSystem.cpp` for the implementation.
- **Forgotten restore in `copySharedParamsFrom` would be silently miscalibrating.** A new flag added to `LinkExemptFlags` without an `if (exempt.X) X = sav_X;` restore line would compile fine but silently propagate the field anyway. **Fix**: `#ifndef NDEBUG` spot-check assertion at the function tail (lifetime / gravity / colorTexture / acceleration). Catches the bug pre-ship on the first propagation in a debug build.
- **`Reset to defaults` had to apply the disagreement check too.** Original draft made Reset bypass the OK-time disagreement flow, which would silently overwrite values when defaults re-shared a field that had drifted. **Fix**: Reset only mutates the local working copy of the flags; the OK button still runs the disagreement check against `oldFlags vs newFlags`, just with `newFlags == defaults`. Consistent semantics across all flag-change paths.
- **`Dissolve link group` orphan exempt entries.** Dissolving a group removed the membership but left the group's `m_linkExempts` entry in place — harmless but bloats files. **Fix**: the dissolve handler now also calls `setLinkExemptFlags(gid, GetDefaultLinkExemptFlags())` which (via the normalize-on-default behaviour) erases the entry from the map.
- **`JoinLinkGroup` was using v1 defaults for newcomers.** Adding an emitter to a custom-exempt group would silently overwrite the joiner's `lifetime` (if `lifetime` was exempt in the group) because `JoinLinkGroup` called `GetLinkExemptFlags()` (v1 defaults) instead of the group's actual flags. **Fix**: pass `system.getLinkExemptFlags(groupId)` instead. Joiners now inherit the group's customization correctly.

---

### Visual link-group bracket for linked emitters

*2026-05-14 · [`075ccbe`](https://github.com/DrKnickers/particle-editor/commit/075ccbe) · #63*

The emitter tree's right margin now carries a coloured bracket per link group, so group membership is legible at scroll-speed. Each group claims a lane (greedy interval scheduling sorted by topmost member's Y; non-overlapping groups share a lane). A 12-entry Tableau-derived palette (luminance-shifted to hit WCAG 2.1 non-text contrast against `COLOR_WINDOW`) is mapped via `groupId % 12`, with the first 6 entries ordered for maximum perceptual distance because realistic particle systems mostly use ≤ 6 simultaneous link groups. Dots mark each member row at `(laneX, rowCentreY)` with a 5 px horizontal stub pointing toward the row text; a vertical lane line connects topmost-to-bottommost dot. Lane width is DPI-aware (6 px at 96 DPI) and floors at 2 px when the system is packed with many overlapping groups.

**Hover** any dot or line and the group's member rows pick up a ~15% alpha tint in the group's palette colour while the lane line thickens to 2× stroke. The line thickening is the primary visual cue ("you're hovering over group N"); the tint confirms which rows belong. Hover transitions invalidate the tree and re-paint within one frame; hover state is cleared on `WM_MOUSELEAVE`, `WM_KILLFOCUS`, and `WM_CAPTURECHANGED` so it never survives a drag, modal dialog, or Alt-Tab. `TrackMouseEvent` with `TME_LEAVE` is re-armed each move.

**Click** any dot or line and the multi-selection becomes the group's full member list with primary set to the topmost viewport-visible member. **Ctrl-click** extends the existing multi-set with the group rather than replacing it. Shift- and Alt-click on a bracket are treated as plain click (no useful "range" semantic when the gesture is "this whole group"). The bracket lives strictly in a 4–9 px right-edge gutter (`rightEdgeOffset = clientRect.right - 4 px DPI-adjusted`), so it never overlaps label text at any sane tree width — clicks even 10 px left of a dot fall through to the regular row-click path.

**High-Contrast theme**: when `SystemParametersInfo(SPI_GETHIGHCONTRAST, …)` reports HC active, all brackets paint in `GetSysColor(COLOR_HIGHLIGHT)` instead of the palette. Group identity in HC mode comes from lane position and the existing `[L<n>]` text prefix in the row label — the user's HC theme intent isn't overridden with custom RGB. `WM_THEMECHANGED` and `WM_SETTINGCHANGE(SPI_SETHIGHCONTRAST)` invalidate the tree so a theme switch is live without restarting.

**Q4 follow-up shipped in the same PR**: `EmitterList_DeleteEmitter` ([src/UI/EmitterList.cpp:3878](src/UI/EmitterList.cpp:3878)) now iterates `multiSelection` rather than acting only on the primary, so bracket-select → Delete kills the whole group in one undo step (a single Ctrl+Z restores all N emitters). Mixed multi-sets work the same: select 3 unlinked + 1 linked, press Delete, all 4 vanish. Single-emitter selection is the multi-set-of-size-1 path — no behavioural change for users who never multi-select.

**How we tackled it.** All state lives on `EmitterListControl` ([src/UI/EmitterList.cpp:229](src/UI/EmitterList.cpp:229)): a new `BracketLayout bracketLayout` cache, `uint32_t hoveredGroupId`, and `bool mouseTrackingArmed`. The cache is rebuilt at `CDDS_PREPAINT` every paint via `RebuildBracketLayout`, which walks expanded tree rows via `TreeView_GetItemRect`, buckets linked emitters by `linkGroup`, filters to groups with ≥ 2 visible members, sorts by `minY`, and assigns lanes via greedy interval scheduling. Always-rebuild keeps the implementation simple and sidesteps an entire class of cache-staleness bugs (scroll, expand/collapse, window resize, group mutation, theme switch all "just work"); the walk is O(N log N) and under 1 ms for hundreds of emitters. The `bracketLayout.valid` flag stays in the struct for future optimisation if profiling surfaces a need.

Painting reuses the existing `NM_CUSTOMDRAW` handler ([src/UI/EmitterList.cpp:2154](src/UI/EmitterList.cpp:2154)). `CDDS_POSTPAINT` paints all bracket geometry (lane line, then per-member stubs + dots) before the marquee frame so an active marquee always appears on top of brackets. `CDDS_ITEMPREPAINT` now composes two effects: the multi-select `COLOR_HIGHLIGHT` background (`CDRF_NEWFONT`) and the hover tint (`CDRF_NOTIFYPOSTPAINT`); the return value bitwise-ORs the flags so the tree's default proc carries both through. The tint itself is painted in a new `CDDS_ITEMPOSTPAINT` case via `AlphaBlend` of a 1×1 source DDB stretched to the row rect with `sourceConstantAlpha = 38` (~15%); composing via AlphaBlend over whatever the row currently shows means the tint stacks correctly on multi-select rows without manually computing the blend per pixel. `msimg32.lib` is link-pulled via `#pragma comment` at the top of the file.

Hit-testing lives in `HitTestBracket` — dots first (more specific than lines), then lane span. Hit slop is `dotRadius + 2 px` for dots and `max(2, strokeWidth + 1) px` for lines. A scroll-position stamp on the cache lets the hit-test reject stale clicks (cursor over a dot's pre-scroll position after a mid-frame wheel scroll) so the click harmlessly does nothing rather than selecting the wrong group. Click handling intercepts at the top of `WM_LBUTTONDOWN` in `EmitterTreeViewWindowProc` ([src/UI/EmitterList.cpp:1417](src/UI/EmitterList.cpp:1417)) — before the existing marquee / tree-row dispatch — and eats the message on bracket hit; manually fires `ELN_SELCHANGED` and calls `TreeView_SelectItem` so the tree's own bookkeeping stays consistent.

`WM_MOUSEMOVE`, `WM_MOUSELEAVE`, `WM_KILLFOCUS`, and `WM_CAPTURECHANGED` all funnel hover-clear through a single `ClearBracketHover` helper to keep the clear paths idempotent. `WM_THEMECHANGED` and `WM_SETTINGCHANGE(SPI_SETHIGHCONTRAST)` invalidate the tree and clear hover (palette colour may have changed under the hover state).

`EmitterList_DeleteEmitter`'s multi-emitter rewrite snapshots `multiSelection` into a `std::vector` before iterating, so it tolerates cascade-deletion (deleting a parent recursively destroys its children; later iterations skip already-cascaded targets via `std::find` against the live emitter list). One `ELN_LISTCHANGED` at the end groups all N deletions into a single undo step.

A `#ifndef NDEBUG`-only palette contrast printer (`DebugVerifyBracketPalette`) fires once on first `EmitterListControl` construction and logs each palette entry's WCAG ratio against `COLOR_WINDOW` — failing entries print with a `LOW_CONTRAST` warning so a future palette regression surfaces at app start, not by visual inspection. Debug instrumentation (`[Link] layout`, `[Link] hover`, `[Link] click select`) shares the `[Link]` prefix with's existing tags so a single grep covers all link-group work.

**Issues encountered and resolutions.**

- **'s `CDDS_ITEMPREPAINT` return of `CDRF_NEWFONT` would suppress the per-item postpaint we needed for hover tinting.** The bitwise return is the only correct option — `CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT` keeps the multi-select highlight intact AND gives us the postpaint slot. **Fix**: compute a single `DWORD ret` and OR both flags when their respective conditions hold; return `CDRF_DODEFAULT` only if neither effect fires.
- **Layout cache could go stale in many ways (scroll, expand, resize, mutation, theme).** Each invalidation point would be a separate hook with its own bug class. **Fix**: always rebuild at `CDDS_PREPAINT`. The walk is fast enough that the simplification is free. The `valid` flag remains in the struct as a future-optimisation seat.
- **`AlphaBlend` requires `msimg32.lib` which the project didn't previously link.** Two options were considered: precompute the blend per palette entry as a static `COLORREF[12]` and `FillRect`, or use real alpha-blending. The static option fails when hover stacks on multi-select highlighted rows (the precomputed blend was against `COLOR_WINDOW`, not `COLOR_HIGHLIGHT`). **Fix**: `#pragma comment(lib, "msimg32.lib")` at the top of `EmitterList.cpp` and use `AlphaBlend` so the blend always composes against the actual painted background.
- **Topmost-of-group "primary" candidate needed to be viewport-visible, not absolute-topmost.** Layout walker collects members in tree pre-order, so `members[0]` is topmost-in-tree — but that may be scrolled above the viewport, leaving the inspector showing a row the user can't see. **Fix**: in the click handler, search the cached members for the first whose `centreY` falls in the tree client rect; fall back to `members[0]` only if all members are scrolled out (then the user can scroll to find their primary).
- **Multi-emitter `EmitterList_DeleteEmitter` could double-delete or hit a dangling pointer.** `ParticleSystem::deleteEmitter` recursively destroys an emitter's children, so if multiSelection includes both a parent and its child, naively iterating crashes on the second iteration. **Fix**: snapshot into a vector, then for each target check whether it's still in `system->getEmitters()` before calling `deleteEmitter` — cascaded targets get skipped harmlessly.

---

### Multi-select for the emitter list

*2026-05-12 · [`ff000c4`](https://github.com/DrKnickers/particle-editor/commit/ff000c4) · #60*

The emitter tree now supports multi-emitter selection: **Ctrl-click** toggles individual emitters in and out of the selection, **Shift-click** selects a tree-order range from the anchor to the clicked row, and **click-and-drag from an empty area** draws a marquee that sweeps up every row whose stripe it crosses. Once two or more emitters are selected, the right-click menu surfaces **`Link selected (N emitters)`** (with the canonical-source `ConfirmLinkOverwrite` dialog from) and **`Add selected to link group →`** for fold-into-existing-group workflows; with a mixed selection (one group represented plus some unlinked rows) the menu offers **`Add unlinked to Group N`** so the joiners merge in one click without dissolving the existing group. The "canonical" emitter that governs a Link-selected operation is now whatever you most recently plain- or Ctrl-clicked, so the rule is *"the emitter you clicked last governs the group"* — not always the topmost.

While two or more emitters are selected, the **inspector and curve-editor are locked** (`EnableWindow(FALSE)` on `hPropertyTabs`, `hTrackTabs`, and each `hTrackEditors[i]`) and a translucent ~19% black overlay covers their area as an unambiguous "editing disabled" signal. The overlay is a `WS_POPUP` top-level layered window with `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE` and a `SetWindowRgn` shape that's the union of the two panel rects — so the viewport between them stays clear and clicks pass through the overlay to the (disabled) controls underneath. The custom-draw paint colours every multi-set member with `COLOR_HIGHLIGHT` (including the primary, while in multi-select mode) so the highlight stays visible after the tree loses focus.

Drag-drop reorder is unaffected: dragging a multi-selected primary moves only the primary, leaving the other selected emitters where they are. This matches the original design rationale of letting linked emitters be repositioned independently for interleaved layering. The same idea drives a small but important behaviour: right-clicking *outside* the current multi-set resets to a single-emitter selection on the right-clicked row, but right-clicking *inside* the set preserves it — so the right-click → batch-action sequence always operates on what you intended.

**How we tackled it.** All state lives on `EmitterListControl` ([src/UI/EmitterList.cpp:178](src/UI/EmitterList.cpp:178)): `std::set<Emitter*> multiSelection`, `Emitter* selectionAnchor`, plus marquee state (`marqueeActive` / `marqueeStart` / `marqueeCurrent` / `marqueePreCtrl` / `marqueeSweptHits`). The existing tree-control subclass `EmitterTreeViewWindowProc` intercepts `WM_LBUTTONDOWN` before the default selection runs, hit-tests with `TreeView_HitTest`, and dispatches to one of three paths: marquee start (click in empty area, gated to the left half of the client width so right-side clicks don't accidentally marquee); plain click (forward to default proc, multi-set replaced with `{clicked}`); or Ctrl/Shift modifier click (eat the message, set primary via explicit `TreeView_SelectItem`). A single `UpdateMultiSelectionFromClick` helper handles all the modifier semantics; Ctrl-clicking out the only remaining member is refused so the invariant *"multi-set is non-empty iff primary is non-NULL"* always holds.

Marquee selection uses **sticky semantics**: `marqueeSweptHits` accumulates every row the rect has ever touched during the drag, and the per-frame multi-set is `marqueePreCtrl ∪ marqueeSweptHits`. So later mouse positions never deselect earlier hits, and shared-row-border edge cases (where `IntersectRect` returns zero on exactly-touching rects) don't lose rows. A 1 px inflation on the hit-test rect adds further forgiveness. The final `WM_LBUTTONUP` repeats the hit-test using the release coordinates — `WM_MOUSEMOVE` doesn't fire for the exact pixel where the user releases, so without this pass the bottommost swept row could miss. `marqueeActive` is flipped to `false` before `ReleaseCapture` so the synchronous `WM_CAPTURECHANGED` doesn't mistake the normal release for a "stolen capture" cancellation and roll the multi-set back to `marqueePreCtrl`.

The new `NM_CUSTOMDRAW` handler on the tree (in `DlgEmitterListProc`'s `WM_NOTIFY`) does two things: paints `COLOR_HIGHLIGHT` background for every multi-set member when the set has ≥ 2 entries (overriding the focus-dependent tree default for the primary, so the row stays bright blue even after the tree loses focus to the right-click menu), and overlays the marquee rectangle frame at `CDDS_POSTPAINT` while a marquee is in progress. `TVS_EX_DOUBLEBUFFER` is enabled to suppress flicker. The marquee invalidates the entire tree on every move so secondary highlights track the swept set immediately.

The lock-out overlay is created once in `WM_CREATE` ([src/main.cpp:1908](src/main.cpp:1908)) via a one-off `WNDCLASS` with a `BLACK_BRUSH` background. `SetLayeredWindowAttributes(... 48, LWA_ALPHA)` gives ~19% black. Positioning happens in `WM_SIZE` and `WM_MOVE` so it tracks the main window; `SetWindowRgn` with the union of the two panel rects keeps the viewport gap uncovered. `SetEmitterInfo` toggles its visibility based on a new `EmitterList_GetMultiSelectionSize` accessor. `ELN_SELCHANGED` fires from every multi-set-mutating path (modifier click, marquee mouse-move when the set size crosses the 1↔2 threshold, marquee mouse-up) so the overlay state stays live during the drag.

**Issues encountered and resolutions.**

- **`ReleaseCapture` firing `WM_CAPTURECHANGED` synchronously rolled marquee selections back to empty.** First cut called `ReleaseCapture` before flipping `marqueeActive` to `false`. The synchronous `WM_CAPTURECHANGED` saw `marqueeActive == true`, treated it as a stolen-capture cancellation, and restored `multiSelection = marqueePreCtrl` (empty for non-Ctrl marquee) — undoing every emitter the user had just selected. **Fix**: flip `marqueeActive = false` before the `ReleaseCapture` call so the cancellation branch correctly sees "we're cleaning up normally" and lets the selection stand.
- **`WS_EX_LAYERED` child windows didn't reliably cover the inspector.** Initial attempt used a layered child window (sibling of `hPropertyTabs` / `hTrackTabs`) with `SetLayeredWindowAttributes` for alpha. The overlay painted the curve-editor area correctly but left the inspector uncovered — the custom controls inside the property tabs (Spinner, ColorButton, EmitterProps's own EDIT children) schedule their own `WM_PAINT` cycles independent of sibling Z-order, so they repainted over the overlay after every refresh. **Fix**: make the overlay a `WS_POPUP` top-level layered window owned by the main window. DWM composites top-level layered windows above any child controls of any window underneath, so the inspector's repaints can't punch through.
- **Overlay covered the 3D viewport.** Once the overlay sat on top of everything, its bounding rect spanned the union of property tabs + track tabs — which includes the viewport gap between them. **Fix**: `SetWindowRgn` with `CombineRgn(RGN_OR)` of the two panel rects (in overlay-local coords). The viewport area is outside the region, so the overlay window simply doesn't paint there and clicks pass through normally.
- **Clicks on the overlay had weird side effects.** Without `WS_EX_TRANSPARENT`, clicks landed on the overlay's `DefWindowProc` and could activate the popup or steal focus in subtle ways. **Fix**: add `WS_EX_TRANSPARENT` so the overlay never receives mouse input — clicks pass through to the (disabled) controls underneath, which ignore them.
- **`SS_BLACKRECT` static didn't paint under layered compositing.** First cut tried a `STATIC` control with the `SS_BLACKRECT` style as the overlay window. It registered fine but painted invisibly under `LWA_ALPHA`. **Fix**: register a one-off `WNDCLASS` with `hbrBackground = GetStockObject(BLACK_BRUSH)` and create the overlay using that class.
- **Marquee paint trail.** The marquee frame stacked up as a vertical trail because `InvalidateRect(... FALSE)` doesn't force `WM_ERASEBKGND`, so the previous frame's pixels stayed in empty inter-row space. **Fix**: `InvalidateRect(... TRUE)` so the tree's background brush erases the prior frame; combined with `TVS_EX_DOUBLEBUFFER` for flicker-free repaint.
- **The bottom-most marquee row never visibly selected.** The data was correct (the emitter *was* in `multiSelection`), but it didn't paint blue. Tracing showed the marquee left the primary on that row, and the tree's default paint for the primary greys out when the tree lacks focus — which it does after the marquee menu closes / focus shifts. **Fix**: in `CDDS_ITEMPREPAINT`, override paint for *every* member of `multiSelection` (including the primary) when the set has ≥ 2 entries. Single-emitter selections still use the focus-aware tree default.
- **Marquee selection only covered the bottom of one row, missing the next.** `TreeView_GetItemRect` with `TRUE` returns the *label* rect (narrower vertically than the row, due to padding). A marquee ending mid-gap between rows missed the next label. Plus the right-side issue: `TreeView_GetItemRect` with `FALSE` returns the full row spanning the tree's whole client width, so a marquee drawn to the right of the labels still caught rows. **Fix**: gate the marquee start on the click being in the left half of the tree (where labels live), then use the full row rect (`FALSE`) for the hit-test — generous Y, and the X gate is enforced at the start point rather than per-row.

---

### Linked emitters (share parameters across a group)

*2026-05-12 · [`6a9c7ab`](https://github.com/DrKnickers/particle-editor/commit/6a9c7ab) · #58*

Two or more emitters in a particle system can now be linked into a **link group** whose non-textural parameters stay in lock-step: edit any field on a linked emitter and every sibling in its group instantly updates to match. The motivating workflow is the *"5 emitters, 5 textures, identical motion"* case — atlas variants, fire/smoke colour pairs, layered weather effects — where today each parameter change requires N parallel edits. With link groups, edit one, the rest follow.

Group management lives in the emitter-list right-click menu. **Link with…** (visible on any unlinked emitter when another unlinked emitter exists) opens a submenu of candidate partners and creates a fresh group from the pair. **Add to link group…** (visible when the system already has at least one group and the selection is unlinked) opens a submenu of existing groups so a third / fourth / fifth member can join. **Remove from link group** and **Dissolve link group** appear when the selected emitter is linked. Both Link-with and Add-to-group show a confirmation dialog when the two sides' parameters differ; the dialog spells out which emitter will be overwritten, names the source of the surviving values, and lists every affected field — so silent loss of unique tuning isn't possible. When the diffs are empty (e.g. linking a just-duplicated emitter back to its source), the dialog is skipped entirely.

Linked emitters wear a `[L<n>]` prefix in the tree-row text so group membership is identifiable at a glance independent of any visual affordance. Rename a linked emitter and the prefix is preserved (the bare name is what's edited; the display rebuilds around it). The exempt set — kept per-emitter, never propagated — is hard-coded in v1 to **`colorTexture`**, **`normalTexture`**, the **`TRACK_INDEX`** (atlas-frame) curve, and the **name**. Future configurability is deferred.

Minimum group size is **two members**. Single-member groups can't exist by any user-visible operation: there's no "create empty group" command, and removing the second-to-last member auto-dissolves the group (the dynamic menu label reads *"Remove from link group (dissolves Group N)"* when that branch will fire, so the action isn't a surprise). All link operations are full-snapshot undoable — a single Ctrl+Z reverses any link, join, remove, dissolve, or propagated edit, including the two-emitter auto-dissolve case.

Persistence rides a new optional emitter-body chunk **`0x0100`** carrying the group ID. The chunk is written only when an emitter actually belongs to a group, so files without link groups are byte-identical to pre-feature output. Clipboard copy explicitly suppresses the chunk, so cross-window paste arrives unlinked by design — link-group IDs are local to a single `ParticleSystem` and don't carry semantics across files. The game engine ignores unknown emitter-body chunks (the existing optional `0x0036` spawn-link and `0x0045` normal-texture chunks rely on the same skip-on-unknown behaviour), so files saved with link groups still load and render correctly in EaW/FoC.

Note: this is v1. Three follow-up pieces are deferred to a future PR — **tree multi-select** for an "Ctrl-click N emitters, Link selected in one step" workflow, a **visual link-group bracket** in the right margin of the emitter list (lane-allocated, colour-coded, hover-highlight, click-to-select-group), and **per-field configurable exempt sets**. The data model and propagation hook are designed so each can land as a UI-only addition without re-touching the persistence or undo plumbing.

**How we tackled it.** A new `uint32_t linkGroup` field on [`ParticleSystem::Emitter`](src/ParticleSystem.h:135) carries membership; `0` means unlinked, non-zero IDs are stable across save/load and unique within a system. Group operations live in a new free-function module [`src/LinkGroup.cpp`](src/LinkGroup.cpp) / [`.h`](src/LinkGroup.h): `CreateLinkGroup`, `JoinLinkGroup`, `LeaveLinkGroup`, `DissolveLinkGroup`, `GetLinkGroupMembers`, `GetAllLinkGroupIds`, `DiffNonExemptParams`. The diff helper drives both the menu-time decision to skip the confirmation when params already agree AND the affected-fields list in the dialog when they don't.

Edit propagation hooks into the universal post-edit chokepoint [`CaptureUndo` in `src/main.cpp`](src/main.cpp:764): immediately before snapshotting, if the just-edited emitter belongs to a link group, every sibling's non-exempt fields are overwritten to match. Because the existing undo system already snapshots the *whole* `ParticleSystem`, one user edit produces one undo step covering every propagated change — no special multi-emitter undo plumbing was needed. The whole-system snapshot also makes link-state itself trivially undoable (the `linkGroup` field rides the snapshot like any other emitter field), and the load-time initial-`CaptureUndo` already wired in [`main.cpp:976`](src/main.cpp:976) means "undo back to before the link op" works even when the link is the first action after opening a file.

The shared-params copy [`Emitter::copySharedParamsFrom`](src/ParticleSystem.cpp:555) reuses the existing copy-constructor pattern (`*this = src` then repoint `tracks[]` via offset arithmetic into our own `trackContents[]`, mirroring `src`'s aliasing) — same approach the Duplicate path has been using safely since #19. Structural fields (`parent`, `spawnOnDeath`, `spawnDuringLife`, `index`), the private `m_instances` set, and the exempt fields are saved before the bulk copy and restored after, so propagation never corrupts the runtime EmitterInstance bookkeeping or the per-emitter hierarchy. The atlas-index track is explicitly de-aliased on the destination to enforce its per-emitter status.

The new chunk type ID `0x0100` was picked above the existing emitter-body range (which tops out at `0x0045`) and below the group internals at `0x1100` — clear headroom on both sides. Writer emits only when `linkGroup != 0` and `!copy` (so clipboard-format buffers never carry the chunk); reader handles the chunk as a third optional position-after-tracks chunk, identical pattern to the existing `0x36` and `0x45` cases. Pre-feature files load with every emitter unlinked.

UI surface lives in [`src/UI/EmitterList.cpp`](src/UI/EmitterList.cpp): `FormatEmitterDisplayName` composes the `[L<n>]` prefix used by every tree-population path (initial load, child population, Move, single-row refresh after a link op); `StripLinkGroupPrefix` guards the rename path so a user who edits the row literally won't persist the prefix into the underlying name. Link-menu items are appended to the existing right-click popup dynamically per click (no `.rc` churn), driven by selection state — `Link with…` and `Add to link group…` submenus are built fresh each time with the current candidate / group sets, and a single cleanup walk removes everything added since the static menu's last entry before returning. The shared `ConfirmLinkOverwrite` helper renders both dialogs.

Resource IDs 40119, 40120, and the 40130–40159 dynamic range were added to both [`resource.en.h`](src/Resources/resource.en.h) and [`resource.de.h`](src/Resources/resource.de.h); no `.rc` menu changes were needed.

**Issues encountered and resolutions.**

- **Double-free risk in dynamic submenu cleanup.** First cut of the popup-cleanup walk called both `DestroyMenu(hSubmenu)` and `DeleteMenu(parent, position, MF_BYPOSITION)` on each MF_POPUP entry. Per MSDN, `DeleteMenu` already destroys any submenu attached to the deleted item — so the explicit `DestroyMenu` was a double-free that would corrupt the heap on the next right-click. Caught during the pre-test audit; fixed by removing the explicit `DestroyMenu` and trusting `DeleteMenu` to do the cleanup. The cleanup walk now stops at the original `ID_EDIT_DELETE` (always the last static menu entry), so it correctly removes every dynamically-appended item including separators and submenus without touching the static menu.
- **`m_instances` corruption from naive copy-via-`operator=`.** `Emitter`'s default copy-assignment operator copies the private `m_instances` set, which holds raw pointers to `EmitterInstance` objects belonging to a specific emitter — if `copySharedParamsFrom` had used that operator directly without restoring, propagation would have left the destination emitter pointing at the source's runtime instances, with double-frees on destruction. **Fix**: save `m_instances` before `*this = src`, restore after. Same pattern for structural and exempt fields. Implementing `copySharedParamsFrom` as a member function (rather than a free function in `LinkGroup.cpp`) gave the access to `m_instances` it needed.
- **Track-aliasing breakage on the bulk copy.** After `*this = src`, the destination's `tracks[]` pointers point into `src`'s `trackContents[]` — releasing those pointers later would crash. The existing copy constructor already handles this with `tracks[i] = trackContents + (src.tracks[i] - src.trackContents)`; `copySharedParamsFrom` reuses the exact pattern. The atlas-index track is then forcibly de-aliased on the destination (`tracks[TRACK_INDEX] = &trackContents[TRACK_INDEX]`) because v1 treats it as intrinsically per-emitter regardless of whatever aliasing the source may have set up.
- **`AnsiToWide(emitter->name).c_str()` lifetime in `swprintf`.** Easy to misread as a dangling-pointer trap, but per the C++ standard the temporary `std::wstring` returned by `AnsiToWide` lives until the end of the full expression that created it — which includes the entire `swprintf` call. Validated this by re-reading [temporary lifetime rules]; the pattern is safe as long as the temporary isn't bound to a longer-lived reference first. The code uses the safe pattern throughout, with the temporary appearing directly as a `swprintf` argument.
- **`CreateLinkGroup` overwrites the second member silently.** Original plan §3.5(c) explicitly stated *"Skipped entirely for Create"* (the rationale being that Create *"seeds from the first selection"*). In practice that still meant the second member's params got overwritten with no warning. Per user direction, added a matching `ConfirmLinkOverwrite` dialog to the Link-with path so both Create and Join surface the diff and the overwrite direction. Same skip-on-empty-diff behaviour as Join; same wording template ("X will be overwritten to match Y"); same exempt-field disclosure.

---

### Duplicate with index increment

*2026-05-12 · [`c60cb2e`](https://github.com/DrKnickers/particle-editor/commit/c60cb2e) · #56*

Two new entries appear in the emitter right-click context menu directly below *Duplicate*: **Duplicate (increment index)** shifts every keyframe on the atlas index track (`TRACK_INDEX`) by +1 in one click; **Duplicate (increment index...)** prompts for an integer increment N (1–999) first, making larger atlas jumps equally fast. The motivating workflow: build one base emitter aimed at atlas frame 0, right-click-duplicate 15 more times, and each copy automatically targets the next sprite-sheet cell — no track editor required.

**How we tackled it.** Three additions to [`src/UI/EmitterList.cpp`](src/UI/EmitterList.cpp): a `ShiftIndexTrack` helper that rebuilds the `std::multiset<Key>` with all values offset by the delta (multiset elements are const-qualified through iterators, so in-place mutation is blocked; rebuild is the correct pattern), an `IncrementIndexDlgProc` / `ShowIncrementDialog` pair for the prompt variant, and two new `case` branches in the right-click dispatch. `EmitterList_DuplicateEmitter` gained a `float indexDelta = 0.0f` parameter; the shift fires on the newly-inserted emitter *before* `NotifyParent(ELN_LISTCHANGED)` so the duplicate and the index change land in a single undo step. Menu items were added to both `.en.rc` and `.de.rc`; a minimal `IDD_INCREMENT_INDEX` dialog (EDIT + `msctls_updown32` spin + OK/Cancel) was added to both RC files. Resource IDs `40117–40118` and dialog `152` were allocated in both resource headers.

**Issues encountered and resolutions.** No surprises — the multiset const-element constraint and the undo ordering risk were both identified in the plan (§4 risks 1 and 3) and their mitigations were baked in from the start. Undo ordering was confirmed safe by tracing `ELN_LISTCHANGED → CaptureUndo` in [`src/main.cpp`](src/main.cpp); the snapshot fires inside `CaptureUndo` called from the `ELN_LISTCHANGED` handler, which is after `NotifyParent`, so placing `ShiftIndexTrack` before that call requires no extra coordination.

---

### Pause / frame-step the preview

*2026-05-11 · [`2899f5b`](https://github.com/DrKnickers/particle-editor/commit/2899f5b) · #53*

Press **F8** to freeze the preview at the current simulation tick; press it again to resume from exactly where time left off (no time-warp pop, no synthetic catch-up burst). While paused, **F9** steps the simulation forward by one notional 60 Hz frame; **F10** steps ten frames (≈167 ms — enough to traverse a one-second particle lifetime in six presses). All three actions also live under *View → Pause Preview / Step 1 Frame / Step 10 Frames*, and as three new toolbar buttons next to the existing Bloom toggle: a pause check-button (cell 8, two-vertical-bars glyph), step-1 (cell 9, ▷|), step-10 (cell 10, ▷▷|). The two step buttons and the corresponding menu entries grey out when not paused. The FPS pane in the status bar suffixes ` · PAUSED` while frozen so the state is glanceable. The clock is process-local — pause always starts off on launch, by design.

(Caveat on F10: Win32 normally treats F10 as the menu-activation key. Registering it as an accelerator overrides that behaviour for this editor — the menu remains reachable via `Alt+<letter>` mnemonics, which were already working. Mirrors how Visual Studio binds F10 to "step over.")

Note: the spawner manual-fire shortcut moved from **Shift+Space** to **Ctrl+Space**. The "Spawn now" button in the Spawner dialog has been relabeled to match; the **F7** open shortcut is unchanged. The rebind preserves `Shift` for any future "modify the gesture" semantics while keeping `Ctrl` for "trigger a discrete action," which is the more idiomatic Win32 split.

**How we tackled it.** Pause hooks into the engine's single time source — [`GetTimeF()` in `src/engine.cpp`](src/engine.cpp:37). Every consumer of "simulation now" — emitter spawn time, particle Update dt, the shader `hTime` uniform, the spawner driver dt — already funnels through that one function, so freezing time at this single site freezes the whole simulation while `Engine::Render()` keeps drawing the last frame. Three new free functions (`SetPreviewPaused` / `IsPreviewPaused` / `StepPreviewFrames`) maintain a small clock-offset state: while running, `simTime = wall - g_pauseOffset`; while paused, `simTime = g_previewPauseAnchor` (frozen). On resume the offset is re-derived from the (possibly stepped) anchor, so pause/resume produces no discontinuity *and* any frame-stepping during the pause persists into the resumed timeline.

UI follows the existing toggle pattern from Show Ground and Bloom: a `BTNS_CHECK`-style toolbar button mirrors the engine state via `TB_CHECKBUTTON`, the View menu carries the canonical `&Pause Preview\tF8` entry (matched in the German `.de.rc`), `DoMenuInit` greys the step entries when not paused, and the WM_COMMAND handler reads `IsPreviewPaused()` as the source of truth so menu / toolbar / accelerator all converge on the same state. The pause WM_COMMAND case additionally calls `TB_SETSTATE` on the two step buttons so their toolbar enabled state mirrors the menu greying. Three toolbar cells were added in two scripts — [`tasks/extend_toolbar1_bmp_pause.ps1`](tasks/extend_toolbar1_bmp_pause.ps1) (128×16 → 144×16, two 3-px vertical bars centered in cell 8) and [`tasks/extend_toolbar1_bmp_step.ps1`](tasks/extend_toolbar1_bmp_step.ps1) (144×16 → 176×16, single triangle + bar in cell 9 and twin triangles + bar in cell 10), mirroring the prior toolbar-extension scripts.

**Issues encountered and resolutions.**

- **Initial clock-offset model lost frame-stepping on resume.** The first cut accumulated `g_pauseOffset += (wall_at_resume - wall_at_pause)`, which was correct for plain pause/resume but ignored any `g_previewPauseAnchor` bumps from `StepPreviewFrames` during the pause — so a user who stepped 10 frames while paused would have those 10 frames silently disappear on resume. **Fix**: re-derive `g_pauseOffset = wall - anchor` at resume time, reading the *current* anchor rather than the wall-time delta. Caught by walking the algebra after writing the first draft; the working derivation is now in the comment above `GetTimeF()` in [`src/engine.cpp`](src/engine.cpp:37). No external bug, no UX cost — the bug was found and fixed pre-merge.
- **Avoided the Space / Period text-entry collision by picking function keys.** The natural pause shortcut is `Space` (media-player convention) but it collides with text entry in the F2-rename edit and any other Win32 EDIT control. Same trap with `.` for step. **Resolution**: F8 / F9 / F10 sidestep both risks at the cost of slightly worse discoverability — function keys can't be eaten by text controls. Fits the existing F5/F6/F7 cluster (reload textures / shaders / spawner dialog) so users already pattern-match the F-key strip as "preview-control row."
- **First cut of step-10 left a visible gap in the trail of spawner-driven moving instances.** Calling `StepPreviewFrames(10)` once advances the simulation clock by 167 ms in a single tick, which makes `ParticleSystemInstance::Update` move the spawner-owned projectile by `velocity × 0.167 s` in one shot — and the smoke emitter (which spawns at the instance's current position each Update) only gets a single spawn opportunity at the post-jump location. Result: a chunk of smoke at the pre-step position, an empty gap of 10× normal spacing, and the next chunk at the post-step position — with the leftover Fire particles from before the step lingering as a "ghost cluster" at the old location. **Fix**: replaced the one-shot `StepPreviewFrames(N)` call with a `DoStepFrames(info, N)` helper in [`src/main.cpp`](src/main.cpp) that loops *N* times, calling `StepPreviewFrames(1)` + `spawner->Tick(1/60)` + `engine->Update()` each iteration — so the projectile interpolates through *N* intermediate positions and the smoke emitter spawns at each one, producing a continuous trail. To make the loop coexist with the natural Render-loop spawner tick, `lastFrameTime` moved from a local static inside `Render()` to file-scope `g_spawnerLastFrameTime`, and `DoStepFrames` resets it after the loop so the next Render doesn't re-apply the elapsed step time.

---

### Two-child emitter support: investigation, not extension

*2026-05-11 · [`2e1b17a`](https://github.com/DrKnickers/particle-editor/commit/2e1b17a) · #51*

closes as an investigation, not a feature change. The question — whether the engine supports more than one on-lifetime child per emitter — is now answered authoritatively from the canonical game binaries: **it does not**. Every emitter holds exactly one death-child pointer and one life-child pointer in its runtime struct; the format-level "could we just stuff a second `0x39` mini-chunk in there?" question is moot because the runtime has nowhere to put a second pointer. The original sub-question (can the existing two slots — one death, one life — be set on a single emitter simultaneously) was already supported end-to-end by our editor; no UI change was needed. Workarounds for the "I want a second life child" case live in [`tasks/multi_child_emitter_investigation.md`](tasks/multi_child_emitter_investigation.md): chain emitters (parent → life-child → life-child → …), duplicate the parent block, or rely on the standard death-channel-plus-life-channel pair.

**How we tackled it.** Static reverse-engineering of `EAW Terrain Editor.exe` and `StarWarsG.exe`, reusing the Ghidra 12.0.4 + Adoptium Temurin JDK 21 install from. Two new Jython scripts drive the analysis: [`tasks/ghidra_scripts/FindEmitterChunkParser.py`](tasks/ghidra_scripts/FindEmitterChunkParser.py) anchors on functions whose instruction stream uses both `0x37` and `0x39` as scalar immediates (the spawn-link mini-chunk IDs), scoring candidates by also-contains `0x36` (the parent chunk ID) and `0xFFFFFFFF` (the "no child" sentinel). Three score=6 hits emerged at sizes 1496 / 2719 / 2968 bytes in the Terrain Editor and matching hits in `StarWarsG.exe`; two were unrelated (a generic data serializer and a Win32 virtual-key-code table), and the 2968-byte candidate was the emitter writer in each binary. [`tasks/ghidra_scripts/FindLifeChildXrefs.py`](tasks/ghidra_scripts/FindLifeChildXrefs.py) then walks every function for instructions whose immediate displacement equals `0x1108` or `0x1110` (the struct slots the writer revealed) and decompiles each — to confirm by independent xref that no spawn-site iterates a list. Full investigation log + provenance in [`tasks/multi_child_emitter_investigation.md`](tasks/multi_child_emitter_investigation.md).

**The actual finding.** The writer at `FUN_140134b50` (Terrain Editor) / `FUN_14015ed60` (StarWarsG.exe) — both 2968-byte byte-identical functions — emits the chunk-`0x36` spawn-link block by reading two specific struct offsets: `*(emitter + 0x1108)` for the death-child pointer and `*(emitter + 0x1110)` for the life-child pointer. Both fields are single 8-byte pointer slots, immediately adjacent in the runtime struct. There is no array, no count, no list. The 47-byte getter `FUN_1401372d0` returns one or the other by `kind` argument (`1` → death, `2` → life) with a single dereference — no iteration. Independently confirmed across 43 functions in the Terrain Editor that touch either offset: none iterates the slots in any pattern consistent with an array. The conclusion is a binary-level invariant of the engine, not a configurable choice.

**Issues encountered and resolutions.**

- **No unique string anchor for the chunk parser.** Unlike bloom (`BloomIteration` is a one-of-a-kind string), the chunk-parser code has no human-readable anchor — chunk IDs are numeric and `0x37` / `0x39` are common as ASCII digits and Win32 virtual key codes. **Resolution**: the byte-pattern triple "both `0x37` AND `0x39` as scalar immediates in the same function" + the also-contains `0x36` and `0xFFFFFFFF` scoring narrowed 21,744 functions in `StarWarsG.exe` (46,775 in the Terrain Editor) down to three score=6 hits per binary — all manually classifiable in under a minute. Pattern is committed as [`FindEmitterChunkParser.py`](tasks/ghidra_scripts/FindEmitterChunkParser.py) and is reusable for the next "find the chunk-X parser" question.
- **Q1 (parser semantics on duplicate `0x39` mini-chunks) ended up moot.** The original plan budgeted time to investigate whether the parser is strict-one / last-wins / list-append on a hand-crafted dual-`0x39` file. The Q2 finding (single pointer slot per child type) made this academic: even a fully list-aware parser would have to discard everything beyond the first match, because the struct has nowhere to put the rest. **Resolution**: skipped the hand-crafted fixture entirely; saved ~1 hour of disassembly + fixture-building. The fixture-generator script ([`tasks/build_dual_life_fixture.py`](tasks/build_dual_life_fixture.py)) is still committed as a future reference for any other "malformed multi-mini fixture" question.
- **Plan called the runtime-struct outcome "the worst case."** That framing was about feature ergonomics, not investigation quality — a binary-level invariant is actually the *best* outcome from a maintenance angle: the answer can't drift, future contributors don't have to re-litigate it, and the workaround paths (chain, duplicate parent) are now documented. **Resolution**: kept the plan's outcome-path matrix unchanged for retrospective honesty; the Review section records the closure as "ships as an investigation, no new ROADMAP entry filed."

---

### Bloom blur-iteration count proven canonical

*2026-05-11 · [`d8f5794`](https://github.com/DrKnickers/particle-editor/commit/d8f5794) · #49*

`BLOOM_BLUR_ITERATIONS = 4` in [`src/engine.cpp`](src/engine.cpp:551) is now provably the canonical engine value, not the educated guess it was when shipped. Comment-only change next to the constant — no behavioural diff, no UI surface, no perf change. Visual A/B against the canonical Terrain Editor is no longer needed for *this* specific question (the value is proven from the binary), though it remains worth doing once as a sanity check on the broader bloom pipeline.

**How we tackled it.** Static reverse-engineering of `EAW Terrain Editor.exe` (Petroglyph 2025 64-bit patch, x64 PE, stripped). Imported into Ghidra 12.0.4 + Adoptium Temurin JDK 21 — both kept persistently under `C:\Tools\` for future RE work. A handful of Jython scripts under [`tasks/ghidra_scripts/`](tasks/ghidra_scripts) drive the analysis: [`FindBloomLoop.py`](tasks/ghidra_scripts/FindBloomLoop.py) anchors on the `BloomStrength`/`BloomCutoff`/`BloomSize`/`BloomIteration`/`Engine\SceneBloom` strings (all confirmed present in `.rdata` via raw byte scan), collects xref-source functions, and decompiles them; [`InspectIterGlobal.py`](tasks/ghidra_scripts/InspectIterGlobal.py) inspects the loop-bound global Ghidra surfaced and searches the entire program for any other reference to that address. Scripts are committed; the Ghidra project database itself is gitignored (888 MB, rebuildable by re-running `analyzeHeadless -import` on either binary). The full investigation log + provenance lives in [`tasks/find_bloom_iterations.md`](tasks/find_bloom_iterations.md).

**The actual finding.** The bloom render path is `FUN_1400effc0` (anchors on all four `Bloom*` parameter names). Its blur loop reads its bound from `DAT_140f09244`, a runtime global — not an immediate. That global lives in the binary's `.data` section (`140f08000–14105adb7`) and is initialized to `04 00 00 00` (little-endian int32 = `4`) at compile time. A QWORD- and DWORD-LE search across the entire program for the address `0x140f09244` returns **zero hits** — meaning no code path writes the value via any pointer, table, or vtable. The constant is hardcoded for the lifetime of the process; there is no graphics-quality dispatch that would scale it.

**Cross-validation against `StarWarsG.exe`.** Same engine source, different PE. Bloom render is `FUN_140183a30` — byte-identical body size (833 bytes) to the Terrain Editor's `FUN_1400effc0`, identical call sequence. Loop bound at `DAT_140a129f4` (different absolute address — different binary), same `.data`-baked int32 value `4`, same zero-writers property. Both binaries agree, removing any ambiguity about whether the Terrain Editor's value differs from the in-game value.

**Issues encountered and resolutions.**

- **PIX legacy unusable on x64 binaries.** The pre-installed DX SDK June 2010 PIX only attaches to 32-bit D3D9 processes; the modern Petroglyph build is x64 across the board (`swfoc.exe`, `StarWarsG.exe`, `EAW Terrain Editor.exe` — all built 2025-08-08). **Resolution**: skipped capture-based approaches (PIX dead, RenderDoc dropped D3D9 in 1.x, apitrace would have worked but wasn't needed) and went straight to static RE. Lesson recorded as `` in [`tasks/lessons.md`](tasks/lessons.md): don't infer "community recompile" from bitness + recent timestamp. The 64-bit binaries are a first-party Petroglyph patch ([IGN coverage](https://www.ign.com/articles/rts-star-wars-empire-at-war-still-getting-updates-17-years-after-launch)), so RE results from them ARE canonical engine values, not third-party reproductions to be hedged.
- **Loop bound was a runtime global, not an immediate.** Risk #3 in the plan ("loop count could scale with graphics quality") materialized: Ghidra surfaced `DAT_140f09244` as the upper bound rather than a hardcoded `4` immediate. **Resolution**: the broader-search script proved zero writers anywhere in the binary, so the runtime indirection is cosmetic — equivalent to a hardcoded constant from our perspective. No quality-tier dispatch to chase.
- **Jython gotchas in headless scripts.** Ghidra 12.0.4 still defaults Jython 2.7 for headless `-postScript`. Three fixes needed: (1) PEP 263 encoding declaration on top of the script (otherwise non-ASCII chars in the source break the loader), (2) `try/except UnicodeEncodeError` around `str(data.getValue())` because some defined-data entries in `EAW Terrain Editor.exe` contain non-ASCII bytes that Jython's default ASCII encoder rejects, (3) `Memory.getInt(addr)` instead of `Memory.getBytes(addr, length)` for reading a value (the latter expects a Java `byte[]` buffer, not a Python int length). Recorded inline in [`tasks/ghidra_scripts/`](tasks/ghidra_scripts) as comments — same trip-hazards apply to any future RE script in this project.

---

### Bloom in the preview renderer
*2026-05-11 · [`0a172eb`](https://github.com/DrKnickers/particle-editor/commit/0a172eb) · #47*

Particles that bloom in-game now bloom in the editor preview. A new **View → Bloom… / Ctrl+B** dialog exposes the three canonical knobs — *Strength*, *Cutoff*, *Size* — plus a master enable, mirroring the bloom panel from the EAW Terrain Editor that ships with the game. A new toolbar button (sunburst icon, right of Heat Debug) toggles bloom on/off in a single click and stays in sync with the dialog and the persisted state. All four values survive across sessions in the registry; **View → Reset View Settings** drops them back to the canonical new-map defaults (`Cutoff = 0.90`, `Strength = 0.00`, `Size = 0.10`). When the shader can't be loaded (no game path configured, file missing, parameter surface doesn't match), the toolbar button and dialog controls grey out — no crash and no garbage rendering.

**How we tackled it.** Engine loads `Engine\SceneBloom.fx` via the existing `ShaderManager::getShader` call ([`src/main.cpp:263`](src/main.cpp:263)) so the resolution chain (mod overlay → game roots → MEG archives) is identical to how particle shaders load. The editor's bloom is therefore *byte-identical to in-game bloom* and automatically picks up any mod's customised bloom on the next `ReloadShaders` (F5 or mod switch). `InitBloomEffect` in [`src/engine.cpp`](src/engine.cpp) introspects the loaded effect at runtime — enumerates every parameter and technique, caches `D3DXHANDLE`s for the ones we drive each frame, and refuses to mark bloom ready if the canonical names don't show up. Output goes to `bloom-diagnostic.log` next to the .exe so a "why is bloom greyed out?" question is answerable without instrumenting the editor.

The pipeline insertion site is in [`Engine::Render`](src/engine.cpp:262) between the scene draw and the heat/distortion compose. The shader exposes one technique `t0` with three sequential passes — bright filter (writes to a full-resolution ping RT), 4-tap diagonal blur (ping-pong between two RTs, run `BLOOM_BLUR_ITERATIONS = 4` times with `BloomIteration` incrementing each pass to widen the kernel), and AddSmooth combine (additively folds the final blur into `m_pSceneTexture`, blend state declared by the .fx pass block). `m_resolutionConstants` is written per-pass to `(1/w, 1/h, 0.5/w, 0.5/h)`; the .zw component is read by every VS as the half-pixel UV offset *and* as the blur kernel's base spacing — without it, the kernel collapses to zero and no blooming visibly happens. The bloom RTs live alongside `m_pSceneTexture` in `ResetParameters`, recreated on device reset.

UI follows the Spawner pattern in [`src/main.cpp`](src/main.cpp): modeless dialog (`IDD_BLOOM`), lazy-create-on-show, hide-on-close, menu check-mark + toolbar button sync. The toolbar button cell was added by [`tasks/extend_toolbar1_bmp_bloom.ps1`](tasks/extend_toolbar1_bmp_bloom.ps1) (112×16 → 128×16, sunburst glyph), mirroring the prior extension scripts. Three sync entry points — toolbar button, dialog `Enable bloom` checkbox, and the persisted `BloomEnabled` registry value — all push to engine + each other on every state change.

**Issues encountered and resolutions.**

- **First-cut matcher looked for three separate techniques (bright/blur/combine).** Initial assumption was that the shader exposed three named techniques the editor would call in sequence. Reality: one technique `t0` with three sequential passes, plus a `BloomIteration` per-pass uniform. **Fix**: replaced the three-technique handles with a single `m_hBloomTechnique` and a pass count, and the render code now does `Begin → BeginPass(0/1/2) → End` in order with `BloomIteration` set per call.
- **Bloom shader appeared loaded but produced no visible glow.** Diagnostic dump on the user's real EAW + Mod install showed the effect loaded fine (47 parameters, technique `t0` with 3 passes validates) but bloom was greyed because the matcher expected three techniques. After fixing the matcher, bloom still rendered as if it weren't running. **Fix**: the `m_resolutionConstants` engine-global was unset — every VS in the shader reads its `.zw` for the half-pixel UV offset *and* as the blur kernel's per-tap base spacing, so `delta = BloomSize * half_pixel * (1 + 2*BloomIteration)` collapsed to zero and every blur tap sampled the same center pixel. Promoted `m_resolutionConstants` to a required handle in `InitBloomEffect`'s readiness check and `SetVector` it before each frame's passes.
- **Blur runs as a loop, not a single pass.** The shader's blur VS uses `BloomIteration` to widen the kernel per call (`delta = … * (1 + 2*BloomIteration)`) and the shader's own header comment says *"a series of bloom passes ping-ponging between two render targets"* — the count is engine-side and not exposed to the canonical Terrain Editor UI. **Fix**: render loop iterates the blur pass `BLOOM_BLUR_ITERATIONS = 4` times, alternating ping/pong each iteration, with `BloomIteration` set to the loop index. Combine pass samples whichever RT held the final result. The 4 is a tuning constant; visual A/B against the canonical editor is the path to refining it.
- **Defaults from the shader source produced bloom too subtle to verify the chain.** The .fx file declares `BloomStrength = 0.1f, BloomCutoff = 1.0f, BloomSize = 0.25f` — but these are placeholders the game overwrites at runtime. The canonical Terrain Editor's new-map defaults are `Cutoff = 0.90, Strength = 0.00, Size = 0.10`. **Fix**: engine defaults updated to match the canonical new-map values. Users have to dial `Strength` up to see bloom (matches how the canonical editor works); the master-enable checkbox stays as a discoverable on/off layered above that.
- **Shader-missing case rendered garbage through the default fallback.** `ShaderManager::getShader` returns the bundled `IDR_DEFAULT_SHADER` when a file isn't found anywhere in the resolution chain. Running our bloom render code through it would have produced visual nonsense. **Fix**: `InitBloomEffect` probes for an expected bloom parameter (`BloomStrength`) after the load. Missing → conclude the loader resolved to the default, set `m_pBloomEffect = NULL`, dialog opens but greys out, toolbar button greys via `TB_ENABLEBUTTON`. No crash, clear UI signal.
- **Tooltip-id collision risk in the toolbar.** Adding a button to the existing `ID_VIEW_BLOOM` (the Ctrl+B menu accelerator) would have made the toolbar button open the dialog instead of quick-toggling. **Fix**: two IDs — `ID_VIEW_BLOOM` for the menu / dialog opener, `ID_VIEW_BLOOM_TOGGLE` for the toolbar button's quick-toggle semantics.

---

### Adjustable ground-plane height in the preview
*2026-05-10 · [`b2b2533`](https://github.com/DrKnickers/particle-editor/commit/b2b2533) · #45*

The preview ground plane is no longer locked to `Z = 0`. A "Ground Height:" spinner sits in the header strip just left of the Background color picker, with a working range of −100 to +100 units and a 0.1-unit step. Scroll-wheel adjusts (Shift = ×10, Ctrl = ×0.1) like every other Spinner in the editor. The value persists across sessions in `HKCU\Software\AloParticleEditor\GroundZ`. When the "Show Ground" toolbar toggle is off, the label and spinner grey out (still visible — disabled, not hidden — so the spatial layout doesn't shift); flipping ground back on re-enables them and the ground returns to the user's last Z, not 0. **View → Reset View Settings** drops the persisted Z back to 0 alongside the existing reset of background color, ground visibility, and the color-picker custom palette.

**How we tackled it.** The engine surface is three lines: a `float m_groundZ` member next to `m_showGround` in [`src/engine.h`](src/engine.h), a one-liner `Engine::SetGroundZ` setter in [`src/engine.cpp`](src/engine.cpp), and the four `Vertex` records in the ground-quad block now pick up the live `m_groundZ` instead of literal zeros. The `static const` ground vertex array becomes a per-frame initializer — four vertices × ~80 bytes of init cost is negligible against the surrounding state changes and `DrawPrimitiveUP`. Persistence in [`src/main.cpp`](src/main.cpp) follows the existing `ReadShowGround` / `WriteShowGround` pair: `GroundZ` is stored as REG_BINARY (4 bytes of `float`) which sidesteps the "is REG_DWORD interpreting these bits as a signed integer" ambiguity that REG_DWORD would invite for a value that goes negative. `ReadGroundZ` validates length and rejects `NaN` / `Inf` via `std::isfinite` so a corrupted blob falls back to 0.0f rather than putting the plane in some surprise location.

The UI side: a "Ground Z:" label (`STATIC`) and a `Spinner` are direct children of the main window, created next to the existing `hLeaveParticles` checkbox and positioned in the same WM_SIZE row as the other header-strip controls. The spinner gets a fresh local control ID `ID_GROUNDZ_SPINNER = 0x5000` — above the `IDC_*` dialog-ID range and below `ID_MOD_NONE`. SN_CHANGE flows naturally to the main window's WM_COMMAND (the spinner forwards via `GetParent(hWnd)` → main window) where a new `else if (code == SN_CHANGE)` branch reads the float, pushes it to the engine, persists it, and forces a viewport redraw. The "Show Ground" toggle's existing WM_COMMAND handler now also calls `EnableWindow` on both label and spinner so the disabled state matches the toggle's; startup applies the same gating after restoring the persisted state.

**Issues encountered and resolutions.**

- **Rebar wouldn't carry the spinner cleanly.** The natural-feeling spot is inside the rebar next to the Show Ground button itself, but the rebar control doesn't forward `WM_COMMAND` from its child windows out of the box — making the spinner a rebar child would have routed SN_CHANGE into the rebar's WNDPROC, where it would die. The two-line fix would have been a custom container window or a subclassed rebar WNDPROC; either added more surface area than the feature warranted. **Resolution**: the label + spinner live in the header strip below the rebar (same row as `hLeaveParticles` / background label), positioned next to the existing controls. Visually they're still in the editor's top-of-window UI band, and the wiring is plain Win32 — spinner is a child of the main window, SN_CHANGE goes to the main WNDPROC directly.
- **Spurious `SN_CHANGE` during startup seeding would have re-written the registry.** `Spinner_SetInfo` updates the edit control's text, which would normally fire EN_CHANGE → SN_CHANGE. **Resolution**: none needed — `Spinner_SetInfo` already sets `allowNotify = false` around the update and restores it after (see [`src/UI/Spinner.cpp`](src/UI/Spinner.cpp)). Confirmed by inspection of the spinner control rather than by patching.

---

### Autosave for in-progress particles (two-tier)
*2026-05-10 · [`eb0a183`](https://github.com/DrKnickers/particle-editor/commit/eb0a183) · #41*

The editor now writes a recovery snapshot of the current particle system to `%TEMP%\AloParticleEditor\` on a periodic schedule. **Two tiers** run side-by-side: a **recent** tier on a 30-second cadence (freshest state, frequent overwrite — for the "crashed 10 seconds ago" case) and a **stable** tier on a 5-minute cadence (older known-good state — for the "the recent file is corrupt" or "I made a bad edit two minutes ago" cases). Both write only when there's an in-memory particle system AND the dirty flag is set, so an idle editor doesn't generate disk churn.

Files are named `autosave-<pid>-recent.alo` / `autosave-<pid>-stable.alo` plus an `autosave-<pid>.meta` sidecar holding the original filename and the most recent autosave's timestamp. The PID tag means two editor instances running side-by-side never clobber each other's recovery files. The editor *never* writes to the user's own `.alo` — the recovery file is always at a distinct TEMP path.

**Recovery flow.** On launch (when no `.alo` is given on the command line), the editor scans `%TEMP%\AloParticleEditor\` for files whose owning PID is no longer a live editor process. If any are found, the most recent orphan session is presented to the user via a MessageBox. The button layout depends on which tiers survived: MB_YESNOCANCEL when both tiers are available (Yes = recent, No = stable, Cancel = discard), MB_YESNO when only one is. After recovery, `info->filename` is reset to the original path so `Ctrl+S` overwrites the right file; the title bar shows the asterisk because the recovered content is still "unsaved" relative to the on-disk original.

**CLI-arg behavior.** When the user launches the editor with a `.alo` on the command line (e.g. by double-clicking a file in Explorer), the recovery prompt is **skipped** — the explicit user gesture wins. The orphan autosave stays untouched in TEMP and surfaces on the next plain launch.

**Cleanup.** Successful `Save` / `Save As`, `File → New`, `File → Close`, and clean `WM_DESTROY` all delete this PID's autosave session. The recovery flow consumes the orphan (deletes all three files) on any prompt resolution — Yes, No, or Cancel — so a "discard" answer doesn't surface the same files again next launch. A side-effect of the scan sweeps any autosave file older than 30 days, so abandoned crashes don't accumulate in TEMP indefinitely.

**How we tackled it.** New self-contained module at [`src/Autosave.{h,cpp}`](src/Autosave.cpp). Five public functions: `Write(sys, originalFilename, tier)`, `DeleteOurSession()`, `ScanForOrphan(out)`, `DeleteOrphan(session)`, and the helper structs / enums in the header. Integration in [`src/main.cpp`](src/main.cpp) is five sites: `WM_CREATE` of the main window starts two `SetTimer`s (`Autosave::RECENT_TIMER_ID` = 3, `Autosave::STABLE_TIMER_ID` = 4); `WM_TIMER` calls `Write` for the firing tier; `WM_DESTROY` kills both timers and calls `DeleteOurSession`; `DoSaveFile` / `DoCloseFile` / `DoNewFile` each call `DeleteOurSession` after the action lands; the startup recovery block lives between the CLI-arg check and the `DoNewFile` fallback. The recovery-side helpers `FormatAge`, `ShowRecoveryPrompt`, and `RestoreFromAutosave` are inline in `main.cpp` (small, only-one-caller). `RestoreFromAutosave` deliberately bypasses `LoadFile` to avoid pushing the temp path into the file-history menu — the user shouldn't see `%TEMP%\...autosave-1234-recent.alo` in the recent-files list.

**Issues encountered and resolutions.**

- **PID-recycling false positives.** A naive "if `OpenProcess` succeeds, the PID is a live editor" check would misclassify any other process that happens to have the same numeric PID after recycling — we'd skip recovery for a truly orphaned file that's still recoverable. **Fix**: combine `OpenProcess` with `QueryFullProcessImageNameW` and case-insensitively tail-match against our own exe basename. Both PID AND image name have to match for the file to count as "owned by a live editor." A coincidentally-recycled PID owned by `chrome.exe` won't fool us.
- **`OpenProcess` ambiguous failures.** `OpenProcess` can fail with `ERROR_INVALID_PARAMETER` (PID definitely doesn't exist) or with `ERROR_ACCESS_DENIED` / other (PID exists but we can't query it). The first means "orphan, safe to recover"; the second is genuinely ambiguous. **Fix**: be conservative on ambiguous error — treat as "still alive" so we don't delete a sibling editor's autosave. Cost: skipping recovery for one cycle. Benefit: never accidentally consuming another editor's in-progress recovery.
- **Crash mid-write → partial `.alo`.** A write that gets interrupted by a process kill would leave a truncated file that loads as corrupt. **Fix**: write to `<dest>.tmp` first, then `MoveFileEx(MOVEFILE_REPLACE_EXISTING)` for atomic rename. Crash before the rename leaves the `.tmp` behind (recovery's `FindFirstFile` pattern `autosave-*` doesn't match `.tmp` files); the destination `.alo` is always either the prior good version or the new complete one, never partial. Belt-and-braces: recovery's `ParticleSystem(IFile*)` already throws `wexception` on corrupt input, and `RestoreFromAutosave` shows the existing `IDS_ERROR_FILE_OPEN` message — the same flow `LoadFile` uses for any corrupt `.alo`.
- **History menu pollution from recovery loads.** First version routed the recovery through `LoadFile`, which adds the loaded path to the file-history menu. That dumped `%TEMP%\...autosave-1234-recent.alo` into the user's recent-files list — confusing and useless (the temp path goes away). **Fix**: dedicated `RestoreFromAutosave` helper that reads bytes from the temp path but pretends `info->filename` is the original. The history is left alone.
- **Tier prompt UX with three states.** A flat "do you want to recover?" prompt was too coarse — the user wants to know which version they're choosing (recent vs stable). A custom dialog felt over-engineered. **Fix**: standard `MessageBoxW` with `MB_YESNOCANCEL` (or `MB_YESNO` when only one tier survived), wording the button semantics in the message body. The caller maps the return code based on which tiers are available — Yes always means "the most recent available," No means "the older one if available, otherwise discard," Cancel always discards.
- **30-day orphan sweep.** Without one, a regularly-crashing editor would silently accumulate `autosave-*.alo` files in TEMP forever. **Fix**: while iterating during `ScanForOrphan`, files past a 30-day mtime threshold are deleted in the same pass. By 30 days the autosave is presumably not actionable for any sane workflow, and `%TEMP%` is supposed to be transient anyway.

---

### Drag-and-drop reparenting in the emitter tree
*2026-05-10 · [`03da959`](https://github.com/DrKnickers/particle-editor/commit/03da959) · #37*

Drop emitter S onto emitter T (mid-row hover) to make S a child of T. The full subtree under S moves with it as a block — children stay attached, source's spawn-field references unchanged. If S was a root, S is no longer a root. If S was already a child of some other emitter P, S is detached from P (P's spawn slot that referenced S becomes -1) and reattached to T.

This extends PR #35's reorder gesture without replacing it. The hit-test is now three-zone per item rect: **top 1/3** still inserts above (reorder; root sources only), **middle 1/3** is the new drop-onto (reparent), **bottom 1/3** still inserts below (reorder). Drop targets that aren't roots are still invalid for reorder, so children-as-source dragged between gaps gets `IDC_NO`; that's the known limitation called out below.

**Slot picker.** Both target slots (`spawnDuringLife` and `spawnOnDeath`) free → small popup at the cursor: *"Reparent as Lifetime child"* / *"Reparent as on-Death child"* / cancel. Only one slot free → auto-pick that slot, no popup. Both slots occupied → `IDC_NO`, no commit. The popup is built at runtime via `CreatePopupMenu` + `AppendMenu` and uses the in-house `TrackPopupMenuEx + TPM_RETURNCMD` pattern; menu strings localized in en + de.

**Visual feedback.** Hovering a drop-onto target sets `TVIS_DROPHILITED` on the target's tree item via `TVM_SETITEM`. Insertion mark cleared whenever the cursor moves into a drop-onto zone (and the highlight cleared whenever it moves into a between-gap zone). `IDC_NO` cursor over invalid drops — drop-on-self, drop-on-descendant (cycle), drop-on-current-parent (slot-switch is out of scope), drop where both slots are occupied, or any drop while source can't legally land.

**Refused gestures.** Dropping S onto a descendant of S (would create a cycle in the spawn-field graph), dropping S onto S itself, dropping S onto its current parent (would be a slot-switch under the same parent — useful but adds a third semantic for the gesture; refused for v1), dropping a child between root gaps (would be a "promote to root + reorder" — also refused for v1). Each is detected in [`UpdateDropFeedback`](src/UI/EmitterList.cpp) before the drop commits.

**How we tackled it.** The data-layer change is small — [`ParticleSystem::reparentEmitter`](src/ParticleSystem.cpp) and a private `IsInSubtreeOf` cycle helper, both in [`src/ParticleSystem.cpp`](src/ParticleSystem.cpp). `reparentEmitter` validates (cycle, slot occupancy, current-parent-refusal), detaches source from its old parent's spawn slot, sets target's chosen slot to source's index, and updates source's parent pointer. m_emitters position is unchanged — `addLifetimeEmitter` already established that vector layout doesn't follow tree layout, so leaving source in place avoids unrelated index churn. The cycle helper walks bottom-up via parent pointers so it can't itself recurse into a malformed cycle.

The UI-layer changes mostly extend PR #35's drag state machine in [`src/UI/EmitterList.cpp`](src/UI/EmitterList.cpp). `DropTarget` grew a `DropKind` enum (`DROP_INVALID` / `DROP_BETWEEN_GAP` / `DROP_ONTO_EMITTER`) plus a `targetEmitter` field; `ComputeDropTarget` now does the thirds-based classification. `UpdateInsertMark` was renamed to `UpdateDropFeedback` and gained drop-highlight management (clearing the *other* feedback channel when one becomes active so a cursor that crosses zones doesn't smear). The single `EndDrag` was split into `EndDragVisual` (capture, image list, insertion mark, drop-highlight, autoscroll timer) and `EndDragLogical` (clears `dragSource`); `WM_CAPTURECHANGED` only does the visual half so the slot-picker popup taking capture mid-flight doesn't disarm the accelerator gate.

`TVN_BEGINDRAG` was loosened: children-as-source is now allowed (the previous PR's `parent != NULL` refusal is gone). Single-emitter system still refused (nothing to drop onto). `WM_RBUTTONDOWN` mid-drag cancels (right-click would otherwise pop the context menu). `WM_MOUSEWHEEL` mid-drag forwards to default tree proc and recomputes drop feedback against the new layout.

**Issues encountered and resolutions.**

- **Drag-image ghost smearing across rows the cursor passed over (during the drag).** `TreeView_SetItem` flipping `TVIS_DROPHILITED` on the row under the cursor triggers a tree-internal row repaint. That repaint isn't coordinated with the imagelist's saved-background restore, so each row the cursor visited ended up with horizontal-stripe ghost residue baked in. **Fix**: wrap every per-message handler (`WM_MOUSEMOVE` / `WM_TIMER` / `WM_MOUSEWHEEL`) in a single `ImageList_DragShowNolock(FALSE/TRUE)` pair around all of: ghost reposition, scroll repaint (where applicable), and tree-state changes. First attempt nested wraps (one in `UpdateDropFeedback`, another in `WM_TIMER` around `WM_VSCROLL`); `DragShowNolock` isn't a refcount, so the inner `TRUE` re-showed the ghost prematurely between the scroll repaint and the row-state update — exactly the window where the row repaint clobbered the saved background. Consolidating to one wrap per message handler with `UpdateDropFeedback` not wrapping internally fixed it. The function comment now explicitly says callers own the wrap.
- **Visual residue after cancellation paths (Esc / right-click / capture loss).** Even with the per-message wrap, occasional residue could persist on rows that had been TVIS_DROPHILITED'd during the drag. **Fix**: `EndDragVisual` ends with `InvalidateRect(hTree, NULL, TRUE) + UpdateWindow(hTree)` whenever any visual state was active. Cheap (the tree isn't tall) and produces unambiguously clean state.
- **Modal slot-picker would disarm the accelerator gate mid-flight.** First version of `EndDrag` cleared `dragSource` before the popup, so `EmitterList_IsDragging` returned false during the popup's modal pump → Ctrl+Z mid-popup would have called `DoUndo` → freed the ParticleSystem under the held `dragSource` pointer (same use-after-free class as the PR #35 root-cause). **Fix**: split `EndDrag` into `EndDragVisual` (called before the popup so the ghost / highlight / capture don't linger across it) and `EndDragLogical` (clears `dragSource`, called once after the popup resolves and the reparent has committed-or-not). The `WM_CAPTURECHANGED` from the popup taking capture only does the visual half, leaving `dragSource` set so the gate stays armed.
- **Slot-switch under the same parent.** Dropping a Lifetime child onto its own parent (with the on-Death slot free) is mechanically valid — detach old slot, attach new — but the UX is "I dropped on the parent and something happened to a different slot." Refused outright in both `UpdateDropFeedback` (shows `IDC_NO`) and `reparentEmitter` (returns false defensively). Documented as a known limitation; future "switch which slot a child occupies" feature can be a separate gesture if anyone asks.
- **Drag-press on a child emitter for reparenting was previously refused** (PR #35 only allowed root sources because reorder doesn't make sense for children). Loosening the refusal in `TVN_BEGINDRAG` was straightforward; the per-kind validity logic in `UpdateDropFeedback` then handles refusing between-gap drops with child sources independently of allowing reparent drops with child sources.

---

### Drag-and-drop reordering in the emitter tree
*2026-05-10 · [`df725b3`](https://github.com/DrKnickers/particle-editor/commit/df725b3) · #35*

Click-and-drag a root emitter in the tree to reorder it past one or more sibling roots. The whole subtree (children, grandchildren, anything reachable via spawn-field traversal) moves with the source as a block; spawn-field indices on every affected parent are rewritten in one shot via the new `ParticleSystem::moveEmitterToRootIndex`. Visual feedback while dragging combines a translucent drag-image ghost (`ImageList_BeginDrag` / `…DragMove`) under the cursor with an insertion-mark line (`TVM_SETINSERTMARK`) showing where the drop will land. `IDC_NO` cursor over invalid drop targets — children, the source's own current gap, and outside the tree's client area — so the user gets unambiguous feedback before committing. Esc cancels mid-drag with no change to the file. One Ctrl+Z reverts a successful drop; the existing undo capture treats `ELN_LISTCHANGED` as a structural op (coalesce-key 0, never coalesced into adjacent edits).

Auto-scroll: when the cursor enters a 16-pixel hot zone at the top or bottom of the tree's client area while dragging, the tree scrolls one line every 50 ms. The timer-driven approach is necessary because `WM_MOUSEMOVE` doesn't fire while the cursor is stationary — without a timer, holding the cursor in the hot zone would stall.

**Scope is reorder-only**: dragging a child as the source is refused (children fill named parent slots, not an ordered sibling list); dropping a root *onto* an emitter (rather than between gaps) is treated as an invalid target. Reparenting via drop-onto-emitter remains its own [ROADMAP entry](ROADMAP.md) for a future PR.

**How we tackled it.** Most of the work lives in [`src/UI/EmitterList.cpp`](src/UI/EmitterList.cpp). The state machine sits on `EmitterListControl` (six new fields tracking source emitter, drag-image list, current insertion-mark target, scroll timer, and direction); `TVN_BEGINDRAG` in the dialog's `WM_NOTIFY` is the entry point, and per-message updates run in the existing tree-subclass `EmitterTreeViewWindowProc` (newly handling `WM_MOUSEMOVE` / `WM_LBUTTONUP` / `WM_KEYDOWN` Esc / `WM_CAPTURECHANGED` / `WM_TIMER`). Helpers `RootIndexOf`, `ComputeDropTarget`, `UpdateInsertMark`, and `EndDrag` factor the four-zone hit-test math, the no-op detection, the cursor / insertion-mark update, and the cleanup into single-responsibility functions so each can be reasoned about in isolation. `ParticleSystem::moveEmitterToRootIndex` ([`src/ParticleSystem.cpp`](src/ParticleSystem.cpp)) is a one-shot reorder — the existing `moveEmitter(±1)` only swaps adjacent roots, and looping it would have generated intermediate spawn-field rewrites for no reason.

A new public accessor `EmitterList_IsDragging(HWND)` ([`src/UI/UI.h`](src/UI/UI.h)) lets the message pump in [`main.cpp`](src/main.cpp) gate `TranslateAccelerator` while a drag is in progress — see Issue #1 below.

**Issues encountered and resolutions.**

- **Accelerator translation mid-drag is a use-after-free.** The pump calls `TranslateAccelerator` regardless of mouse-capture state. A stray `Ctrl+Z` mid-drag would translate to `ID_EDIT_UNDO` → `DoUndo` → `RestoreFromSnapshot` → `delete info->particleSystem` while the drag's `dragSource` field still pointed into the freed `Emitter` — crash on the next mouse message's hit-test. **Fix**: three layers. (a) Pump-level gate at [`main.cpp:3245`](src/main.cpp:3245): `if (!consumed && (dragging || !TranslateAccelerator(...)) && !IsDialogMessage(...))`, where `dragging` reads through the new `EmitterList_IsDragging` accessor. Catches every destructive accelerator (Ctrl+Z, Ctrl+Y, Ctrl+S, Ctrl+N, Ctrl+O, Delete, F5, F6, F7) in one stroke. (b) Belt-and-braces `if (EmitterList_IsDragging(...)) return;` at the top of `DoUndo` and `DoRedo` — two lines, value is "we don't crash if the pump regresses." (c) Confirmed Esc still reaches the subclass `WM_KEYDOWN` because the main window isn't a dialog and `IsDialogMessage` returns FALSE without consuming.
- **`WM_CAPTURECHANGED` re-entry through `EndDrag`'s own `ReleaseCapture`.** First draft of `EndDrag` cleared `dragSource` *after* `ReleaseCapture`; the `WM_CAPTURECHANGED` that fires synchronously then re-entered `EndDrag` (which is harmless because every step null-checks, but confusing in a debugger). **Fix**: clear `dragSource` *first* so the `WM_CAPTURECHANGED` handler's `dragSource != NULL` check fails and short-circuits the recursive call.
- **The four-zone hit-test math is easy to get wrong.** Above-first / between / below-last / over-child are all special-cased differently. **Fix**: factored into one `ComputeDropTarget(hTree, pt, numRoots) -> {gap, hTarget, after, valid}` function with a documented gap-index contract (gap 0 = above first root, gap N = below last, gap K in between = before root K). The `WM_LBUTTONUP` commit reuses the same `DropTarget` returned by `UpdateInsertMark`, so the insertion line shown to the user and the actual drop position can't disagree.
- **No-op detection has to use root-only indices, not flat `m_emitters` indices.** Children sit between roots in the flat vector and skew the count, so a no-op test against `m_emitters` would mistakenly accept some valid drops as no-ops (or vice versa). **Fix**: `RootIndexOf(sys, emitter)` walks `m_emitters` filtering on `parent == NULL` and returns the position in the root-only sequence. Source at root index `S` occupies gap range `[S, S+1]`; dropping at either of those gaps is the no-op case. The math also handles a collapsed-source root correctly because it operates on the data model, not on tree-visible positions.
- **Auto-scroll fights insertion-mark math if the timer doesn't re-anchor everything.** When `WM_VSCROLL` fires, item rects shift but `WM_MOUSEMOVE` doesn't fire (cursor is stationary). Without recomputing, the ghost smears across the scrolled-by content and the insertion line points at stale items. **Fix**: the `WM_TIMER` handler does all four updates atomically — `SendMessage(WM_VSCROLL)`, `GetCursorPos` + `ScreenToClient` (cursor is the only stable reference; the timer's lParam doesn't carry coords), `ImageList_DragMove` to the absolute coords, then `UpdateInsertMark` against the new layout.
- **Defensive teardown on file-open / dialog-destroy.** If a drag is somehow still active when `OnParticleSystemChange` runs (file open / new fired despite the accelerator gate, or `EmitterListControl::dragSource` got out of sync somehow), the drag's `Emitter*` would dangle into the about-to-be-deleted system. **Fix**: `OnParticleSystemChange` and the dialog's `WM_DESTROY` both call `EndDrag` defensively. `EndDrag` is idempotent so the no-drag case is a fast no-op.

---

### Bump-mapped particles inherit curve-editor color tracks
*2026-05-10 · [`06c6452`](https://github.com/DrKnickers/particle-editor/commit/06c6452) · #33*

The Red / Green / Blue tracks in the curve editor now tint bump-mapped particles (`BLEND_BUMP`, `BLEND_DECAL_BUMP`) the same way they tint every other blend mode. Previously, the editor silently dropped those tracks for bump particles — the alpha track flowed through but RGB was overwritten with a rotation-tangent encoding `(0.5+0.5·cos(angle), 0.5+0.5·sin(angle), 0)`, which produced an apparent green/yellow/red hue cycle that depended on each particle's spawn rotation and bore no relation to anything the user had authored. The override didn't match what the EaW engine actually writes in-game, so the editor's render diverged from the in-game appearance for any bump particle the user attempted to colorize.

**How we tackled it.** One delete in [`src/EmitterInstance.cpp`](src/EmitterInstance.cpp:597). The conditional that branched on `m_emitter.blendMode == BLEND_BUMP || BLEND_DECAL_BUMP` and overwrote `color.x/y/z` with the rotation tangent is gone; both branches now fall through to the same `color.{x,y,z} += SampleTrack(...)` path that non-bump modes already used. The pre-existing comment "the RGB components of the vertex color contain the tangent vector" was a Petroglyph-shader-design note that the editor had picked up as a literal CPU contract, but the in-game engine never honored it that way — it just writes curve-editor color for every blend mode.

**Issues encountered and resolutions.**

- **Took an in-game diagnostic to confirm the engine's actual behaviour.** The shader header comment in `PrimParticleBumpAlpha.fx` documented the design contract as "vertex color RGB = tangent for bump particles," and the editor faithfully implemented it. Reasoning from the comment alone, the natural conclusion was that the engine did the same and the special case must stay. To verify, a temporary diagnostic build of `PrimParticleBumpAlpha.fxo` was deployed to the Mod folder that simply returned `In.Diff.rgb` as the pixel color; in-game testing showed bump particles rendering with the curve-editor color, proving the engine does not honor the documented contract for bump-mode vertex color. The editor's special case was the only divergent actor. Trust shader comments as design intent, not engine behaviour.
- **Bump shader's tangent dependency.** The original bump shader (`PrimParticleBumpAlpha.fx`) reads vertex color RGB to construct the tangent space, so freeing that channel for color tinting depends on the bump shader sourcing its tangent elsewhere. The shader-side change — deriving tangent from `ddx/ddy` of UV in the pixel shader — lives in the Mod mod folder for now (`Data/Art/SHADERS/Source/Engine/PrimParticleBumpAlpha.fx`) and will be re-homed when this work moves to the appropriate shader repository. Without that shader change, the editor change still works in isolation — bump particles just have garbage tangent data, which only matters if you also use the bump shader.

---

### Undo / redo for the particle editor (`Ctrl+Z` / `Ctrl+Y`)
*2026-05-10 · [`a0be64a`](https://github.com/DrKnickers/particle-editor/commit/a0be64a) · #31*

`Ctrl+Z` undoes and `Ctrl+Y` (or `Ctrl+Shift+Z`) redoes any edit that survives a `.alo` save/load: every property field on the three Emitter tabs, every track key, every random-parameter group, structural emitter ops (add / delete / duplicate / move / rename / paste), and the `Leave Particles` system toggle. Editor-only state is intentionally excluded — visibility toggles, selection, expand/collapse, viewport / camera / background / ground / Spawner config, and mod selection do not enter the stack.

UI lives in three places, all wired in both `en.rc` and `de.rc`:

- **Edit menu** — `Undo Ctrl+Z` and `Redo Ctrl+Y` at the top of the existing Edit popup, before Cut/Copy/Paste, with a separator. Greyed when the stack ends are reached.
- **Toolbar** — two new buttons between the File group and the View toggles, with tooltips. Toolbar1 went from 5 to 7 cells.
- **Accelerators** — `Ctrl+Z`, `Ctrl+Y`, plus `Ctrl+Shift+Z` as a redo synonym.

Stack is depth-capped at **100 entries**; oldest fall off when full. File ops (New / Open) clear the stack and re-seed it with a load-time baseline so the very first `Ctrl+Z` rewinds back into the loaded file rather than into nothing. Save marks the current entry as "matches disk" so undoing back to a saved state clears the title-bar asterisk and redoing past it restores the asterisk.

Edits within ~1.5 s on the same emitter coalesce into one undo step. That window is wide enough to fold "edit a text field, click into a spinner, edit it" into a single step (which is how users describe an "edit session" on a property panel) but tight enough that a deliberate "tweak A, pause, tweak B" produces two distinct undo entries.

After undo / redo, selection is restored to the emitter that was active at capture time — including child emitters. Live engine instances (Shift-spawned previews, Spawner-driven instances) are killed on undo because they hold C++ references to Emitter objects we're about to delete; the user re-spawns to see the reverted state.

**How we tackled it.** Whole-system snapshot stack rather than a command pattern. Each entry is the byte buffer produced by `ParticleSystem::write` into a `MemoryFile`, plus the selected-emitter index. Restore deserializes via `ParticleSystem(IFile*)` and swaps the new system in. The save/load round-trip is already battle-tested by file open / save and clipboard paste, `.alo` files are tiny (single-digit KB to <100 KB), and snapshot-and-swap sidesteps the hardest part of the command approach — re-creating an `Emitter*` after a delete-undo with the right pointer-equality for live `EmitterInstance` references. New code lives in [`src/UndoStack.{h,cpp}`](src/UndoStack.h).

Three notification sites in [`main.cpp`](src/main.cpp)'s `WM_NOTIFY` handler (`EP_CHANGE`, `TE_CHANGE`, `ELN_LISTCHANGED`) plus the `BN_CLICKED` for the `Leave Particles` checkbox are the capture points. Coalesce key is composed from `(notify-code, emitter-index-or-track)`; structural ops pass key 0 to disable coalescing across an add/delete. A `m_applying` re-entrancy flag in [`UndoStack`](src/UndoStack.h:74) guards against capturing during restore (the rebuild fires its own `EP_CHANGE` / `ELN_SELCHANGED` notifications during `EmitterProps_SetEmitter` / `EmitterList_SetParticleSystem`).

Selection restoration uses a new `EmitterList_SelectEmitter(HWND, Emitter*)` helper in [`src/UI/EmitterList.cpp`](src/UI/EmitterList.cpp) that walks the tree depth-first looking for the item whose `lParam` matches the captured emitter, then `TreeView_SelectItem`s it. The walk is necessary because the tree's structural shape mirrors the spawn-field hierarchy rather than the flat `m_emitters` index.

Toolbar bitmap was extended from 80×16 (5 cells) to 112×16 (7 cells) using the same 4bpp BMP-rewrite pattern as the earlier Move Up / Move Down work; the script is at [`tasks/extend_toolbar1_bmp.ps1`](tasks/extend_toolbar1_bmp.ps1) for reference.

**Issues encountered and resolutions.**

- **Initial draft crashed on undo with "child emitter vanished".** First version of `RestoreFromSnapshot` set `info->particleSystem = sys` and `info->selectedEmitter = &sys->getEmitter(selIdx)` *before* calling `EmitterList_SetParticleSystem`. `TreeView_DeleteAllItems` inside the tree rebuild fires `TVN_SELCHANGED` while items still hold `lParam` pointers to the just-`delete`d old `Emitter` objects. The handler bubbled `ELN_SELCHANGED` up to `main.cpp`, which read `EmitterList_GetSelection()` (a stale pointer) into `info->selectedEmitter`, and `SetEmitterInfo` → `EmitterProps_SetEmitter` then dereferenced it for `emitter->name` etc. on freed memory. **Fix**: mirror `LoadFile` + `OnFileChange`'s safe order — set `info->particleSystem = NULL` and `info->selectedEmitter = NULL` *before* the rebuild, install the new system *after*. `SetEmitterInfo` early-bails when `particleSystem == NULL`. Comment-block at [`main.cpp`](src/main.cpp) explains the trap so the next contributor doesn't re-introduce it.
- **750 ms coalesce window felt twitchy.** First version split "edit color texture, click into the textureSize spinner, edit that" into two undo entries because the gap between leaving the text field and clicking the spinner exceeded 750 ms. **Fix**: bumped [`UndoStack::COALESCE_WINDOW_MS`](src/UndoStack.h:42) to 1500 ms, which folds natural back-to-back tweaks on the same emitter into one step. Below 1500 ms, switching control type (text → spinner → combo) reliably lost the coalesce.
- **Whole-system swap kills live preview instances.** `engine->Clear()` is unavoidable on undo because `EmitterInstance` holds a C++ reference (`Emitter& m_emitter`) to its source emitter — references can't be re-bound, so when the source `ParticleSystem` is replaced the instances must die. Re-pointing them via reflection isn't possible in C++. The user-visible effect is "Ctrl+Z killed my Shift-spawned preview"; a follow-up could re-spawn an instance at the original position after restore, but bundling it here would have grown scope.
- **`Leave Particles` toggle pre-dated `SetFileChanged`.** Pre-existing code mutated `info->particleSystem->setLeaveParticles(...)` on the checkbox click without dirtying the file (no asterisk, no save-on-close prompt). Adding undo capture for it without `SetFileChanged(true)` would have produced an inconsistent state — undoable model change, but title bar said "clean". Added `SetFileChanged(true)` next to the capture call as a small adjacent fix.
- **`MemoryFile` doesn't expose its buffer directly.** The class is `RefCounted` and lacks a `data()` accessor, so `Serialize` writes into a `MemoryFile`, then `seek(0)` + `read` to copy the bytes back into a `std::vector<char>`. One extra copy per snapshot, irrelevant at the file sizes involved (a few KB). Considered adding `MemoryFile::data()` but the round-trip pattern is also what `Deserialize` needs and keeping the class surface untouched felt cleaner than a one-caller accessor.

---

### Programmable particle spawner (v1) — `Emitters → Spawner…` / `F7`
*2026-05-10 · #30*

Replaces the "hold Shift, click in viewport, spawn one instance" preview flow with a modeless **Spawner** dialog hosting a configurable test driver. Two modes:

- **Manual** — fires a single burst on "Spawn now" or `Shift+Space`.
- **Auto** — fires bursts on a recurring schedule when Enabled.

Each *burst* emits up to 10 `ParticleSystemInstance` objects spaced `(c)` seconds apart; in Auto mode bursts repeat with `(d)` seconds between the end of one burst and the start of the next (the skip rule: bursts don't overlap). Each spawned instance starts at a configurable world position with a configurable initial velocity, moves at constant velocity for at most `maxLifetime` seconds, then `StopSpawning()`s so existing particles fade naturally.

UI: dialog opens via `Emitters → Spawner…` (Alt+M, S) or `F7`; close via the `X`, `F7`, or the same menu (toggles). Window position persists across sessions; spawner config does not (resets to defaults each launch — burst size 1, spacing 0, interval 10 s, position (0,0,0), velocity (0,0,0), lifetime 5 s, mode Auto, disabled).

Hard caps:

| Limit | Value |
|---|---|
| Max simultaneous spawner instances | **50** |
| Per-frame emission cap | **≤ 5** |
| Burst size | **1–10** |
| Spacing within burst | **0–10 s** |
| Interval between bursts | **0–60 s** |
| Max lifetime per instance | **0–600 s** (0 = unlimited) |
| Position / velocity / jitter range | **±10 000 world units** |

The 50-cap counts only spawner-owned instances; Shift+click spawns aren't included. When at the cap, the status counter reads `Status: 50/50 active (limited)` and new spawns are dropped silently until live ones expire.

**How we tackled it.** The driver lives in [`src/SpawnerDriver.{h,cpp}`](src/SpawnerDriver.h), called once per frame from `Render(info)` before `engine->Update()`. State machine is two phases (Waiting / BurstFiring) tracking `m_burstRemaining`, `m_timeUntilNextInstance`, `m_timeUntilNextBurst`. Each spawn stamps a transient `SpawnerAnchor` (an `Object3D` subclass with public position/velocity setters) with the configured position+velocity (plus jitter), calls `engine->SpawnParticleSystem(*sys, &anchor)`, then `MarkSpawnerOwned` + `SetMaxLifetime` + `Detach` on the resulting instance. Per-instance ballistic motion runs inside `ParticleSystemInstance::Update`: `m_position += m_velocity·dt` for spawner-owned instances, plus a lifetime check that triggers `StopSpawning()` on expiry.

**Issues encountered and resolutions.**

- **`Object3D::Detach` doesn't capture velocity.** It captures absolute position so the instance stays put when reparented, but leaves `m_velocity` at the constructor default of `(0,0,0)` — the legacy `mouseCursor` Shift-click flow intentionally drops velocity on Shift-release. After the first build, spawned instances had the right initial position but didn't move. **Fix**: capture velocity eagerly in `MarkSpawnerOwned` (`m_velocity = GetVelocity()`), which runs while the parent anchor is still set, before `Detach`. Doesn't affect Shift+click since that path never calls `MarkSpawnerOwned`.
- **`SetConfig` reset state on every keystroke.** The dialog calls `SetConfig` on every spinner `SN_CHANGE`. Original implementation reset the entire burst-state machine including `m_timeUntilNextBurst = 0`, which (a) aborted in-flight bursts and (b) triggered an immediate burst on the next Tick because the timer was zero. So typing `10` into the interval spinner generated two unintended bursts. **Fix**: only reset state on *transitions* — mode change or enable toggle. Parameter tweaks within the same mode preserve the timer; in-flight bursts continue with `m_burstRemaining`'s captured value, while spacing changes apply mid-burst.
- **First Auto enable fired immediately.** With the new 10 s default interval, an immediate first burst was surprising. **Fix**: when `enabled` transitions false→true while in `Phase::Waiting`, set `m_timeUntilNextBurst = intervalSec` so the user sees the first burst after one full interval.
- **Dialog visibility tracking.** The dialog is created lazily on first show via `CreateDialogParam`, then hidden/shown via `ShowWindow(SW_HIDE/SW_SHOW)` rather than destroyed. Window position is captured to `info->spawnerWindowRect` on hide and restored on show, validated against virtual-screen bounds (fallback to system default when the saved RECT is fully off-screen, e.g. monitor disconnected).

**Limits design rationale**: 50 active instances bounds every downstream cost — particles, draw calls, CPU update cost. 5 emissions/frame survives stutter without storming. Burst size 10 keeps a single burst small relative to the 50-cap so a maxed burst still leaves headroom. See `tasks/todo.md` for the full reasoning.

**Deferred to a v2 roadmap entry**: arc paths, velocity shorthand (magnitude + azimuth + elevation), named presets, and path visualization in the preview. User-drawn curve paths and "draw-in-viewport" interactive mode were dropped as too much UX complexity for the value.

---

### Shaders load from the mod folder
*2026-05-09 · [`4942747`](https://github.com/DrKnickers/particle-editor/commit/4942747) · #28*

When a mod is active, the editor resolves all 14 engine shaders through the mod folder before falling back to the base game. Concretely: if a mod ships `Data\Art\Shaders\Engine\PrimModulate.fx` (or any of the other shader files in `ShaderNames[]`), the editor renders with that shader instead of the base game's. The swap happens immediately when a mod is selected — `SelectMod` calls `ReloadShaders()`, which does an all-or-nothing flush and reload of all 14 slots, so any mod-local `.fx` files are picked up in that single call. If a mod shader fails to compile, the previous set is kept alive and a status-bar message reports the failure; a bad mod shader cannot brick a running session.

**How we tackled it.** No new code was required — two existing pieces compose to produce the behaviour. `FileManager::getFile` ([`src/managers.cpp`](src/managers.cpp:13)) prepends `modpath` to any relative path lookup when a mod is active, checking that physical file before iterating base-game paths and megafiles. `ShaderManager::load` ([`src/main.cpp`](src/main.cpp:251)) always resolves shader filenames through that same `FileManager`, so the `ReloadShaders` → `getShader` → `load` → `getFile` chain picks up mod-local shaders automatically once `SetModPath` has been called. This entry was written because the connection between the two was non-obvious: the Mods menu entry (PR #5) describes file-resolution priority, and the Hot-reload entry (PR #8) describes the reload trigger, but neither made the end-to-end shader-override capability explicit.

**Issues encountered and resolutions.** None — the composition works correctly as-is. The all-or-nothing semantics of `ReloadShaders()` already guard against partial failure: new shaders are loaded into a temporary array first and only swapped into `m_pShaders[]` if all 14 succeed.

---

### Persist view settings across sessions (background color, ground toggle, custom colors) + Reset View Settings
*2026-05-09 · #27*

Three view-state values now round-trip across launches via the existing `HKCU\Software\AloParticleEditor\` registry key:

- **`BackgroundColor`** (REG_DWORD) — `Engine::m_background`. Persisted on every `CBN_CHANGE` from the swatch button.
- **`ShowGround`** (REG_DWORD, 0/1) — `Engine::m_showGround`. Persisted on every `Ctrl+G` / View → Show Ground toggle.
- **`CustomColors`** (REG_BINARY, 64 bytes) — the 16 user-customizable slots in the system `ChooseColor` dialog. Same write window as the background color, since `CBN_CHANGE` fires *after* the dialog modifies the palette.

Plus a new **View → Reset View Settings** menu item. Confirmation dialog → deletes all three registry values → restores the engine to its constructor defaults (`RGB(0x14,0x08,0x34)` background, ground on) and clears the custom-colors palette to all zeros. Camera reset is intentionally NOT bundled in — it has its own command above and isn't a persisted setting. Same handler on both `en.rc` and `de.rc` ("Reset View Settings" / "Ansicht zurücksetzen").

**How we tackled it.** Six static helpers in [`src/main.cpp`](src/main.cpp) following the existing `ReadLastMod` / `WriteLastMod` pattern — one `Read*` + one `Write*` per setting, plus `ResetViewSettings()` for the bulk delete. Each `Read*` takes a `defaultValue` so callers can pass the engine's existing default and a fresh registry behaves identically to before this feature. Writes happen on every change (matches the existing convention; no exit-path bugs). Reads happen once, immediately after `new Engine(...)` in [`main.cpp`](src/main.cpp).

The 16-slot `ChooseColor` palette was a function-local `static COLORREF CustomColors[16] = {0}` inside [`ColorButton.cpp`'s `WM_LBUTTONUP`](src/UI/ColorButton.cpp). Promoted to a file-static `g_customColors` so all `ColorButton` instances share one palette (matching what the user expects from any color picker), and exposed via two accessors `ColorButton_GetCustomColors` / `ColorButton_SetCustomColors` so `main.cpp` can drive the persistence without leaking the internal array.

**Issues encountered and resolutions.**

- **First launch after toggling ground off looked broken even though it wasn't.** The `Show Ground` toolbar button is added with hardcoded `TBSTATE_ENABLED | TBSTATE_CHECKED` ([`main.cpp:1116`](src/main.cpp:1116)). Reading `ShowGround=0` and calling `SetGround(false)` correctly suppressed the ground render, but the toolbar button still painted as pressed — and the next click would `SetGround(!GetGround())` = `true`, the opposite of what the user expected. Fix: explicit `TB_CHECKBUTTON` re-sync immediately after the registry-restored `SetGround`, mirroring what the toggle handler already does.
- **Forward-declare the helpers near the existing `static` block at the top of `main.cpp`.** The `Read*` / `Write*` definitions sit alongside `ReadLastMod` / `WriteLastMod` (~line 1976) but they're called much earlier (CBN_CHANGE handler, ground toggle handler). Without the forward decls, the compiler refused to find them. Same pattern the existing `WriteModNickname` already uses.

If you want to inspect/change the persisted values manually, they're under `HKEY_CURRENT_USER\Software\AloParticleEditor`. Bad / wrong-type values are silently dropped by the helpers and the engine default is used instead — no crash, no migration code needed.

---

### Move Up / Move Down buttons for root emitters
*2026-05-09 · #25*

Two new buttons on the emitter-list toolbar — **▲** (Move Up) and **▼** (Move Down) — that reorder the selected root emitter past its previous / next root sibling. Same actions are available via the right-click context menu (**Move Up** / **Move Down**, between *Rescale* and *Toggle Visibility*) and the `Alt+Up` / `Alt+Down` keyboard shortcuts. The whole subtree of the selected root moves with it as a block — children, grandchildren, everything reachable via spawn-field traversal. Buttons grey out when the selection is a child emitter (children fill named slots `spawnDuringLife` / `spawnOnDeath` on their parent — they don't form an ordered sibling list, so reordering them isn't meaningful), or when the selection is the topmost / bottommost root in that direction.

Toolbar layout: the new buttons sit in their own group between Delete and the visibility eye — `[New ▾] | [Delete] | [▲][▼] | [👁] | [Show All][Hide All]`. Adjacent to Delete because both target the current selection; not at the far right with the bulk-action buttons.

**How we tackled it.** New backend method [`ParticleSystem::moveEmitter(emitter, direction)`](src/ParticleSystem.cpp) — direction is `-1` (up) or `+1` (down). Identifies the neighbor root by walking `m_emitters` filtered to `parent == NULL`, collects both subtrees by spawn-field DFS, then rearranges so that the union of occupied positions is filled in the swapped order while emitters belonging to neither subtree stay where they are. All `index` fields and parent spawn-field references are rewritten in a single pass.

**Issues encountered and resolutions.**

- **Auto-selected first emitter loaded with Move Down greyed out.** [`EmitterList_SetParticleSystem`](src/UI/EmitterList.cpp) calls `OnParticleSystemChange` *before* assigning `control->system`. Inside that path, `TreeView_SelectItem` fires `TVN_SELCHANGED`, which calls `NotifyParent(ELN_SELCHANGED)` and recomputes toolbar enable state — but at that moment `control->system` is still `NULL`, so the new Up/Down enable check (which scans the emitter list to find a neighbor root) saw no neighbor and disabled both buttons. The pre-existing Delete / Visibility checks only test `control->selection`, so they were unaffected. Fix: re-fire `ELN_SELCHANGED` once after `control->system = system` to reconcile state.
- **Toolbar bitmap was 4bpp paletted, not 24bpp.** [`src/Resources/toolbar2.bmp`](src/Resources/toolbar2.bmp) lives in the format that `LoadBitmap` + `ImageList_AddMasked` expect (per the icon-loading work in the original x64 port). Generating new icons in 24bpp would have broken the chroma-key match. Wrote [`tasks/extend_toolbar_bitmap.ps1`](tasks/extend_toolbar_bitmap.ps1) to extend the existing 80×15 bitmap to 112×15 in-place by appending two 16×15 arrow glyphs at palette index 0 (black) on a chroma-key background of palette index 6 (`RGB(0,128,128)`). Same script is the reproducible source of truth — re-run if the icons need to change.
- **Reorder doesn't fire `TVN_SELCHANGED`.** Tree rebuild via `OnParticleSystemChange` clears and reselects the moved emitter, but the move itself doesn't change *which* emitter is selected — only its position. Without an explicit notification, the Up/Down enable state would be stale (e.g., after moving down, Down might still appear enabled even if the moved emitter is now at the bottom). Fix: extend the `NotifyParent` enable-update branch to also fire on `ELN_LISTCHANGED`, and have `EmitterList_MoveEmitter` send both `ELN_LISTCHANGED` and `ELN_SELCHANGED`.

Foundation for the upcoming drag-and-drop reordering roadmap item — same backend method, same tree-rebuild path; only the UI input changes.

---

### Duplicate / paste auto-rename
*2026-05-09 · [`33e0913`](https://github.com/DrKnickers/particle-editor/commit/33e0913) · #23*

Duplicating an emitter or pasting one from the clipboard now appends a `_<n>` suffix where `<n>` is one greater than the highest numeric suffix already in use for that base name. So duplicating an emitter named `Fire Small` yields `Fire Small_1`; the next duplicate (whether of `Fire Small` or `Fire Small_1`) yields `Fire Small_2`, and so on. The same rule applies to `Ctrl+V` paste, *Paste as Lifetime Child*, and *Paste as Death Child*. Replaces the earlier `_ (copy)` suffix that PR #19 shipped — `_<n>` is collision-free, monotonic, and reads cleanly when several duplicates exist side-by-side.

The increment scans every emitter currently in the system, including any whose name was already manually edited to end in `_<digits>`, so the new emitter never collides with an existing name. If the source name itself ends in `_<digits>`, that suffix is stripped before scanning — duplicating `Foo_3` while `Foo_5` exists yields `Foo_6`, not `Foo_3_1`.

**How we tackled it.** Single static helper [`GenerateDuplicateName`](src/UI/EmitterList.cpp) at the top of [`src/UI/EmitterList.cpp`](src/UI/EmitterList.cpp) takes the system pointer and the source name; the rule lives in one place rather than being open-coded at each call site. Wired into both `EmitterList_DuplicateEmitter` (replacing the `(copy)` line) and `PasteEmitter` (new rename right before the construction-time clipboard emitter is handed off to the add-emitter functor). No file-format change; pure UI behavior.

---

### Tailed particles ignore rotation track (preview parity with game)
*2026-05-09 · [`f5bbcd1`](https://github.com/DrKnickers/particle-editor/commit/f5bbcd1) · #22*

The EaW runtime's tail render path orients the quad along velocity and **ignores** the rotation-speed track entirely — even when the emitter's rotation fields are set. The editor preview previously *added* the rotation-track contribution on top of the velocity-orientation term, so a tailed emitter with a non-trivial rotation track would spin in the preview but stand still in-game. Discovered while debugging `Mods/Mod/.../P_hp_imperial_damage.alo` "Fire Small": rotation values populated, preview rotated, in-game did not.

**Fix.** [`src/EmitterInstance.cpp`](src/EmitterInstance.cpp:533) — inside the `if (m_emitter.hasTail)` branch, reset `angle = 0` before the velocity-direction term and switch the velocity-orientation assignment from `+=` to `=`. The rotation-track integration above the branch still runs (cheap; could be skipped under `hasTail`, but the result is now thrown away regardless), and the BUMP-blend tangent at line 596 now encodes velocity direction for tailed particles, which matches what the engine does for tail+bump.

If a future user hits the inverse confusion ("I want my tailed particles to also spin"), the answer is the engine doesn't allow it — disable `hasTail` and accept that velocity-facing goes away. Don't add a preview-only "spin tailed particles" mode; preview parity beats convenience.

---

### Resource-file encoding: UTF-8 with BOM
*2026-05-08 · [`0d6f6cc`](https://github.com/DrKnickers/particle-editor/commit/0d6f6cc) · #20*

Both [`src/ParticleEditor.en.rc`](src/ParticleEditor.en.rc) and [`src/ParticleEditor.de.rc`](src/ParticleEditor.de.rc) are now stored as **UTF-8 with BOM** and declare `#pragma code_page(65001)`. Previously they declared cp1252 with no BOM, which any editor defaulting to UTF-8 would silently corrupt: high bytes (`°`, `±`, `²`, `ä`, `ö`, `ü`, `ß`) decoded as invalid UTF-8 → got substituted with `U+FFFD` → were saved back as the three-byte sequence `EF BF BD`. The RC compiler then read those three bytes per the `cp1252` pragma as `ï¿½`, which is what the user saw on dialog labels.

A previous commit ([`ef30981`](https://github.com/DrKnickers/particle-editor/commit/ef30981) · #13) hand-fixed three specific positions on the Appearance tab but didn't address the underlying encoding mismatch — so the same class of mojibake remained in 3 other `units/s²` labels in `en.rc` and 70 sites in `de.rc` (every umlaut, plus the same `s²`). This change repairs all of them in one pass and prevents regressions: any modern editor will correctly round-trip the BOM-tagged UTF-8 file.

**How we tackled it.** A one-shot PowerShell script ([`tasks/fix_rc_encoding.ps1`](tasks/fix_rc_encoding.ps1)) reads each file as cp1252 (so legitimate `0xB0`/`0xB1`/`0xB2` decode correctly while `EF BF BD` becomes the 3-char string `"ï¿½"`), applies an ordered list of word-level substitutions (longest / most-specific first, e.g. `Größenänderung` before `Größe`), swaps the pragma, and writes UTF-8 with BOM via `Encoding.UTF8` constructor with `encoderShouldEmitUTF8Identifier = true`. Replacement table is a list of `(pattern, replacement)` pairs rather than a hashtable — see issues below.

**Issues encountered and resolutions.**
1. **PowerShell hashtables are case-insensitive** — `[ordered]@{}` collapsed `"Einfügen"` and `"einfügen"` (and `"Löschen"` / `"löschen"`) into one entry, so the uppercase variants silently dropped, leaving 6 mojibake sites un-replaced. Fix: switch the replacement table to an ordered array of `@(pattern, replacement)` pairs and iterate explicitly.
2. **PowerShell 5.1 reads `.ps1` files as ANSI without a BOM**, so the script's own German source-string literals were misinterpreted on first run (parse errors at `Änderungen`, `&` characters mis-tokenized). Fix: ensure the script file itself is saved as UTF-8 *with* BOM. Worth knowing for any future repair scripts touching non-ASCII source.
3. **One mnemonic placement was off-pattern**: the German "Edit / Paste" menu item is `"E&infügen"` — the `&` mnemonic underline sits between `E` and `inf`, not before the leading letter as in `"&Einfügen"`. The generic pattern `Einfügen` therefore didn't match it. Added an explicit `E&infügen` entry alongside the regular one.
4. **The label at `IDC_STATIC11` reads `Stößverzögerung`, not `Stoßverzögerung`.** The mojibake byte count forces three umlauts between `St` and `gerung`, which only fits the (nonstandard) `Stöß…` form — most likely a typo in the original German translation. Restored verbatim rather than "fixing" it; out of scope for an encoding-repair change.

If a future edit ever re-introduces `EF BF BD` triplets, run `tasks/fix_rc_encoding.ps1` (or just grep both `.rc` files for those bytes) to catch it.

---

### Right-click → Duplicate Emitter
*2026-05-08 · [`81e63c9`](https://github.com/DrKnickers/particle-editor/commit/81e63c9) · #19*

**What ships.** Right-clicking an emitter in the tree now offers a *Duplicate* item between Copy and Paste. Selecting it creates a copy of the emitter directly below the original in the tree (and at `original.index + 1` in the underlying `m_emitters` vector), suffixes the name with ` (copy)`, and selects the new emitter. Faster than Copy → Paste because it skips the clipboard round-trip and the duplicate ends up positioned next to its source rather than at the end of the list.

**How we tackled it.** Two new pieces. (1) `ParticleSystem::insertEmitterAfter(reference, source)` mirrors `deleteEmitter`'s index-shift logic in reverse: the new emitter takes index `reference->index + 1`, every existing emitter at that slot or above gets bumped by one, and any parent's `spawnDuringLife` / `spawnOnDeath` reference that pointed at a shifted emitter is updated to its new index. The duplicate itself is reset to be a root (no parent, no spawn-children) — spawn-field slots are exclusive on each parent and a duplicate of a child literally can't share its source's slot. (2) `EmitterList_DuplicateEmitter` in `src/UI/EmitterList.cpp` rounds the source through the same chunk-serializer/-reader flow the clipboard-Copy path already uses, so the new `Emitter` starts with a clean (empty) `m_instances`. The tree gets a new `HTREEITEM` inserted at root level after the source's tree item.

**Issues encountered and resolutions.**

- **`Emitter`'s copy constructor shallow-copies `m_instances`.** The `*this = emitter;` in `Emitter::Emitter(const Emitter&)` propagates the source's `std::set<EmitterInstance*>` to the duplicate. With live particles spawned, that means two `Emitter` objects claim ownership of the same `EmitterInstance` pointers — when either is later deleted, `~Emitter` calls `RemoveEmitter` for each instance and the second destructor double-frees. The fix is to never construct duplicates directly with the copy constructor on a live emitter: instead, serialize through `ChunkWriter`, deserialize through `ChunkReader`, and let the `Emitter(reader)` ctor produce a clean object with empty `m_instances`. The Copy/Paste path already does this safely; we reuse it.
- **Tree placement when the source is a child emitter.** The duplicate is a tree-root (`parent=NULL`), but `TreeView_InsertItem` requires `hInsertAfter` to be a sibling at the same level as `hParent`. If the source itself is a tree-child, `hInsertAfter = source's tree item` would mix levels. We fall back to `TVI_LAST` (append at end of root list) in that case; "right below the original" only fully applies when source is itself a root. Documented in the function comment.

---

### Spinner mouse-wheel input
*2026-05-08 · [`23b20f9`](https://github.com/DrKnickers/particle-editor/commit/23b20f9) · #16*

`Spinner` controls accept `WM_MOUSEWHEEL` to nudge the value by their already-defined `Increment`. Modifiers: `Shift` ⇒ 10× step, `Ctrl` ⇒ 0.1× step on float spinners (integer spinners keep 1× to avoid rounding the step to a no-op).

The Win32 nuance worth recording: hover-wheel (the Win10/11 *"Scroll inactive windows when I hover over them"* setting, on by default) delivers `WM_MOUSEWHEEL` to whichever child window the cursor is over — so a single handler on the parent isn't enough. The `Spinner` registers `WM_MOUSEWHEEL` on **both** the parent (`SpinnerWindowProc` — cursor over the up/down arrows) and the subclassed Edit child (`SpinnerEditWindowProc` — cursor over the editable field, the common case). Both call into one helper that routes through the existing range-clamping path so wheel input respects `MinValue` / `MaxValue` identically to keyboard `VK_UP` / `VK_DOWN`.

If you ever add another scroll-wheel-aware native control with child windows, repeat this pattern.

---

### Tolerating malformed `.alo` data
*2026-05-07 · [`dc97123`](https://github.com/DrKnickers/particle-editor/commit/dc97123) · #11*

Some `.alo` files in the wild store a `spawnOnDeath` or `spawnDuringLife` index that points past the end of the emitter list — usually the residue of a delete operation in an external tool / older editor build that didn't update cross-references. Pre-fix, the `!= -1` guard in `ParticleSystem::ParticleSystem`'s post-process loop didn't catch this, and `m_emitters[badIndex]` tripped *vector subscript out of range* before the file finished loading.

**Policy**: in the post-process loop, if a non-sentinel spawn-field index is `>= m_emitters.size()`, log a `[Load]` warning with the offending emitter name + bad value + emitter count, then clamp to `(size_t)-1` so the rest of the load can continue. The user can re-save the file to commit the cleanup.

Concrete example: `p_starfighter_explosion.ALO` from Mod stores `spawnDuringLife = 78` on emitter 8 in a 26-emitter file. Pre-fix that crashed the editor on open; now it loads with a warning line.

If you ever add another place that indexes into `m_emitters` from a value that came out of a file (especially fields stored as 32-bit and read into `size_t`), apply the same bound-check pattern.

---

### Object lifetime: Emitter ↔ EmitterInstance
*2026-05-07 · [`4073880`](https://github.com/DrKnickers/particle-editor/commit/4073880) · #9*

`EmitterInstance` objects are owned by `std::unique_ptr` inside `ParticleSystemInstance::m_emitters`. Each `EmitterInstance` registers a raw `this` pointer with its template `ParticleSystem::Emitter::m_instances` for back-reference.

**Important rule**: never raw-`delete` an `EmitterInstance`. The `unique_ptr` owns it. Use `ParticleSystemInstance::RemoveEmitter(EmitterInstance*)`, which `erase()`s the matching `unique_ptr` so the proper destructor runs.

`Emitter::~Emitter()` walks `m_instances` and calls `inst->GetSystem().RemoveEmitter(inst)` for each — that path triggers `~EmitterInstance` (which calls `m_emitter.unregisterEmitterInstance(this)` and shrinks `m_instances`) so the loop terminates cleanly. Pre-fix this was a raw `delete` and any live-particle delete crashed on the next render frame.

If you find yourself wanting to call `delete` on a raw `EmitterInstance*` anywhere else, you have a bug.

---

### Debugging methodology that worked
*2026-05-07 · [`f2030b7`](https://github.com/DrKnickers/particle-editor/commit/f2030b7) · #10*

For data-dependent crashes (load-X, delete-Y) we used three tools in sequence and they paid off cleanly:

1. **Out-of-process file parse first.** Wrote a small Python script (`.claude/dump_alo.py`) that walks the `.alo` chunk format the same way `ChunkReader` does and dumps every emitter's name + `spawnDuringLife` + `spawnOnDeath`. Done before instrumenting any C++. Tells you whether the file is malformed (unusual indices, sentinels, etc.) or whether the bug is purely in the editor's logic. **Watch out**: the `0x36` chunk (spawn fields) is a *data* chunk holding mini-chunks, not a *container* — the high bit of the size field tells you which.
2. **Targeted printf instrumentation.** Add `[Tag] enter / step N / exit` traces around the suspected code path. Build, hand the user the binary, have them paste the console output. Two cycles of this got us from "crashes sometimes" to "this exact line dereferences freed memory."
3. **State-condition guesses.** When the trace looked clean but the user said it crashed, the bug was timing/state-dependent. Asking *"did you spawn particles before deleting?"* turned a sporadic crash into a 100%-reproducible one — and exposed a double-ownership bug between raw `delete` and `unique_ptr`.

The Python parser lives at `.claude/dump_alo.py` and is worth keeping for any future "this specific file crashes" report. A more recent companion script — [`tasks/dump_alo_rotation.ps1`](tasks/dump_alo_rotation.ps1) — does the same trick for rotation / render-mode flags (added with the tailed-particle preview-parity fix above).

---

### Hot-reload (View menu)
*2026-05-07 · [`e083cfd`](https://github.com/DrKnickers/particle-editor/commit/e083cfd) · #8*

Two manual reload commands plus mod-aware automatic reload on selection change.

- **View → Reload Textures (F5)** — `Engine::ReloadTextures()` flushes `TextureManager`'s cache and pushes every active `EmitterInstance` to re-fetch via `OnParticleSystemChanged(-1)`. Lets you edit a `.tga` in your image editor and see the change without respawning particles.
- **View → Reload Shaders (F6)** — `Engine::ReloadShaders()` flushes `ShaderManager`'s cache and re-loads every entry from `ShaderNames[]` with **all-or-nothing semantics**: new shaders go into a temporary array first, only commit to `m_pShaders[]` if all 14 succeed. On failure the previous set stays alive (a malformed mod shader can't brick a running session). Status bar reports success / "keep previous" failure.

Both menu items grayed when `info->engine == NULL`. The `texture_filename` annotation pass on each effect (binding named textures) was extracted into `BindShaderTextures()` so it runs both at initial construction and on hot-reload.

`ITextureManager` and `IShaderManager` grew `Clear()` so the engine can encapsulate the cache flush without `main.cpp` knowing the concrete manager types.

`SelectMod` now just calls `ReloadShaders()` + `ReloadTextures()` after `SetModPath` — no manual cache plumbing on the call site.

---

### Mods menu (right-click for nickname)
*2026-05-07 · [`0342219`](https://github.com/DrKnickers/particle-editor/commit/0342219) · #6*

`WM_MENURBUTTONUP` is **not** delivered for menubar dropdowns by default — Windows treats right-click as "cancel" and dismisses the menu silently. Three things made this work:

1. **`MNS_DRAGDROP` on the menu and submenus** (via `SetMenuInfo`). Without it, no message is sent.
2. **Defer the dialog with `EndMenu()` + `PostMessage(WM_APP_SHOW_NICKNAME)`.** Showing a modal dialog directly inside `WM_MENURBUTTONUP` fails because the menu's modal tracking loop is still tearing down. Posting the deferred message lets the menu finish closing first.
3. **Use a real `.rc` dialog (`IDD_MOD_NICKNAME`) shown via `DialogBoxParam`.** Hand-rolled in-memory `DLGTEMPLATE` is fragile (`id` is `WORD`, not `DWORD`, etc.); a resource dialog is reliable and adds proper i18n support to both `.en.rc` and `.de.rc`.

**Owner-drawn rendering for "FolderName *(nickname)*".** Plain Win32 menu items can't mix regular and italic text in a single label. Mod entries are inserted with `MFT_OWNERDRAW`, with the mod's index stashed in `dwItemData`. `WM_MEASUREITEM` sizes the item using `GetTextExtentPoint32` against both font variants; `WM_DRAWITEM` paints:
- Background (`COLOR_HIGHLIGHT` when `ODS_SELECTED`, else `COLOR_MENU`).
- Optional checkmark via `DrawFrameControl(DFC_MENU, DFCS_MENUCHECK)` when `ODS_CHECKED`.
- Folder name in the system menu font (from `SystemParametersInfo(SPI_GETNONCLIENTMETRICS).lfMenuFont`).
- `" (nickname)"` in an italic copy of that font when a nickname is set.

Both fonts are cached on `APPLICATION_INFO` (`hMenuFont`, `hMenuItalicFont`), lazy-init via `EnsureMenuFonts`.

---

### Mods menu
*2026-05-07 · [`84ba36a`](https://github.com/DrKnickers/particle-editor/commit/84ba36a) · #5*

Top-level **Mods** menu inserted between **View** and **Help**, built dynamically at runtime (no `.rc` edits for the menu itself). Lists every subdirectory of `<game>\corruption\Mods\` and `<game>\GameData\Mods\`, alphabetical by folder name within FoC and base-game submenus.

**Hot-swap, no restart required.** Selecting a mod prepends its folder to the file-resolution chain via `FileManager::SetModPath`. `getFile()` checks `<modpath>\<relpath>` as a `PhysicalFile` before iterating the regular base paths, so loose files in the mod folder shadow the base game's. The texture and shader caches (`TextureManager::Clear`, `ShaderManager::Clear`) are flushed on every selection so the next lookup re-reads from the new path. Currently-rendered emitter instances keep their existing `AddRef`'d textures until naturally re-fetched.

**Persistence.**
- `HKCU\Software\AloParticleEditor\LastMod` — selected mod path; empty / missing = Unmodded. Restored on launch if the folder still exists.
- `HKCU\Software\AloParticleEditor\ModNicknames` — value name = full mod folder path, value = user-set nickname.

---

### CI / GitHub Actions
*2026-05-07 · [`02aa6e8`](https://github.com/DrKnickers/particle-editor/commit/02aa6e8) · #4*

Workflow at `.github/workflows/build.yml`. Builds `Debug` and `Release` × `Win32` and `x64` on `windows-latest`.

**Two non-obvious bits, both already wired up:**

1. **DirectX SDK is not pre-installed.** The `.vcxproj` references `$(DXSDK_DIR)` for `d3dx9.h` and the matching libs. The workflow installs the SDK via `choco install directx-sdk -y --no-progress` and exports `DXSDK_DIR` to `$GITHUB_ENV`. The notorious S1023 redistributable conflict has not bitten us in practice on `windows-latest`; if it ever does, the workaround is to first `Get-Package "Microsoft Visual C++ 2010*Redistributable*" | Uninstall-Package` before the choco install.
2. **Platform Toolset must be `v143`.** Newer Visual Studio releases (VS18 / VS2026 Insiders) silently bump `<PlatformToolset>` to `v145` when you open the solution. Stock VS2022 on the runner only has `v143`, so CI fails with `MSB8020: build tools for v145 cannot be found`. **Always revert the auto-bump in both `src/ParticleEditor.vcxproj` and `libs/expat-2.2.0/expatw_static.vcxproj` before committing.**

---

### Platform Toolset locked to v143
*2026-05-07 · [`8f66d0c`](https://github.com/DrKnickers/particle-editor/commit/8f66d0c) · #3*

Reverted an auto-bump from `v145` back to `v143` in both `src/ParticleEditor.vcxproj` and `libs/expat-2.2.0/expatw_static.vcxproj`, so the project builds on stock VS2022 / CI. See the CI section above for the full context.

---

### Z-write disabled for particle render order (preview parity with game)
*2026-05-07 · [`b19ea95`](https://github.com/DrKnickers/particle-editor/commit/b19ea95) · #2*

**Symptom:** Editor preview rendered overlapping emitters in the opposite order from the actual game. Top-of-list emitter appeared on top of the stack instead of behind.

**Root cause:** `Engine::Render` enables `D3DRS_ZWRITEENABLE` for the ground plane and never resets it before particle passes. With Z-write on, the first particle drawn at any depth wins the depth test and occludes everything drawn after it at that depth — exactly inverse of painter's order.

**Fix:** `m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE)` once before the particle render loop in `engine.cpp`. Z-test stays on (so particles are still occluded by scene geometry), but particles no longer write to it, leaving emitter draw order to decide overlap stacking — matching the game.

---

### x64 port + game-data-path lookup
*2026-05-07 · [`954d069`](https://github.com/DrKnickers/particle-editor/commit/954d069) · #1*

Bring-up of the codebase as a working VS2022 / x64 build, plus the registry-backed game-data path management. Five distinct issues bundled into one big port commit; recorded individually below for searchability.

#### `(LONG)(LONG_PTR)` pointer truncation (caused startup hang/crash)

**Symptom:** App launched, console flashed, app exited. WM_INITDIALOG handlers ran successfully, but the next message (WM_SIZE) crashed before any handler code ran — because the dereferenced `control` pointer was garbage.

**Root cause:** The codebase stored pointers in window data via:
```cpp
SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG)(LONG_PTR)pointer);
```
On x64, `LONG` is still 32-bit but pointers are 64-bit. The `(LONG)` cast **truncated** the pointer; sign-extension on retrieval gave back garbage. WM_INITDIALOG worked because it used `lParam` directly; subsequent message handlers retrieved via `GetWindowLongPtr` and crashed.

**Fix:** Removed the `(LONG)` cast at all 20 sites across 9 files:
- `src/main.cpp`, `src/Rescale.cpp`
- `src/UI/EmitterList.cpp`, `src/UI/Emitter.cpp`, `src/UI/Spinner.cpp`
- `src/UI/TrackEditor.cpp`, `src/UI/RandomParam.cpp`
- `src/UI/ColorButton.cpp`, `src/UI/CurveEditor.cpp`

`(LONG_PTR)` alone is correct: it's 64-bit on x64, 32-bit on Win32.

**Exception:** In `src/UI/TrackEditor.cpp:365`, `control->iTrack = (int)(LONG_PTR)pcs->lpCreateParams` is correct as-is — that line *intentionally* narrows a small int that was packed into `lpCreateParams`.

#### `size_t` field receiving 32-bit `0xFFFFFFFF` sentinel (caused vector OOR on file open)

**Symptom:** `Debug Assertion Failed: vector subscript out of range` (vector header line 1931) when opening an `.alo` file.

**Root cause (partial):** `ParticleSystem::Emitter::spawnOnDeath` and `spawnDuringLife` are declared `size_t` (64-bit on x64). The file format stores them as 32-bit and uses `0xFFFFFFFF` as the "no emitter" sentinel. `readInteger()` returns `unsigned long` (32-bit). Assignment widens to `size_t` *without sign extension*: `0xFFFFFFFF` becomes `0x00000000FFFFFFFF`, not the all-ones `(size_t)-1` the rest of the code compares against. The check `if (spawnOnDeath != -1)` returns true, then `m_emitters[0xFFFFFFFF]` blows up.

**Fix:** In `src/ParticleSystem.cpp:475-476`, normalize the sentinel after reading:
```cpp
spawnOnDeath = readInteger(reader);
if (spawnOnDeath == 0xFFFFFFFF) spawnOnDeath = (size_t)-1;
```

Continued in the malformed-`.alo`-data entry above.

#### Toolbar / tree-view icons missing

**Symptom:** Top toolbar (File new/open/save), emitter list toolbar, and treeview emitter icons all rendered blank.

**Root cause:** `ImageList_LoadImage` with `flags=0` silently failed on the project's 4bpp paletted bitmaps under modern comctl32 / x64. Adding `LR_CREATEDIBSECTION` made the load succeed but converted the bitmap to a 32bpp DIB, after which `ImageList_AddMasked`'s chroma-key match against `RGB(0,128,128)` no longer matched the converted pixels.

**Fix:** Replaced each `ImageList_LoadImage` with the legacy `LoadBitmap` (returns a DDB matching the screen format, which is what `ImageList_AddMasked` was designed for) + manual `ImageList_Create` + `ImageList_AddMasked`:

```cpp
HBITMAP hBmp = LoadBitmap(hInstance, MAKEINTRESOURCE(IDR_TOOLBAR1));
HIMAGELIST hImgList = ImageList_Create(16, 16, ILC_COLOR24 | ILC_MASK, 5, 0);
ImageList_AddMasked(hImgList, hBmp, RGB(0,128,128));
DeleteObject(hBmp);
```

Sites: `src/main.cpp` (top toolbar), `src/UI/EmitterList.cpp` (treeview imagelist + emitter list toolbar).

#### `TBBUTTON` size grew on x64 → toolbar buttons non-functional

**Symptom:** Icons rendered correctly, but clicking any toolbar button did nothing.

**Root cause:** `TBBUTTON::dwData` is 8 bytes on x64 (was 4 on Win32). Without `TB_BUTTONSTRUCTSIZE`, the toolbar control reads each entry at the old stride, so command IDs and indices come out garbled.

**Fix:** Send `TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON)` to every toolbar before `TB_ADDBUTTONS`. Three sites: top toolbar in `main.cpp`, emitter list toolbar and track-editor toolbar in `src/UI/`.

#### Game data path management

The editor expects to be pointed at an Empire at War / Forces of Corruption installation. The Steam Gold Pack splits assets across two siblings:
- `...\Star Wars Empire at War\GameData\` — base EaW
- `...\Star Wars Empire at War\corruption\` — FoC additions

Selected path is persisted to `HKEY_CURRENT_USER\Software\AloParticleEditor\GameDataPath` and re-read on launch.

**Sibling auto-add:** If the user picks one of those two folders, `AddSiblingGamePath` (in `main.cpp`) automatically also includes the other. Required because most particle textures live in the base game's `GameData\Data\Textures.meg`, but FoC-only models reference shaders/textures shipped in `corruption\Data\`.

**Default texture not loading?** Check the debug console for `[FM] Searching N megafiles for: ...` lines. If the path the editor is checking doesn't include both `GameData` and `corruption`, the sibling auto-add wasn't triggered (e.g. the saved registry path was ad-hoc, not one of those two).

---

### VS2022 port (initial bring-up — `afxres.h`, DXSDK, C4005, MFC IDs)
*2024-11-05 · [`f8d6991`](https://github.com/DrKnickers/particle-editor/commit/f8d6991)*

Pre-PR, before the GitHub Actions workflow existed. Four resource-compiler / build-config issues that surfaced moving the project to Visual Studio 2022:

#### `afxres.h` not found

**Problem:** `.rc` files and `src/UI/UI.h` included `afxres.h`, an MFC header not present without the MFC workload.

**Fix:** Replaced `afxres.h` with `winres.h` in all `.rc` files. Removed the include entirely from `UI.h` (resource-compiler headers don't belong in C++ source).

**Files changed:**
- `src/ParticleEditor.rc`
- `src/ParticleEditor.en.rc`
- `src/ParticleEditor.de.rc`
- `src/UI/UI.h`

#### `d3dx9.h` not found

**Problem:** The project expected the DXSDK at `$(SolutionDir)libs\dx9\`, which didn't exist in the repo.

**Fix:** Updated all four build configurations in `src/ParticleEditor.vcxproj` to use the installed DXSDK via the `$(DXSDK_DIR)` environment variable (set automatically by the DXSDK installer):
- Include: `$(DXSDK_DIR)Include`
- Lib x86: `$(DXSDK_DIR)Lib\x86`
- Lib x64: `$(DXSDK_DIR)Lib\x64`

#### C4005 macro redefinition warnings (treated as errors)

**Problem:** After switching to `$(DXSDK_DIR)`, the DXSDK headers defined `RT_MANIFEST` and related manifest constants, which were then redefined by `winres.h` → `winuser.rh`, producing C4005 warnings that were fatal due to `TreatWarningAsError`.

**Root cause:** `winres.h` was incorrectly included in `src/UI/UI.h`. It's a resource-compiler header and must not appear in C++ translation units.

**Fix:** Removed `#include <winres.h>` from `src/UI/UI.h`. The `.rc` files still include it correctly (for the RC compiler only).

#### Undeclared MFC command IDs (`ID_FILE_NEW`, `ID_FILE_OPEN`, etc.)

**Problem:** These standard MFC command IDs were previously defined by `afxres.h`. After removing that header, they were undefined in both C++ code and the resource compiler.

**Fix:** Created `src/mfc_ids.h` with the standard MFC values:
```c
#define ID_FILE_NEW     0xE100
#define ID_FILE_OPEN    0xE101
#define ID_FILE_SAVE    0xE103
#define ID_FILE_SAVE_AS 0xE104
#define ID_EDIT_CUT     0xE123
#define ID_EDIT_COPY    0xE122
#define ID_EDIT_PASTE   0xE125
```
Included from:
- `src/resource.h` (for C++ code)
- All three `.rc` files (for the resource compiler, after `winres.h`)

---

## Reference

Long-lived build / runtime documentation. Doesn't track individual commits — update these in place when their facts change.

### Project Overview

A DirectX 9 particle editor for Star Wars: Empire at War / Forces of Corruption modding. Written in C++ using Win32 and D3DX9. Built with Visual Studio 2022 (toolset v143), targeting x64 and Win32.

Solution: `ParticleEditor.sln`  
Main project: `src/ParticleEditor.vcxproj`

### Build Environment Requirements

- **Visual Studio 2022** (toolset `v143`). Newer VS releases (e.g. VS18/2026 Insiders) will silently bump this to a higher toolset (`v145`+) when you open the solution; revert any such change before committing or CI will fail with `MSB8020: build tools for v145 cannot be found`.
- **DirectX SDK June 2010** — must be installed. The project uses `$(DXSDK_DIR)` to find headers and libs. Install from: https://www.microsoft.com/en-us/download/details.aspx?id=6812
- **Windows 10 SDK** (10.0) — configured via `WindowsTargetPlatformVersion`
- MFC is **not** required

#### Building

```
MSBuild ParticleEditor.sln /p:Configuration=Debug /p:Platform=x64
```

Or open the solution in Visual Studio and build normally.

### Runtime Requirements

#### `d3dx9_43.dll`

The June 2010 DXSDK links against `d3dx9_43.dll`. Windows does **not** ship this DLL. It must be provided one of two ways:

**Option A — System install:**  
Install the DirectX End-User Runtime: https://www.microsoft.com/en-us/download/details.aspx?id=35

**Option B — Local (next to exe):**  
Extract from the DXSDK redist cab:
```
expand "C:\Program Files (x86)\Microsoft DirectX SDK (June 2010)\Redist\Jun2010_d3dx9_43_x64.cab" -F:d3dx9_43.dll <output_dir>
```
Place `d3dx9_43.dll` alongside the built `.exe`.

### Resource File Structure

Three RC files are compiled into the exe:
- `src/ParticleEditor.rc` — shared resources (bitmaps, shaders, icons); includes `Resources/resource.h`
- `src/ParticleEditor.en.rc` — English strings, menus, dialogs; includes `Resources/resource.en.h`
- `src/ParticleEditor.de.rc` — German strings, menus, dialogs; includes `Resources/resource.de.h`

Resource IDs are split across:
- `src/Resources/resource.h` — shared IDs (bitmaps, toolbar, ground texture, etc.)
- `src/Resources/resource.en.h` — English dialog/string/menu IDs (`IDR_MENU1`, `IDD_EMITTER_LIST`, `IDS_*`, etc.)
- `src/Resources/resource.de.h` — German equivalents
- `src/mfc_ids.h` — MFC standard command IDs (not auto-generated)
- `src/resource.h` — wrapper that includes all of the above for C++ code

### Debug Build Notes

The debug build calls `AllocConsole()` for a console window on launch. Exceptions are **not** caught at the WinMain level in debug builds (the try/catch is `#ifdef NDEBUG` only) — any unhandled exception will crash rather than showing a message box.

The app requires a game data path (Empire at War / Forces of Corruption installation) on first run. If the current directory doesn't contain `Data\MegaFiles.xml`, a folder browser dialog will appear asking for the game data location.

### Reverse-engineering the canonical engine binaries

We sometimes need to recover a "magic number" that the engine bakes into its binary but doesn't expose through any shader source or canonical editor UI — for example, the bloom blur iteration count (proven to be `4` via the [investigation in PR #49](#bloom-blur-iteration-count-proven-canonical), full plan + review at [`tasks/find_bloom_iterations.md`](tasks/find_bloom_iterations.md)). This section is the kit for doing it again.

#### What you're working with

- **Petroglyph 2025 64-bit patch** binaries at `<path> Wars Empire at War\` (path discovered via `HKLM\SOFTWARE\WOW6432Node\LucasArts\Star Wars Empire at War Forces of Corruption\1.0\exepath` — see [src/main.cpp:2467](src/main.cpp:2467) for the full key list the editor itself uses).
- **`StarWarsG.exe`** (12.4 MB, x64 PE, stripped) — the actual game engine. `swfoc.exe` is a thin launcher; ignore it.
- **`EAW Terrain Editor.exe`** (17.1 MB, x64 PE, stripped) at `…\corruption\Mods\Mod\` — same engine code as `StarWarsG.exe`, used as the canonical reference for editor-tool behaviour. Bloom function bodies are byte-identical in size between the two; only addresses differ.
- **No `.pdb`** — both binaries are stripped. Symbol names will be `FUN_140xxxxxxx` / `DAT_140xxxxxxx`.
- **PIX legacy is unusable** — the DX SDK June 2010 PIX only attaches to 32-bit D3D9; these binaries are x64. RenderDoc dropped D3D9 support in 1.x. apitrace would work for *capture-based* analysis but isn't needed for static answers.

#### Toolchain (already installed; not part of the editor build)

- **`C:\Tools\jdk-21.0.11+10`** — Adoptium Temurin JDK 21 (Ghidra 12.x dependency).
- **`C:\Tools\ghidra_12.0.4_PUBLIC`** — Ghidra 12.0.4 reverse-engineering suite.

To re-install or upgrade:
```powershell
# JDK 21 latest GA from Adoptium GitHub releases
gh api repos/adoptium/temurin21-binaries/releases/latest | python -c "import json,sys; r=json.load(sys.stdin); print([a['browser_download_url'] for a in r['assets'] if 'jdk_x64_windows_hotspot' in a['name'] and a['name'].endswith('.zip')][0])"
# Ghidra latest from NSA GitHub releases
gh api repos/NationalSecurityAgency/ghidra/releases/latest --jq '.assets[0].browser_download_url'
```
Verify SHA-256 against the `.sha256.txt` published next to the JDK zip, and against the SHA-256 line in the Ghidra release notes (`gh api ... --jq '.body'`). Extract both into `C:\Tools\`.

#### The reproducer

The four committed scripts under [`tasks/ghidra_scripts/`](tasks/ghidra_scripts) are general-purpose enough that each new investigation is roughly: *(1) clone-edit one of them with new anchor strings, (2) run via `analyzeHeadless`, (3) read the decompiled output.*

| Script | Purpose |
|---|---|
| [`FindBloomLoop.py`](tasks/ghidra_scripts/FindBloomLoop.py) | Anchors on a list of strings (`ANCHORS = [...]`), finds defined-data hits, collects xref-source functions, walks one level up the call graph, decompiles every candidate. Edit the `ANCHORS` list for a different feature. |
| [`FindBloomIterGlobal.py`](tasks/ghidra_scripts/FindBloomIterGlobal.py) | Once the loop function is identified and the bound is a global, this finds all readers/writers of that global address (`TARGET = 0x…`) and decompiles every writer function. |
| [`InspectIterGlobal.py`](tasks/ghidra_scripts/InspectIterGlobal.py) | Reads the initial bytes (`mem.getInt`) at a `.data` address and brute-force-searches the entire program for the address as a QWORD-LE / DWORD-LE byte pattern (catches references the auto-analyzer's xref builder missed). |
| [`InspectIterGlobalSWG.py`](tasks/ghidra_scripts/InspectIterGlobalSWG.py) | The same inspector with the `StarWarsG.exe` address constant. Pattern for cross-validation: clone the script with the cross-binary address. |

First-time import + auto-analysis on a 12–17 MB binary takes ~8–11 minutes. Subsequent script runs on the saved project use `-process` + `-noanalysis` and finish in seconds.

```powershell
# Set up the JDK Ghidra needs
$env:JAVA_HOME = 'C:\Tools\jdk-21.0.11+10'
$env:PATH      = "$env:JAVA_HOME\bin;$env:PATH"
$gh            = 'C:\Tools\ghidra_12.0.4_PUBLIC\support\analyzeHeadless.bat'
$proj          = 'tasks\ghidra_project'   # gitignored; rebuildable
$scripts       = 'tasks\ghidra_scripts'

# First time: import + auto-analyze a binary (slow, ~10 min)
& $gh $proj BloomRE -import 'D:\…\corruption\Mods\Mod\EAW Terrain Editor.exe' `
    -scriptPath $scripts -postScript FindBloomLoop.py -overwrite -loader PeLoader 2>&1 |
    ForEach-Object { "$_" } | Out-File log.txt -Encoding utf8

# Subsequent runs on the saved project (fast, seconds)
& $gh $proj BloomRE -process 'EAW Terrain Editor.exe' `
    -scriptPath $scripts -postScript FindBloomLoop.py -noanalysis 2>&1 |
    ForEach-Object { "$_" } | Out-File log.txt -Encoding utf8
```

#### Jython gotchas (Ghidra 12.0.4 still defaults to Jython 2.7 for `-postScript`)

- **PEP 263 encoding declaration required.** Top of every script: `# -*- coding: utf-8 -*-`. Without it, any non-ASCII byte in the source file (em-dash, arrow, etc.) breaks the script loader before line 1 runs.
- **Wrap `str(data.getValue())` in `try/except UnicodeEncodeError`.** Some defined-data entries in the EaW binaries contain non-ASCII bytes; Jython's default ASCII string encoder rejects them and crashes the iteration.
- **Use `Memory.getInt(addr)` to read an int**, not `Memory.getBytes(addr, length)`. The latter expects a Java `byte[]` buffer, not a Python int length, and the coercion error message is unhelpful (`2nd arg can't be coerced to byte[]`).
- **For `Memory.findBytes`**, the pattern must be a Java `byte[]`. Build it from a Python int list via `jarray.array([...], 'b')` with values mapped from `0..255` to `-128..127` because Java bytes are signed.

#### PowerShell-on-Win11 pitfalls when driving Ghidra

- **`Tee-Object` and `Out-File` default to UTF-16 LE in PS5.1.** This is harmless for human reading but breaks `rg`/`grep` (they expect UTF-8). Always pass `-Encoding utf8` explicitly when capturing analyzeHeadless output for later grepping.
- **`Invoke-WebRequest`'s `.Content` is a `byte[]`, not a string** in PS5.1 (changed in PS Core). Calling `.Trim()` on it throws `MethodNotFound`. Either use the SHA-256 published via the Adoptium GitHub Releases API instead of the `.sha256.txt` sidecar, or decode bytes via `[System.Text.Encoding]::ASCII.GetString(...)`.
- **Native exe stderr lines get wrapped in `RemoteException` PowerShell errors** when the call uses the call operator (`& exe`). The exit code is still correct; the stderr text is preserved in the captured output. Don't be alarmed by red console text from `java -version` or `analyzeHeadless.bat` — exit codes are authoritative.

#### Cross-validation pattern

When recovering a constant from one binary, **always re-run on the other one too.** Both `EAW Terrain Editor.exe` and `StarWarsG.exe` are compiled from the same engine source — bloom render function bodies are byte-identical in size, the call graph is identical in shape, but absolute addresses differ. If the constant *doesn't* match across both binaries, that's load-bearing information (the editor and game disagree about something), and the canonical reference for editor-tool behaviour is the Terrain Editor's value.

The Ghidra project at `tasks/ghidra_project/` is gitignored (~888 MB) — it's a rebuildable artifact. The committed scripts under `tasks/ghidra_scripts/` are the durable reproducer.

---

## Open Issues

- **Mod-bundled megafiles** (`Mods\<name>\Data\MegaFiles.xml`) are not loaded. Most particle-overriding mods ship loose files, which the loose-file path covers. Total conversions like Mod's Revenge or Awakening of the Rebellion that package assets in their own `.meg` would need a follow-up: extend `FileManager` with a `m_modMegafiles` vector that's searched before `m_megafiles`, populated/cleared on `SetModPath`.
- **`d3dx9_43.dll` redistribution.** D3DX9 is a DLL-only library — there is no static-link variant. The DLL must be findable at load time (alongside the exe, in `System32`, or via PATH). Per the DXSDK redist license we can ship it next to the exe in releases. Replacing D3DX9 with DirectXMath / DirectXTK / Effects11 would let us produce a single self-contained exe but is a large refactor woven through `engine.cpp` and `EmitterInstance.cpp`; deferred indefinitely.
