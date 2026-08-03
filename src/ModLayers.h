#ifndef MOD_LAYERS_H
#define MOD_LAYERS_H
// Pure helpers for mod-layer load-order stacking. Header-only, no engine
// / Win32 / D3D deps (mirrors GizmoSizing.h / ManipReadout.h) so it unit-tests
// standalone. A "layer" is an absolute path to a folder; the canonical form is
// slash-free (display + compare + persistence), while FileManager content roots
// are slash-TERMINATED (getFile concatenates root + relative path).
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <cwchar>

namespace modlayers {

// Persist the chosen layer stack to the registry ONLY when the post-apply shader
// reload succeeded — so a failed reload never records a stack the next launch
// cannot render. Pure; unit-tested in tests/test_mod_layers.cpp.
inline bool ShouldPersistLayers(bool reloadOk)
{
    return reloadOk;
}

// Strip trailing whitespace and path separators. Casing preserved (Windows paths
// are case-insensitive, but on-disk casing is kept for display).
inline std::wstring CanonicalizeLayerPath(std::wstring p)
{
    while (!p.empty() && (p.back() == L' ' || p.back() == L'\t')) p.pop_back();
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/'))  p.pop_back();
    return p;
}

// Case-insensitive equality of canonicalized layer paths.
inline bool LayerPathsEqual(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(CanonicalizeLayerPath(a).c_str(),
                    CanonicalizeLayerPath(b).c_str()) == 0;
}

// The user's configured order and the subset currently usable by the runtime
// are deliberately different facts. A removable drive can be offline for one
// launch; that must not turn temporary unavailability into a persisted deletion.
struct ResolvedLayerStack
{
    std::vector<std::wstring> configured;  // canonical, deduplicated, may be unavailable
    std::wstring              primary;     // first configured path that currently exists
};

inline ResolvedLayerStack ResolveLayerStack(
    const std::vector<std::wstring>& layers,
    const std::function<bool(const std::wstring&)>& dirExists)
{
    ResolvedLayerStack out;
    for (const std::wstring& raw : layers)
    {
        const std::wstring c = CanonicalizeLayerPath(raw);
        if (c.empty()) continue;

        bool dup = false;
        for (const std::wstring& configured : out.configured)
            if (LayerPathsEqual(configured, c)) { dup = true; break; }
        if (dup) continue;

        out.configured.push_back(c);
        if (out.primary.empty() && dirExists(c))
            out.primary = c;
    }
    return out;
}

// Build FileManager content roots from an ordered layer stack (front = highest
// precedence). Each layer is canonicalized, kept only if dirExists() AND not a
// case-insensitive duplicate of an earlier kept layer, then emitted with a
// trailing backslash. "NOT Data\Art-gated" means there is no Data\Art-tree
// requirement (unlike the legacy submod gate in BuildModContentRoots) — but a
// uniform directory-EXISTENCE check IS applied to every layer, including the root
// (legacy pushed the mod root with no check at all).
inline std::vector<std::wstring> BuildContentRoots(
    const std::vector<std::wstring>& layers,
    const std::function<bool(const std::wstring&)>& dirExists)
{
    std::vector<std::wstring> roots;
    for (const std::wstring& raw : layers)
    {
        const std::wstring c = CanonicalizeLayerPath(raw);
        if (c.empty()) continue;
        if (!dirExists(c)) continue;
        bool dup = false;
        for (const std::wstring& r : roots)
            if (_wcsicmp(CanonicalizeLayerPath(r).c_str(), c.c_str()) == 0) { dup = true; break; }
        if (dup) continue;
        roots.push_back(c + L"\\");
    }
    return roots;
}

// Serialize an ordered list to a REG_MULTI_SZ blob (each string null-terminated,
// then a final extra null). An empty list is a single null.
inline std::wstring SerializeMultiSz(const std::vector<std::wstring>& items)
{
    std::wstring blob;
    for (const std::wstring& s : items) { blob += s; blob.push_back(L'\0'); }
    blob.push_back(L'\0');
    return blob;
}

// Parse a REG_MULTI_SZ buffer (countChars = wchar_t count incl. terminators) back
// into a list. Stops at the block-terminating empty string or the buffer end
// (robust to a missing final null).
inline std::vector<std::wstring> ParseMultiSz(const wchar_t* buf, size_t countChars)
{
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i < countChars && buf[i] != L'\0')
    {
        const size_t start = i;
        while (i < countChars && buf[i] != L'\0') ++i;
        out.emplace_back(buf + start, buf + i);
        if (i < countChars) ++i;  // skip the null separator
    }
    return out;
}

} // namespace modlayers
#endif // MOD_LAYERS_H
