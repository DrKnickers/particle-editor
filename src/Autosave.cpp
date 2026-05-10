#include "Autosave.h"

#include "ParticleSystem.h"
#include "files.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <psapi.h>
#include <string>
#include <vector>
#include <unordered_map>
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

static std::wstring PathForPid(DWORD pid, const wchar_t* suffix)
{
    std::wstring dir = GetAutosaveDir();
    if (dir.empty()) return L"";
    wchar_t buf[64];
    swprintf_s(buf, 64, L"\\%ls%lu%ls", kFilePrefix, (unsigned long)pid, suffix);
    return dir + buf;
}

static std::wstring OurRecentPath() { return PathForPid(GetCurrentProcessId(), kRecentSuffix); }
static std::wstring OurStablePath() { return PathForPid(GetCurrentProcessId(), kStableSuffix); }
static std::wstring OurMetaPath()   { return PathForPid(GetCurrentProcessId(), kMetaSuffix);   }

// ----- PID liveness ----------------------------------------------

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
static bool IsLiveEditorPid(DWORD pid)
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

bool Write(const ParticleSystem& sys,
           const std::wstring&   originalFilename,
           Tier                  tier)
{
    if (!EnsureAutosaveDir()) return false;

    std::wstring dest = (tier == Tier::Recent) ? OurRecentPath() : OurStablePath();
    if (dest.empty()) return false;
    std::wstring tmp = dest + L".tmp";

    // Write to a temp file then atomically rename into place — a
    // crash mid-write leaves the .tmp behind but the destination
    // .alo is either the previous good version or absent (never
    // partial).
    try
    {
        PhysicalFile* f = new PhysicalFile(tmp, PhysicalFile::WRITE);
        const_cast<ParticleSystem&>(sys).write(f);
        f->Release();
    }
    catch (...)
    {
        DeleteFileW(tmp.c_str());
        AUTOSAVE_LOG("[Autosave] tier=%s write FAILED %ls (PhysicalFile threw)\n",
                     tier == Tier::Recent ? "recent" : "stable", tmp.c_str());
        return false;
    }

    if (!MoveFileExW(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING))
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

void DeleteOurSession()
{
    DeleteFileW(OurRecentPath().c_str());
    DeleteFileW(OurStablePath().c_str());
    DeleteFileW(OurMetaPath().c_str());
    // The .tmp may exist if a write was interrupted; sweep it too.
    std::wstring tmpRecent = OurRecentPath() + L".tmp";
    std::wstring tmpStable = OurStablePath() + L".tmp";
    DeleteFileW(tmpRecent.c_str());
    DeleteFileW(tmpStable.c_str());
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

// Parse `<prefix>123<suffix>` style filenames. Returns the PID on
// success or 0 on failure (and *outIsRecent flagged appropriately).
static DWORD ParsePidFromAutosaveName(const wchar_t* filename,
                                      bool* outIsRecent,
                                      bool* outIsStable,
                                      bool* outIsMeta)
{
    *outIsRecent = false;
    *outIsStable = false;
    *outIsMeta   = false;
    const size_t prefLen = wcslen(kFilePrefix);
    if (_wcsnicmp(filename, kFilePrefix, prefLen) != 0) return 0;
    const wchar_t* p = filename + prefLen;

    // Parse PID digits.
    DWORD pid = 0;
    while (*p >= L'0' && *p <= L'9')
    {
        pid = pid * 10 + (DWORD)(*p - L'0');
        p++;
    }
    if (pid == 0) return 0;

    if      (_wcsicmp(p, kRecentSuffix) == 0) *outIsRecent = true;
    else if (_wcsicmp(p, kStableSuffix) == 0) *outIsStable = true;
    else if (_wcsicmp(p, kMetaSuffix)   == 0) *outIsMeta   = true;
    else return 0;
    return pid;
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

    // Group by PID. Files older than the sweep threshold get
    // collected for deletion as a side effect of the scan.
    struct Group
    {
        std::wstring recentPath;
        std::wstring stablePath;
        std::wstring metaPath;
        FILETIME     recentMtime;
        FILETIME     stableMtime;
    };
    std::unordered_map<DWORD, Group> byPid;
    std::vector<std::wstring> sweepList;
    FILETIME sweepThreshold = FtSubtractDays(kSweepOlderThanDays);

    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        bool isRecent = false, isStable = false, isMeta = false;
        DWORD pid = ParsePidFromAutosaveName(fd.cFileName, &isRecent, &isStable, &isMeta);
        if (pid == 0) continue;

        std::wstring full = dir + L"\\" + fd.cFileName;

        // Old-file sweep — drop anything past the threshold.
        if (FtNewer(sweepThreshold, fd.ftLastWriteTime))
        {
            sweepList.push_back(full);
            continue;
        }

        // Skip files belonging to a live editor.
        if (IsLiveEditorPid(pid)) continue;

        Group& g = byPid[pid];
        if      (isRecent) { g.recentPath = full; g.recentMtime = fd.ftLastWriteTime; }
        else if (isStable) { g.stablePath = full; g.stableMtime = fd.ftLastWriteTime; }
        else if (isMeta)   { g.metaPath   = full; }
    }
    while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    for (const std::wstring& victim : sweepList)
    {
        DeleteFileW(victim.c_str());
        AUTOSAVE_LOG("[Autosave] swept stale %ls\n", victim.c_str());
    }

    if (byPid.empty()) return false;

    // Pick the orphan session with the newest file across either
    // tier. Ties break toward whichever PID iterates first; the
    // user gets at least one recoverable session either way.
    DWORD bestPid = 0;
    FILETIME bestMtime = { 0, 0 };
    for (const auto& kv : byPid)
    {
        const Group& g = kv.second;
        FILETIME m = g.recentMtime;
        if (FtNewer(g.stableMtime, m)) m = g.stableMtime;
        if (bestPid == 0 || FtNewer(m, bestMtime))
        {
            bestPid = kv.first;
            bestMtime = m;
        }
    }
    if (bestPid == 0) return false;

    const Group& g = byPid[bestPid];
    out->pid          = bestPid;
    out->recentPath   = g.recentPath;
    out->stablePath   = g.stablePath;
    out->metaPath     = g.metaPath;
    out->recentMtime  = g.recentMtime;
    out->stableMtime  = g.stableMtime;
    if (!g.metaPath.empty())
    {
        ReadMeta(g.metaPath, &out->originalFilename);
    }

    AUTOSAVE_LOG("[Autosave] orphan PID=%lu recent=%s stable=%s origfile='%ls'\n",
                 (unsigned long)out->pid,
                 g.recentPath.empty() ? "no"  : "yes",
                 g.stablePath.empty() ? "no"  : "yes",
                 out->originalFilename.c_str());
    return true;
}

void DeleteOrphan(const OrphanSession& session)
{
    if (!session.recentPath.empty()) DeleteFileW(session.recentPath.c_str());
    if (!session.stablePath.empty()) DeleteFileW(session.stablePath.c_str());
    if (!session.metaPath.empty())   DeleteFileW(session.metaPath.c_str());
    AUTOSAVE_LOG("[Autosave] discard PID=%lu\n", (unsigned long)session.pid);
}

} // namespace Autosave
