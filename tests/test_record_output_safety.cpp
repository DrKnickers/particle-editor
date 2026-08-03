// Unit test for src/host/RecordOutputSafety.h (2026-07 audit, B-REC-001).
//
// The audit proved with an actual Release run that a --record timeline whose
// `out` named an existing directory deleted that directory's contents: it
// created victim/sentinel-do-not-preserve.txt, ran with out:"victim", got exit
// 0, and the sentinel was gone. `out` validation rejects absolute paths,
// drive letters and "..", so the run never ESCAPED the launch directory -- it
// destroyed something inside it, which the traversal rules were never checking.
//
// MayReplaceOutputDir is the confinement predicate: overwrite only an absent,
// empty, or purely-record-output directory. Re-shooting a clip into the same
// place (the pipeline's normal workflow) must keep working.

#include "RecordOutputSafety.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

using recordsafety::IsRecordArtifactName;
using recordsafety::MayReplaceOutputDir;

int main()
{
    std::printf("test_record_output_safety\n");

    // --- artifact-name recognition -----------------------------------------
    CHECK(IsRecordArtifactName(L"frame_00000.png"), "frame_00000.png is a record artifact");
    CHECK(IsRecordArtifactName(L"frame_99999.png"), "frame_99999.png is a record artifact");
    CHECK(IsRecordArtifactName(L"frame_7.png"),     "short digit run still counts");
    CHECK(IsRecordArtifactName(L"cursor-sidecar.json"), "cursor sidecar is a record artifact");

    CHECK(!IsRecordArtifactName(L"frame_.png"),        "no digits -> not an artifact");
    CHECK(!IsRecordArtifactName(L"frame_00a01.png"),   "non-digit in the run -> not an artifact");
    CHECK(!IsRecordArtifactName(L"frame_00001.txt"),   "wrong extension -> not an artifact");
    CHECK(!IsRecordArtifactName(L"myframe_00001.png"), "wrong prefix -> not an artifact");
    CHECK(!IsRecordArtifactName(L"notes.txt"),         "ordinary file -> not an artifact");
    CHECK(!IsRecordArtifactName(L"assets"),            "a subdirectory name -> not an artifact");

    std::wstring why;

    // --- the safe cases: overwriting must keep working ----------------------
    CHECK(MayReplaceOutputDir(false, {}, why), "absent dir: safe to create");
    CHECK(MayReplaceOutputDir(true, {}, why),  "existing EMPTY dir: safe to replace");

    {
        // The normal re-shoot: the directory holds only the previous run's output.
        const std::vector<std::wstring> prior = {
            L"frame_00000.png", L"frame_00001.png", L"frame_00002.png", L"cursor-sidecar.json",
        };
        CHECK(MayReplaceOutputDir(true, prior, why),
              "dir holding ONLY prior record output: safe to replace (re-shoot works)");
    }

    // --- the audit's repro: a foreign file must block the publish -----------
    {
        const std::vector<std::wstring> victim = { L"sentinel-do-not-preserve.txt" };
        const bool may = MayReplaceOutputDir(true, victim, why);
        CHECK(!may, "the audit's victim/ dir is REFUSED (sentinel would have been deleted)");
        CHECK(why.find(L"sentinel-do-not-preserve.txt") != std::wstring::npos,
              "refusal names the exact entry that blocked it");
    }

    {
        // The nastier shape: mostly-record output with one real file mixed in.
        // A count- or majority-based check would wave this through.
        const std::vector<std::wstring> mixed = {
            L"frame_00000.png", L"frame_00001.png", L"important-notes.md",
        };
        CHECK(!MayReplaceOutputDir(true, mixed, why),
              "ONE foreign file among real frames still refuses (no majority rule)");
    }

    {
        // A record output is flat, so any subdirectory is foreign by definition.
        const std::vector<std::wstring> nested = { L"frame_00000.png", L"assets" };
        CHECK(!MayReplaceOutputDir(true, nested, why),
              "a subdirectory makes the dir foreign (record output is flat)");
    }

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
