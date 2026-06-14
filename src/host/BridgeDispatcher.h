// BridgeDispatcher — parses JSON wire messages from
// `chrome.webview.postMessage` (React → host), dispatches by `kind`,
// and emits JSON `res`/`evt` envelopes back to React. Single source of
// truth for the request/correlation-id machinery.
//
// The schema is mirrored from web/packages/bridge-schema/src/index.ts.
// Wire shapes:
//   request : { type: "req",  id, kind, params }
//   response: { type: "res",  id, ok: true, data }
//             { type: "res",  id, ok: false, error }
//   event   : { type: "evt",  kind, payload }
//
// Task 2.1 surface:
//   - layout/viewport-rect              → LayoutBroker::Apply(...)
//   - register-accelerators             → AcceleratorBridge::RegisterCombos
//   - engine/state/snapshot             → full EngineStateDto (every getter)
//   - engine/set/*           (17 of)    → setter + engine/state/changed event
//   - engine/action/*        (4 of)     → action + engine/state/changed event
//                                         (on-particle-system-changed skips
//                                         the event — engine re-renders next
//                                         frame anyway)
//   - engine/query/*         (3 of)     → IsGroundSlotEmpty / IsSkydomeSlotEmpty
//                                         / IsBloomAvailable
// Everything else (emitters/*, file/*, undo/*, spawner/*) still returns
// `{ ok: false, error: "not implemented yet (Phase 3+)" }`.
#ifndef HOST_BRIDGE_DISPATCHER_H
#define HOST_BRIDGE_DISPATCHER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "third_party/nlohmann/json.hpp"

//: Autosave::OrphanSession is held by value in the recovery stash.
#include "../Autosave.h"

class Engine;
class UndoStack;
class ParticleSystem;
class ParticleSystemInstance;
class SpawnerDriver;
class IFileManager;
class ModManager;

namespace host {

class AcceleratorBridge;
class LayoutBroker;
class InputDispatcher;

class BridgeDispatcher
{
public:
    using EmitFn = std::function<void(const std::string& json)>;

    // `useTestHost` mirrors HostWindow's `--test-host` flag. When true,
    // settings handlers that would otherwise touch the registry return
    // deterministic defaults / no-op their writes, so the a11y goldens
    // (e.g. dialog-lighting's Force Align checkbox) see ctor defaults
    // regardless of the dev box's saved registry — the same determinism
    // gate the HostWindow registry-restore block uses (see).
    BridgeDispatcher(Engine* engine, LayoutBroker& layout, AcceleratorBridge& accel,
                     EmitFn emit, bool useTestHost = false);

    // Sets / replaces the live Engine pointer. The host can install this
    // before or after the Engine is constructed; null is treated as
    // "engine not ready, snapshot requests return ok:false".
    // Body in BridgeDispatcher.cpp — Engine is only forward-declared here,
    // so the cached-config reapply (which calls m_engine->SetOverloadGuard)
    // cannot be inlined in this header.
    void SetEngine(Engine* engine);

    // Inject the UndoStack used to service `undo/perform` requests.
    // Stack is constructed by HostWindow alongside the Engine. Null is
    // treated as "no undo available, applied:false". Phase 3+ emitter
    // mutation handlers wrap each mutating Request in a captureUndo()
    // call PRE-mutation; `undo/perform` deserializes the recorded
    // ParticleSystem snapshot and swaps it into the host-owned slot via
    // ApplyUndoSnapshot below.
    void SetUndoStack(UndoStack* undo) { m_undo = undo; }

    // The HostWindow's top-level HWND. Required by file/open's
    // GetOpenFileNameW to parent the modal dialog. Null means the
    // picker will run unparented (works, but doesn't block input on the
    // main window — set this in HostWindow once hMain exists).
    void SetHostHwnd(HWND hwnd) { m_hostHwnd = hwnd; }

    // D6: inject the ModManager that owns mod discovery + active-
    // mod state. The dispatcher routes `mods/list`, `mods/select`, and
    // `mods/refresh` requests through it, and includes the active path
    // in the engine-state snapshot via `activeModPath`. Null is
    // tolerated (the three mods/* handlers will return ok:false; the
    // snapshot's activeModPath will be null), but production launches
    // bind this in HostWindow before the WebView2 navigates.
    void SetModManager(::ModManager* mm) { m_modManager = mm; }

    // Bind the editor's host-owned state pointers used by the
    // forward-deferred handlers activated in host-state plumbing
    // (file/new, file/open, file/save, file/save-as,
    // engine/action/rescale-system, spawner/start /trigger /stop,
    // emitters/preview-from-file).
    //
    // `ppSystem` is a pointer-to-pointer because file/new and file/open
    // replace the *current* unique_ptr<ParticleSystem> instance — the
    // dispatcher must always read through the host's slot, never cache
    // its own copy. Mirrors legacy `info->particleSystem`.
    //
    // `spawner` is a single owned instance whose config is mutated via
    // `SetConfig` and inspected via `GetConfig`. The host constructs
    // and tears it down; the dispatcher holds a borrow.
    //
    // `fileManager` is the same file manager that legacy main.cpp uses
    // (FileManager from createFileManager); passed in so future host-
    // routed loaders that need it (the EaW VFS lookup) can reuse it,
    // even though `LoadParticleSystem` / `SaveParticleSystem` go
    // through `PhysicalFile` directly today.
    //
    // Null pointers are tolerated — the affected handlers fall back to
    // a friendly ok:false envelope. Wired exactly once by HostWindow
    // after constructing the unique_ptrs in WM_CREATE.
    void BindHostState(std::unique_ptr<ParticleSystem>* ppSystem,
                       SpawnerDriver*                   spawner,
                       IFileManager*                    fileManager)
    {
        m_pParticleSystem = ppSystem;
        m_spawnerDriver   = spawner;
        m_fileManager     = fileManager;
    }

    // shift-click-to-spawn: the host owns a single attached
    // cursor-bound ParticleSystemInstance pointer (nulled when no Shift
    // is held). file/new + file/open need to kill any in-flight attached
    // instance before swapping `*m_pParticleSystem` — the old instance
    // can't outlive the system it was spawned from. Wired alongside
    // `BindHostState`.
    void BindAttachedSystem(ParticleSystemInstance** ppAttached)
    {
        m_ppAttachedParticleSystem = ppAttached;
    }

    // Capture the currently-bound ParticleSystem as the "saved" baseline
    // for the dirty-bit content-compare in ApplyUndoSnapshot. Called
    // once by HostWindow after BindHostState seeds the boot-state PS,
    // so Ctrl+Z back to the boot content clears dirty without requiring
    // a File → New first. Subsequent file/new + file/open + file/save
    // handlers re-seed via their own paths.
    void ResetSavedBaseline();

    // Phase 2: inject the InputDispatcher that owns the hidden
    // viewport popup HWND. The `viewport/input` request arm forwards
    // its params object to InputDispatcher::Dispatch. Null is tolerated
    // — that's the legacy-popup path where input flows directly from
    // the OS and the request should never fire. Wired once in
    // HostWindow when m_archCMode is true.
    void SetInputDispatcher(InputDispatcher* input) { m_input = input; }

    // Called from the WebView2 WebMessageReceived handler. The string is
    // the raw JSON sent by `chrome.webview.postMessage` on the React side.
    // Response (if any) is emitted asynchronously via m_emit.
    void Dispatch(const std::string& jsonRequest);

    // Synchronous variant used by HostBridgeProxy (the host-object IPC
    // channel that survives CDP attachment — see tasks/lessons.md).
    // Parses the request, runs the same kind-handler ladder as Dispatch,
    // and returns the serialised response envelope directly to the caller
    // instead of emitting it. Events (engine/state/changed etc.) still
    // flow through m_emit — those are pushes, independent of the
    // request/response round-trip.
    std::string DispatchSync(const std::string& jsonRequest);

    // Convenience emitters (host-driven). EmitStatsTick is the 4 Hz status
    // bar push; EmitEngineStateChanged is the post-setter broadcast.
    void EmitEngineStateChanged();

    // Commit the reference object's CURRENT transform after a
    // host-side manipulator drag: gated persist + markDirty + EmitEngineStateChanged.
    // The per-move drag already set the transform on the engine (+ emitted so the
    // spinners track live, no persist); this runs once on release so the gesture
    // persists + flips the dirty flag exactly once. Reads the live engine values.
    void CommitReferenceObjectTransform();
    void EmitStatsTick(float fps, int emitters, int particles, int instances, bool overload);

    // Phase 3 Screen 4 Batch B1 — emit `emitters/tree/changed` with the
    // live ParticleSystem's tree as payload. Called after each
    // mutation handler (duplicate / delete / rename / rescale /
    // link-group exempt-set edit) so the React EmitterTree re-fetches
    // via `emitters/list`.
    void EmitEmittersTreeChanged();

    // render loop: real spawner/active-count source. Called from
    // HostWindow::RenderD3D9 once per frame when Engine::GetNumInstances()
    // changes. Replaces the MockBridge-driven mock-only timer source —
    // SpawnerPanel's badge subscription is unchanged.
    void EmitSpawnerActiveCount(int count);

    // Phase 3 Screen 8 Batch 3 — editor-level file state.
    //
    // The dispatcher owns three pieces of state that don't belong on
    // Engine (Engine is engine parameters; these are editor state):
    //   - m_currentFilePath  : path to the .alo backing the in-memory
    //                          ParticleSystem; empty when untitled.
    //   - m_dirty            : true if any engine mutation has occurred
    //                          since the last file/new/open/save.
    //   - m_recentFiles      : registry-backed history list (max 9
    //                          entries), shared with legacy via
    //                          HKCU\Software\AloParticleEditor.
    //
    // SetDirty(true) is called at the end of every mutating handler
    // (engine/set/*, engine/action/clear, engine/action/rescale-system).
    // SetDirty(false) is called in file/new, file/open, file/save
    // success paths. Both transitions emit dirty/changed.
    void SetDirty(bool dirty);
    bool GetDirty() const { return m_dirty; }
    const std::wstring& GetCurrentFilePath() const { return m_currentFilePath; }

    // Emits an `accelerator/pressed` event to React with the matched combo
    // string (e.g. "Ctrl+S"). Called by HostWindow's AcceleratorKeyPressed
    // handler after AcceleratorBridge::TryDispatch returns true.
    void EmitAcceleratorPressed(const std::string& combo);

    // (Group A): push the 3D ground-plane intersection of the
    // viewport mouse cursor (world-space). Called from the viewport
    // popup's WM_MOUSEMOVE — throttled host-side to ~30 Hz so the
    // WebView2 message channel isn't saturated.
    void EmitCursorPosition3D(float x, float y, float z);

private:
    // Builds the response envelope for one parsed `req` envelope. Single
    // source of truth for the kind-string ladder — both the async
    // `Dispatch` (which emits the response via m_emit) and the sync
    // `DispatchSync` (which returns the serialised envelope to the
    // host-object caller) route through this.
    //
    // Returns the response envelope as JSON. If the request has no `id`,
    // the returned envelope still carries `id: null` so callers can
    // serialise it unambiguously.
    nlohmann::json DispatchInternal(const nlohmann::json& reqEnvelope);

    // Emits a `dirty/changed` event with the current m_dirty value.
    void EmitDirtyChanged();
    // Emits a `recent/changed` event with the current m_recentFiles
    // serialised as a JSON array of strings.
    void EmitRecentChanged();

    // Auto-cap-aware "can undo" check for the Edit menu enable-state.
    // UndoStack::CanUndo requires cursor>=2; this method accounts for
    // undo/perform's head-of-history auto-cap (which makes single-
    // mutation history undoable too) by reporting true when cursor==
    // depth AND depth>=1. See undo/perform's comment block for the
    // full design.
    bool ComputeCanUndo() const;

    // Deserialize a ParticleSystem snapshot from an UndoStack entry and
    // swap it into the host-owned slot. Mirrors legacy
    // `RestoreFromSnapshot` at src/main.cpp:916 — same teardown
    // ordering (kill attached → engine Clear → swap → OnPSChanged →
    // ReloadTextures), adapted for the new-UI host-state plumbing.
    // Selection scalar is restored from the snapshot's captured
    // selectedIndex; out-of-range maps to -1 (no selection). Wrapped
    // in UndoStack::BeginApplying/EndApplying so the swap doesn't
    // recursively trigger Capture(). Caller is responsible for emitting
    // engine/state/changed + emitters/tree/changed after this returns.
    void ApplyUndoSnapshot(const std::vector<char>& buf, size_t selIdx);

    // Walks the currently-bound ParticleSystem's emitters and,
    // for every positive linkGroup with exactly one member, demotes
    // that lone member's linkGroup to 0. Idempotent — a second call
    // produces no change. Matches the render-layer filter at
    // computeLinkGroupBrackets (web/.../link-group-colors.ts) which
    // hides single-member groups from the gutter, so data and view
    // agree end-to-end. No-op when the ParticleSystem isn't bound.
    // O(emitters): two passes (count + demote). Does NOT call
    // captureUndo or SetDirty — callers decide how the sweep
    // composes with their own undo/dirty semantics.
    void EnforceSingleMemberLinkGroups();

    Engine*            m_engine;
    // [guard-config] Last config applied via engine/set/overload-guard —
    // reapplied by SetEngine (see above).
    bool m_overloadGuardCached       = false;
    bool m_overloadGuardEnabled      = true;
    int  m_overloadGuardMaxParticles = 15'000;
    // [hard-guard] Last estimate pushed via engine/set/estimated-load —
    // cached + reapplied by SetEngine, mirroring the guard config.
    bool   m_estimatedLoadCached      = false;
    double m_estimatedLoadPerInstance = 0.0;
    LayoutBroker&      m_layout;
    AcceleratorBridge& m_accel;
    EmitFn             m_emit;

    // Mirrors HostWindow's `--test-host` flag — gates the registry-backed
    // settings handlers to deterministic defaults (see ctor comment +).
    bool               m_testHost = false;

    // Test seam: set from the ALO_SETTINGS_LIVE env var at construction.
    // When true it LIFTS the `--test-host` settings gate, so the
    // `settings/*` handlers hit the real registry even under `--test-host`
    // — letting a CDP test drive the genuine read/write round-trip without
    // disturbing the a11y harness (which never sets the env var, so its
    // plain `--test-host` launch stays deterministic). Read once; default
    // false.
    bool               m_settingsLive = false;

    // T9] When true, EmitStatsTick is a no-op and React's
    // StatusBar receives a stats/frozen-changed event so it clears
    // its local stats+cursor state and renders `—` placeholders.
    // Set via the `stats/set-frozen` bridge request. Used by a11y
    // spec beforeEach hooks to freeze the StatusBar's volatile
    // FPS / Emitters / Particles / cursor values for deterministic
    // UIA goldens; harmless in non-test mode (default false).
    bool               m_statsFrozen = false;
    UndoStack*         m_undo     = nullptr;
    HWND               m_hostHwnd = nullptr;
    ::ModManager*      m_modManager = nullptr;  // D6: mods/* surface
    InputDispatcher*   m_input      = nullptr;  // Phase 2: viewport/input

    // host-state plumbing — pointers borrowed from HostWindow.
    // `m_pParticleSystem` is a pointer-to-unique_ptr so file/new and
    // file/open can swap the owned instance under the host's feet
    // (mirrors legacy `info->particleSystem`). The other two are
    // single-instance borrows. All three are nullable; handlers check
    // before dereferencing and fall back to ok:false on absence.
    std::unique_ptr<ParticleSystem>* m_pParticleSystem = nullptr;
    SpawnerDriver*                   m_spawnerDriver   = nullptr;
    IFileManager*                    m_fileManager     = nullptr;

    // shift-click-to-spawn: pointer-to-pointer borrow of
    // HostWindowImpl::m_attachedParticleSystem so file/new + file/open
    // can drop the cursor-bound instance before its parent system goes
    // away. Engine pointer is already cached in m_engine.
    ParticleSystemInstance**         m_ppAttachedParticleSystem = nullptr;

    // Phase 3 Screen 8 Batch 3 — editor-level file state. Owned here
    // rather than on Engine because they're editor concerns (not
    // engine parameters). The snapshot builder reads both fields.
    std::wstring              m_currentFilePath;
    bool                      m_dirty = false;
    std::vector<std::wstring> m_recentFiles;

    // Serialized "last saved" state for dirty-bit recomputation on
    // undo / redo. Refreshed on file/new + file/open + file/save
    // success (the three points where the user's "saved" reference
    // moves). ApplyUndoSnapshot content-compares the restored
    // ParticleSystem against this buffer to clear the dirty bit when
    // the user undoes back to the saved state. Empty buffer means
    // "no saved baseline yet" (boot state); restores always stay
    // dirty in that case. Replaces the legacy
    // UndoStack::MarkSaved/IsAtSavedState pair, which doesn't fit
    // the new-UI's PRE-mutation captureUndo convention — the cursor
    // after undo lands on the pre-mutation snapshot, not the
    // post-save snapshot, so the legacy isSavedState flag misses.
    std::vector<char>         m_savedSnapshot;

    // autosave crash-recovery. `autosave/check-recovery` scans
    // %TEMP%\AloParticleEditor\ for an orphan from a crashed prior session
    // and stashes it here so `autosave/recover` can consume its temp paths
    // without a re-scan; recover clears the stash (and DeleteOrphan's the
    // files) regardless of the user's choice. `m_hasPendingOrphan` guards
    // an empty stash (recover with no prior check, or a double recover).
    Autosave::OrphanSession   m_pendingOrphan{};
    bool                      m_hasPendingOrphan = false;

    // Phase 3 Screen 8 Batch 4 — spawner config cache for snapshot
    // parity. The host doesn't yet own a SpawnerDriver* (matches Batch 3
    // for ParticleSystem*); spawner/start handlers cache the incoming
    // params here so a subsequent engine/state/snapshot returns the
    // user's last-committed config. JSON-shaped to avoid pulling
    // SpawnerDriver.h into the dispatcher header.
    nlohmann::json m_spawnerConfig;

    // Phase 3 Screen 4 Batch A — selected-emitter id (editor state, not
    // engine state). -1 means "no selection"; the snapshot serialises
    // that as JSON null. `emitters/select` writes this directly; the
    // snapshot builder reads it. Kept on the dispatcher (not plumbed
    // through BindHostState) because selection is a UI concern the host
    // window doesn't otherwise need.
    int m_selectedEmitterId = -1;

    // Phase 3 Screen 4 Batch C — process-local emitter clipboard. One
    // buffer per copied subtree, serialised via the same
    // `MemoryFile` + `Emitter::write(writer, copy=true)` pattern as
    // import-from-file (BridgeDispatcher.cpp:1607). `emitters/
    // copy` and `emitters/cut` replace the entire vector each call;
    // `emitters/paste` reads it back. Empty vector = "clipboard is
    // empty"; paste in that case is a silent no-op. Doesn't span
    // process boundaries (matches legacy CF_PARTICLE_EMITTER, which
    // is Win32-clipboard-bound but in practice nothing else
    // recognises that format).
    std::vector<std::vector<uint8_t>> m_emitterClipboard;
};

} // namespace host

#endif // HOST_BRIDGE_DISPATCHER_H
