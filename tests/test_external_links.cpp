// Unit test for the two fixed browser targets (src/host/ExternalLinks.h).

#include "host/ExternalLinks.h"

#include <cstdio>
#include <string>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

int main()
{
    std::printf("test_external_links\n");

    const wchar_t* guide = host::ExternalLinkUrl("guide");
    const wchar_t* repository = host::ExternalLinkUrl("repository");
    CHECK(guide != nullptr, "guide resolves");
    CHECK(repository != nullptr, "repository resolves");
    CHECK(guide && std::wstring(guide).find(L"https://") == 0, "guide uses https");
    CHECK(repository && std::wstring(repository).find(L"https://") == 0, "repository uses https");
    CHECK(host::ExternalLinkUrl("") == nullptr, "empty target refused");
    CHECK(host::ExternalLinkUrl("Guide") == nullptr, "guide casing must match");
    CHECK(host::ExternalLinkUrl("Repository") == nullptr, "repository casing must match");
    CHECK(host::ExternalLinkUrl("https://example.com") == nullptr, "URL target refused");
    CHECK(host::ExternalLinkUrl("file:///C:/test") == nullptr, "protocol target refused");
    CHECK(host::ExternalLinkUrl("other") == nullptr, "unknown name refused");

    std::printf(g_failed ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failed);
    return g_failed ? 1 : 0;
}
