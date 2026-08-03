#ifndef MOD_SCAN_H
#define MOD_SCAN_H
// Win32 directory-scan layer for mod-layer discovery. Kept separate from the
// pure ModLayers.h because it needs <windows.h>; both ModManager.cpp and the catalog
// unit test include this (no engine deps), so the test links without the engine.
#include <string>
#include <vector>
#include <algorithm>
#include <windows.h>
#include "ModLayers.h"   // modlayers::CanonicalizeLayerPath

// A selectable content layer: a folder carrying a Data\Art tree.
struct LayerRef
{
    std::wstring path;   // absolute, slash-free (canonical)
    std::wstring label;  // folder name (display)
};

// Immediate child folders of modRoot that carry a Data\Art tree, as absolute
// slash-free paths + folder-name labels, sorted case-insensitively.
inline void ScanModNestedLayers(const std::wstring& modRoot, std::vector<LayerRef>& out)
{
    out.clear();
    if (modRoot.empty()) return;
    const std::wstring base = modlayers::CanonicalizeLayerPath(modRoot) + L"\\";

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW((base + L"*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        const std::wstring art = base + fd.cFileName + L"\\Data\\Art";
        const DWORD attr = GetFileAttributesW(art.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
        out.push_back(LayerRef{ modlayers::CanonicalizeLayerPath(base + fd.cFileName), fd.cFileName });
    }
    while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    std::sort(out.begin(), out.end(), [](const LayerRef& a, const LayerRef& b) {
        return _wcsicmp(a.label.c_str(), b.label.c_str()) < 0;
    });
}

// Whether modRoot itself carries a Data\Art tree (the mod root is a layer too).
inline bool ModRootHasArt(const std::wstring& modRoot)
{
    const std::wstring art = modlayers::CanonicalizeLayerPath(modRoot) + L"\\Data\\Art";
    const DWORD attr = GetFileAttributesW(art.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
#endif // MOD_SCAN_H
