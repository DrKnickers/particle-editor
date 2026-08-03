#include "StartupCallbackAdapter.h"

#include <utility>
#include <wrl.h>

namespace host
{

Microsoft::WRL::ComPtr<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>
MakeEnvironmentStartupCallback(
    StartupCallbackGuard::Token token,
    EnvironmentStartupContinuation continuation)
{
    return Microsoft::WRL::Callback<
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [token = std::move(token),
         continuation = std::move(continuation)](
            HRESULT hr, ICoreWebView2Environment* environment) -> HRESULT
        {
            if (!token.OwnerAlive()) return S_OK;
            return continuation(hr, environment);
        });
}

Microsoft::WRL::ComPtr<
    ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>
MakeCompositionControllerStartupCallback(
    StartupCallbackGuard::Token token,
    CompositionControllerStartupContinuation continuation)
{
    return Microsoft::WRL::Callback<
        ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
        [token = std::move(token),
         continuation = std::move(continuation)](
            HRESULT hr,
            ICoreWebView2CompositionController* controller) -> HRESULT
        {
            if (!token.OwnerAlive()) return S_OK;
            return continuation(hr, controller);
        });
}

}   // namespace host
