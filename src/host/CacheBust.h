#pragma once
// ---------------------------------------------------------------------
// Cache-bust helpers for the app.local virtual-host navigation.
//
// WebView2 caches resources served from a SetVirtualHostNameToFolderMapping
// origin (both its HTTP disk cache and the V8 code cache). Vite content-
// hashes the JS/CSS bundle, so those URLs self-bust on a rebuild — but the
// entry document `index.html` keeps a fixed URL, and a stale cached
// index.html keeps pointing at the OLD hashed asset URLs, masking the whole
// rebuild. We cannot inject a Cache-Control response header on the mapped
// host (the mapping short-circuits WebResourceRequested), so we
// bust index.html via a query string on the navigation URL instead: append
// the build's index.html mtime as `?v=<token>`. The token changes on every
// rebuild (Vite's emptyOutDir rewrites index.html) and is stable across
// relaunches of the SAME build, so unchanged assets stay cache-warm.
//
// Pure (no Win32 / std::filesystem) so the URL composition is unit-tested
// (tests/test_cache_bust.cpp); the host reads the mtime and calls these.
// ---------------------------------------------------------------------
#include <string>
#include <algorithm>

namespace host {

// Format a filesystem mtime tick count into a short, URL-safe cache-bust
// token (lowercase hex). Non-positive ticks (e.g. the mtime read failed)
// → empty string, signalling the caller to navigate without a `?v=`.
inline std::wstring CacheBustToken(long long ticks)
{
    if (ticks <= 0) return std::wstring();
    unsigned long long v = static_cast<unsigned long long>(ticks);
    static const wchar_t kHex[] = L"0123456789abcdef";
    std::wstring out;
    while (v) { out.push_back(kHex[v & 0xF]); v >>= 4; }
    std::reverse(out.begin(), out.end());
    return out;
}

// Append `?v=<token>` to a URL that has no existing query. An empty token
// returns the URL unchanged. The app.local nav URL is a fixed literal with
// no query of its own, so a bare `?` append is correct here.
inline std::wstring AppendCacheBustQuery(const std::wstring& url,
                                         const std::wstring& token)
{
    if (token.empty()) return url;
    return url + L"?v=" + token;
}

} // namespace host
