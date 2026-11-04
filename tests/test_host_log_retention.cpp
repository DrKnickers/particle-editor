// Unit test for src/host/HostLogRetention.h.
//
// Every --drive / --record / --observe / --replay-proof run leaves host-<mode>-<pid>.log beside
// host.log, and 430 of them piled up. Callers read a run's log after it exits, several runs at
// once, so the sweep may delete a per-run log only when it is both outside the newest N and
// older than the grace window. It must never touch host.log, anything not shaped like a
// per-run log, or a log a live run still has open.

#include "host/HostLogRetention.h"

#include <windows.h>
#include <share.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

static int g_failed = 0;
#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
        {                                                                                                              \
            std::printf("  ok: %s\n", msg);                                                                            \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            ++g_failed;                                                                                                \
            std::printf("  FAIL: %s\n", msg);                                                                          \
        }                                                                                                              \
    } while (0)

namespace fs = std::filesystem;
using std::chrono::hours;
using std::chrono::minutes;

static void Write(const fs::path& path, fs::file_time_type written)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "[host] === host session started ===";
    fs::last_write_time(path, written);
}

int main()
{
    std::printf("test_host_log_retention\n");

    // Only the exact per-run shapes OpenLog writes are candidates.
    CHECK(host::IsPerRunHostLogName(L"host-observe-4242.log", L"host"), "an observe log");
    CHECK(host::IsPerRunHostLogName(L"host-replay-proof-7.log", L"host"), "a replay-proof log");
    CHECK(host::IsPerRunHostLogName(L"host-record-12.log", L"host") &&
              host::IsPerRunHostLogName(L"host-drive-12.log", L"host"),
          "record and drive logs");
    CHECK(host::IsPerRunHostLogName(L"AloParticleEditor_host-drive-12.log", L"AloParticleEditor_host"),
          "a TEMP-fallback log");
    bool rejected = true;
    for (const wchar_t* leaf :
         {L"host.log", L"host-observe-.log", L"host-observe-12x.log", L"host-observe-12.txt", L"host-capture-12.log",
          L"host-replay-12.log", L"host-observe-12.log.bak", L"xhost-observe-12.log", L"host-observe-12345678901.log",
          L"AloParticleEditor_host-drive-12.log"})
        rejected = rejected && !host::IsPerRunHostLogName(leaf, L"host");
    CHECK(rejected, "host.log, other modes and every other shape are never candidates");

    const fs::path  root = fs::temp_directory_path() / (L"AloHostLogTest-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    fs::remove_all(root, error);
    const fs::path canonical = root / L"host.log";
    const auto     now       = fs::file_time_type::clock::now();

    // Five old logs and two fresh ones; keep the newest three.
    for (int i = 0; i < 5; ++i)
        Write(root / (L"host-observe-" + std::to_wstring(100 + i) + L".log"), now - hours(3) - minutes(i));
    Write(root / L"host-record-200.log", now - minutes(1));
    Write(root / L"host-record-201.log", now - minutes(2));
    Write(canonical, now - hours(48));
    Write(root / L"host-capture-300.log", now - hours(48));
    fs::create_directories(root / L"host-drive-400.log"); // a folder named like a log
    fs::last_write_time(root / L"host-drive-400.log", now - hours(48));

    const int deleted = host::SweepPerRunHostLogs(canonical, now, 3, hours(1));
    CHECK(deleted == 4, "the sweep reports the four logs beyond the newest three");
    CHECK(fs::exists(root / L"host-record-200.log") && fs::exists(root / L"host-record-201.log") &&
              fs::exists(root / L"host-observe-100.log"),
          "the newest three are kept");
    CHECK(!fs::exists(root / L"host-observe-101.log") && !fs::exists(root / L"host-observe-104.log"),
          "old logs beyond the newest three are deleted");
    CHECK(fs::exists(canonical) && fs::exists(root / L"host-capture-300.log") &&
              fs::is_directory(root / L"host-drive-400.log"),
          "host.log, unknown modes and a folder named like a log are kept");

    // The grace window protects a just-finished log however many newer ones exist.
    for (int i = 0; i < 4; ++i)
        Write(root / (L"host-record-" + std::to_wstring(500 + i) + L".log"), now - minutes(30 + i));
    CHECK(host::SweepPerRunHostLogs(canonical, now, 0, hours(1)) == 1 && fs::exists(root / L"host-record-503.log") &&
              fs::exists(root / L"host-record-200.log") && !fs::exists(root / L"host-observe-100.log"),
          "with nothing kept by count, only logs past the grace window go");

    // A live run's log is opened as OpenLog opens it; the sweep cannot delete it.
    const fs::path live = root / L"host-observe-600.log";
    Write(live, now - hours(5));
    FILE* open = _wfsopen(live.c_str(), L"a", _SH_DENYNO);
    CHECK(open != nullptr, "test holds a log open the way OpenLog does");
    host::SweepPerRunHostLogs(canonical, now + hours(2), 0, hours(1));
    CHECK(fs::exists(live), "a log a live run holds open survives");
    if (open) std::fclose(open);
    CHECK(host::SweepPerRunHostLogs(canonical, now + hours(2), 0, hours(1)) >= 1 && !fs::exists(live),
          "the same log goes once its run has closed it");

    fs::remove_all(root, error);
    std::printf("%s (%d failures)\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===", g_failed);
    return g_failed ? 1 : 0;
}
