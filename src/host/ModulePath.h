#pragma once

// Path of the running executable, read the only way that is correct.
//
// GetModuleFileNameW must be called in a grow-until-it-fits loop, never into a
// bare fixed MAX_PATH buffer. Given a path longer than the buffer it TRUNCATES,
// returns the buffer size, and reports ERROR_INSUFFICIENT_BUFFER — and on
// Windows before Server 2003 SP1 it does not null-terminate what it wrote. A
// truncated read is the dangerous case precisely because it does not look like
// a failure: the caller gets a well-formed path string that points somewhere
// else, so every parent_path() walk built on it silently resolves to the wrong
// directory.
//
// That is load-bearing here. The release layout derives the app.local mapping
// by walking three parents up from the exe (see scripts/package-release.ps1,
// which documents the tree as load-bearing for exactly this reason), so a
// truncated read maps the virtual host at a directory that does not exist and
// the WebView shows ERR_NAME_NOT_RESOLVED with no other symptom — a blank
// window and exit code 0 (2026-07 audit, an-audit-finding). A user who extracts the
// release zip under a long path gets that and nothing to go on.
//
// The loop below is the shape BundledWebView2SetupPath already used correctly;
// this header is where it lives now so the sibling caller cannot get it wrong
// again. `Probe` is the injection seam: production supplies
// GetModuleFileNameW, and tests supply a fake that reproduces the truncation
// and failure contracts without needing a 400-character path on disk.

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace host
{

// One GetModuleFileNameW call's full outcome. The error code is part of the
// result rather than read from thread state so a test fake can reproduce the
// truncation contract without touching SetLastError.
struct ModuleNameProbeResult
{
    DWORD written;     // the API's return value: characters written, excluding NUL
    DWORD lastError;   // GetLastError() sampled immediately after the call
};

// The Win32 extended-length path ceiling. A read still failing at this size is
// not going to start succeeding, so stop rather than grow without bound.
const size_t kMaxModulePathChars = 32768;

// Returns the module path, or an EMPTY string if it could not be read in full.
// Empty is the only failure signal: a caller must never treat a short result as
// a usable path, because the whole point of this loop is that a truncated read
// is indistinguishable from a real one by inspection.
template <typename Probe>
inline std::wstring ModuleFilePathWith(Probe probe)
{
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;)
    {
        const ModuleNameProbeResult r = probe(buf.data(), static_cast<DWORD>(buf.size()));

        // Zero written = the call genuinely failed (not a size problem).
        if (r.written == 0) return std::wstring();

        // It fit only if BOTH hold: the count left room for a terminator, and
        // the API did not report the buffer as too small. Checking the count
        // alone is not enough — that is the off-by-one that lets a
        // buffer-filling path through as if it were complete.
        if (r.written < buf.size() && r.lastError != ERROR_INSUFFICIENT_BUFFER)
        {
            // Terminate explicitly: the pre-2003-SP1 contract does not.
            buf[r.written] = L'\0';
            return std::wstring(buf.data(), r.written);
        }

        if (buf.size() >= kMaxModulePathChars) return std::wstring();
        buf.resize(buf.size() * 2);
    }
}

// Production binding of the above.
inline std::wstring ModuleFilePath()
{
    return ModuleFilePathWith([](wchar_t* buf, DWORD cap) -> ModuleNameProbeResult
    {
        SetLastError(ERROR_SUCCESS);
        const DWORD n = GetModuleFileNameW(nullptr, buf, cap);
        ModuleNameProbeResult r;
        r.written   = n;
        r.lastError = GetLastError();
        return r;
    });
}

// Directory holding the running executable, without a trailing separator, or
// EMPTY when the module path could not be read. Callers must treat empty as a
// hard failure and say so — a path built from an empty root silently becomes
// relative to the process CWD, which is not the install directory.
inline std::wstring ModuleDirectory()
{
    const std::wstring p = ModuleFilePath();
    if (p.empty()) return std::wstring();
    return std::filesystem::path(p).parent_path().wstring();
}

}   // namespace host
