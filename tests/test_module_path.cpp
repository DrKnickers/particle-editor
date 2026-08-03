// Regression test for the module-path grow-until-it-fits loop
// (src/host/ModulePath.h).
//
// The defect this pins: GetModuleFileNameW into a fixed MAX_PATH buffer does
// not fail on a long path, it TRUNCATES — and a truncated module path is a
// perfectly well-formed string pointing at the wrong directory, so a caller like
// BundledWebView2SetupPath silently looks for the bootstrapper beside the WRONG
// directory (2026-07 audit, D-WV-04). (The app.local UI is now embedded in the
// exe, so the former three-parent dist walk that also relied on this read is gone.)
//
// Every truncation case therefore asserts the SPECIFIC wrong value the old
// fixed-buffer code produced — not merely "we got something" — because
// "non-empty" would pass against the bug. The short-path and exact-fit cases
// are the other half: they fail if the fix OVERREACHES and starts rejecting
// reads that were always fine, which would take the editor from "blank window"
// to "will not start at all".
//
// Header-only; see tests/build_test_module_path.bat.

#include "host/ModulePath.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

static int g_failed = 0;

#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

// A fake GetModuleFileNameW over a path of `full.size()` characters.
// Reproduces the documented contract: when the buffer cannot hold path + NUL it
// fills the buffer completely, returns the buffer size, and reports
// ERROR_INSUFFICIENT_BUFFER.
struct FakeModule
{
    std::wstring full;
    int          calls     = 0;
    bool         hardFail  = false;   // return 0: the call genuinely failed
    bool         neverFits = false;   // report too-small whatever the size

    host::ModuleNameProbeResult operator()(wchar_t* buf, DWORD cap)
    {
        ++calls;
        host::ModuleNameProbeResult r;

        if (hardFail)
        {
            r.written   = 0;
            r.lastError = ERROR_ACCESS_DENIED;
            return r;
        }

        if (neverFits || full.size() + 1 > cap)
        {
            for (DWORD i = 0; i < cap; ++i) buf[i] = full[i % full.size()];
            r.written   = cap;
            r.lastError = ERROR_INSUFFICIENT_BUFFER;
            return r;
        }

        for (size_t i = 0; i < full.size(); ++i) buf[i] = full[i];
        buf[full.size()] = L'\0';
        r.written   = static_cast<DWORD>(full.size());
        r.lastError = ERROR_SUCCESS;
        return r;
    }
};

static bool IsOwnedTempRoot(const std::filesystem::path& tempBase,
                            const std::filesystem::path& root)
{
    const std::wstring leaf = root.filename().wstring();
    return root.parent_path() == tempBase &&
           leaf.rfind(L"alo_module_path_", 0) == 0;
}

static int RunProductionBindingChild(const std::wstring& expectedPath)
{
    const std::wstring got = host::ModuleFilePath();
    const std::wstring truncated =
        expectedPath.substr(0, (std::min)(expectedPath.size(),
                                         static_cast<size_t>(MAX_PATH - 1)));

    CHECK(expectedPath.size() > MAX_PATH,
          "child executable really is beyond MAX_PATH");
    CHECK(got != truncated,
          "production binding does NOT return the old 259-character prefix");
    CHECK(got == expectedPath,
          "production ModuleFilePath returns the exact long executable path");
    CHECK(got.size() == expectedPath.size(),
          "production path length is complete, not merely non-empty");
    return g_failed ? 1 : 0;
}

static void ExerciseProductionBindingBeyondMaxPath()
{
    wchar_t tempBuffer[MAX_PATH] = {};
    const DWORD tempLength = GetTempPathW(MAX_PATH, tempBuffer);
    CHECK(tempLength > 0 && tempLength < MAX_PATH,
          "temporary directory is available for long-path child");
    if (tempLength == 0 || tempLength >= MAX_PATH) return;

    std::filesystem::path tempBase =
        std::filesystem::path(tempBuffer).lexically_normal();
    if (tempBase.filename().empty()) tempBase = tempBase.parent_path();
    const std::wstring leaf =
        L"alo_module_path_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
        std::to_wstring(GetTickCount64());
    const std::filesystem::path nativeRoot = tempBase / leaf;
    CHECK(IsOwnedTempRoot(tempBase, nativeRoot),
          "cleanup root is the exact PID-scoped child of the temp directory");
    if (!IsOwnedTempRoot(tempBase, nativeRoot)) return;

    // Claim the exact root atomically. A false return with no error means the
    // name already existed; in that case it is not ours and must never be
    // traversed or removed.
    std::error_code ec;
    const bool madeRoot = std::filesystem::create_directory(nativeRoot, ec);
    CHECK(madeRoot && !ec,
          "atomically created a fresh PID-scoped long-path test root");
    if (!madeRoot || ec) return;

    const std::filesystem::path extendedRoot =
        std::filesystem::path(L"\\\\?\\" + nativeRoot.wstring());
    std::filesystem::path childDirectory = extendedRoot;
    const std::wstring segment =
        L"module_path_segment_0123456789abcdef0123456789abcdef";
    while ((childDirectory / L"test_module_path.exe").wstring().size() <=
           MAX_PATH + 80)
    {
        childDirectory /= segment;
    }
    const std::filesystem::path childPath =
        childDirectory / L"test_module_path.exe";

    // Cleanup is armed only after the root above was proven newly created.
    const bool madeDirectories =
        std::filesystem::create_directories(childDirectory, ec);
    CHECK(madeDirectories && !ec,
          "created fresh descendants below the owned long-path root");
    if (!madeDirectories || ec)
    {
        ec.clear();
        std::filesystem::remove_all(extendedRoot, ec);
        CHECK(!ec, "removed the owned root after descendant setup failed");
        return;
    }

    const std::wstring parentPath = host::ModuleFilePath();
    CHECK(!parentPath.empty() && parentPath.size() < MAX_PATH,
          "ordinary production module path remains a valid short-path control");
    CHECK(std::filesystem::exists(parentPath),
          "ordinary production module path names the running executable");

    bool copied = false;
    bool childStopped = true;
    if (!parentPath.empty())
    {
        copied = CopyFileW(parentPath.c_str(), childPath.c_str(), TRUE) != FALSE;
    }
    CHECK(copied, "copied the real test executable to the long path");

    if (copied)
    {
        std::wstring commandLine =
            L"\"" + childPath.wstring() + L"\" --production-binding-child \"" +
            childPath.wstring() + L"\"";
        std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
        mutableCommand.push_back(L'\0');

        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process = {};
        const BOOL launched =
            CreateProcessW(childPath.c_str(), mutableCommand.data(), nullptr,
                           nullptr, FALSE, 0, nullptr, nullptr, &startup, &process);
        CHECK(launched != FALSE,
              "launched the real executable through its extended-length path");

        if (launched)
        {
            const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
            DWORD exitCode = STILL_ACTIVE;
            if (wait == WAIT_OBJECT_0)
                GetExitCodeProcess(process.hProcess, &exitCode);
            else
            {
                // This is the exact child created above. Stop it before closing
                // its handles so remove_all cannot race a still-running image.
                TerminateProcess(process.hProcess, ERROR_TIMEOUT);
                childStopped =
                    WaitForSingleObject(process.hProcess, 5000) ==
                    WAIT_OBJECT_0;
                CHECK(childStopped,
                      "timed-out long-path child stopped before cleanup");
            }
            CHECK(wait == WAIT_OBJECT_0,
                  "long-path production child completed within 30 seconds");
            CHECK(exitCode == 0,
                  "long-path production binding child passed exact-value checks");
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
    }

    // If even termination did not stop our child, leave the validated temp
    // root in place rather than attempting a partial delete under a live image.
    if (!childStopped) return;

    ec.clear();
    const uintmax_t removed = std::filesystem::remove_all(extendedRoot, ec);
    CHECK(!ec && removed > 0,
          "removed only the validated PID-scoped long-path test root");
    CHECK(!std::filesystem::exists(extendedRoot),
          "long-path test root is gone after cleanup");
}

int wmain(int argc, wchar_t** argv)
{
    std::printf("test_module_path\n");

    if (argc == 3 && std::wstring(argv[1]) == L"--production-binding-child")
        return RunProductionBindingChild(argv[2]);

    // --- 1. The overreach guard. An ordinary install path fits on the first
    // call and must come back byte-for-byte. A fix that grew too eagerly, or
    // that treated any ERROR_INSUFFICIENT_BUFFER-shaped result as fatal, fails
    // here — and failing here means the editor cannot start at all.
    {
        FakeModule f;
        f.full = L"C:\\Program Files\\AloParticleEditor\\x64\\Release\\ParticleEditor.exe";
        const std::wstring got = host::ModuleFilePathWith(std::ref(f));
        CHECK(got == f.full, "short path returned verbatim (overreach guard)");
        CHECK(f.calls == 1,  "short path costs exactly one probe");
    }

    // --- 2. THE REVERT ASSERTION. A 409-character path does not fit MAX_PATH.
    // The old fixed-buffer code returned the first 259 characters; assert we do
    // NOT return that prefix, and DO return the whole thing. Asserting only
    // "non-empty" would pass against the bug.
    {
        FakeModule f;
        f.full  = std::wstring(390, L'd');
        f.full += L"\\ParticleEditor.exe";           // 390 + 19 == 409

        const std::wstring truncated = f.full.substr(0, MAX_PATH - 1);   // the bug's output
        const std::wstring got       = host::ModuleFilePathWith(std::ref(f));

        CHECK(got != truncated,  "long path is NOT the 259-char truncation the bug returned");
        CHECK(got == f.full,     "long path returned in full");
        CHECK(got.size() == 409, "long path is 409 chars, not MAX_PATH-1");
    }

    // --- 3. The doubling actually happens, and terminates. 260 -> 520 -> 1040
    // covers a 700-char path on the third probe.
    {
        FakeModule f;
        f.full = std::wstring(700, L'x');
        const std::wstring got = host::ModuleFilePathWith(std::ref(f));
        CHECK(got.size() == 700, "700-char path fully read");
        CHECK(f.calls == 3,      "buffer doubled twice (260 -> 520 -> 1040)");
    }

    // --- 4. A genuine API failure yields empty, not a partial buffer. Empty is
    // the caller's only failure signal, so a partially-filled return here would
    // be read as a valid path.
    {
        FakeModule f;
        f.full     = L"C:\\whatever\\ParticleEditor.exe";
        f.hardFail = true;
        const std::wstring got = host::ModuleFilePathWith(std::ref(f));
        CHECK(got.empty(),  "hard failure returns empty");
        CHECK(f.calls == 1, "hard failure does not retry");
    }

    // --- 5. The ceiling holds. A probe that never fits must stop, not grow
    // forever — an unbounded loop would hang startup rather than fail it.
    {
        FakeModule f;
        f.full      = L"C:\\x\\ParticleEditor.exe";
        f.neverFits = true;
        const std::wstring got = host::ModuleFilePathWith(std::ref(f));
        CHECK(got.empty(), "never-fits returns empty rather than looping");
        // 260 doubling to >= 32768 is eight probes. A runaway loop fails by
        // hanging, so pin the count too.
        CHECK(f.calls > 0 && f.calls <= 10, "never-fits gives up within ten probes");
    }

    // --- 6. Exact-fit boundary, the second overreach guard. A path that fills
    // the buffer with exactly one slot to spare for the terminator DID fit. An
    // off-by-one in the size comparison would either grow needlessly or, worse,
    // hand back a buffer-filling read as complete.
    {
        FakeModule f;
        f.full = std::wstring(MAX_PATH - 1, L'e');   // 259 chars + NUL == 260
        const std::wstring got = host::ModuleFilePathWith(std::ref(f));
        CHECK(got == f.full, "exact-fit path accepted without growing");
        CHECK(f.calls == 1,  "exact-fit costs exactly one probe");
    }

    // --- 7. PRODUCTION CALL SITE. Run this same executable from an actual
    // extended-length path. This invokes ModuleFilePath(), not the injectable
    // ModuleFilePathWith helper, so restoring the old fixed-MAX_PATH production
    // binding returns the specific 259-character prefix and fails in the child.
    ExerciseProductionBindingBeyondMaxPath();

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
