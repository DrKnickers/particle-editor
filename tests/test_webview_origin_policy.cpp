// Unit test for the WebView2 origin allow-list (src/host/WebViewOriginPolicy.h).
//
// IsApprovedWebViewOrigin gates both add_NavigationStarting (cancel off-origin
// navigation) and the WebMessageReceived handler (drop messages from an
// untrusted document). It moved out of HostWindow.cpp unchanged so it could be
// tested here; the cases below pin today's behaviour, lookalike hosts included.

#include "host/WebViewOriginPolicy.h"

#include <cstdio>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

int main()
{
    std::printf("test_webview_origin_policy\n");

    // Production origin: the embedded bundle on the app.local virtual host.
    CHECK(IsApprovedWebViewOrigin(L"https://app.local/", false),           "prod origin root");
    CHECK(IsApprovedWebViewOrigin(L"https://app.local/index.html", false), "prod origin page");
    CHECK(IsApprovedWebViewOrigin(L"HTTPS://APP.LOCAL/index.html", false), "scheme + host compare case-insensitively");
    CHECK(IsApprovedWebViewOrigin(L"https://app.local/?perfTrace=1", true), "prod origin also allowed with --dev-ui");

    // The trailing '/' is load-bearing.
    CHECK(!IsApprovedWebViewOrigin(L"https://app.local", false),            "no trailing slash: refused");
    CHECK(!IsApprovedWebViewOrigin(L"https://app.local.evil.test/", false), "lookalike host: refused");
    CHECK(!IsApprovedWebViewOrigin(L"https://app.localhost/", false),       "app.localhost: refused");
    CHECK(!IsApprovedWebViewOrigin(L"http://app.local/", false),            "plain http app.local: refused");
    CHECK(!IsApprovedWebViewOrigin(L"https://evil.test/https://app.local/", false), "prefix only, not substring");

    // WebView2's own initial navigation.
    CHECK(IsApprovedWebViewOrigin(L"about:blank", false), "about:blank");
    CHECK(IsApprovedWebViewOrigin(L"ABOUT:blank", false), "about: is case-insensitive");

    // Vite dev server: only with --dev-ui.
    CHECK(IsApprovedWebViewOrigin(L"http://localhost:5174/", true),            "dev server with --dev-ui");
    CHECK(IsApprovedWebViewOrigin(L"http://localhost:5174/src/main.tsx", true), "dev server page with --dev-ui");
    CHECK(!IsApprovedWebViewOrigin(L"http://localhost:5174/", false),          "dev server without --dev-ui: refused");
    CHECK(!IsApprovedWebViewOrigin(L"http://localhost:5174", true),            "dev server, no trailing slash: refused");
    CHECK(!IsApprovedWebViewOrigin(L"http://localhost:51740/", true),          "different port: refused");
    CHECK(!IsApprovedWebViewOrigin(L"https://localhost:5174/", true),          "https dev scheme: refused");
    CHECK(!IsApprovedWebViewOrigin(L"http://127.0.0.1:5174/", true),           "loopback IP instead of localhost: refused");

    // Degenerate inputs.
    CHECK(!IsApprovedWebViewOrigin(nullptr, true),  "null source: refused (fail closed)");
    CHECK(!IsApprovedWebViewOrigin(L"", true),      "empty source: refused");
    CHECK(!IsApprovedWebViewOrigin(L"file:///C:/x.html", true), "file: scheme: refused");

    std::printf(g_failed ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failed);
    return g_failed ? 1 : 0;
}
