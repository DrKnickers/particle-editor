#pragma once

#include <string>
#include <vector>
#include <cctype>
#include <cstdlib>

// Returns "<base>_<n>": base is sourceName with one trailing "_<digits>"
// removed, n is one more than the highest "_<digits>" suffix already used
// by a name in existingNames that shares that base (an exact match of base
// counts as 0).
inline std::string NextDuplicateName(
    const std::vector<std::string>& existingNames,
    const std::string& sourceName)
{
    auto trailingDigitCount = [](const std::string& s, size_t startAfter) -> size_t {
        size_t n = 0;
        for (size_t i = startAfter; i < s.size(); ++i)
        {
            if (!std::isdigit((unsigned char)s[i])) return 0;
            ++n;
        }
        return n;
    };

    std::string base = sourceName;
    size_t underscore = base.rfind('_');
    if (underscore != std::string::npos && trailingDigitCount(base, underscore + 1) > 0)
    {
        base.resize(underscore);
    }

    int maxN = 0;
    for (size_t i = 0; i < existingNames.size(); ++i)
    {
        const std::string& name = existingNames[i];
        if (name == base) continue;  // n=0; maxN already starts there
        if (name.size() > base.size() + 1 &&
            name.compare(0, base.size(), base) == 0 &&
            name[base.size()] == '_' &&
            trailingDigitCount(name, base.size() + 1) > 0)
        {
            int n = std::atoi(name.c_str() + base.size() + 1);
            if (n > maxN) maxN = n;
        }
    }

    return base + "_" + std::to_string(maxN + 1);
}
