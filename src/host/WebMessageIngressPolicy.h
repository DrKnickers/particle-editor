// Bridge-ingress admission policy for host-bound WebMessages.
//
// Extracted from HostWindow's add_WebMessageReceived callback for the same
// reason WebViewModalPolicy.h was: the decision is pure, the callback it lives
// in is unreachable from the standalone test harness (HostWindow.cpp pulls in
// WebView2 + D3D9), and an untested boundary is exactly where an off-by-one
// hides.
//
// WHY THERE IS A CAP (2026-07 audit). OnWebMessage parses the whole
// string, so before this, one postMessage could drive an arbitrarily large
// allocation on the UI thread. This is defence-in-depth rather than a live
// hole -- the origin check upstream means the only speaker is our own bundle --
// but an unbounded parse sitting behind a trust boundary should not be the
// thing keeping the window alive.
//
// The cap is applied to BOTH ingress paths (TryGetWebMessageAsString and the
// get_WebMessageAsJson fallback). A cap on one of two doors is not a cap.

#ifndef HOST_WEBMESSAGE_INGRESS_POLICY_H
#define HOST_WEBMESSAGE_INGRESS_POLICY_H

#include <cstddef>

// True when a message of `chars` UTF-16 characters may be dispatched.
// Inclusive at the cap: a message of exactly `cap` characters is legal, so the
// documented limit is a size the bridge accepts rather than one it refuses.
inline bool ShouldAcceptWebMessage(std::size_t chars, std::size_t cap)
{
    return chars <= cap;
}

#endif  // HOST_WEBMESSAGE_INGRESS_POLICY_H
