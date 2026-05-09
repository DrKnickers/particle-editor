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

---

## Open Issues

- **Mod-bundled megafiles** (`Mods\<name>\Data\MegaFiles.xml`) are not loaded. Most particle-overriding mods ship loose files, which the loose-file path covers. Total conversions like Mod's Revenge or Awakening of the Rebellion that package assets in their own `.meg` would need a follow-up: extend `FileManager` with a `m_modMegafiles` vector that's searched before `m_megafiles`, populated/cleared on `SetModPath`.
- **`d3dx9_43.dll` redistribution.** D3DX9 is a DLL-only library — there is no static-link variant. The DLL must be findable at load time (alongside the exe, in `System32`, or via PATH). Per the DXSDK redist license we can ship it next to the exe in releases. Replacing D3DX9 with DirectXMath / DirectXTK / Effects11 would let us produce a single self-contained exe but is a large refactor woven through `engine.cpp` and `EmitterInstance.cpp`; deferred indefinitely.
