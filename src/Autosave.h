#ifndef AUTOSAVE_H
#define AUTOSAVE_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

class ParticleSystem;

// Periodic autosave for in-progress particles, with two tiers:
//   - "recent" — every 30 seconds; freshest state, frequent overwrite
//   - "stable" — every 5 minutes; older known-good state, fallback if
//                 recent is corrupt or the user wants to roll back a
//                 bad edit they made in the last few minutes
//
// Files live under `%TEMP%\AloParticleEditor\` named
// `autosave-<pid>-<tier>.alo` so concurrent editor instances don't
// clobber each other's autosave. A companion `autosave-<pid>.meta`
// records the original filename so the recovery prompt can show
// "Restore unsaved changes to fire.alo?" rather than just an
// anonymous TEMP path.
//
// Both autosaves are ALWAYS at separate paths from the user's
// `.alo` — the editor never silently overwrites their file.
//
// All operations are best-effort: IO failures (disk full, permission
// denied, missing TEMP dir) are swallowed silently so the editor
// doesn't pop an error dialog every 30 seconds. Worst case the user
// loses no data — they just have no recovery safety net.

namespace Autosave
{
    enum class Tier
    {
        Recent,   // 30 s cadence
        Stable,   // 5 min cadence
    };

    // Timer IDs and intervals. Timer IDs 1 and 2 are reserved for the
    // tree-control auto-scroll (EmitterList.cpp); these start at 3.
    static const UINT_PTR RECENT_TIMER_ID    = 3;
    static const UINT_PTR STABLE_TIMER_ID    = 4;
    static const UINT     RECENT_INTERVAL_MS = 30 * 1000;
    static const UINT     STABLE_INTERVAL_MS = 5 * 60 * 1000;

    // One orphan recovery candidate, picked up at startup by ScanForOrphan.
    // Either recentPath or stablePath may be empty (the editor may have
    // crashed in the first 30 s before a recent write, or between a
    // stable write and the next recent tick), but at least one is set.
    struct OrphanSession
    {
        DWORD              pid;
        std::wstring       recentPath;        // empty if no recent file
        std::wstring       stablePath;        // empty if no stable file
        std::wstring       metaPath;          // for cleanup
        std::wstring       originalFilename;  // from .meta, may be empty
        FILETIME           recentMtime;
        FILETIME           stableMtime;
    };

    // Write the system to the chosen tier's autosave path for the
    // current PID. originalFilename is recorded in the .meta sidecar
    // for the recovery prompt to display. No-op + return false on
    // any IO failure.
    bool Write(const ParticleSystem& sys,
               const std::wstring&   originalFilename,
               Tier                  tier);

    // Delete this PID's autosave files (both tiers + meta).
    // Best-effort; missing files are not an error.
    void DeleteOurSession();

    // Scan %TEMP%\AloParticleEditor\ for orphan autosave sessions
    // (files whose owning PID is no longer a live editor process).
    // Returns the session with the most-recently-modified file across
    // its tiers, or fills `out` with all empty paths if none found.
    // Also sweeps autosave files older than 30 days as a side effect.
    bool ScanForOrphan(OrphanSession* out);

    // Delete an orphan session's files (both tiers + meta). Called
    // after the recovery prompt resolves, regardless of the user's
    // choice — the session is consumed either way.
    void DeleteOrphan(const OrphanSession& session);
}

#endif
