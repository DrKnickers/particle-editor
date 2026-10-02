#pragma once

#include <string>

namespace host
{

inline constexpr wchar_t GUIDE_URL[] = L"https://drknickers.github.io/particle-editor/guide/home.html";
inline constexpr wchar_t REPOSITORY_URL[] = L"https://github.com/DrKnickers/particle-editor";

// Returns the fixed https URL for a known link name, or nullptr.
inline const wchar_t* ExternalLinkUrl(const std::string& target)
{
    if (target == "guide") return GUIDE_URL;
    if (target == "repository") return REPOSITORY_URL;
    return nullptr;
}

} // namespace host
