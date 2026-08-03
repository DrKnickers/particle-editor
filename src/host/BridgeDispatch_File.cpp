// Kind handlers for the file/* + autosave/* + undo/* bridge domain(s), moved out of
// DispatchInternal's ladder (Phase A dispatch split --
// tasks/2026-07-06-heavyweight-refactor-plan.md).

#include "BridgeDispatcher.h"
#include "BridgeDispatchShared.h"
#include "BridgeRequestContext.h"

#include "StringConv.h"        // host::Utf8ToWide / WideToUtf8
#include "../ModManager.h"     // initial-dir resolution (file/open, file/save-as)
#include "../ParticleSystemIO.h"  // LoadParticleSystem / SaveParticleSystem
#include "../Autosave.h"       // autosave/check-recovery, autosave/recover

#include <commdlg.h>           // GetOpenFileNameW / GetSaveFileNameW
#include <limits>

using nlohmann::json;

namespace host {

// The composed-host recovery oracle needs the real check/recover call site, but
// ordinary --test-host runs must remain isolated from a developer's real
// autosaves. The native harness supplies both an isolated child-only TEMP root
// and this sentinel; the request must opt in as well. Release builds never lift
// the suppression through this seam.
static bool AllowTestHostAutosaveRecovery(bool testHost, const json& params)
{
#ifndef NDEBUG
    if (!testHost || !params.value("__testAllowRecovery", false)) return false;
    wchar_t value[8] = {};
    const DWORD n =
        GetEnvironmentVariableW(L"ALO_AUTOSAVE_RECOVERY_TEST", value, 8);
    return n == 1 && value[0] == L'1';
#else
    (void)testHost;
    (void)params;
    return false;
#endif
}

// Mutation-test seam for the composed recovery oracle. An out-of-range group
// type is emitted by ParticleSystem::write but rejected by the production
// ParticleSystem parser. Therefore WriteRecoveryHandoff must fail before
// replacement, while an accidental wiring change to ordinary Autosave::Write
// would succeed and be caught by the native bridge spec.
//
// Keep the same test-host + request + environment boundary as the real scanner
// opt-in above. The loaded object is attempt-local, so a rejected handoff leaves
// both the live document and the on-disk orphan untouched for a clean retry.
static void CorruptRecoveryCandidateForTest(bool testHost,
                                            const json& params,
                                            ParticleSystem& loaded)
{
#ifndef NDEBUG
    if (!params.value("__testCorruptHandoffCandidate", false)
        || !AllowTestHostAutosaveRecovery(testHost, params))
        return;

    ParticleSystem::Emitter* emitter = loaded.getEmitters().empty()
        ? loaded.addRootEmitter()
        : loaded.getEmitters()[0];
    emitter->groups[ParticleSystem::GROUP_SPEED].type =
        ParticleSystem::NUM_GROUP_TYPES;
#else
    (void)testHost;
    (void)params;
    (void)loaded;
#endif
}

bool BridgeDispatcher::TryDispatchFile(BridgeRequestContext& ctx, const std::string& kind)
{
    // DispatchInternal-local aliases so the moved ladder blocks below stay
    // verbatim (plan #3A transforms only).
    const json&        params = ctx.params;
    const std::string& id     = ctx.id;

    // Native-only budget seam. It intentionally queries the bound UndoStack
    // instance rather than echoing UndoStack::MAX_TOTAL_BYTES, so the native
    // contract test proves HostWindow's shipped construction. Configuration is
    // allowed only in --test-host and only while the stack is empty/synchronized.
    if (kind == "undo/test/budget")
    {
        if (!m_testHost)
        {
            ctx.SendErr("undo/test/budget requires --test-host");
            return true;
        }
        if (!m_undo)
        {
            ctx.SendErr("undo/test/budget: undo stack unavailable");
            return true;
        }
        if (params.contains("maxTotalBytes"))
        {
            const json& value = params["maxTotalBytes"];
            if (!value.is_number_unsigned())
            {
                ctx.SendErr("undo/test/budget: maxTotalBytes must be a non-negative integer");
                return true;
            }
            const json::number_unsigned_t requested =
                value.get<json::number_unsigned_t>();
            if (requested > static_cast<json::number_unsigned_t>(
                    (std::numeric_limits<size_t>::max)()))
            {
                ctx.SendErr("undo/test/budget: maxTotalBytes exceeds size_t");
                return true;
            }
            if (!m_undo->SetMaxTotalBytesForTesting(
                    static_cast<size_t>(requested)))
            {
                ctx.SendErr(
                    "undo/test/budget: stack must be empty and synchronized");
                return true;
            }
        }
        ctx.SendOk(json{
            {"maxTotalBytes", m_undo->MaxTotalBytes()},
            {"totalBytes", m_undo->TotalBytes()},
            {"depth", m_undo->Depth()},
            {"cursor", m_undo->Cursor()},
        });
        return true;
    }

    // -------- test-host document-replacement seams ------------------
    //
    // Successful orphan discovery is intentionally suppressed under
    // --test-host, so native tests need a narrow way to seed the real recovery
    // handler. The paired emit seam primes both coalescing clocks immediately
    // before calling the production notification chokepoint. Both are rejected
    // outside --test-host.
    if (kind == "debug/seed-autosave-recovery")
    {
        if (!m_testHost)
        {
            ctx.SendErr("debug/seed-autosave-recovery requires --test-host");
            return true;
        }
        const std::string path8 = params.value("path", std::string{});
        if (path8.empty())
        {
            ctx.SendErr("debug/seed-autosave-recovery requires a path");
            return true;
        }
        m_pendingOrphan = Autosave::OrphanSession{};
        m_pendingOrphan.pid = GetCurrentProcessId();
        m_pendingOrphan.recentPath = Utf8ToWide(path8);
        m_pendingOrphan.originalFilename =
            Utf8ToWide(params.value("originalFilename", std::string{}));
        m_hasPendingOrphan = true;
        ctx.SendOk(json::object());
        return true;
    }
    if (kind == "debug/emit-document-replaced")
    {
        if (!m_testHost)
        {
            ctx.SendErr("debug/emit-document-replaced requires --test-host");
            return true;
        }
        const unsigned long long now = GetTickCount64();
        m_lastStateEmitTick = now;
        m_lastTreeEmitTick  = now;
        ResetSelectionAndEmitDocumentChanged();
        ctx.SendOk(json::object());
        return true;
    }

    // -------- undo/perform --------
    //
    // New-UI mutation handlers call captureUndo() PRE-mutation, so the
    // snapshot at entries[cursor-1] represents the state BEFORE the
    // most recent mutation — not the current live state. UndoStack's
    // Undo() is built around the legacy POST-mutation convention
    // (cursor-- ; return entries[cursor-1] = the previous live state).
    // To reconcile: at the head of history (cursor == size) AND only
    // when live is genuinely skewed ahead of the tip (IsLiveAhead),
    // snapshot the current live state once before stepping back. That
    // auto-capped entry IS the live state, so Undo's math now returns
    // the PRE-mutation snapshot — exactly what Ctrl+Z should restore.
    // The IsLiveAhead() guard matters: cursor == size is ALSO true right
    // after a Redo() (redo to the tip leaves cursor == size), but there
    // live is already IN SYNC with entries[cursor-1]. Auto-capping in
    // that case duplicated the tip and the following Undo() returned the
    // duplicate, silently swallowing the undo (undo→redo→undo lost the
    // second undo). Skip the auto-cap mid-redo-branch (the post-state is
    // already at entries[cursor]) or on an empty stack (CanUndo would
    // be false anyway).
    if (kind == "undo/perform")
    {
        std::string dir = params.value("direction", std::string("undo"));
        bool applied = false;
        if (m_undo && m_pParticleSystem && *m_pParticleSystem)
        {
            if (dir == "undo"
                && m_undo->Cursor() == m_undo->Depth()
                && m_undo->Depth() > 0
                && m_undo->IsLiveAhead())
            {
                // Route through the chokepoint so the auto-capped
                // live state carries the CURRENT ref transform (else the first
                // Ctrl+Z after a gizmo drag would restore an older transform).
                CaptureUndoPoint(
                    0, UndoStack::BudgetRetention::PreserveImmediatePair);
            }

            const std::vector<char>* snap = nullptr;
            size_t selIdx = SIZE_MAX;
            UndoStack::EditorAux aux;   // restored ref transform
            if (dir == "undo" && m_undo->CanUndo())
            {
                applied = m_undo->Undo(&snap, &selIdx, &aux);
            }
            else if (dir == "redo" && m_undo->CanRedo())
            {
                applied = m_undo->Redo(&snap, &selIdx, &aux);
            }

            if (applied && snap != nullptr)
            {
                ApplyUndoSnapshot(*snap, selIdx, aux);
            }
        }
        ctx.SendOk(json{{"applied", applied}});
        if (applied)
        {
            EmitEngineStateChanged();
            EmitEmittersTreeChanged();
        }
        return true;
    }


    // -------- file/* ----------------------
    //
    // The new-UI host doesn't yet own a ParticleSystem* (emitter / file-
    // load wiring is later work). So the file handlers
    // perform the *editor-level* side of the operation — currentFilePath
    // tracking, dirty flag, recent-files registry, native picker
    // round-trip — but skip the engine-level ParticleSystem read/write
    // until that pointer exists. Same forward-compatible no-op pattern
    // as engine/action/rescale-system. (The legacy `DoNewFile` /
    // `DoOpenFile` / `DoSaveFile` handlers in src/main.cpp were since
    // removed; these bridge handlers are now the only file-operation path.)

    // -------- file/new --------
    // replace the host-owned ParticleSystem with a fresh empty
    // one + one root emitter (mirrors legacy DoNewFile at
    // src/main.cpp:1289). Clear editor path / dirty.
    if (kind == "file/new")
    {
        // shift-click-to-spawn: if the user is mid-Shift-hold when
        // they hit file/new, kill the cursor-bound instance before
        // dropping the ParticleSystem it was spawned from. Mirrors the
        // legacy DoNewFile teardown sequence for `attachedParticleSystem`
        // at src/main.cpp:1289-1305.
        if (m_pAttachedParticleSystem && *m_pAttachedParticleSystem && m_engine)
        {
            fprintf(stderr, "[ArchC-kill] file/new killing attached=%p\n",
                    static_cast<void*>(m_pAttachedParticleSystem->ptr));
            m_engine->KillParticleSystem(*m_pAttachedParticleSystem);
            m_pAttachedParticleSystem->Reset();
        }
        // The record preview borrow dies with the document too — null it
        // eagerly (Clear() frees the instance; see the preview/* UAF guard
        // in BridgeDispatch_Spawner.cpp).
        m_recordPreviewAttached.Reset();
        if (m_pParticleSystem)
        {
            *m_pParticleSystem = std::make_unique<ParticleSystem>();
            (*m_pParticleSystem)->addRootEmitter();
            // Legacy parity (DoNewFile): start with the seeded root emitter
            // (index 0) SELECTED, so the Inspector/curve panel open populated.
            // The emitters/selected emit below syncs React's selection atom.
            m_selectedEmitterId = 0;
        }
        // render loop: notify Engine that the ParticleSystem pointer
        // it knows about is now stale. Mirrors legacy DoNewFile at
        // src/main.cpp:1207 (Clear() then OnParticleSystemChanged(-1))
        // so the engine drops cached instances + per-emitter state.
        if (m_engine)
        {
            m_engine->Clear();
            m_engine->OnParticleSystemChanged(-1);
        }
        // Reset undo stack — prior session's entries reference the
        // now-freed ParticleSystem and would crash a future restore.
        // Mirrors legacy LoadFile at src/main.cpp:1103.
        if (m_undo) m_undo->Clear();
        // Refresh the "saved" reference snapshot — the fresh-with-
        // one-root state is the new dirty-bit baseline. Mutate + Ctrl+Z
        // back here clears the title-bar asterisk in
        // ApplyUndoSnapshot's content-compare.
        if (m_pParticleSystem && *m_pParticleSystem)
            m_savedSnapshot = UndoStack::Serialize(**m_pParticleSystem);
        m_currentFilePath.clear();
        ctx.SendOk(json::object());
        SetDirty(false);
        EmitEngineStateChanged();
        // Polish round 3: React's EmitterTree subscribes to
        // emitters/tree/changed; without this emit the tree stays on
        // its pre-file/new state even after the ParticleSystem has
        // been swapped under it.
        EmitEmittersTreeChanged();
        // Legacy parity: announce the default selection so React's selection
        // atom (and thus the Inspector + curve panel) updates — the snapshot's
        // selectedEmitterId alone isn't re-read post-mount; the EmitterTree
        // tracks the `emitters/selected` event.
        if (m_emit)
        {
            json env = {
                {"type",    "evt"},
                {"kind",    "emitters/selected"},
                {"payload", json{{"id", m_selectedEmitterId < 0 ? json(nullptr)
                                                                 : json(m_selectedEmitterId)}}},
            };
            m_emit(env.dump());
        }
        return true;
    }


    // -------- file/open + file/pick-open --------
    //
    // `path` source (both kinds):
    //   - If `params.path` is provided (Recent Files / drag-drop / Open
    //     dialog already resolved on the React side), use it directly.
    //   - Otherwise pop GetOpenFileNameW. The native dialog runs a
    //     nested message loop; host pump pauses for the dialog's
    //     lifetime, which is fine because the JS caller is awaiting.
    //
    // Post-pick behaviour DIFFERS by kind:
    //   - file/pick-open (and the skydome/ground TEXTURE filter variants)
    //     return the chosen path WITHOUT touching the document — no commit
    //     to m_currentFilePath, recents, events, dirty reset, or engine load
    //     (release-audit #2: a Browse must never replace the active document).
    //   - file/open with the default .alo filter commits the path into
    //     m_currentFilePath, pushes to recents, fires recent/changed +
    //     engine/state/changed, and clears dirty; the ParticleSystem load is
    //     forward-deferred.
    if (kind == "file/open" || kind == "file/pick-open")
    {
        std::wstring path;
        if (auto pit = params.find("path"); pit != params.end() && pit->is_string())
        {
            path = Utf8ToWide(pit->get<std::string>());
        }
        // Resolve optional filter discriminator. Default "alo" keeps
        // File→Open / recents / drag-drop behaviour unchanged; the
        // picker panels pass "skydome" / "ground" so the dialog
        // defaults to the texture filter the user actually needs AND
        // the post-pick load path is skipped (textures aren't .alo
        // files).
        std::string filterId = "alo";
        if (auto fit = params.find("filter"); fit != params.end() && fit->is_string())
        {
            filterId = fit->get<std::string>();
        }

        if (path.empty())
        {
            const wchar_t* lpstrFilter = L"Particle Files (*.alo)\0*.alo\0All Files (*.*)\0*.*\0\0";
            const wchar_t* lpstrTitle  = L"Open particle system";
            if (filterId == "skydome")
            {
                lpstrFilter = L"Texture Files (*.dds;*.tga)\0*.dds;*.tga\0All Files (*.*)\0*.*\0\0";
                lpstrTitle  = L"Open skydome texture";
            }
            else if (filterId == "ground")
            {
                lpstrFilter = L"Texture Files (*.dds;*.tga)\0*.dds;*.tga\0All Files (*.*)\0*.*\0\0";
                lpstrTitle  = L"Open ground texture";
            }

            // Default the "Open particle system" picker to the selected
            // mod's Models folder (where .alo models live), mirroring the
            // textures/browse default above. Gated on the .alo case
            // (filterId == "alo", the default when no `filter` key is
            // passed) so the skydome/ground TEXTURE variants are NOT
            // pointed at Models — they keep the dialog's default dir.
            // Covers BOTH File->Open (file/open) and Import Emitters' Browse
            // (file/pick-open) — both use the default "alo" filter (no `filter`
            // key). Fallback: mod root -> default.
            std::wstring initialDir;
            if (filterId == "alo" && m_modManager)
            {
                const std::wstring mod = m_modManager->GetPrimaryLayerPath();
                if (!mod.empty())
                {
                    auto isDir = [](const std::wstring& p) -> bool {
                        DWORD a = GetFileAttributesW(p.c_str());
                        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    };
                    const std::wstring modelsDir = mod + L"\\Data\\Art\\Models";
                    if (isDir(modelsDir)) initialDir = modelsDir;
                    else if (isDir(mod))  initialDir = mod;
                }
            }

            wchar_t buf[MAX_PATH] = {};
            OPENFILENAMEW ofn = {};
            ofn.lStructSize     = sizeof(ofn);
            ofn.hwndOwner       = m_hostHwnd;
            ofn.lpstrFile       = buf;
            ofn.nMaxFile        = MAX_PATH;
            ofn.lpstrFilter     = lpstrFilter;
            ofn.lpstrTitle      = lpstrTitle;
            ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
            ofn.Flags           = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

            if (!GetOpenFileNameW(&ofn))
            {
                // User cancelled / dialog failure.
                // G3: intentional sendOk — user-cancel is not an error; the
                // file/* family deliberately returns failures as sendOk so
                // request() won't throw (caller reads nested ok).
                ctx.SendOk(json{{"ok", false}, {"error", "user-cancelled"}});
                return true;
            }
            path = buf;
        }

        // Non-mutating picker: file/pick-open returns the chosen path WITHOUT
        // loading it into the host (no document swap, undo clear, dirty reset,
        // recents, or engine touch). Used by Import Emitters' Browse and the
        // texture pickers so a Browse can never replace the active dirty
        // document (release-audit #2).
        if (kind == "file/pick-open")
        {
            ctx.SendOk(json{{"ok", true}, {"path", WideToUtf8(path)}});
            return true;
        }

        // Texture-picker variants short-circuit here: just return the
        // path so the React side can route it through the appropriate
        // engine setter (engine/set/skydome-custom-path or
        // engine/set/ground-slot-custom-path). Don't touch
        // m_currentFilePath / recents / engine state — those belong to
        // the "open particle system" semantics of the .alo path below.
        if (filterId != "alo")
        {
            ctx.SendOk(json{{"ok", true}, {"path", WideToUtf8(path)}});
            return true;
        }

        // actually load the .alo into the host-owned slot.
        std::string err;
        std::unique_ptr<ParticleSystem> loaded = LoadParticleSystem(path, &err);
        if (!loaded)
        {
            // Don't touch m_currentFilePath / recents on failure —
            // matches legacy LoadFile behaviour (history append only
            // happens after a successful parse).
            // G3: intentional sendOk — file/open returns failures as
            // ctx.SendOk({ok:false}) by design so request() won't throw; the
            // success path returns ctx.SendOk({ok:true,path}) and the caller
            // reads nested ok. Converting would split this handler's contract.
            ctx.SendOk(json{{"ok", false}, {"error", err.empty() ? std::string("load failed") : err}});
            return true;
        }
        // shift-click-to-spawn: kill any cursor-bound instance
        // attached to the about-to-be-replaced ParticleSystem before
        // swapping. Same reasoning as the file/new branch above.
        if (m_pAttachedParticleSystem && *m_pAttachedParticleSystem && m_engine)
        {
            fprintf(stderr, "[ArchC-kill] file/open killing attached=%p\n",
                    static_cast<void*>(m_pAttachedParticleSystem->ptr));
            m_engine->KillParticleSystem(*m_pAttachedParticleSystem);
            m_pAttachedParticleSystem->Reset();
        }
        // The record preview borrow dies with the document too — null it
        // eagerly (Clear() frees the instance; see the preview/* UAF guard
        // in BridgeDispatch_Spawner.cpp).
        m_recordPreviewAttached.Reset();
        if (m_pParticleSystem)
        {
            *m_pParticleSystem = std::move(loaded);
        }
        // Load-time sweep: older .alo files may contain
        // single-member link groups. Sweep once after binding so the
        // data layer matches the render-layer filter from frame one.
        // Does NOT call ctx.MarkDirty() — the correction is a
        // normalization, not user-driven mutation; marking dirty
        // would force a save-prompt on every open of a legacy file
        // even when the user makes no further changes. Subsequent
        // mutations re-fire the sweep via the same helper.
        EnforceSingleMemberLinkGroups();
        // Reset undo stack — prior session's entries reference the
        // now-freed ParticleSystem. Mirrors legacy LoadFile at
        // src/main.cpp:1103.
        if (m_undo) m_undo->Clear();
        // Refresh the "saved" reference snapshot — the just-loaded
        // state IS the saved file's content. Captured AFTER the
        // link-group sweep so legacy singleton-link-group .alo files don't show
        // as dirty when the user undoes back to "as loaded".
        if (m_pParticleSystem && *m_pParticleSystem)
            m_savedSnapshot = UndoStack::Serialize(**m_pParticleSystem);
        // render loop: same notification sequence as file/new —
        // the engine's cached per-instance / per-emitter state is now
        // stale and must be cleared. Matches the legacy DoOpenFile path.
        if (m_engine)
        {
            m_engine->Clear();
            m_engine->OnParticleSystemChanged(-1);
            // Polish round 3: the legacy native DoOpenFile relied
            // on first-render lazy texture binding via per-instance
            // construction; the host's WebView2 composition timing
            // produces white-fallback particles unless we explicitly
            // invalidate the cache. ReloadTextures
            // is the same operation View → Reload Textures already
            // does on demand; calling it here makes file/open
            // self-sufficient.
            m_engine->ReloadTextures();
        }
        m_currentFilePath = path;
        m_recentFiles = PersistsUserState() ? WriteRecentFile(path) : ReadRecentFiles();
        ctx.SendOk(json{{"ok", true}, {"path", WideToUtf8(path)}});
        SetDirty(false);
        EmitRecentChanged();
        // Reset before the state snapshot, then preserve the established
        // state -> tree -> selection event order. Otherwise the immediate
        // state path publishes the previous document's positional id.
        ResetSelectionAndEmitDocumentChanged();
        return true;
    }


    // -------- file/save --------
    //
    // If `params.path` provided, use it. Else if m_currentFilePath is
    // set (named document), save there. Else pop GetSaveFileNameW.
    // Matches legacy `DoSaveFile(info, /*saveas=*/false)`.
    if (kind == "file/save")
    {
        std::wstring path;
        if (auto pit = params.find("path"); pit != params.end() && pit->is_string())
        {
            path = Utf8ToWide(pit->get<std::string>());
        }
        if (path.empty()) path = m_currentFilePath;
        if (path.empty())
        {
            // No remembered path → pop save-as picker.
            wchar_t buf[MAX_PATH] = {};
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = m_hostHwnd;
            ofn.lpstrFile   = buf;
            ofn.nMaxFile    = MAX_PATH;
            ofn.lpstrFilter = L"Particle Files (*.alo)\0*.alo\0All Files (*.*)\0*.*\0\0";
            ofn.lpstrDefExt = L"alo";
            ofn.lpstrTitle  = L"Save particle system";
            ofn.Flags       = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

            if (!GetSaveFileNameW(&ofn))
            {
                // G3: intentional sendOk — user-cancel is not an error; the
                // file/* family returns failures as sendOk so request() won't
                // throw (success path is ctx.SendOk({ok:true,path})).
                ctx.SendOk(json{{"ok", false}, {"error", "user-cancelled"}});
                return true;
            }
            path = buf;
        }

        // actually write the host-owned ParticleSystem to disk.
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            // G3: intentional sendOk — same dual-result contract as the
            // success path below (ctx.SendOk({ok:true,path})); caller reads
            // nested ok. Converting would split this handler's contract.
            ctx.SendOk(json{{"ok", false}, {"error", "particle system not bound"}});
            return true;
        }
        std::string err;
        if (!SaveParticleSystem(m_pParticleSystem->get(), path, &err))
        {
            // G3: intentional sendOk — see above; failure stays nested-ok so
            // it matches the success payload shape the caller inspects.
            ctx.SendOk(json{{"ok", false}, {"error", err.empty() ? std::string("save failed") : err}});
            return true;
        }
        m_currentFilePath = path;
        m_recentFiles = PersistsUserState() ? WriteRecentFile(path) : ReadRecentFiles();
        // Refresh the "saved" reference snapshot — what we just wrote
        // to disk IS the new saved state. ApplyUndoSnapshot uses this
        // to clear the title-bar asterisk when the user undoes back
        // to a content-equal state.
        m_savedSnapshot = UndoStack::Serialize(**m_pParticleSystem);
        ctx.SendOk(json{{"ok", true}, {"path", WideToUtf8(path)}});
        SetDirty(false);
        // The work is now on disk — this session's autosave is
        // redundant. Delete it so a clean exit leaves no orphan to prompt
        // for. Further edits re-create it on the next tick. (Mirrors legacy
        // main.cpp DeleteOurSession-after-save.)
        Autosave::DeleteOurSession();
        EmitRecentChanged();
        EmitEngineStateChanged();
        return true;
    }


    // -------- file/save-as --------
    //
    // ALWAYS pops GetSaveFileNameW. Matches legacy
    // `DoSaveFile(info, /*saveas=*/true)`. Same path-commit / recents /
    // dirty side effects as file/save.
    if (kind == "file/save-as")
    {
        wchar_t buf[MAX_PATH] = {};
        // Seed with the current filename so the dialog opens at the
        // existing path's directory — matches legacy save-as ergonomics.
        if (!m_currentFilePath.empty() &&
            m_currentFilePath.size() < MAX_PATH)
        {
            wcscpy_s(buf, MAX_PATH, m_currentFilePath.c_str());
        }
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = m_hostHwnd;
        ofn.lpstrFile   = buf;
        ofn.nMaxFile    = MAX_PATH;
        ofn.lpstrFilter = L"Particle Files (*.alo)\0*.alo\0All Files (*.*)\0*.*\0\0";
        ofn.lpstrDefExt = L"alo";
        ofn.lpstrTitle  = L"Save particle system as";
        ofn.Flags       = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

        if (!GetSaveFileNameW(&ofn))
        {
            // G3: intentional sendOk — user-cancel is not an error; the
            // file/* family returns failures as sendOk so request() won't
            // throw (success path is ctx.SendOk({ok:true,path})).
            ctx.SendOk(json{{"ok", false}, {"error", "user-cancelled"}});
            return true;
        }

        std::wstring path = buf;
        // actually write to disk.
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            // G3: intentional sendOk — same dual-result contract as the
            // success path below (ctx.SendOk({ok:true,path})); caller reads
            // nested ok. Converting would split this handler's contract.
            ctx.SendOk(json{{"ok", false}, {"error", "particle system not bound"}});
            return true;
        }
        std::string err;
        if (!SaveParticleSystem(m_pParticleSystem->get(), path, &err))
        {
            // G3: intentional sendOk — see above; failure stays nested-ok so
            // it matches the success payload shape the caller inspects.
            ctx.SendOk(json{{"ok", false}, {"error", err.empty() ? std::string("save failed") : err}});
            return true;
        }
        m_currentFilePath = path;
        m_recentFiles = PersistsUserState() ? WriteRecentFile(path) : ReadRecentFiles();
        // Refresh the "saved" reference snapshot — see file/save above.
        m_savedSnapshot = UndoStack::Serialize(**m_pParticleSystem);
        ctx.SendOk(json{{"ok", true}, {"path", WideToUtf8(path)}});
        SetDirty(false);
        // See file/save — autosave is redundant once saved to disk.
        Autosave::DeleteOurSession();
        EmitRecentChanged();
        EmitEngineStateChanged();
        return true;
    }


    // -------- file/recent/list --------
    //
    // Re-reads the registry on every call. Cheap (≤ 9 entries) and keeps
    // the in-memory list in lockstep with the on-disk recent-files registry
    // (e.g. writes from another running instance).
    if (kind == "file/recent/list")
    {
        m_recentFiles = ReadRecentFiles();
        json paths = json::array();
        for (const auto& w : m_recentFiles) paths.push_back(WideToUtf8(w));
        ctx.SendOk(json{{"paths", paths}});
        return true;
    }


    // -------- autosave/check-recovery --------
    //
    // React calls this once on mount. Scan %TEMP%\AloParticleEditor\ for an
    // orphaned autosave left by a crashed prior session and return it (or
    // null). The prompt is suppressed under --test-host, automation mode
    // (--record / --drive), or when a document is already loaded — see
    // Autosave::ShouldSuppressRecoveryPrompt for the per-mode rationale. Stash
    // the live OrphanSession so autosave/recover can consume its temp paths
    // w/o re-scan.
    if (kind == "autosave/check-recovery")
    {
        m_hasPendingOrphan = false;
        const bool allowTestRecovery =
            AllowTestHostAutosaveRecovery(m_testHost, params);
        const bool suppressForTestHost = m_testHost && !allowTestRecovery;
        if (Autosave::ShouldSuppressRecoveryPrompt(
                suppressForTestHost, m_ephemeral, !m_currentFilePath.empty()))
        {
            ctx.SendOk(json{{"orphan", nullptr}});
            return true;
        }
        Autosave::OrphanSession s;
        if (!Autosave::ScanForOrphan(&s))
        {
            ctx.SendOk(json{{"orphan", nullptr}});
            return true;
        }
        m_pendingOrphan    = s;
        m_hasPendingOrphan = true;
#ifndef NDEBUG
        fprintf(stderr, "[autosave] check-recovery: orphan found (recent=%d stable=%d)\n",
                s.recentPath.empty() ? 0 : 1, s.stablePath.empty() ? 0 : 1);
#endif

        // FILETIME (100-ns ticks since 1601) → Unix epoch ms for React.
        auto mtimeMs = [](const FILETIME& ft) -> json {
            ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
            const unsigned long long EPOCH_DIFF_100NS = 116444736000000000ULL;
            if (u.QuadPart < EPOCH_DIFF_100NS) return json(nullptr);
            return json((u.QuadPart - EPOCH_DIFF_100NS) / 10000ULL);
        };
        json orphan = {
            {"originalFilename", WideToUtf8(s.originalFilename)},
            {"recentMtimeMs", s.recentPath.empty() ? json(nullptr) : mtimeMs(s.recentMtime)},
            {"stableMtimeMs", s.stablePath.empty() ? json(nullptr) : mtimeMs(s.stableMtime)},
        };
        ctx.SendOk(json{{"orphan", orphan}});
        return true;
    }


    // -------- autosave/recover --------
    //
    // Apply the recovery choice from the React dialog. For recent/stable,
    // load the chosen temp .alo via the SAME swap+notify sequence file/open
    // uses (so the attached-cursor reseat runs), then present it AS the
    // original filename with dirty=true: an empty saved baseline keeps it
    // dirty until a real save (the temp content matches no on-disk file), and
    // the temp path never enters recents. For discard, leave the boot
    // document untouched. The orphan files are consumed (deleted) ONLY on a
    // successful recover or an explicit discard — a FAILED load keeps them so the
    // other tier (or the next launch) can still recover (release-audit #3).
    if (kind == "autosave/recover")
    {
        if (!m_hasPendingOrphan)
        {
            // No prior orphan check, or it was already consumed by a successful
            // recover / discard.
            ctx.SendOk(json{{"status", "failed"}, {"reason", "no_pending_session"}});
            return true;
        }
        const std::string choice = params.value("choice", std::string("discard"));
        // Copy the session for the attempt but DO NOT consume it yet — a failed
        // load must leave it intact so the user can retry the other tier.
        const Autosave::OrphanSession s = m_pendingOrphan;

        std::wstring restorePath;
        if (choice == "recent")      restorePath = s.recentPath;
        else if (choice == "stable") restorePath = s.stablePath;
#ifndef NDEBUG
        fprintf(stderr, "[autosave] recover: choice=%s restore=%d\n",
                choice.c_str(), restorePath.empty() ? 0 : 1);
#endif

        Autosave::RecoverOutcome outcome = Autosave::RecoverOutcome::Failed;
        std::string reason;

        if (choice == "discard")
        {
            outcome = Autosave::RecoverOutcome::Discarded;
        }
        else if (restorePath.empty())
        {
            // A recover choice whose tier has no file on disk — a failure, never a
            // silent delete, so the dialog can offer the other tier.
            outcome = Autosave::RecoverOutcome::Failed;
            reason  = "no_file";
        }
        else
        {
            std::string err;
            std::unique_ptr<ParticleSystem> loaded = LoadParticleSystem(restorePath, &err);
            if (loaded)
            {
                CorruptRecoveryCandidateForTest(m_testHost, params, *loaded);
                // Establish and parse-verify a current-session recovery point
                // BEFORE touching the boot document or consuming the old
                // orphan. A crash at every later point still leaves at least
                // this copy; a handoff failure leaves both old tiers and the
                // boot document untouched for retry.
                if (!Autosave::WriteRecoveryHandoff(*loaded, s.originalFilename))
                {
                    outcome = Autosave::RecoverOutcome::Failed;
                    reason = "handoff_write_failed";
                }
                else
                {
                    if (m_pAttachedParticleSystem && *m_pAttachedParticleSystem && m_engine)
                    {
                        m_engine->KillParticleSystem(*m_pAttachedParticleSystem);
                        m_pAttachedParticleSystem->Reset();
                    }
                    // Record preview borrow: same document-teardown null (see the
                    // preview/* UAF guard in BridgeDispatch_Spawner.cpp).
                    m_recordPreviewAttached.Reset();
                    if (m_pParticleSystem) *m_pParticleSystem = std::move(loaded);
                    EnforceSingleMemberLinkGroups();
                    if (m_undo) m_undo->Clear();
                    if (m_engine)
                    {
                        m_engine->Clear();
                        m_engine->OnParticleSystemChanged(-1);
                        m_engine->ReloadTextures();
                    }
                    m_currentFilePath = s.originalFilename;  // "" → untitled
                    m_savedSnapshot.clear();                 // stays dirty until saved
                    SetDirty(true);
                    // Same replacement notification chokepoint as file/open. The
                    // actual recovery path is suppressed under --test-host, so
                    // sharing this call prevents its ordering from drifting.
                    ResetSelectionAndEmitDocumentChanged();
                    outcome = Autosave::RecoverOutcome::Recovered;
                }
            }
            else
            {
                // Load failed — keep the orphan (other tier / next launch can still
                // recover). The boot document is untouched.
                outcome = Autosave::RecoverOutcome::Failed;
                reason  = "load_error";
            }
        }

        // Consume (delete files + clear pending) ONLY on a successful recover or an
        // explicit discard — never on a failed load.
        if (Autosave::ShouldDeleteOrphan(outcome))
        {
            Autosave::DeleteOrphan(s);
            m_hasPendingOrphan = false;
            m_pendingOrphan = Autosave::OrphanSession{};
        }

        const char* status =
            outcome == Autosave::RecoverOutcome::Recovered ? "recovered" :
            outcome == Autosave::RecoverOutcome::Discarded ? "discarded" : "failed";
        json out = json{{"status", status}};
        if (!reason.empty()) out["reason"] = reason;
        ctx.SendOk(out);
        return true;
    }


    return false;   // kind not in this domain
}

} // namespace host
