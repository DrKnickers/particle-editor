#ifndef GAME_ROOTS_H
#define GAME_ROOTS_H
// Pure helpers for classifying a game install root. Header-only, no engine /
// D3D deps (mirrors ModLayers.h) so it unit-tests standalone
// (tests/test_game_roots.cpp).
//
// EaW Gold Pack on Steam splits assets across "GameData" (base EaW) and
// "corruption" (FoC) under one parent folder. The leaf folder name is the
// engine-flavour discriminator: startup uses it to add the sibling root, and
// mod discovery uses it to tag each root's Mods\ folder as FoC or base game.
#include <string>
#include <cwchar>

namespace gameroots {

enum class RootFlavor { Other, FoC, BaseGame };

// Strip trailing path separators (casing preserved).
inline std::wstring TrimTrailingSlashes(std::wstring path)
{
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return path;
}

// Classify a root by its leaf folder name, case-insensitively: "corruption" is
// FoC, "GameData" is the base game, anything else is neither.
inline RootFlavor ClassifyGameRoot(const std::wstring& root)
{
    const std::wstring trimmed = TrimTrailingSlashes(root);
    const size_t sep = trimmed.find_last_of(L"\\/");
    const std::wstring leaf = (sep == std::wstring::npos) ? trimmed : trimmed.substr(sep + 1);
    if (_wcsicmp(leaf.c_str(), L"corruption") == 0) return RootFlavor::FoC;
    if (_wcsicmp(leaf.c_str(), L"GameData") == 0)   return RootFlavor::BaseGame;
    return RootFlavor::Other;
}

// The other half of a GameData / corruption pair: "<parent>\GameData" for a
// corruption root and vice versa. Empty when the root is neither or has no
// parent folder. Existence is the caller's check.
inline std::wstring SiblingGameRoot(const std::wstring& root)
{
    const std::wstring trimmed = TrimTrailingSlashes(root);
    const size_t sep = trimmed.find_last_of(L"\\/");
    if (sep == std::wstring::npos) return std::wstring();
    const std::wstring parent = trimmed.substr(0, sep);
    switch (ClassifyGameRoot(trimmed))
    {
    case RootFlavor::FoC:      return parent + L"\\GameData";
    case RootFlavor::BaseGame: return parent + L"\\corruption";
    default:                   return std::wstring();
    }
}

} // namespace gameroots

#endif // GAME_ROOTS_H
