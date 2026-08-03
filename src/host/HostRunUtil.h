#ifndef HOST_RUN_UTIL_H
#define HOST_RUN_UTIL_H
//
// Tiny QPC + path helpers shared by HostWindowImpl::Run's pump and the
// one-shot runners (CaptureRunner). Formerly HostWindow.cpp file-statics;
// hoisted (inline, one logical definition) for the Phase C extraction —
// tasks/2026-07-06-heavyweight-refactor-plan.md.

#include <windows.h>

#include <string>

namespace host {

// The QPC frequency is fixed for the process lifetime, so cache it once.
inline LONGLONG PerfQpcFreq()
{
    static LONGLONG freq = 0;
    if (freq == 0) { LARGE_INTEGER f; if (QueryPerformanceFrequency(&f)) freq = f.QuadPart; }
    return freq;
}
inline LONGLONG PerfQpcNow()
{
    LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart;
}

// Elapsed milliseconds from a QPC tick delta. Guarded for freq<=0 (QPC
// unavailable); callers treat 0.0 as "no time elapsed" and fall back to an
// iteration counter.
inline double QpcMs(LONGLONG deltaTicks, LONGLONG freq)
{
    return freq > 0
        ? static_cast<double>(deltaTicks) * 1000.0 / static_cast<double>(freq)
        : 0.0;
}

// Insert `suffix` before the filename's extension, e.g. ("C:\\a\\b.png",
// "-composite") -> "C:\\a\\b-composite.png". The last dot is accepted as an
// extension only if it falls after the last path separator, so a dot in a
// directory name (C:\\my.dir\\refobj) isn't mistaken for an extension; a
// dotless filename gets ".png" appended.
inline std::wstring DeriveSibling(const std::wstring& path, const std::wstring& suffix)
{
    const size_t sep = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos && (sep == std::wstring::npos || dot > sep))
    {
        std::wstring out = path;
        out.insert(dot, suffix);
        return out;
    }
    return path + suffix + L".png";
}

}  // namespace host

#endif  // HOST_RUN_UTIL_H
