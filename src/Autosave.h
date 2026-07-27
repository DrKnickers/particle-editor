#ifndef AUTOSAVE_H
#define AUTOSAVE_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <cwchar>   // wcslen / _wcsicmp / _wcsnicmp (ClassifyAutosaveName)

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

    // Outcome of an autosave recovery attempt. Decides whether the orphan
    // session's files are consumed (deleted) or kept on disk for a later retry.
    enum class RecoverOutcome
    {
        Recovered,   // user chose a tier and it loaded successfully
        Discarded,   // user explicitly discarded the recovery
        Failed,      // load failed, or no path resolved — keep the files
    };

    // ---- autosave filename classification (pure) --------------------------
    //
    // Shared with Autosave.cpp so tests/test_autosave_recover.cpp can cover it
    // header-only, like the two predicates below.
    //
    // The `.tmp` case is why this is here at all. An interrupted write leaves
    // e.g. `autosave-1234-recent.alo.tmp`, and the classifier used to compare
    // the whole tail EXACTLY against the three tier suffixes — so that name
    // matched nothing, the scan skipped it, and a crashed session's temp was
    // neither offered for recovery nor swept. They accumulated forever across
    // crashes (2026-07 audit, an-audit-finding).
    static const wchar_t kNamePrefix[]    = L"autosave-";
    static const wchar_t kNameRecent[]    = L"-recent.alo";
    static const wchar_t kNameStable[]    = L"-stable.alo";
    static const wchar_t kNameMeta[]      = L".meta";
    static const wchar_t kNameTmpExt[]    = L".tmp";

    struct AutosaveName
    {
        unsigned long pid   = 0;      // 0 = not one of ours
        bool isRecent       = false;
        bool isStable       = false;
        bool isMeta         = false;
        bool isTmp          = false;  // interrupted write; NEVER recovery material
    };

    inline AutosaveName ClassifyAutosaveName(const wchar_t* filename)
    {
        AutosaveName out;
        if (filename == nullptr) return out;

        const size_t prefLen = wcslen(kNamePrefix);
        if (_wcsnicmp(filename, kNamePrefix, prefLen) != 0) return out;
        const wchar_t* p = filename + prefLen;

        unsigned long pid = 0;
        while (*p >= L'0' && *p <= L'9')
        {
            pid = pid * 10 + (unsigned long)(*p - L'0');
            ++p;
        }
        if (pid == 0) return out;

        // Strip ONE trailing ".tmp" before classifying, so the tier comparison
        // below stays an exact match on the remainder.
        std::wstring tail = p;
        const size_t tmpLen = wcslen(kNameTmpExt);
        if (tail.size() > tmpLen
            && _wcsicmp(tail.c_str() + tail.size() - tmpLen, kNameTmpExt) == 0)
        {
            out.isTmp = true;
            tail.resize(tail.size() - tmpLen);
        }

        if      (_wcsicmp(tail.c_str(), kNameRecent) == 0) out.isRecent = true;
        else if (_wcsicmp(tail.c_str(), kNameStable) == 0) out.isStable = true;
        else if (_wcsicmp(tail.c_str(), kNameMeta)   == 0) out.isMeta   = true;
        else return AutosaveName();   // not ours after all

        out.pid = pid;
        return out;
    }

    // Orphan files are deleted ONLY on a successful recover or an explicit
    // discard, NEVER on a failed load — so the other tier (or the next launch)
    // can still recover. Pure; unit-tested in tests/test_autosave_recover.cpp.
    inline bool ShouldDeleteOrphan(RecoverOutcome outcome)
    {
        return outcome == RecoverOutcome::Recovered ||
               outcome == RecoverOutcome::Discarded;
    }

    // Whether autosave/check-recovery should skip the recovery prompt and
    // report no orphan, given the host's mode. Suppress when:
    //   testHost      — the a11y/CDP harness must never get a recovery modal
    //                   (it would pollute composition captures);
    //   ephemeral     — automation mode (--record / --drive): the mount-time
    //                   check fires BEFORE the timeline opens its file, so an
    //                   unrelated real orphan would otherwise sit center-screen
    //                   over every captured frame — and automation never writes
    //                   an autosave of its own to recover;
    //   hasCurrentFile — a document is already loaded (a CLI file / any
    //                   non-untitled state), which "wins" over recovery.
    // Suppression only skips the PROMPT; it never touches the on-disk orphan,
    // so a normal launch still recovers it. Pure; unit-tested in
    // tests/test_autosave_recover.cpp.
    inline bool ShouldSuppressRecoveryPrompt(bool testHost, bool ephemeral, bool hasCurrentFile)
    {
        return testHost || ephemeral || hasCurrentFile;
    }

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

    // Delete an orphan session's files (both tiers + meta). Call ONLY when
    // ShouldDeleteOrphan(outcome) is true — i.e. a successful recover or an
    // explicit discard. On a failed load, leave the files on disk so the other
    // tier or the next launch can still recover them.
    void DeleteOrphan(const OrphanSession& session);
}

#endif
