#pragma once
#include <string>
#include <vector>
#include <cwctype>

// --record output-directory confinement (2026-07 audit, an-audit-finding).
//
// ClipTimeline validates `out` as a relative, traversal-free path — which stops
// a timeline escaping the launch directory, but NOT from destroying an
// arbitrary EXISTING directory under it. The publish step is an unconditional
// `remove_all(recordOutDir)` followed by a rename, so a timeline whose `out`
// happened to name a real directory ("captures", "docs", "src") silently
// deleted it. The audit proved this with an actual Release run: `out: "victim"`
// exited 0 and the sentinel file inside was gone.
//
// The fix is NOT to forbid overwriting — re-shooting a clip into the same
// directory is the normal workflow, and the whole pipeline depends on it. The
// fix is to overwrite ONLY a directory that is either absent, empty, or holds
// nothing but a previous record's own output. Anything else is refused and the
// run fails loudly rather than deleting a stranger's files.
//
// Pure and header-only (takes an already-enumerated listing, no filesystem
// calls) so tests/test_record_output_safety.cpp can exercise it directly.

namespace recordsafety {

// A file a --record publish is allowed to have created itself:
//   frame_00000.png … frame_99999.png
//   cursor-sidecar.json
inline bool IsRecordArtifactName(const std::wstring& name)
{
    if (name == L"cursor-sidecar.json") return true;

    // frame_<digits>.png — at least one digit, digits only.
    const std::wstring prefix = L"frame_";
    const std::wstring suffix = L".png";
    if (name.size() <= prefix.size() + suffix.size()) return false;
    if (name.compare(0, prefix.size(), prefix) != 0) return false;
    if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) return false;

    const size_t digitsBegin = prefix.size();
    const size_t digitsEnd   = name.size() - suffix.size();
    for (size_t i = digitsBegin; i < digitsEnd; ++i)
    {
        if (name[i] < L'0' || name[i] > L'9') return false;
    }
    return true;
}

// Decide whether the publish step may `remove_all` the output directory.
//
// `exists`  — does the directory exist at all?
// `entries` — its immediate children (file AND directory names). A record
//             output is flat, so ANY subdirectory is foreign by definition and
//             is rejected via the same name check.
//
// Returns true when overwriting is safe. On false, `reason` explains which
// entry blocked it so the operator can see exactly what was protected.
inline bool MayReplaceOutputDir(bool exists,
                                const std::vector<std::wstring>& entries,
                                std::wstring& reason)
{
    reason.clear();
    if (!exists) return true;          // nothing to destroy
    if (entries.empty()) return true;  // empty dir: safe to replace

    for (const std::wstring& e : entries)
    {
        if (!IsRecordArtifactName(e))
        {
            reason = L"contains '" + e + L"', which is not a --record output file";
            return false;
        }
    }
    return true;   // exclusively this pipeline's own prior output
}

} // namespace recordsafety
