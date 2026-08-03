#pragma once

#include "StartupCallbackGuard.h"

#include "WebView2.h"

#include <functional>
#include <wrl/client.h>

namespace host
{

using EnvironmentStartupContinuation =
    std::function<HRESULT(HRESULT, ICoreWebView2Environment*)>;
using CompositionControllerStartupContinuation =
    std::function<HRESULT(HRESULT, ICoreWebView2CompositionController*)>;

Microsoft::WRL::ComPtr<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>
MakeEnvironmentStartupCallback(
    StartupCallbackGuard::Token token,
    EnvironmentStartupContinuation continuation);

Microsoft::WRL::ComPtr<
    ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>
MakeCompositionControllerStartupCallback(
    StartupCallbackGuard::Token token,
    CompositionControllerStartupContinuation continuation);

}   // namespace host
