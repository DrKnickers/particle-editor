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
    // Reject a segment made only of dots and spaces that contains TWO OR MORE
    // dots. The obvious rule — "is this segment exactly `..`" — is not enough
    // on Win32, which strips trailing dots and spaces from a path component, so
    // ".. \x" reaches the parent directory exactly like "../x". Enumerating the
    // normalized spellings is a losing game, so the whole two-dot class goes.
    //
    // The two-dot floor matters: a lone "." (or ". ") is the CURRENT directory
    // and is a legitimate spelling — "FX\.\FIRE.TGA" is a real relative name.
    // Rejecting it would not merely refuse the name, it would send it through
    // SanitizeAssetName's basename reduction and quietly load "FIRE.TGA" from
    // somewhere else, i.e. a silent wrong-texture regression for existing mods.
    // A silent misresolution is worse than the traversal this guards against.
    size_t segStart = 0;
    for (size_t i = 0; i <= n.size(); ++i)
    {
        const bool atEnd = (i == n.size());
        if (!atEnd && n[i] != '\\' && n[i] != '/') continue;
        bool dotsOnly = (i > segStart);               // non-empty segment
        size_t dots = 0;
        for (size_t j = segStart; j < i && dotsOnly; ++j)
        {
            if (n[j] == '.') ++dots;
            else if (n[j] != ' ') dotsOnly = false;
        }
        if (dotsOnly && dots >= 2) return false;
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

// ---------------------------------------------------------------------------
// Operator-supplied ABSOLUTE paths: the ground / skydome custom texture slots.
//
// A different case from the .alo asset names above, and IsSafeRelativeAssetName
// is the WRONG test for it — here an absolute local path like
// "C:\textures\grass.dds" is the normal, legitimate input, because the operator
// picked the file. Running it through the relative-name rule would reject every
// real use.
//
// What must still be refused is a path that sends the OS to another MACHINE.
// "\\host\share\x.dds" makes CreateFile authenticate outbound over SMB, which
// discloses the user's NetNTLM hash to whoever runs that host — the same
// primitive F-PATH closed for .alo names, reached instead through a string the
// operator can be talked into pasting. It is worse than a one-shot here: both
// slots persist their path and REPLAY it at every startup (HostWindow.cpp's
// registry restore), so one pasted string keeps leaking on every launch
// (2026-07 audit, B-9).
//
// The rule is deliberately syntactic: a two-separator path is accepted only
// when its server component is an exact, case-insensitive match for a local
// endpoint ("localhost", "127.0.0.1", or Windows' "wsl.localhost" pseudo-share).
// Every other UNC host, the device namespace ("\\.\PhysicalDrive0"), and the
// extended-length prefix ("\\?\...") remain refused. Matching the complete
// server component matters: "\\localhost.attacker\share" is remote.
//
// NOT covered, deliberately: a mapped network drive ("Z:\x.dds") is still
// remote, but telling it apart needs GetDriveTypeW — a filesystem call this
// predicate stays free of — and it requires the user to have configured the
// mapping themselves, a far higher bar than pasting a string.
//
// Empty is permitted: it is how a slot's custom path is CLEARED, and the
// startup restore passes it for every unset slot.
inline bool IsLocalCustomAssetPath(const std::wstring& p)
{
    if (p.empty()) return true;
    const bool sep0 = (p[0] == L'\\' || p[0] == L'/');
    const bool sep1 = (p.size() >= 2) && (p[1] == L'\\' || p[1] == L'/');
    if (!(sep0 && sep1)) return true;

    const size_t hostStart = 2;
    const size_t hostEnd = p.find_first_of(L"\\/", hostStart);
    const size_t hostLength =
        (hostEnd == std::wstring::npos ? p.size() : hostEnd) - hostStart;
    auto hostEquals = [&](const wchar_t* expected) {
        size_t expectedLength = 0;
        while (expected[expectedLength] != L'\0') ++expectedLength;
        if (hostLength != expectedLength) return false;
        for (size_t i = 0; i < hostLength; ++i)
        {
            wchar_t actual = p[hostStart + i];
            if (actual >= L'A' && actual <= L'Z') actual += L'a' - L'A';
            if (actual != expected[i]) return false;
        }
        return true;
    };
    return hostEquals(L"localhost") ||
           hostEquals(L"127.0.0.1") ||
           hostEquals(L"wsl.localhost");
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
