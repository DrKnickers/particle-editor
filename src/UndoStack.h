#ifndef UNDO_STACK_H
#define UNDO_STACK_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <deque>
#include <vector>
#include <string>
#include <cstddef>

class ParticleSystem;

// Whole-system snapshot stack. Each entry is the byte-buffer produced
// by ParticleSystem::write into a MemoryFile, plus the
// selected-emitter index at capture time. Restore deserializes via
// ParticleSystem(IFile*) and the caller swaps the new system into the
// editor.
//
// Why snapshots and not a command pattern: the surface of editable
// fields is huge (every spinner / checkbox / combo on three property
// tabs, every track key, every random-parameter group, plus structural
// ops). The save/load round-trip is already battle-tested, .alo files
// are tiny (<100KB typical), and snapshot-and-swap sidesteps the
// hardest part of the command approach — re-creating an Emitter*
// after a delete-undo with the right pointer-equality for live
// EmitterInstance references. Engine::Clear() tears down all live
// instances on every restore, so the new graph starts clean.
class UndoStack
{
public:
    // Side-band editor state that travels with a snapshot but is
    // NOT part of the ParticleSystem buffer: the reference-object transform
    // (position + rotation). It persists to the registry, not the .alo, so it
    // can't live in the serialized buffer. Carried on EVERY entry so undoing a
    // particle edit restores the ref transform it captured — i.e. leaves the
    // reference object where it is. Plain floats keep this header D3D-free.
    //
    // refName is the reference object SHOWN at capture. Each object now keeps its
    // own transform (per-object memory), so the restore is gated on it: undoing a
    // particle edit re-applies the captured transform only when the SAME object is
    // still shown -- otherwise a transform captured for object A would teleport an
    // unrelated object B that the user swapped to. Empty = no object.
    struct EditorAux
    {
        float       refPos[3] = {0.0f, 0.0f, 0.0f};
        float       refRot[3] = {0.0f, 0.0f, 0.0f};
        std::string refName;
    };

    // Coalescing: if a new capture's coalesceKey matches the previous
    // entry's AND the new timestamp is within COALESCE_WINDOW_MS, the
    // previous entry's snapshot is replaced in place rather than a new
    // entry being pushed. Keeps a 100-tick spinner drag from filling
    // the stack with intermediate states.
    //
    // Pass coalesceKey == 0 to disable coalescing for this capture
    // (used for structural ops, where collapsing across an
    // add/delete/move is wrong).
    //
    // 1500ms is generous enough to fold "edit field A, click into
    // field B, edit it" on the same emitter into one undo step, which
    // matches how users describe an "edit session" on a property
    // panel. The previous 750ms felt twitchy — switching from a text
    // field to a spinner exceeded it.
    static const DWORD COALESCE_WINDOW_MS = 1500;
    static const size_t MAX_ENTRIES = 100;
    // Aggregate byte budget, the companion to the entry cap above (2026-07
    // audit, an-audit-finding). MAX_ENTRIES bounds HOW MANY snapshots are resident and
    // says nothing about their size: a typical snapshot is well under 100 KB
    // (~10 MB for a full stack), but snapshot size scales with emitter and
    // track-key count, both of which run to five figures under the .alo caps —
    // so a pathological system has no ceiling at all. 256 MB is far above any
    // realistic stack, so normal editing never reaches it and behaviour there
    // is unchanged; it only truncates history that would otherwise be unbounded.
    static const size_t MAX_TOTAL_BYTES = 256u * 1024u * 1024u;

    // Byte-budget eviction normally retains only the newest snapshot when one
    // entry alone exceeds the budget. The immediate-pair policy is a narrow
    // exception for undo/perform's PRE + LIVE pair: retaining both is the only
    // way the first Undo can still reach the PRE state.
    //
    // This is cardinality-based, not a doubled-byte hard cap. Two serialized
    // snapshots may retain more than 2 * MAX_TOTAL_BYTES when either snapshot
    // is itself unusually large.
    enum class BudgetRetention
    {
        Normal,
        PreserveImmediatePair,
    };

    // maxTotalBytes defaults to MAX_TOTAL_BYTES; production never passes it.
    // It is a parameter so the eviction path can be exercised at a threshold a
    // test can actually reach -- constructing 256 MB of real snapshots costs
    // ~400 MB resident, which is a poor trade for a cap whose whole design
    // intent is never to fire in normal use.
    explicit UndoStack(size_t maxTotalBytes = MAX_TOTAL_BYTES);
    ~UndoStack();

    // Snapshot the current state. selectedIndex is the index into
    // sys.getEmitters() of the currently-selected emitter (or
    // SIZE_MAX if no selection). Returns true if a new entry was
    // pushed; false if the call coalesced into an existing entry or
    // was suppressed by the m_applying guard.
    bool Capture(const ParticleSystem& sys, size_t selectedIndex,
                 DWORD coalesceKey, const EditorAux& aux = {},
                 BudgetRetention retention = BudgetRetention::Normal);

    // PRE-mutation coalescing variant for the bridge. Callers
    // snapshot BEFORE applying the edit, so the FIRST capture of a rapid
    // burst already holds the correct undo target (the session-start
    // state). When the previous entry shares coalesceKey within the time
    // window AND we are at the head of history, SKIP the capture entirely —
    // keeping that session-start snapshot — so the whole burst (e.g. a
    // scroll-wheel spinning a spinner, or a held arrow) collapses into a
    // single undo step. Otherwise pushes a new entry. coalesceKey == 0
    // disables coalescing (always pushes).
    //
    // Contrast Capture(), whose coalesce branch REPLACES the tail snapshot
    // with the current state — correct for legacy's POST-mutation capture
    // (the tail must track the LATEST state), but wrong for PRE-mutation
    // callers (it would overwrite the session-start state we undo back to).
    bool CapturePreCoalesced(const ParticleSystem& sys, size_t selectedIndex,
                             DWORD coalesceKey, const EditorAux& aux = {});

    bool CanUndo() const;
    bool CanRedo() const;

    // Move cursor backward / forward. On success, fills outSnapshot
    // with a pointer to the buffer to restore from (owned by this
    // stack — copy or use immediately) and outSelectedIndex with the
    // selection-index at capture time.
    // outAux is defaulted to nullptr so callers that don't need the
    // EditorAux side-channel can call Undo/Redo with two args.
    bool Undo(const std::vector<char>** outSnapshot,
              size_t* outSelectedIndex, EditorAux* outAux = nullptr);
    bool Redo(const std::vector<char>** outSnapshot,
              size_t* outSelectedIndex, EditorAux* outAux = nullptr);

    // Clear all entries (used on file open / new).
    void Clear();

    // Mark the entry at the current cursor as "matches what's on
    // disk". Used to drive the title-bar asterisk: after restore, the
    // file is "modified" iff the current entry isn't the saved one.
    // Clears the saved-bit on every other entry.
    void MarkSaved();
    bool IsAtSavedState() const;

    // Re-entrancy guard. EmitterProps_SetEmitter and
    // EmitterList_SetParticleSystem may dispatch EP_CHANGE /
    // ELN_LISTCHANGED notifications while we're applying a restore.
    // Capture() short-circuits when this is true.
    bool IsApplying() const { return m_applying; }
    void BeginApplying() { m_applying = true; }
    void EndApplying()   { m_applying = false; }

    // For the menu / toolbar enable-state queries.
    size_t Depth()  const { return m_entries.size(); }
    size_t Cursor() const { return m_cursor; }

    // True when the live ParticleSystem holds an un-snapshotted edit
    // sitting one step AHEAD of the stack tip (entries[cursor-1]).
    // Set by Capture() (every editing capture is immediately followed by
    // a mutation, so live becomes skewed ahead), cleared by Undo()/Redo()
    // (navigation re-syncs live to the entry it just restored).
    //
    // The new-UI captures PRE-mutation, so after a fresh edit cursor ==
    // Depth() AND live is skewed — undo/perform's head-of-history
    // auto-capture relies on that to snapshot live before stepping back.
    // But cursor == Depth() is ALSO true right after a Redo() (redo to
    // the tip leaves cursor == size) where live is already IN SYNC. An
    // auto-cap there is spurious: it duplicates the tip and the following
    // Undo() returns that duplicate, silently swallowing the undo. Gate
    // the auto-cap (and ComputeCanUndo) on this flag to tell the two
    // cursor==Depth() states apart.
    bool IsLiveAhead() const { return m_liveAhead; }

    // Compose a coalesce key from a notification code and a
    // sub-discriminator (typically the selected emitter index, or for
    // TE_CHANGE the (track << 16 | emitterIdx) combo). Two captures
    // with the same key collapse; structural ops should pass key 0.
    static DWORD MakeCoalesceKey(WORD notifyCode, WORD discriminator);

    // Snapshot helpers — public so callers can serialize directly when
    // they hold a ParticleSystem and want a buffer (currently only
    // used internally, but symmetric with Deserialize).
    static std::vector<char> Serialize(const ParticleSystem& sys);
    static ParticleSystem*   Deserialize(const std::vector<char>& buf);

private:
    struct Entry
    {
        std::vector<char> snapshot;
        size_t            selectedIndex;
        DWORD             coalesceKey;
        DWORD             timestamp;
        bool              isSavedState;
        EditorAux         aux;   // ref-object transform at capture
    };

    std::deque<Entry> m_entries;
    // m_cursor points at the entry representing "current state".
    // Undo decrements; redo increments. New captures at cursor==N
    // truncate any redo branch above N before pushing.
    size_t            m_cursor;
    bool              m_applying;
    // See IsLiveAhead(): tracks whether live is skewed ahead of the tip.
    bool              m_liveAhead;
    // Aggregate snapshot budget for this stack; MAX_TOTAL_BYTES unless a
    // test-host override changed it while the stack was empty.
    size_t            m_maxTotalBytes;

    // Evict from the front until BOTH caps hold. Called from the two push
    // paths (Capture and CapturePreCoalesced) so they can't drift apart --
    // the entry cap was previously duplicated inline in both.
    //
    // minEntriesToKeep is normally one. undo/perform's LIVE auto-capture may
    // request two so the immediately preceding PRE state remains reachable.
    void EvictToBudget(size_t minEntriesToKeep);

public:
    // Resident snapshot bytes, for tests and diagnostics.
    size_t TotalBytes() const;
    size_t MaxTotalBytes() const { return m_maxTotalBytes; }

    // Native-test seam. Reconfiguration is safe only before the stack has any
    // history and while live is synchronized; production never calls this.
    bool SetMaxTotalBytesForTesting(size_t maxTotalBytes);
};

#endif
