#pragma once
// StringConv.h — the one UTF-8 ↔ UTF-16 (std::wstring) conversion pair for the
// host layer (DRY audit 2026-06-22, cpp-host-0). Consolidates: BridgeDispatcher's
// Utf8ToWide/WideToUtf8 and HostWindow's Utf8ToUtf16/Utf16ToUtf8 (two byte-identical
// pairs that had drifted in NAME only), plus HostBridgeProxy's INLINED BSTR
// conversions (same underlying MultiByteToWideChar/WideCharToMultiByte calls,
// re-expressed here via std::wstring). `inline` so every translation unit shares
// one definition with no ODR conflict.
//
// Canonical names: "Wide" == UTF-16 on Windows (std::wstring), matching the
// project's existing AnsiToWide/WideToAnsi convention in utils.h.

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace host {

inline std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

inline std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

} // namespace host
