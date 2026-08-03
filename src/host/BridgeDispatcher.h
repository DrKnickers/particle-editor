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
// All domains (engine/*, emitters/*, linkGroups/*, file/*, undo/*,
// autosave/*, textures/*, mods/*, window/layout/viewport/*, spawner/*,
// settings/*) are implemented in per-domain TUs (BridgeDispatch_*.cpp);
// only an unmatched kind returns `{ ok: false, error: "not implemented yet" }`.
#ifndef HOST_BRIDGE_DISPATCHER_H
#define HOST_BRIDGE_DISPATCHER_H

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "third_party/nlohmann/json.hpp"

// Autosave::OrphanSession is held by value in the recovery stash.
#include "../Autosave.h"
#include "../UndoStack.h"   // full def needed for UndoStack::EditorAux in ApplyUndoSnapshot
#include "../ParticleSystem.h"   // full def needed for ParticleSystem::Emitter* in getEmitterById
#include "../MouseCursor.h"       // by-value record-preview anchor (preview/* record kinds)
#include "PreviewEncodeWorker.h"   // [C3] async texture-preview encode

class Engine;
class ParticleSystemInstance;
class SpawnerDriver;
class IFileManager;
class ModManager;

namespace host {

class AcceleratorBridge;
class LayoutBroker;
class InputDispatcher;
struct BridgeRequestContext;   // BridgeRequestContext.h (Phase A dispatch split)

class BridgeDispatcher
{
public:
    using EmitFn = std::function<void(const std::string& json)>;

    // `useTestHost` mirrors HostWindow's `--test-host` flag. When true,
    // settings handlers that would otherwise touch the registry return
    // deterministic defaults / no-op their writes, so the a11y goldens
    // (e.g. dialog-lighting's Force Align checkbox) see ctor defaults
    // regardless of the dev box's saved registry — the same determinism
    // gate the HostWindow registry-restore block uses.
    // `ephemeral` (true in --drive mode): suppress ALL registry writes the same
    // way `useTestHost` does, WITHOUT enabling the test-host-only behaviors
    // (CDP port, a11y determinism). It ORs into every persist gate; the three
    // WriteRecentFile (MRU) sites gate on PersistsUserState(), which folds this
    // flag together with the test-host settings gate.
    BridgeDispatcher(Engine* engine, LayoutBroker& layout, AcceleratorBridge& accel,
                     EmitFn emit, bool useTestHost = false, bool ephemeral = false);

    // Sets / replaces the live Engine pointer. The host can install this
    // before or after the Engine is constructed; null is treated as
    // "engine not ready, snapshot requests return ok:false".
    // Body in BridgeDispatcher.cpp — Engine is only forward-declared here,
    // so the cached-config reapply (which calls m_engine->SetOverloadGuard)
    // cannot be inlined in this header.
    void SetEngine(Engine* engine);

    // Inject the UndoStack used to service `undo/perform` requests.
    // Stack is constructed by HostWindow alongside the Engine. Null is
    // treated as "no undo available, applied:false". Emitter
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

    // [C3] Async texture-preview pipeline. The get-preview handler serves LRU
    // hits synchronously; a miss decodes on the UI thread (device-bound) and
    // hands the raw pixels to PreviewEncodeWorker, answering {status:pending}.
    // The worker PostMessages WM_APP_PREVIEW_READY; the wndproc calls
    // DrainPreviewResults, which caches the finished dataUri and emits
    // `textures/preview-ready` so the web refetches (a cache hit).
    // ShutdownPreviewWorker joins the worker — MUST run before
    // GdiplusShutdown (the worker encodes via GDI+).
    void DrainPreviewResults();
    void ShutdownPreviewWorker() { if (m_previewWorker) m_previewWorker->Finish(); }
    // Enable the record-only emit throttle (issue #510). Set true ONLY for a
    // --record run (never --drive). See m_recordEmitThrottle.
    void SetRecordEmitThrottle(bool on) { m_recordEmitThrottle = on; }

    // inject the ModManager that owns mod discovery + active-
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

    // Seed the initial emitter selection (editor state, an index into
    // getEmitters()). Used by HostWindow at boot to select the seeded root
    // emitter so the editor opens on a populated Inspector/curve panel —
    // legacy parity: DoNewFile started with the default emitter SELECTED.
    // No event is emitted (React reads it from the boot snapshot on mount).
    void SetSelectedEmitterId(int id) { m_selectedEmitterId = id; }

    // shift-click-to-spawn: the host owns a single attached cursor-bound
    // ParticleSystemInstance handle (empty when no Shift is held).
    // file/new + file/open need to kill any in-flight attached
    // instance before swapping `*m_pParticleSystem` — the old instance
    // can't outlive the system it was spawned from. Wired alongside
    // `BindHostState`.
    void BindAttachedSystem(ParticleSystemInstanceHandle* pAttached)
    {
        m_pAttachedParticleSystem = pAttached;
    }

    // Capture the currently-bound ParticleSystem as the "saved" baseline
    // for the dirty-bit content-compare in ApplyUndoSnapshot. Called
    // once by HostWindow after BindHostState seeds the boot-state PS,
    // so Ctrl+Z back to the boot content clears dirty without requiring
    // a File → New first. Subsequent file/new + file/open + file/save
    // handlers re-seed via their own paths.
    void ResetSavedBaseline();

    // Inject the InputDispatcher that owns the hidden
    // viewport popup HWND. The `viewport/input` request arm forwards
    // its params object to InputDispatcher::Dispatch. Null is tolerated
    // (the request just no-ops). Wired once in HostWindow at startup.
    void SetInputDispatcher(InputDispatcher* input) { m_input = input; }

    // Called from the WebView2 WebMessageReceived handler. The string is
    // the raw JSON sent by `chrome.webview.postMessage` on the React side.
    // Response (if any) is emitted asynchronously via m_emit.
    void Dispatch(const std::string& jsonRequest);

    // Synchronous variant used by HostBridgeProxy (the host-object IPC
    // channel that survives CDP attachment).
    // Parses the request, runs the same kind-handler ladder as Dispatch,
    // and returns the serialised response envelope directly to the caller
    // instead of emitting it. Events (engine/state/changed etc.) still
    // flow through m_emit — those are pushes, independent of the
    // request/response round-trip.
    std::string DispatchSync(const std::string& jsonRequest);

    // Convenience emitters (host-driven). EmitStatsTick is the 4 Hz status
    // bar push; EmitEngineStateChanged is the post-setter broadcast.
    void EmitEngineStateChanged();

    // Deliver any live-coalesced trailing broadcast (see the
    // m_stateEmitPending block for the full contract). Called from the
    // paced idle branch (once per display frame), the top of
    // Dispatch/DispatchSync (a pending evt must land BEFORE the next
    // response), and the 4 Hz stats timer (heartbeat through modal
    // dialogs' own message pumps). No-op when nothing is pending.
    void FlushPendingEmits();

    // Commit the reference object's CURRENT transform after a
    // host-side manipulator drag: gated persist + markDirty + EmitEngineStateChanged.
    // The per-move drag already set the transform on the engine (+ emitted so the
    // spinners track live, no persist); this runs once on release so the gesture
    // persists + flips the dirty flag exactly once. Reads the live engine values.
    void CommitReferenceObjectTransform();

    // Push ONE pre-mutation undo point for a reference-object
    // transform change driven from the host (the gizmo). Thin wrapper over
    // CaptureUndoPoint(0) — a gizmo gesture never coalesces. Called from
    // HostWindow on the FIRST per-move mutation of a drag (so a grab without
    // a drag captures nothing). The capture reads the engine's CURRENT
    // transform, which at that first move is still the pre-drag value.
    void CaptureReferenceTransformUndoPoint();
    void EmitStatsTick(float fps, int emitters, int particles, int instances, bool overload);

    // Emit `emitters/tree/changed` with the
    // live ParticleSystem's tree as payload. Called after each
    // mutation handler (duplicate / delete / rename / rescale /
    // link-group exempt-set edit) so the React EmitterTree re-fetches
    // via `emitters/list`.
    void EmitEmittersTreeChanged();

    // Reset the selection to the new document's root (or none, if it has no
    // emitters) and announce it, so React's selection atom — and thus the
    // Inspector and curve panel — follow the swap.
    // The scalar is set before an atomic state -> tree -> selection stream;
    // document replacement bypasses edit coalescing and drops pending
    // old-document broadcasts.
    //
    // file/new establishes the same order inline. file/open and
    // autosave-recover share this helper so their replacement choreography
    // cannot drift and expose the previous document's positional id.
    void ResetSelectionAndEmitDocumentChanged();

    // render loop: real spawner/active-count source. Called from
    // HostWindow::RenderD3D9 once per frame when Engine::GetNumInstances()
    // changes. Replaces the MockBridge-driven mock-only timer source —
    // SpawnerPanel's badge subscription is unchanged.
    void EmitSpawnerActiveCount(int count);
    // Frameless title bar: emit so the web TitleBar swaps the maximize↔restore
    // glyph. Returns false if the web isn't wired yet (m_emit null) so the host
    // can replay the state once React is ready (see HostWindowImpl).
    bool EmitWindowState(bool maximized);
    // Autosave health (2026-07 audit, P2-06). Returns false when the web isn't
    // wired yet, so the caller can replay on app/ready — same contract as
    // EmitWindowState above.
    bool EmitAutosaveHealth(bool healthy);

    // Editor-level file state.
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

    // Push the 3D ground-plane intersection of the
    // viewport mouse cursor (world-space). Called from the viewport
    // popup's WM_MOUSEMOVE — throttled host-side to ~30 Hz so the
    // WebView2 message channel isn't saturated.
    void EmitCursorPosition3D(float x, float y, float z);

    void EmitManipulatorDrag(const nlohmann::json& payload);   // readout pill

    // Native frame-X on a dirty doc emits `app/close-requested` so
    // React pops the same Save/Discard/Cancel prompt File→Exit uses (the host
    // can't render the React prompt itself). Called from HostWindow's wndproc.
    void EmitCloseRequested();

private:
    // Deterministic construction seam for the preview-queue production-call
    // oracle. Always declared (never macro-selected), so production and test
    // compile the same class layout. Unlike the normal constructor, this skips
    // registry/config initialization that is irrelevant to PreviewCacheClear.
    struct PreviewQueueTestTag {};
    BridgeDispatcher(LayoutBroker& layout, AcceleratorBridge& accel,
                     PreviewQueueTestTag)
        : m_engine(nullptr), m_layout(layout), m_accel(accel), m_emit()
    {
    }
    friend struct PreviewQueueWiringTestAccess;

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

    // ---- Per-domain kind dispatchers (Phase A split) ----
    // Each lives in its own TU (BridgeDispatch_<Domain>.cpp), holds that
    // domain's `kind ==` handlers moved out of DispatchInternal's ladder,
    // and returns true when it handled `kind` (response written via ctx).
    // DispatchInternal calls all six in sequence; kinds are exact-match and
    // mutually exclusive, so call order carries no semantics.
    friend struct BridgeRequestContext;   // RequireEngine/MarkDirty need privates
    bool TryDispatchEngine  (BridgeRequestContext& ctx, const std::string& kind);
    bool TryDispatchEmitters(BridgeRequestContext& ctx, const std::string& kind);  // + linkGroups/*
    bool TryDispatchFile    (BridgeRequestContext& ctx, const std::string& kind);  // + autosave/undo
    bool TryDispatchAssets  (BridgeRequestContext& ctx, const std::string& kind);  // textures/mods
    bool TryDispatchShell   (BridgeRequestContext& ctx, const std::string& kind);  // window/layout/viewport/host/app/debug/stats/…
    bool TryDispatchSpawner (BridgeRequestContext& ctx, const std::string& kind);  // + settings

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
    void ApplyUndoSnapshot(const std::vector<char>& buf, size_t selIdx,
                           const UndoStack::EditorAux& aux);

    // Single source of truth for a PRE-mutation undo capture:
    // snapshots the live ParticleSystem + selection (as the captureUndo
    // lambda did) PLUS the engine's current reference-object transform into
    // the snapshot's side-band aux. The engine read is guarded — m_engine is
    // null when D3D init fails — so a particle edit on an engine-less host
    // still captures (aux stays {0,0,0}) instead of dereferencing null.
    // Preserves both capture paths: coalesceKey!=0 -> CapturePreCoalesced,
    // else -> Capture, matching the lambda it replaces.
    void CaptureUndoPoint(
        DWORD coalesceKey,
        UndoStack::BudgetRetention retention = UndoStack::BudgetRetention::Normal);

    // ---- Promoted DispatchInternal helpers ----
    // Formerly lambdas defined mid-ladder inside DispatchInternal; promoted to
    // members so kind handlers can move into per-domain translation units
    // (BridgeDispatch_*.cpp). Names keep the lambda camelCase so the dozens of
    // ladder call sites are untouched by the promotion.

    // Lookup helper: emitter pointer by integer index. Returns nullptr on
    // out-of-range / no-system / null-slot.
    ParticleSystem::Emitter* getEmitterById(int id);
    // Capture the selected emitter's process-stable identity before a
    // structural deletion, then resolve the new positional wire id afterward.
    // A missing survivor clears selection; an unchanged position emits nothing.
    unsigned int selectedEmitterStableId() const;
    void reconcileSelectionAfterDeletion(unsigned int stableId);
    // PRE-mutation undo capture; forwards to CaptureUndoPoint (which folds in
    // the engine's ref-transform aux). coalesceKey 0 = never coalesce
    // (structural ops must never fold across an add/delete/move).
    void captureUndo(DWORD coalesceKey = 0);
    // F4 link-group propagation: after a shared (non-exempt) field edit on a
    // linked emitter, copy non-exempt params to every group sibling. MUST run
    // AFTER the mutation; reseats all live cursor iterators via
    // Engine::OnParticleSystemChanged(-1) at this single choke point.
    void propagateLinkGroup(ParticleSystem::Emitter* edited);

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
    // settings handlers to deterministic defaults (see ctor comment).
    bool               m_testHost = false;

    // --drive ephemeral mode: ORs into every persist gate + guards the MRU
    // writes so a --drive run performs NO registry writes (without enabling the
    // test-host-only CDP/a11y behaviors). Set once at construction.
    bool               m_ephemeral = false;

    // True when this run may write user-visible persistent state (the
    // recent-files MRU): not drive/record-ephemeral, and not a --test-host run
    // unless ALO_SETTINGS_LIVE lifted the gate — the SAME predicate every
    // settings/* write uses (cf. BridgeDispatch_Assets.cpp mods/set-layers).
    // Before this existed the MRU sites checked only m_ephemeral, so every
    // playwright-native (--test-host) file open/save wrote its test path into
    // the daily driver's real Recent Files menu (2026-07 audit follow-up).
    bool PersistsUserState() const { return !m_ephemeral && !(m_testHost && !m_settingsLive); }

    // --record throttle (issue #510): during a clip record the driver scrubs
    // curves host-side, firing emitters/tree/changed + engine/state/changed far
    // faster than the capture rate. Each push makes the web re-fetch the whole
    // tree/props/tracks/mods, which saturates the web thread and starves the
    // per-frame capture/ack protocol -> 0 frames. When enabled, coalesce those two
    // broadcasts to ~kRecordEmitThrottleMs (leading-edge). RECORD-ONLY: never for
    // --drive (a drive-assert reads state via DriveRunner and must see every
    // change). Panels still update at ~30 Hz — visually identical for a <=60fps
    // clip, and the record's end-hold captures the settled state.
    bool               m_recordEmitThrottle = false;
    unsigned long long m_lastTreeEmitTick   = 0;
    unsigned long long m_lastStateEmitTick  = 0;
    static constexpr unsigned long long kRecordEmitThrottleMs = 33;

    // Live-mode trailing coalesce (perf remediation B1): interactive drags
    // call the two broadcasts at pointer-move rate, and each emit serializes
    // a full engine-state snapshot / the whole emitter tree. The FIRST emit
    // in a burst still goes out immediately (leading edge — spinners track
    // the drag live); a follow-up within kEmitCoalesceMs only marks PENDING,
    // delivered by FlushPendingEmits() (call sites documented on its decl).
    // Worst-case staleness is one display frame (idle-branch flush), or one
    // stats tick (250 ms) inside a modal dialog's own pump. NOT active in
    // --record (#510's leading-edge throttle owns that mode — its clip gates
    // assert the ~30 Hz cadence) and NOT in --drive (m_ephemeral: a
    // drive-assert must see every change, per the #510 note above).
    bool               m_stateEmitPending = false;
    bool               m_treeEmitPending  = false;
    static constexpr unsigned long long kEmitCoalesceMs = 16;

    // Unconditional builders behind the coalesce gates (the pre-B1 bodies).
    void EmitEngineStateChangedNow();
    void EmitEmittersTreeChangedNow();

    // Test seam: set from the ALO_SETTINGS_LIVE env var at construction.
    // When true it LIFTS the `--test-host` settings gate, so the
    // `settings/*` handlers hit the real registry even under `--test-host`
    // — letting a CDP test drive the genuine read/write round-trip without
    // disturbing the a11y harness (which never sets the env var, so its
    // plain `--test-host` launch stays deterministic). Read once; default
    // false.
    bool               m_settingsLive = false;

    // When true, EmitStatsTick is a no-op and React's
    // StatusBar receives a stats/frozen-changed event so it clears
    // its local stats+cursor state and renders `—` placeholders.
    // Set via the `stats/set-frozen` bridge request. Used by a11y
    // spec beforeEach hooks to freeze the StatusBar's volatile
    // FPS / Emitters / Particles / cursor values for deterministic
    // UIA goldens; harmless in non-test mode (default false).
    bool               m_statsFrozen = false;
    UndoStack*         m_undo     = nullptr;
    HWND               m_hostHwnd = nullptr;
    ::ModManager*      m_modManager = nullptr;  // mods/* surface
    InputDispatcher*   m_input      = nullptr;  // viewport/input

    // [C3] Preview LRU + in-flight dedupe + epoch (see DrainPreviewResults).
    // Key = "<filename>|<0/1 flattenAlpha>" (maxBound is a handler constant).
    // Epoch bumps on mod-stack changes (next to ClearBridgeThumbCache) so a
    // result encoded pre-switch can never populate the post-switch cache.
    struct PreviewCacheEntry {
        std::string status, dataUri;
        int srcW = 0, srcH = 0;
    };
    std::list<std::pair<std::string, PreviewCacheEntry>> m_previewLru;  // front = MRU
    std::map<std::string,
             std::list<std::pair<std::string, PreviewCacheEntry>>::iterator>
                        m_previewLruIdx;
    std::set<std::string> m_previewInFlight;
    unsigned            m_previewEpoch = 0;
    static constexpr size_t kPreviewLruCap = 64;
    std::unique_ptr<host::PreviewEncodeWorker> m_previewWorker;
    void PreviewCachePut(const std::string& key, PreviewCacheEntry entry);
    // [C3] Mod-stack change: drop everything + bump the epoch so in-flight
    // worker results (old stack's pixels) are discarded on arrival.
    size_t PreviewCacheClear()
    {
        m_previewLru.clear();
        m_previewLruIdx.clear();
        m_previewInFlight.clear();
        ++m_previewEpoch;
        // m_previewInFlight is what BOUNDS the encode queue -- a texture
        // already queued is never queued twice. Clearing it above without also
        // clearing the queue removed that bound at the one moment the queue was
        // about to grow: the palette re-requests everything, each key now
        // misses both the LRU and the dedupe gate, and the previous epoch's jobs
        // stay queued at up to 4 MB of raw BGRA each (2026-07 audit, P2-03).
        // Their results would be discarded on arrival anyway, so dropping them
        // here costs nothing and is the only thing keeping the two structures
        // in step.
        return m_previewWorker
            ? m_previewWorker->DropStaleQueued(m_previewEpoch)
            : 0;
    }

    // host-state plumbing — pointers borrowed from HostWindow.
    // `m_pParticleSystem` is a pointer-to-unique_ptr so file/new and
    // file/open can swap the owned instance under the host's feet
    // (mirrors legacy `info->particleSystem`). The other two are
    // single-instance borrows. All three are nullable; handlers check
    // before dereferencing and fall back to ok:false on absence.
    std::unique_ptr<ParticleSystem>* m_pParticleSystem = nullptr;
    SpawnerDriver*                   m_spawnerDriver   = nullptr;
    IFileManager*                    m_fileManager     = nullptr;

    // shift-click-to-spawn: borrow of
    // HostWindowImpl::m_attachedParticleSystem so file/new + file/open
    // can drop the cursor-bound instance before its parent system goes
    // away. Engine pointer is already cached in m_engine.
    ParticleSystemInstanceHandle*    m_pAttachedParticleSystem = nullptr;

    // [record-preview] Cursor-bound preview instance for the preview/*
    // record kinds (BridgeDispatch_Spawner.cpp) — the scripted mirror of
    // the native Shift-hover spawn. The anchor is a dispatcher-lifetime
    // Object3D (instances parented to it never outlive it); the tokenized
    // handle is cleared by preview/place (detach) and preview/kill.
    MouseCursor                      m_recordPreviewCursor;
    ParticleSystemInstanceHandle     m_recordPreviewAttached;

    // Editor-level file state. Owned here
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
    // %TEMP%\AloParticleEditor\ for an orphan from a crashed prior process
    // session (PID + process-creation FILETIME)
    // and stashes it here so `autosave/recover` can consume its temp paths
    // without a re-scan. recover consumes the stash (clear + DeleteOrphan the
    // files) ONLY on a successful recover or an explicit discard; a FAILED load
    // leaves the stash intact so the other tier / next launch can still recover
    // (release-audit #3). `m_hasPendingOrphan` guards an empty stash (recover
    // with no prior check, or a double recover).
    Autosave::OrphanSession   m_pendingOrphan{};
    bool                      m_hasPendingOrphan = false;

    // Spawner config cache for snapshot
    // parity. The host doesn't yet own a SpawnerDriver* (matches the
    // ParticleSystem* situation); spawner/start handlers cache the incoming
    // params here so a subsequent engine/state/snapshot returns the
    // user's last-committed config. JSON-shaped to avoid pulling
    // SpawnerDriver.h into the dispatcher header.
    nlohmann::json m_spawnerConfig;

    // Selected-emitter id (editor state, not
    // engine state). -1 means "no selection"; the snapshot serialises
    // that as JSON null. `emitters/select` writes this directly; the
    // snapshot builder reads it. Kept on the dispatcher (not plumbed
    // through BindHostState) because selection is a UI concern the host
    // window doesn't otherwise need.
    int m_selectedEmitterId = -1;

    // Process-local emitter clipboard. One
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
