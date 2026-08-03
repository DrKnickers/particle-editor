#ifndef HOST_SAVE_PATH_CONFINE_H
#define HOST_SAVE_PATH_CONFINE_H

// Confinement check for record-mode file/save targets: the target path must
// resolve UNDER the timeline-declared saveRoot. Deliberately header-only and
// free of WebView2/D3D9/editor includes so the standalone unit test
// (tests/test_clip_save_confinement.cpp) can build it device-free, per the
// wiki-media pipeline spec §1.3.
//
// Resolution model (Windows):
//  - saveRoot must EXIST (it is the stage tree) and is resolved to its FINAL
//    path (junctions/symlinks/8.3 short names) via GetFinalPathNameByHandleW.
//  - The target FILE may not exist yet, so its PARENT directory (which must
//    exist) is resolved the same way, then the final component is re-appended.
//    A final component of "." or ".." (or empty) is rejected outright.
//  - Compare is case-insensitive with a separator-boundary prefix check, so
//    "C:\stage-evil" can never pass for root "C:\stage". Same-volume falls out
//    of the prefix compare.

#include <windows.h>
#include <string>
#include <algorithm>

namespace clip {

inline std::wstring ConfineUtf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

inline std::string ConfineWideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// Resolve an EXISTING directory to its final, normalized path. Empty on failure.
inline std::wstring ResolveFinalDirPath(const std::wstring& dir) {
    HANDLE h = CreateFileW(dir.c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::wstring();
    wchar_t buf[4096];
    const DWORD n = GetFinalPathNameByHandleW(h, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])),
                                              FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return std::wstring();
    std::wstring out(buf, n);
    // GetFinalPathNameByHandleW returns \\?\C:\... (or \\?\UNC\server\share\...);
    // strip to the classic form so prefix compares work on either input style.
    if (out.rfind(L"\\\\?\\UNC\\", 0) == 0)      out = L"\\\\" + out.substr(8);
    else if (out.rfind(L"\\\\?\\", 0) == 0)       out = out.substr(4);
    // Normalize: no trailing separator (except a bare drive root "C:\").
    while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return out;
}

// True if `targetPath` (file; may not exist, parent must) resolves under
// `saveRoot` (must exist). On false, `err` explains which rule failed.
// `resolvedOut` (optional) receives the fully resolved target path on success.
inline bool ConfineSavePath(const std::wstring& saveRoot, const std::wstring& targetPath,
                            std::wstring* resolvedOut, std::string& err) {
    if (saveRoot.empty())  { err = "saveRoot is empty"; return false; }
    if (targetPath.empty()) { err = "target path is empty"; return false; }

    const std::wstring root = ResolveFinalDirPath(saveRoot);
    if (root.empty()) { err = "saveRoot does not exist or is not resolvable"; return false; }

    // Split target into parent + final component (accept either separator).
    std::wstring t = targetPath;
    std::replace(t.begin(), t.end(), L'/', L'\\');
    const size_t slash = t.find_last_of(L'\\');
    if (slash == std::wstring::npos) { err = "target path must be absolute"; return false; }
    const std::wstring parent = t.substr(0, slash);
    const std::wstring leaf   = t.substr(slash + 1);
    if (leaf.empty() || leaf == L"." || leaf == L"..") {
        err = "target final component must be a plain file name"; return false;
    }
    // Reject an NTFS alternate-data-stream leaf (e.g. "out.alo:hidden"): the ':'
    // would make the write target a stream, not the plain .alo the pipeline expects,
    // yet the parent still resolves under saveRoot. A plain filename never contains
    // ':' (the drive colon is before the last separator). Also reject a trailing dot
    // or space (Win32 strips them, so "a.alo " != "a.alo" would sneak past a compare).
    if (leaf.find(L':') != std::wstring::npos) {
        err = "target final component must not contain ':' (alternate data stream)"; return false;
    }
    if (leaf.back() == L'.' || leaf.back() == L' ') {
        err = "target final component must not end with '.' or space"; return false;
    }

    const std::wstring parentFinal = ResolveFinalDirPath(parent);
    if (parentFinal.empty()) { err = "target parent directory does not exist or is not resolvable"; return false; }

    // Case-insensitive, separator-boundary prefix check: parentFinal == root,
    // or parentFinal starts with root + '\'.
    auto lower = [](std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), ::towlower);
        return s;
    };
    const std::wstring rl = lower(root);
    const std::wstring pl = lower(parentFinal);
    const bool under = (pl == rl) ||
        (pl.size() > rl.size() && pl.compare(0, rl.size(), rl) == 0 && pl[rl.size()] == L'\\');
    if (!under) {
        err = "target resolves outside saveRoot (target parent: " + ConfineWideToUtf8(parentFinal)
            + ", saveRoot: " + ConfineWideToUtf8(root) + ")";
        return false;
    }
    if (resolvedOut) *resolvedOut = parentFinal + L"\\" + leaf;
    return true;
}

}  // namespace clip
#endif  // HOST_SAVE_PATH_CONFINE_H
