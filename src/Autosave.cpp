#include "Autosave.h"

#include "ParticleSystem.h"
#include "files.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <psapi.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <ctime>
#include <cstdio>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

namespace Autosave
{

// Subdirectory under %TEMP% where all autosave files live.
static const wchar_t kDirName[]   = L"AloParticleEditor";
static const wchar_t kFilePrefix[] = L"autosave-";
static const wchar_t kRecentSuffix[] = L"-recent.alo";
static const wchar_t kStableSuffix[] = L"-stable.alo";
static const wchar_t kMetaSuffix[]   = L".meta";

// Sweep autosave files older than this many days — by then they're
// not actionable for a typical user and TEMP is supposed to be
// transient anyway. Avoids unbounded accumulation if the editor
// crashes regularly with no recovery taken.
static const int kSweepOlderThanDays = 30;

#ifndef NDEBUG
#define AUTOSAVE_LOG(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#else
#define AUTOSAVE_LOG(...) ((void)0)
#endif

// ----- Path helpers ----------------------------------------------

static std::wstring GetAutosaveDir()
{
    wchar_t tempPath[MAX_PATH];
    DWORD len = GetTempPathW(MAX_PATH, tempPath);
    if (len == 0 || len > MAX_PATH) return L"";
    std::wstring dir(tempPath);
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    dir += kDirName;
    return dir;
}

// Ensure the autosave dir exists. Returns true on success or if it
// already existed; false on any failure (caller swallows the error).
static bool EnsureAutosaveDir()
{
    std::wstring dir = GetAutosaveDir();
    if (dir.empty()) return false;
    int rc = SHCreateDirectoryExW(NULL, dir.c_str(), NULL);
    return (rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS
         || rc == ERROR_FILE_EXISTS);
}

struct SessionKey
{
    DWORD     pid;
    ULONGLONG creationTime100ns;
    bool      hasCreationTime;

    bool operator<(const SessionKey& other) const
    {
        if (pid != other.pid) return pid < other.pid;
        if (hasCreationTime != other.hasCreationTime)
            return hasCreationTime < other.hasCreationTime;
        return creationTime100ns < other.creationTime100ns;
    }
};

static ULONGLONG FileTimeValue(const FILETIME& ft)
{
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

// The PID is not a durable launch identity: Windows may reuse it after a
// process exits. Pair it with the kernel-recorded process creation FILETIME and
// cache the pair once so every tier/meta write in this launch shares one key.
// If this first-party identity cannot be read, autosave fails closed instead of
// falling back to collision-prone PID-only names.
static const SessionKey& OurSessionKey()
{
    static const SessionKey key = []() {
        SessionKey out = {};
        out.pid = GetCurrentProcessId();
        FILETIME creation = {}, exit = {}, kernel = {}, user = {};
        if (out.pid != 0
            && GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        {
            out.creationTime100ns = FileTimeValue(creation);
            out.hasCreationTime = out.creationTime100ns != 0;
        }
        return out;
    }();
    return key;
}

static std::wstring PathForSession(const SessionKey& session, const wchar_t* suffix)
{
    if (session.pid == 0 || !session.hasCreationTime
        || session.creationTime100ns == 0)
        return L"";

    std::wstring dir = GetAutosaveDir();
    if (dir.empty()) return L"";
    wchar_t buf[96];
    swprintf_s(buf, 96, L"\\%ls%lu-%016llx%ls",
               kFilePrefix,
               (unsigned long)session.pid,
               (unsigned long long)session.creationTime100ns,
               suffix);
    return dir + buf;
}

static std::wstring OurRecentPath() { return PathForSession(OurSessionKey(), kRecentSuffix); }
static std::wstring OurStablePath() { return PathForSession(OurSessionKey(), kStableSuffix); }
static std::wstring OurMetaPath()   { return PathForSession(OurSessionKey(), kMetaSuffix);   }

// ----- Process-session liveness ---------------------------------

static std::wstring OurExeBaseName()
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return L"";
    const wchar_t* base = wcsrchr(path, L'\\');
    return base ? std::wstring(base + 1) : std::wstring(path);
}

// True if `pid` is a currently-running process whose image file
// basename matches our own (i.e. another live ParticleEditor.exe).
//
// On ambiguous error (OpenProcess fails with anything other than
// ERROR_INVALID_PARAMETER), conservatively returns TRUE. We'd rather
// skip recovery for one cycle than delete a sibling editor's
// in-progress autosave.
static bool IsLiveLegacyEditorPid(DWORD pid)
{
    if (pid == GetCurrentProcessId()) return true;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == NULL)
    {
        DWORD err = GetLastError();
        if (err == ERROR_INVALID_PARAMETER) return false;  // unambiguously not a process
        return true;  // access denied / other — be conservative
    }
    wchar_t imagePath[MAX_PATH];
    DWORD   size = MAX_PATH;
    bool    isEditor = false;
    if (QueryFullProcessImageNameW(h, 0, imagePath, &size))
    {
        const wchar_t* base = wcsrchr(imagePath, L'\\');
        const wchar_t* name = base ? base + 1 : imagePath;
        std::wstring ours = OurExeBaseName();
        if (!ours.empty() && _wcsicmp(name, ours.c_str()) == 0) isEditor = true;
    }
    else
    {
        // Can't read the image name but the process exists. Be
        // conservative: treat as a live editor so we don't delete
        // its files.
        isEditor = true;
    }
    CloseHandle(h);
    return isEditor;
}

// New-format files name an exact process lifetime. A reused PID is not live for
// the old file unless the creation FILETIME also matches. On ambiguous process
// query errors, remain conservative and treat the session as live.
static bool IsLiveEditorSession(const AutosaveName& name)
{
    if (!name.hasCreationTime)
        return IsLiveLegacyEditorPid((DWORD)name.pid);

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)name.pid);
    if (h == NULL)
    {
        const DWORD err = GetLastError();
        if (err == ERROR_INVALID_PARAMETER) return false;
        return true;
    }

    FILETIME creation = {}, exit = {}, kernel = {}, user = {};
    const bool queried =
        GetProcessTimes(h, &creation, &exit, &kernel, &user) != FALSE;
    CloseHandle(h);
    if (!queried) return true;
    return FileTimeValue(creation) == name.creationTime100ns;
}

// ----- Meta file read/write --------------------------------------

// Write UTF-16LE BOM + two CRLF-terminated lines:
//   line 1: original filename (may be empty for unsaved-new)
//   line 2: ISO-8601 timestamp of this write
static bool WriteMeta(const std::wstring& originalFilename)
{
    std::wstring path = OurMetaPath();
    if (path.empty()) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t ts[64];
    swprintf_s(ts, 64, L"%04d-%02d-%02dT%02d:%02d:%02d",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::wstring content;
    content.push_back((wchar_t)0xFEFF);                  // UTF-16LE BOM
    content += originalFilename;
    content += L"\r\n";
    content += ts;
    content += L"\r\n";

    DWORD written = 0;
    BOOL ok = WriteFile(h, content.data(),
                        (DWORD)(content.size() * sizeof(wchar_t)),
                        &written, NULL);
    CloseHandle(h);
    return (ok != 0);
}

static bool ReadMeta(const std::wstring& path, std::wstring* outOriginalFilename)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > 64 * 1024) { CloseHandle(h); return false; }

    std::vector<unsigned char> bytes(size);
    DWORD read = 0;
    BOOL ok = ReadFile(h, bytes.data(), size, &read, NULL);
    CloseHandle(h);
    if (!ok || read < 2) return false;

    // Strip UTF-16LE BOM and decode.
    size_t offset = 0;
    if (read >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) offset = 2;
    const wchar_t* wbase = reinterpret_cast<const wchar_t*>(bytes.data() + offset);
    size_t wcount = (read - offset) / sizeof(wchar_t);
    std::wstring text(wbase, wcount);

    // First line up to the first CR/LF is the original filename.
    size_t eol = text.find_first_of(L"\r\n");
    *outOriginalFilename = (eol == std::wstring::npos) ? text : text.substr(0, eol);
    return true;
}

// ----- Public API ------------------------------------------------

#ifdef AUTOSAVE_TESTING
static RecoveryCandidateHook g_recoveryCandidateHook = NULL;

void SetRecoveryCandidateHookForTest(RecoveryCandidateHook hook)
{
    g_recoveryCandidateHook = hook;
}
#endif

static bool VerifyParticleSystemFile(const std::wstring& path)
{
    PhysicalFile* f = NULL;
    try
    {
        f = new PhysicalFile(path, PhysicalFile::READ);
        std::unique_ptr<ParticleSystem> verified(new ParticleSystem(f));
        f->Release();
        return true;
    }
    catch (...)
    {
        if (f) f->Release();
        return false;
    }
}

static bool WriteTier(const ParticleSystem& sys,
                      const std::wstring&   originalFilename,
                      Tier                  tier,
                      bool                  verifyBeforeCommit)
{
    std::wstring dest = (tier == Tier::Recent) ? OurRecentPath() : OurStablePath();
    if (dest.empty()) return false;
    if (!EnsureAutosaveDir()) return false;
    std::wstring tmp = dest + L".tmp";

    // Write to a temp file then atomically rename into place — a
    // crash mid-write leaves the .tmp behind but the destination
    // .alo is either the previous good version or absent (never
    // partial).
    PhysicalFile* f = NULL;
    try
    {
        f = new PhysicalFile(tmp, PhysicalFile::WRITE);
        const_cast<ParticleSystem&>(sys).write(f);
        f->Release();
        f = NULL;
    }
    catch (...)
    {
        // Release BEFORE deleting. PhysicalFile opens without FILE_SHARE_DELETE,
        // so DeleteFileW fails while the handle is live and the .tmp survives —
        // and the next autosave targets that same path, cannot reopen it for
        // writing, and throws again. One failed write would otherwise disable
        // autosave silently for the rest of the session.
        if (f) { f->Release(); f = NULL; }
        DeleteFileW(tmp.c_str());
        AUTOSAVE_LOG("[Autosave] tier=%s write FAILED %ls (PhysicalFile threw)\n",
                     tier == Tier::Recent ? "recent" : "stable", tmp.c_str());
        return false;
    }

    // Recovery handoff has a stronger contract than periodic autosave: prove
    // that the still-uncommitted candidate can be loaded by the production
    // ParticleSystem parser before replacing any prior current-session tier.
    // Periodic writes keep their existing single-serialization cost.
    if (verifyBeforeCommit)
    {
#ifdef AUTOSAVE_TESTING
        if (g_recoveryCandidateHook) g_recoveryCandidateHook(tmp);
#endif
        if (!VerifyParticleSystemFile(tmp))
        {
            DeleteFileW(tmp.c_str());
            AUTOSAVE_LOG("[Autosave] recovery handoff verify FAILED %ls\n", tmp.c_str());
            return false;
        }
    }

    DWORD moveFlags = MOVEFILE_REPLACE_EXISTING;
    if (verifyBeforeCommit) moveFlags |= MOVEFILE_WRITE_THROUGH;
    if (!MoveFileExW(tmp.c_str(), dest.c_str(), moveFlags))
    {
        DeleteFileW(tmp.c_str());
        AUTOSAVE_LOG("[Autosave] tier=%s rename FAILED %ls -> %ls err=%lu\n",
                     tier == Tier::Recent ? "recent" : "stable",
                     tmp.c_str(), dest.c_str(), (unsigned long)GetLastError());
        return false;
    }

    // Best-effort: refresh the meta file each write so the timestamp
    // reflects the latest tier write. Meta is shared between the two
    // tiers so either tier writing it is fine.
    WriteMeta(originalFilename);

    AUTOSAVE_LOG("[Autosave] tier=%s write OK %ls\n",
                 tier == Tier::Recent ? "recent" : "stable", dest.c_str());
    return true;
}

bool Write(const ParticleSystem& sys,
           const std::wstring&   originalFilename,
           Tier                  tier)
{
    return WriteTier(sys, originalFilename, tier, false);
}

bool WriteRecoveryHandoff(const ParticleSystem& sys,
                          const std::wstring&   originalFilename)
{
    return WriteTier(sys, originalFilename, Tier::Recent, true);
}

void DeleteOurSession()
{
    const std::wstring recent = OurRecentPath();
    const std::wstring stable = OurStablePath();
    const std::wstring meta = OurMetaPath();
    if (!recent.empty()) DeleteFileW(recent.c_str());
    if (!stable.empty()) DeleteFileW(stable.c_str());
    if (!meta.empty())   DeleteFileW(meta.c_str());
    // The .tmp may exist if a write was interrupted; sweep it too.
    if (!recent.empty())
    {
        const std::wstring tmpRecent = recent + L".tmp";
        DeleteFileW(tmpRecent.c_str());
    }
    if (!stable.empty())
    {
        const std::wstring tmpStable = stable + L".tmp";
        DeleteFileW(tmpStable.c_str());
    }
}

// Compare two FILETIMEs as 64-bit values; return true if a > b.
static bool FtNewer(const FILETIME& a, const FILETIME& b)
{
    ULARGE_INTEGER ai, bi;
    ai.LowPart = a.dwLowDateTime;  ai.HighPart = a.dwHighDateTime;
    bi.LowPart = b.dwLowDateTime;  bi.HighPart = b.dwHighDateTime;
    return ai.QuadPart > bi.QuadPart;
}

// Subtract `days` days from `t`, in-place. Used by the sweep threshold.
static FILETIME FtSubtractDays(int days)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER ui;
    ui.LowPart  = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    // FILETIME ticks are 100ns intervals; one day = 24*60*60*1e7 ticks.
    ui.QuadPart -= (ULONGLONG)days * 24ULL * 60ULL * 60ULL * 10000000ULL;
    ft.dwLowDateTime  = ui.LowPart;
    ft.dwHighDateTime = ui.HighPart;
    return ft;
}

bool ScanForOrphan(OrphanSession* out)
{
    if (out == NULL) return false;
    *out = OrphanSession();

    std::wstring dir = GetAutosaveDir();
    if (dir.empty()) return false;

    std::wstring pattern = dir + L"\\" + kFilePrefix + L"*";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return false;

    // Group by complete process-session identity. Files older than the sweep threshold get
    // collected for deletion as a side effect of the scan.
    struct Group
    {
        std::wstring recentPath;
        std::wstring stablePath;
        std::wstring metaPath;
        FILETIME     recentMtime;
        FILETIME     stableMtime;
    };
    std::map<SessionKey, Group> bySession;
    std::vector<std::wstring> sweepList;
    FILETIME sweepThreshold = FtSubtractDays(kSweepOlderThanDays);

    do
    {
        if ((fd.dwFileAttributes
             & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
            continue;
        const AutosaveName name = ClassifyAutosaveName(fd.cFileName);
        if (name.pid == 0) continue;

        std::wstring full = dir + L"\\" + fd.cFileName;

        // Liveness precedes EVERY destructive decision. In particular, an
        // unusually long-running editor's tier may be older than the retention
        // threshold, and its active write must never be swept from under it.
        if (IsLiveEditorSession(name)) continue;

        // A .tmp is an INTERRUPTED write. It is never recovery material — it may
        // be truncated, and handing the user a half-written .alo is worse than
        // telling them there is nothing to recover — but it must not be left to
        // pile up either. Delete it as soon as its owning session is gone, with
        // no age threshold: unlike a real autosave it has no value to preserve.
        if (name.isTmp)
        {
            sweepList.push_back(full);
            AUTOSAVE_LOG("[Autosave] orphaned temp from dead session %lu: %ls\n",
                         name.pid, fd.cFileName);
            continue;
        }

        // Old-file sweep — drop anything past the threshold.
        if (FtNewer(sweepThreshold, fd.ftLastWriteTime))
        {
            sweepList.push_back(full);
            continue;
        }

        const SessionKey key = {
            (DWORD)name.pid,
            (ULONGLONG)name.creationTime100ns,
            name.hasCreationTime,
        };
        Group& g = bySession[key];
        if      (name.isRecent) { g.recentPath = full; g.recentMtime = fd.ftLastWriteTime; }
        else if (name.isStable) { g.stablePath = full; g.stableMtime = fd.ftLastWriteTime; }
        else if (name.isMeta)   { g.metaPath   = full; }
    }
    while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    for (const std::wstring& victim : sweepList)
    {
        DeleteFileW(victim.c_str());
        AUTOSAVE_LOG("[Autosave] swept stale %ls\n", victim.c_str());
    }

    if (bySession.empty()) return false;

    // Pick the orphan session with the newest file across either
    // tier. Ties break toward whichever complete key iterates first; the
    // user gets at least one recoverable session either way.
    SessionKey bestKey = {};
    bool haveBest = false;
    FILETIME bestMtime = { 0, 0 };
    for (const auto& kv : bySession)
    {
        const Group& g = kv.second;
        if (g.recentPath.empty() && g.stablePath.empty()) continue;
        FILETIME m = g.recentMtime;
        if (FtNewer(g.stableMtime, m)) m = g.stableMtime;
        if (!haveBest || FtNewer(m, bestMtime))
        {
            bestKey = kv.first;
            bestMtime = m;
            haveBest = true;
        }
    }
    if (!haveBest) return false;

    const Group& g = bySession.find(bestKey)->second;
    out->pid                 = bestKey.pid;
    out->creationTime100ns   = bestKey.creationTime100ns;
    out->hasCreationTime     = bestKey.hasCreationTime;
    out->recentPath          = g.recentPath;
    out->stablePath          = g.stablePath;
    out->metaPath            = g.metaPath;
    out->recentMtime         = g.recentMtime;
    out->stableMtime         = g.stableMtime;
    if (!g.metaPath.empty())
    {
        ReadMeta(g.metaPath, &out->originalFilename);
    }

    AUTOSAVE_LOG("[Autosave] orphan PID=%lu creation=%016llx recent=%s stable=%s origfile='%ls'\n",
                 (unsigned long)out->pid,
                 (unsigned long long)out->creationTime100ns,
                 g.recentPath.empty() ? "no"  : "yes",
                 g.stablePath.empty() ? "no"  : "yes",
                 out->originalFilename.c_str());
    return true;
}

void DeleteOrphan(const OrphanSession& session)
{
    const std::wstring currentRecent = OurRecentPath();
    const std::wstring currentStable = OurStablePath();
    const std::wstring currentMeta = OurMetaPath();
    const auto deleteOldPath = [&](const std::wstring& path) {
        if (path.empty()) return;
        if ((!currentRecent.empty() && _wcsicmp(path.c_str(), currentRecent.c_str()) == 0)
            || (!currentStable.empty() && _wcsicmp(path.c_str(), currentStable.c_str()) == 0)
            || (!currentMeta.empty() && _wcsicmp(path.c_str(), currentMeta.c_str()) == 0))
        {
            AUTOSAVE_LOG("[Autosave] refused orphan delete of current path %ls\n", path.c_str());
            return;
        }
        DeleteFileW(path.c_str());
    };
    deleteOldPath(session.recentPath);
    deleteOldPath(session.stablePath);
    deleteOldPath(session.metaPath);
    AUTOSAVE_LOG("[Autosave] discard PID=%lu\n", (unsigned long)session.pid);
}

} // namespace Autosave
