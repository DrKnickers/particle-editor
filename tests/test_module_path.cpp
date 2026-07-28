// Regression test for the module-path grow-until-it-fits loop
// (src/host/ModulePath.h).
//
// The defect this pins: GetModuleFileNameW into a fixed MAX_PATH buffer does
// not fail on a long path, it TRUNCATES — and a truncated module path is a
// perfectly well-formed string pointing at the wrong directory, so the
// three-parent walk that derives the app.local mapping silently resolves
// somewhere that does not exist (2026-07 audit, an-audit-finding).
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

#include <cstdio>
#include <functional>
#include <string>

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

int main()
{
    std::printf("test_module_path\n");

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

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
