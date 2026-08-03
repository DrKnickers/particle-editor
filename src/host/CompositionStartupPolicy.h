#pragma once

#include <windows.h>

namespace host
{

// CreateCoreWebView2CompositionController has two failure channels: its
// completion callback and the HRESULT returned while dispatching that callback.
// The latter is synchronous and never reaches the completion callback, so the
// host must promote it to the same fatal-composition path itself.
//
// Use FAILED rather than `hr != S_OK`: successful status values such as
// S_FALSE must not turn a non-failing dispatch into a fatal startup.
inline bool ShouldFailCompositionControllerDispatch(HRESULT dispatchHr)
{
    return FAILED(dispatchHr);
}

} // namespace host
