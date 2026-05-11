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
