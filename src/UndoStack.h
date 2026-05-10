#ifndef UNDO_STACK_H
#define UNDO_STACK_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <deque>
#include <vector>
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

    UndoStack();
    ~UndoStack();

    // Snapshot the current state. selectedIndex is the index into
    // sys.getEmitters() of the currently-selected emitter (or
    // SIZE_MAX if no selection). Returns true if a new entry was
    // pushed; false if the call coalesced into an existing entry or
    // was suppressed by the m_applying guard.
    bool Capture(const ParticleSystem& sys, size_t selectedIndex,
                 DWORD coalesceKey);

    bool CanUndo() const;
    bool CanRedo() const;

    // Move cursor backward / forward. On success, fills outSnapshot
    // with a pointer to the buffer to restore from (owned by this
    // stack — copy or use immediately) and outSelectedIndex with the
    // selection-index at capture time.
    bool Undo(const std::vector<char>** outSnapshot,
              size_t* outSelectedIndex);
    bool Redo(const std::vector<char>** outSnapshot,
              size_t* outSelectedIndex);

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
    };

    std::deque<Entry> m_entries;
    // m_cursor points at the entry representing "current state".
    // Undo decrements; redo increments. New captures at cursor==N
    // truncate any redo branch above N before pushing.
    size_t            m_cursor;
    bool              m_applying;
};

#endif
