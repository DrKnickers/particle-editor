#pragma once
#include <fstream>
#include <string>
#include <vector>

// --record pump-schedule trace (PR 12 / an-audit-finding). A flag-gated (PE_RECORD_TRACE)
// oracle that records, per emitted frame, the ORDERED sequence of pump phase
// tokens AS THEY EXECUTE. Its whole purpose is a before/after byte-compare
// across the RecordSession extraction: the barrier/ack/grab pump is the
// preserved-verbatim mechanism whose *ordering* the extraction risks, and a
// pixel compare cannot prove ordering (on a fast idle machine a
// capture-before-barrier reorder still yields byte-identical pixels while
// producing stale frames under compositor load — HostWindow.cpp's barrier
// exists precisely to defeat asynchronous DComp composition). This trace is
// deterministic even when the pixels are not.
//
// DETERMINISM CONTRACT — record decisions + CONFIGURED policy only:
//   * Phase tokens are emitted at each phase's IMPLEMENTATION SITE (adjacent to
//     GrabWindowPixels, at the barrier loop, at the ack wait / cursor-tick), NOT
//     at a callback's entry — so a refactor that moves the real grab above the
//     barrier within the capture lambda shows up as a reordered trace line.
//   * The token ORDER within a frame is the signal (a set of presence-booleans
//     would compare equal under exactly the stale-pixel reorders this catches).
//   * NEVER record runtime iteration counts or probe outcomes: the adaptive
//     barrier's actual flush count and DwmGetCompositionTimingInfo success are
//     timing-dependent and would false-reject unchanged code. Those live in
//     RecordTiming (the summary), excluded from the compare. What IS recorded is
//     the *configured* cap and compositor-advance target — constexpr policy that
//     a weakening extraction (cap 3->1, advance +2->+1) would change.
//
// Header-only: a std::ofstream member with inline methods, one line flushed per
// frame. No .cpp, so no build-registration churn.

namespace host {

class RecordTrace {
public:
    explicit RecordTrace(const std::wstring& path)
        : m_out(path, std::ios::binary | std::ios::trunc) {}

    bool Ok() const { return m_out.is_open(); }

    // Append one phase token to the CURRENT frame's ordered sequence.
    void Emit(std::string token) { m_tokens.push_back(std::move(token)); }

    // Flush the current frame's ordered tokens as one line ("f<idx> t0 t1 ...")
    // and reset for the next frame. LF-terminated + explicit flush so a crashed
    // run still leaves a compareable prefix and the newline stays platform-fixed
    // (the file is opened binary — no CRLF translation to perturb byte-compare).
    void EndFrame(int idx)
    {
        std::string line = "f" + std::to_string(idx);
        for (const std::string& t : m_tokens) { line += ' '; line += t; }
        line += '\n';
        m_out.write(line.data(), static_cast<std::streamsize>(line.size()));
        m_out.flush();
        m_tokens.clear();
    }

private:
    std::ofstream            m_out;
    std::vector<std::string> m_tokens;   // current frame, ordered by emission
};

}  // namespace host
