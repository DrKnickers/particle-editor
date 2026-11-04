#pragma once
#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// Automation runs (--drive, --record, --observe, --replay-proof) each write
// host-<mode>-<pid>.log beside host.log, and nothing deleted them: 430 piled up in two days of
// observe and replay-proof lanes. A run cannot delete its own log on the way out, because
// callers read it after the process exits (scripts/clip-batch/render-all.mjs,
// scripts/wiki-media/build.mjs, scripts/verify-headless-fatal-no-hang.ps1). render-all runs
// several records at once, so "its PID has exited" cannot mean "delete it" either: a sibling
// would delete a just-finished log before it is read. Retention therefore goes by count and
// age. Header-only so tests/test_host_log_retention.cpp can drive it against a temp directory.

namespace host
{

// True when `leaf` is exactly <stem>-<mode>-<digits>.log for a mode OpenLog writes, where
// <stem> is the canonical log's name without ".log" ("host", or "AloParticleEditor_host" in the
// TEMP fallback). A mode missing here is never swept, which fails safe.
inline bool IsPerRunHostLogName(const std::wstring& leaf, const std::wstring& stem)
{
    const std::wstring prefix = stem + L"-";
    const std::wstring suffix = L".log";
    if (stem.empty() || leaf.size() <= prefix.size() + suffix.size()) return false;
    if (leaf.compare(0, prefix.size(), prefix) != 0) return false;
    if (leaf.compare(leaf.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    const std::wstring middle = leaf.substr(prefix.size(), leaf.size() - prefix.size() - suffix.size());
    for (const wchar_t* mode : {L"drive-", L"record-", L"observe-", L"replay-proof-"})
    {
        const std::wstring m = mode;
        if (middle.size() <= m.size() || middle.compare(0, m.size(), m) != 0) continue;
        const std::wstring pid = middle.substr(m.size());
        return pid.size() <= 10 &&
               std::all_of(pid.begin(), pid.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
    }
    return false;
}

// Best effort: beside `canonicalLog`, deletes the per-run logs that are BOTH outside the
// `keepNewest` most recently written AND older than `grace` at `now`. A live run's log is open
// without delete sharing, so deleting it fails and is ignored. Returns how many were deleted.
inline int SweepPerRunHostLogs(const std::filesystem::path&              canonicalLog,
                               std::filesystem::file_time_type           now,
                               size_t                                    keepNewest,
                               std::filesystem::file_time_type::duration grace)
{
    struct Log
    {
        std::filesystem::path           path;
        std::filesystem::file_time_type written;
    };
    const std::wstring stem = canonicalLog.stem().wstring();
    std::vector<Log>   logs;
    std::error_code    error;
    for (std::filesystem::directory_iterator it(canonicalLog.parent_path(), error), end; !error && it != end;
         it.increment(error))
    {
        std::error_code entryError;
        if (it->symlink_status(entryError).type() != std::filesystem::file_type::regular) continue;
        if (!IsPerRunHostLogName(it->path().filename().wstring(), stem)) continue;
        const auto written = it->last_write_time(entryError);
        if (!entryError) logs.push_back({it->path(), written});
    }
    std::sort(logs.begin(), logs.end(), [](const Log& a, const Log& b)
              { return a.written != b.written ? a.written > b.written : a.path < b.path; });
    int deleted = 0;
    for (size_t i = keepNewest; i < logs.size(); ++i)
    {
        if (now - logs[i].written < grace) continue;
        if (std::filesystem::remove(logs[i].path, error)) ++deleted;
    }
    return deleted;
}

} // namespace host
