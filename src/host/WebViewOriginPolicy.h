#ifndef HOST_WEBVIEW_ORIGIN_POLICY_H
#define HOST_WEBVIEW_ORIGIN_POLICY_H

// WebView2 origin allow-list. The host must trust only the
// page it deliberately loads, not "whatever is currently navigated". Three
// origins are legitimate:
//   - https://app.local/     prod: the virtual origin whose requests the
//                            WebResourceRequested handler answers from the exe's
//                            embedded RCDATA web bundle.
//   - http://localhost:5174/ dev: the Vite HMR server (kDevServerPort), only
//                            when --dev-ui is active.
//   - about:                 WebView2's own about:blank initial navigation.
// The trailing '/' on the two host prefixes is load-bearing: it stops a
// lookalike like https://app.local.evil.test/ from slipping through. Scheme
// and host compare case-insensitively per RFC 3986, hence _wcsnicmp. Used by
// add_NavigationStarting (cancel off-origin nav) and the WebMessageReceived
// handler (drop messages from an untrusted document source).
//
// Extracted from HostWindow.cpp unchanged (2026-09-30 audit H1 follow-up) for
// the same reason as WebMessageIngressPolicy.h: HostWindow.cpp pulls in WebView2
// + D3D9 and cannot be linked by the standalone test harness, so the boundary
// was untested. tests/test_webview_origin_policy.cpp pins it.

#include <cstring>
#include <cwchar>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

inline bool IsApprovedWebViewOrigin(PCWSTR uri, bool devUi)
{
    if (!uri) return false;
    const auto hasPrefix = [uri](PCWSTR prefix) -> bool
    {
        return _wcsnicmp(uri, prefix, wcslen(prefix)) == 0;
    };
    if (hasPrefix(L"https://app.local/")) return true;
    if (hasPrefix(L"about:"))             return true;
    if (devUi && hasPrefix(L"http://localhost:5174/")) return true;
    return false;
}

#endif  // HOST_WEBVIEW_ORIGIN_POLICY_H
