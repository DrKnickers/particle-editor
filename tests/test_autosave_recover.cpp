// Production-linked autosave recovery contract test.
//
// This executable links src/Autosave.cpp and the real ParticleSystem
// serializer/parser. It runs against a unique TEMP root so it can prove the
// filesystem behavior that the former header-only test could not:
//   - PID + creation-FILETIME identity and PID-reuse liveness;
//   - dead .tmp sweep through ScanForOrphan's real sweepList push;
//   - live-session overreach protection (including the 30-day sweep);
//   - parse verification before recovery-handoff replacement;
//   - current-session paths surviving DeleteOrphan even if an aliased record is
//     supplied.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Autosave.h"
#include "ParticleSystem.h"
#include "ParticleSystemInstance.h"
#include "files.h"

// ParticleSystem::Emitter::~Emitter routes live instances through this D3D-
// coupled owner method. This data-only test never registers an EmitterInstance,
// so the established standalone-test stub keeps the link graph host-free.
void ParticleSystemInstance::RemoveEmitter(EmitterInstance*) {}

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static std::wstring GetEnv(const wchar_t* name, bool* hadValue)
{
    const DWORD needed = GetEnvironmentVariableW(name, NULL, 0);
    if (needed == 0)
    {
        *hadValue = false;
        return L"";
    }
    std::vector<wchar_t> buf(needed);
    const DWORD got = GetEnvironmentVariableW(name, buf.data(), needed);
    *hadValue = got > 0 && got < needed;
    return *hadValue ? std::wstring(buf.data(), got) : std::wstring();
}

class EnvGuard
{
public:
    EnvGuard(const wchar_t* name, const std::wstring& value)
        : m_name(name), m_old(GetEnv(name, &m_hadOld))
    {
        CHECK(SetEnvironmentVariableW(m_name.c_str(), value.c_str()) != FALSE);
    }

    ~EnvGuard()
    {
        SetEnvironmentVariableW(
            m_name.c_str(), m_hadOld ? m_old.c_str() : NULL);
    }

private:
    std::wstring m_name;
    std::wstring m_old;
    bool         m_hadOld = false;
};

class TempTree
{
public:
    TempTree()
    {
        wchar_t base[MAX_PATH] = {};
        const DWORD n = GetTempPathW(MAX_PATH, base);
        CHECK(n > 0 && n < MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return;

        for (unsigned int attempt = 0; attempt < 100; ++attempt)
        {
            wchar_t leaf[96] = {};
            swprintf_s(
                leaf, L"AloAutosaveRecoverTest-%lu-%llu-%u",
                (unsigned long)GetCurrentProcessId(),
                (unsigned long long)GetTickCount64(),
                attempt);
            m_root = base;
            if (!m_root.empty() && m_root.back() != L'\\') m_root += L'\\';
            m_root += leaf;
            if (CreateDirectoryW(m_root.c_str(), NULL)) return;
            if (GetLastError() != ERROR_ALREADY_EXISTS) break;
        }
        CHECK(false && "could not create isolated autosave TEMP root");
        m_root.clear();
    }

    ~TempTree()
    {
        if (m_root.empty()) return;
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(m_root), ec);
        CHECK(!ec);
    }

    const std::wstring& Root() const { return m_root; }
    std::wstring AutosaveDir() const { return m_root + L"\\AloParticleEditor"; }

private:
    std::wstring m_root;
};

static bool WriteRaw(const std::wstring& path, const std::vector<unsigned char>& bytes)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(
        h, bytes.empty() ? NULL : bytes.data(), (DWORD)bytes.size(), &written, NULL);
    CloseHandle(h);
    return ok != FALSE && written == bytes.size();
}

static std::vector<unsigned char> ReadRaw(const std::wstring& path)
{
    std::vector<unsigned char> out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return out;
    const DWORD size = GetFileSize(h, NULL);
    if (size != INVALID_FILE_SIZE)
    {
        out.resize(size);
        DWORD read = 0;
        if (!ReadFile(h, out.data(), size, &read, NULL) || read != size)
            out.clear();
    }
    CloseHandle(h);
    return out;
}

static bool IsRegularFile(const std::wstring& path)
{
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES
        && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static void SetLastWriteTicks(const std::wstring& path, ULONGLONG ticks)
{
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h == INVALID_HANDLE_VALUE) return;
    ULARGE_INTEGER u;
    u.QuadPart = ticks;
    FILETIME ft = { u.LowPart, u.HighPart };
    CHECK(SetFileTime(h, NULL, NULL, &ft) != FALSE);
    CloseHandle(h);
}

static ULONGLONG NowTicks()
{
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

static std::wstring MakeName(DWORD pid, unsigned long long creation,
                             const wchar_t* suffix)
{
    wchar_t name[128] = {};
    swprintf_s(name, L"autosave-%lu-%016llx%ls",
               (unsigned long)pid, creation, suffix);
    return name;
}

static std::wstring FindCurrentPath(const std::wstring& dir,
                                    bool recent, bool stable, bool meta)
{
    WIN32_FIND_DATAW fd = {};
    const std::wstring pattern = dir + L"\\autosave-*";
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return L"";
    std::wstring found;
    do
    {
        const Autosave::AutosaveName name =
            Autosave::ClassifyAutosaveName(fd.cFileName);
        if (name.pid == GetCurrentProcessId()
            && name.hasCreationTime
            && !name.isTmp
            && name.isRecent == recent
            && name.isStable == stable
            && name.isMeta == meta)
        {
            found = dir + L"\\" + fd.cFileName;
            break;
        }
    }
    while (FindNextFileW(find, &fd));
    FindClose(find);
    return found;
}

static std::unique_ptr<ParticleSystem> LoadSystem(const std::wstring& path)
{
    PhysicalFile* f = NULL;
    try
    {
        f = new PhysicalFile(path, PhysicalFile::READ);
        std::unique_ptr<ParticleSystem> loaded(new ParticleSystem(f));
        f->Release();
        return loaded;
    }
    catch (...)
    {
        if (f) f->Release();
        return nullptr;
    }
}

static void CorruptRecoveryCandidate(const std::wstring& candidate)
{
    const unsigned char corrupt[] = { 0x09, 0x00, 0x00 };
    WriteRaw(candidate, std::vector<unsigned char>(
        corrupt, corrupt + sizeof(corrupt)));
}

static DWORD DefinitelyDeadPid()
{
    const DWORD candidates[] = { 0xFFFFFFFEUL, 0xFFFFFFFCUL, 0x7FFFFFFCUL };
    for (DWORD pid : candidates)
    {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h == NULL && GetLastError() == ERROR_INVALID_PARAMETER) return pid;
        if (h) CloseHandle(h);
    }
    return 0;
}

static std::wstring Uppercase(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return (wchar_t)towupper(c); });
    return value;
}

enum class LegacyProcessProbeMode
{
    QueryFails,
    BasenameMatches,
    OwnNameFails,
};

static LegacyProcessProbeMode g_legacyProcessProbeMode;
static DWORD g_legacyProcessProbePid = 0;

static host::ModuleNameProbeResult ProbeLegacyProcessName(DWORD pid,
                                                          wchar_t* buffer,
                                                          DWORD capacity)
{
    std::wstring path;
    if (pid == g_legacyProcessProbePid)
    {
        if (g_legacyProcessProbeMode == LegacyProcessProbeMode::QueryFails)
            return host::ModuleNameProbeResult{ 0, ERROR_ACCESS_DENIED };
        path = L"C:\\Sibling\\ParticleEditor.exe";
    }
    else if (pid == GetCurrentProcessId())
    {
        if (g_legacyProcessProbeMode == LegacyProcessProbeMode::OwnNameFails)
            return host::ModuleNameProbeResult{ 0, ERROR_ACCESS_DENIED };
        path = L"C:\\Current\\ParticleEditor.exe";
    }
    else
    {
        return host::ModuleNameProbeResult{ 0, ERROR_INVALID_PARAMETER };
    }

    if (path.size() >= capacity)
        return host::ModuleNameProbeResult{ capacity, ERROR_INSUFFICIENT_BUFFER };
    std::copy(path.begin(), path.end(), buffer);
    return host::ModuleNameProbeResult{
        static_cast<DWORD>(path.size()),
        ERROR_SUCCESS,
    };
}

static PROCESS_INFORMATION StartLiveSibling()
{
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    wchar_t command[] = L"cmd.exe /d /c ping -n 30 127.0.0.1 >nul";
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &startup, &process))
        process = {};
    return process;
}

int main()
{
    using Autosave::ClassifyAutosaveName;
    using Autosave::RecoverOutcome;
    using Autosave::ShouldDeleteOrphan;
    using Autosave::ShouldSuppressRecoveryPrompt;

    // Recovery/discard and prompt-mode pure contracts remain pinned.
    CHECK(ShouldDeleteOrphan(RecoverOutcome::Recovered));
    CHECK(ShouldDeleteOrphan(RecoverOutcome::Discarded));
    CHECK(!ShouldDeleteOrphan(RecoverOutcome::Failed));
    CHECK(ShouldSuppressRecoveryPrompt(false, true, false));
    CHECK(ShouldSuppressRecoveryPrompt(true, false, false));
    CHECK(ShouldSuppressRecoveryPrompt(false, false, true));
    CHECK(!ShouldSuppressRecoveryPrompt(false, false, false));

    // Legacy names remain readable.
    {
        const auto recent = ClassifyAutosaveName(L"autosave-1234-recent.alo");
        CHECK(recent.pid == 1234 && recent.isRecent);
        CHECK(!recent.hasCreationTime && !recent.isTmp);

        const auto tmp = ClassifyAutosaveName(L"autosave-1234-recent.alo.tmp");
        CHECK(tmp.pid == 1234 && tmp.isRecent && tmp.isTmp);
        CHECK(!tmp.hasCreationTime);
    }

    // New grammar: exact PID + 16-hex creation FILETIME + suffix.
    {
        const auto recent =
            ClassifyAutosaveName(L"autosave-1234-01dbee4f1234abcd-recent.alo");
        CHECK(recent.pid == 1234 && recent.isRecent && recent.hasCreationTime);
        CHECK(recent.creationTime100ns == 0x01dbee4f1234abcdULL);

        const auto stableTmp =
            ClassifyAutosaveName(L"AUTOSAVE-77-ABCDEF0123456789-STABLE.ALO.TMP");
        CHECK(stableTmp.pid == 77 && stableTmp.isStable && stableTmp.isTmp);
        CHECK(stableTmp.hasCreationTime);
        CHECK(stableTmp.creationTime100ns == 0xABCDEF0123456789ULL);

        const auto meta =
            ClassifyAutosaveName(L"autosave-5-0000000000000001.meta");
        CHECK(meta.pid == 5 && meta.isMeta && meta.hasCreationTime);

        // The concrete values that break malformed/overflow acceptance.
        CHECK(ClassifyAutosaveName(
            L"autosave-4294967295-0000000000000001-recent.alo").pid
            == 0xFFFFFFFFUL);
        CHECK(ClassifyAutosaveName(
            L"autosave-4294967296-0000000000000001-recent.alo").pid == 0);
        CHECK(ClassifyAutosaveName(
            L"autosave-1-000000000000001-recent.alo").pid == 0);   // 15 hex
        CHECK(ClassifyAutosaveName(
            L"autosave-1-00000000000000001-recent.alo").pid == 0); // 17 hex
        CHECK(ClassifyAutosaveName(
            L"autosave-1-0000000000000000-recent.alo").pid == 0);  // zero token
        CHECK(ClassifyAutosaveName(
            L"autosave-1-000000000000000g-recent.alo").pid == 0);
        CHECK(ClassifyAutosaveName(
            L"autosave-1-0000000000000001-recent.alo.tmp.tmp").pid == 0);
        CHECK(ClassifyAutosaveName(L"autosave-1234-bogus.alo.tmp").pid == 0);
        CHECK(ClassifyAutosaveName(nullptr).pid == 0);
    }

    TempTree tree;
    CHECK(!tree.Root().empty());
    if (tree.Root().empty()) return 1;
    EnvGuard tmpGuard(L"TMP", tree.Root());
    EnvGuard tempGuard(L"TEMP", tree.Root());

    ParticleSystem original;
    original.setName("AutosaveOriginal");
    ParticleSystem::Emitter* originalEmitter = original.addRootEmitter();
    originalEmitter->name = "OriginalEmitter";

    CHECK(Autosave::Write(original, L"C:\\Effects\\original.alo",
                          Autosave::Tier::Recent));
    CHECK(Autosave::Write(original, L"C:\\Effects\\original.alo",
                          Autosave::Tier::Stable));

    const std::wstring dir = tree.AutosaveDir();
    const std::wstring currentRecent = FindCurrentPath(dir, true, false, false);
    const std::wstring currentStable = FindCurrentPath(dir, false, true, false);
    const std::wstring currentMeta = FindCurrentPath(dir, false, false, true);
    CHECK(IsRegularFile(currentRecent));
    CHECK(IsRegularFile(currentStable));
    CHECK(IsRegularFile(currentMeta));

    const Autosave::AutosaveName currentName =
        ClassifyAutosaveName(std::filesystem::path(currentRecent).filename().c_str());
    CHECK(currentName.pid == GetCurrentProcessId());
    CHECK(currentName.hasCreationTime && currentName.creationTime100ns != 0);

    // Verified handoff must parse BEFORE replacement. Corrupt the closed `.tmp`
    // through the test-only hook: false is required and the exact old bytes must
    // survive. A verify-after-replace implementation changes these bytes.
    ParticleSystem replacement;
    replacement.setName("AutosaveReplacement");
    ParticleSystem::Emitter* replacementEmitter = replacement.addRootEmitter();
    replacementEmitter->name = "ReplacementEmitter";
    const std::vector<unsigned char> beforeFailedHandoff = ReadRaw(currentRecent);
    CHECK(!beforeFailedHandoff.empty());
    Autosave::SetRecoveryCandidateHookForTest(CorruptRecoveryCandidate);
    CHECK(!Autosave::WriteRecoveryHandoff(
        replacement, L"C:\\Effects\\replacement.alo"));
    Autosave::SetRecoveryCandidateHookForTest(NULL);
    CHECK(ReadRaw(currentRecent) == beforeFailedHandoff);
    CHECK(!IsRegularFile(currentRecent + L".tmp"));

    CHECK(Autosave::WriteRecoveryHandoff(
        replacement, L"C:\\Effects\\replacement.alo"));
    std::unique_ptr<ParticleSystem> loaded = LoadSystem(currentRecent);
    CHECK(loaded != nullptr);
    if (loaded)
    {
        CHECK(loaded->getName() == "AutosaveReplacement");
        CHECK(loaded->getEmitters().size() == 1);
        if (loaded->getEmitters().size() == 1)
            CHECK(loaded->getEmitters()[0]->name == "ReplacementEmitter");
    }

    // Defense in depth for an aliased/crafted pending record: Windows path
    // comparison is case-insensitive, and DeleteOrphan must never remove a
    // current-session tier/meta.
    Autosave::OrphanSession aliased = {};
    aliased.pid = GetCurrentProcessId();
    aliased.creationTime100ns = currentName.creationTime100ns;
    aliased.hasCreationTime = true;
    aliased.recentPath = Uppercase(currentRecent);
    aliased.stablePath = Uppercase(currentStable);
    aliased.metaPath = Uppercase(currentMeta);
    Autosave::DeleteOrphan(aliased);
    CHECK(IsRegularFile(currentRecent));
    CHECK(IsRegularFile(currentStable));
    CHECK(IsRegularFile(currentMeta));

    // Live-session overreach: even a 31-day-old current tier and a live `.tmp`
    // survive the real scanner. Age-before-liveness or sweep-all-temp mutants
    // delete these exact paths.
    const ULONGLONG dayTicks = 24ULL * 60ULL * 60ULL * 10000000ULL;
    SetLastWriteTicks(currentRecent, NowTicks() - 31ULL * dayTicks);
    CHECK(WriteRaw(currentRecent + L".tmp", { 1, 2, 3, 4 }));
    Autosave::OrphanSession scanned = {};
    CHECK(!Autosave::ScanForOrphan(&scanned));
    CHECK(IsRegularFile(currentRecent));
    CHECK(IsRegularFile(currentRecent + L".tmp"));
    CHECK(DeleteFileW((currentRecent + L".tmp").c_str()) != FALSE);
    SetLastWriteTicks(currentRecent, NowTicks());

    // The production scanner must preserve a legacy-PID autosave when a real
    // live sibling has an unreadable image name, a matching basename under a
    // different full path, or an unreadable own module name. The hook replaces
    // only image-name reads; ScanForOrphan still opens and classifies the child
    // process through the production liveness path.
    PROCESS_INFORMATION liveSibling = StartLiveSibling();
    CHECK(liveSibling.hProcess != NULL);
    if (liveSibling.hProcess != NULL)
    {
        CloseHandle(liveSibling.hThread);
        g_legacyProcessProbePid = liveSibling.dwProcessId;
        const LegacyProcessProbeMode modes[] = {
            LegacyProcessProbeMode::QueryFails,
            LegacyProcessProbeMode::BasenameMatches,
            LegacyProcessProbeMode::OwnNameFails,
        };
        const char* labels[] = {
            "unreadable sibling image name is conservative-live",
            "matching basename under another full path is live",
            "unreadable own module name is conservative-live",
        };
        for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i)
        {
            const std::wstring legacyLive =
                dir + L"\\autosave-" + std::to_wstring(liveSibling.dwProcessId)
                + L"-recent.alo";
            CHECK(CopyFileW(currentRecent.c_str(), legacyLive.c_str(), FALSE) != FALSE);
            SetLastWriteTicks(legacyLive, NowTicks());
            g_legacyProcessProbeMode = modes[i];
            Autosave::SetProcessNameProbeForTest(ProbeLegacyProcessName);
            // CHECK() is 1-arg (prints #cond); print the per-mode label
            // explicitly so a failure names which classification regressed.
            if (Autosave::ScanForOrphan(&scanned) || !IsRegularFile(legacyLive))
            {
                std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, labels[i]);
                ++g_fail;
            }
            CHECK(DeleteFileW(legacyLive.c_str()) != FALSE);
            Autosave::SetProcessNameProbeForTest(NULL);
            g_legacyProcessProbePid = liveSibling.dwProcessId;
        }
        TerminateProcess(liveSibling.hProcess, 0);
        WaitForSingleObject(liveSibling.hProcess, INFINITE);
        CloseHandle(liveSibling.hProcess);
    }

    const DWORD deadPid = DefinitelyDeadPid();
    CHECK(deadPid != 0);
    if (deadPid != 0)
    {
        // the exact production push: a fresh dead-session `.tmp` is
        // deleted immediately. Removing sweepList.push_back leaves this file.
        const std::wstring deadTmp =
            dir + L"\\" + MakeName(deadPid, 1, L"-recent.alo") + L".tmp";
        CHECK(WriteRaw(deadTmp, { 9, 8, 7 }));
        CHECK(!Autosave::ScanForOrphan(&scanned));
        CHECK(!IsRegularFile(deadTmp));

        // Legacy PID-only recovery remains compatible under its conservative
        // liveness policy.
        const std::wstring legacy =
            dir + L"\\autosave-" + std::to_wstring(deadPid) + L"-recent.alo";
        CHECK(CopyFileW(currentRecent.c_str(), legacy.c_str(), FALSE) != FALSE);
        SetLastWriteTicks(legacy, NowTicks());
        CHECK(Autosave::ScanForOrphan(&scanned));
        CHECK(scanned.pid == deadPid && !scanned.hasCreationTime);
        CHECK(_wcsicmp(scanned.recentPath.c_str(), legacy.c_str()) == 0);
        Autosave::DeleteOrphan(scanned);
        CHECK(!IsRegularFile(legacy));
    }

    // PID reuse: the live current PID with a DIFFERENT creation token is an
    // orphan, and two such tokens never merge into one session. Make the stable
    // session newer so the exact expected result is deterministic.
    unsigned long long oldCreationA = currentName.creationTime100ns ^ 1ULL;
    if (oldCreationA == 0 || oldCreationA == currentName.creationTime100ns)
        oldCreationA = 1;
    unsigned long long oldCreationB = oldCreationA + 1;
    if (oldCreationB == 0 || oldCreationB == currentName.creationTime100ns)
        ++oldCreationB;

    const std::wstring oldRecent =
        dir + L"\\" + MakeName(GetCurrentProcessId(), oldCreationA, L"-recent.alo");
    const std::wstring oldStable =
        dir + L"\\" + MakeName(GetCurrentProcessId(), oldCreationB, L"-stable.alo");
    CHECK(CopyFileW(currentRecent.c_str(), oldRecent.c_str(), FALSE) != FALSE);
    CHECK(CopyFileW(currentRecent.c_str(), oldStable.c_str(), FALSE) != FALSE);
    const ULONGLONG now = NowTicks();
    SetLastWriteTicks(oldRecent, now - 2ULL * 10000000ULL);
    SetLastWriteTicks(oldStable, now - 1ULL * 10000000ULL);

    CHECK(Autosave::ScanForOrphan(&scanned));
    CHECK(scanned.pid == GetCurrentProcessId());
    CHECK(scanned.hasCreationTime);
    CHECK(scanned.creationTime100ns == oldCreationB);
    CHECK(scanned.recentPath.empty()); // never merged from token A
    CHECK(_wcsicmp(scanned.stablePath.c_str(), oldStable.c_str()) == 0);
    Autosave::DeleteOrphan(scanned);
    CHECK(!IsRegularFile(oldStable));
    CHECK(IsRegularFile(oldRecent));

    CHECK(Autosave::ScanForOrphan(&scanned));
    CHECK(scanned.creationTime100ns == oldCreationA);
    CHECK(_wcsicmp(scanned.recentPath.c_str(), oldRecent.c_str()) == 0);
    CHECK(scanned.stablePath.empty());
    Autosave::DeleteOrphan(scanned);
    CHECK(!IsRegularFile(oldRecent));

    Autosave::DeleteOurSession();
    CHECK(!IsRegularFile(currentRecent));
    CHECK(!IsRegularFile(currentStable));
    CHECK(!IsRegularFile(currentMeta));

    if (g_fail == 0) std::printf("=== AutosaveRecover: ALL PASS ===\n");
    else             std::printf("=== AutosaveRecover: %d FAILURE(S) ===\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
