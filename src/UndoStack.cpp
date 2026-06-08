#include "UndoStack.h"

#include "ParticleSystem.h"
#include "files.h"

UndoStack::UndoStack()
    : m_cursor(0)
    , m_applying(false)
    , m_liveAhead(false)
{
}

UndoStack::~UndoStack()
{
}

std::vector<char> UndoStack::Serialize(const ParticleSystem& sys)
{
    MemoryFile* mf = new MemoryFile();
    // ParticleSystem::write isn't const but doesn't mutate observable
    // state — it walks the emitter list and emits chunks. The const_cast
    // is the same trick the rest of the codebase would need; safe here.
    const_cast<ParticleSystem&>(sys).write(mf);

    std::vector<char> buf((size_t)mf->size());
    if (!buf.empty())
    {
        mf->seek(0);
        mf->read(buf.data(), mf->size());
    }
    mf->Release();
    return buf;
}

ParticleSystem* UndoStack::Deserialize(const std::vector<char>& buf)
{
    if (buf.empty()) return NULL;

    MemoryFile* mf = new MemoryFile();
    mf->write(buf.data(), (unsigned long)buf.size());
    mf->seek(0);

    ParticleSystem* sys = NULL;
    try
    {
        sys = new ParticleSystem(mf);
    }
    catch (...)
    {
        mf->Release();
        throw;
    }
    mf->Release();
    return sys;
}

DWORD UndoStack::MakeCoalesceKey(WORD notifyCode, WORD discriminator)
{
    // Reserve 0 for "never coalesce" — it should never collide with a
    // real notification code (lowest WM_APP-derived codes are >= 1).
    DWORD key = ((DWORD)notifyCode << 16) | (DWORD)discriminator;
    return (key == 0) ? 1 : key;
}

bool UndoStack::Capture(const ParticleSystem& sys, size_t selectedIndex,
                         DWORD coalesceKey)
{
    if (m_applying) return false;

    // Any non-applying capture is immediately followed by a mutation in
    // the calling handler, so the live system is now (about to be) one
    // step ahead of the snapshot we're recording. Mark it so the
    // head-of-history auto-capture in undo/perform knows live is skewed
    // and fires; Undo()/Redo() clear it once live is re-synced to a tip.
    // Applies to both the coalesce-in-place and push paths below.
    m_liveAhead = true;

    // New edit invalidates redo branch.
    if (m_cursor < m_entries.size())
    {
        m_entries.erase(m_entries.begin() + m_cursor, m_entries.end());
    }

    DWORD now = GetTickCount();

    // Coalesce: if the previous entry has the same key and we're
    // within the time window, replace its snapshot in place. Skip on
    // key == 0 (structural ops).
    if (coalesceKey != 0
        && !m_entries.empty()
        && m_entries.back().coalesceKey == coalesceKey
        && (now - m_entries.back().timestamp) <= COALESCE_WINDOW_MS)
    {
        Entry& tail = m_entries.back();
        tail.snapshot      = Serialize(sys);
        tail.selectedIndex = selectedIndex;
        tail.timestamp     = now;
        // Don't touch isSavedState — coalescing into a saved entry
        // means we're still at the saved state's content if no real
        // change accumulated, which is fine for most cases. If the
        // coalesced content actually differs we'd want to clear it,
        // but the save-state flag is recomputed against MarkSaved on
        // next save anyway.
        tail.isSavedState  = false;
        return false;
    }

    Entry e;
    e.snapshot      = Serialize(sys);
    e.selectedIndex = selectedIndex;
    e.coalesceKey   = coalesceKey;
    e.timestamp     = now;
    e.isSavedState  = false;
    m_entries.push_back(std::move(e));
    m_cursor = m_entries.size();

    // Cap stack depth.
    while (m_entries.size() > MAX_ENTRIES)
    {
        m_entries.pop_front();
        if (m_cursor > 0) m_cursor--;
    }
    return true;
}

bool UndoStack::CapturePreCoalesced(const ParticleSystem& sys,
                                    size_t selectedIndex, DWORD coalesceKey)
{
    if (m_applying) return false;

    // PRE-mutation capture: the caller mutates live right after this, so
    // live becomes one step ahead of the tip (see IsLiveAhead()).
    m_liveAhead = true;

    DWORD now = GetTickCount();

    // Skip-coalesce — only at the head of history (no redo branch). If the
    // previous entry shares this key within the window, it ALREADY holds the
    // burst's pre-mutation (session-start) state — exactly the undo target —
    // so skip the capture and just slide the window. One undo then reverts
    // the whole burst (the head-of-history auto-cap in undo/perform snapshots
    // the final live state before stepping back). Mid-redo-branch
    // (cursor < size) we never skip: a fresh edit after an undo must push.
    if (coalesceKey != 0
        && m_cursor == m_entries.size()
        && !m_entries.empty()
        && m_entries.back().coalesceKey == coalesceKey
        && (now - m_entries.back().timestamp) <= COALESCE_WINDOW_MS)
    {
        m_entries.back().timestamp = now;
        return false;
    }

    // No coalesce: invalidate any redo branch and push the pre-mutation
    // state as a new entry (mirrors Capture()'s push path).
    if (m_cursor < m_entries.size())
    {
        m_entries.erase(m_entries.begin() + m_cursor, m_entries.end());
    }

    Entry e;
    e.snapshot      = Serialize(sys);
    e.selectedIndex = selectedIndex;
    e.coalesceKey   = coalesceKey;
    e.timestamp     = now;
    e.isSavedState  = false;
    m_entries.push_back(std::move(e));
    m_cursor = m_entries.size();

    while (m_entries.size() > MAX_ENTRIES)
    {
        m_entries.pop_front();
        if (m_cursor > 0) m_cursor--;
    }
    return true;
}

bool UndoStack::CanUndo() const
{
    // Need at least one entry behind the cursor. Cursor at N points at
    // "state after entry N-1"; undo means restore entry N-2.
    return m_cursor >= 2;
}

bool UndoStack::CanRedo() const
{
    return m_cursor < m_entries.size();
}

bool UndoStack::Undo(const std::vector<char>** outSnapshot,
                     size_t* outSelectedIndex)
{
    if (!CanUndo()) return false;
    // Live will be restored to the returned snapshot by the caller, so it
    // is back in sync with the tip — no pending edit ahead.
    m_liveAhead = false;
    m_cursor--;
    const Entry& e = m_entries[m_cursor - 1];
    if (outSnapshot)         *outSnapshot         = &e.snapshot;
    if (outSelectedIndex)    *outSelectedIndex    = e.selectedIndex;
    return true;
}

bool UndoStack::Redo(const std::vector<char>** outSnapshot,
                     size_t* outSelectedIndex)
{
    if (!CanRedo()) return false;
    // Live will be restored to the returned snapshot by the caller, so it
    // is back in sync with the tip — no pending edit ahead.
    m_liveAhead = false;
    const Entry& e = m_entries[m_cursor];
    m_cursor++;
    if (outSnapshot)         *outSnapshot         = &e.snapshot;
    if (outSelectedIndex)    *outSelectedIndex    = e.selectedIndex;
    return true;
}

void UndoStack::Clear()
{
    m_entries.clear();
    m_cursor = 0;
    m_liveAhead = false;
}

void UndoStack::MarkSaved()
{
    if (m_entries.empty()) return;
    for (size_t i = 0; i < m_entries.size(); i++)
    {
        m_entries[i].isSavedState = false;
    }
    // Cursor==N means the state matching disk is entry N-1.
    if (m_cursor > 0 && m_cursor <= m_entries.size())
    {
        m_entries[m_cursor - 1].isSavedState = true;
    }
}

bool UndoStack::IsAtSavedState() const
{
    if (m_cursor == 0 || m_cursor > m_entries.size()) return false;
    return m_entries[m_cursor - 1].isSavedState;
}
