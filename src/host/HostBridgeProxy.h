// HostBridgeProxy — COM IDispatch shim exposed to the WebView2 page
// as `chrome.webview.hostObjects.hostBridge` via
// `ICoreWebView2::AddHostObjectToScript`. Used ONLY in --test-host
// mode; production builds do not construct or expose this object.
//
// Why this exists: Playwright drives the React app through a CDP
// connection (--remote-debugging-port=9222). WebView2 silently drops
// `chrome.webview.postMessage` calls while a CDP debugger is attached
// (see tasks/lessons.md). The host-object IPC channel is on a
// separate marshalling path and is unaffected, so test traffic routes
// through it instead.
//
// JS-side usage:
//
//     const hb = chrome.webview.hostObjects.hostBridge;
//     const res = await hb.dispatchRequest(JSON.stringify(env));
//     const dto = JSON.parse(res);
//
// (Method calls return promises across the host-object IPC; the host
// IDispatch::Invoke returns synchronously, WebView2 marshals back.)
//
// One method exposed: `dispatchRequest(BSTR jsonReq) -> BSTR jsonRes`.
// Events are NOT carried on this channel — see TestHostBridge in
// web/apps/editor/src/bridge/test-host.ts for the JS-side rationale.
#ifndef HOST_HOST_BRIDGE_PROXY_H
#define HOST_HOST_BRIDGE_PROXY_H

#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <oaidl.h>
#include <wrl/implements.h>
#include <wrl/client.h>

#include <functional>
#include <string>

namespace host {

class HostBridgeProxy
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IDispatch>
{
public:
    using DispatchSyncFn = std::function<std::string(const std::string&)>;

    HostBridgeProxy() = default;
    HRESULT RuntimeClassInitialize(DispatchSyncFn dispatchSync);

    // IDispatch
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* pctinfo) override;
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT iTInfo, LCID lcid, ITypeInfo** ppTInfo) override;
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID riid, LPOLESTR* rgszNames, UINT cNames,
                                            LCID lcid, DISPID* rgDispId) override;
    HRESULT STDMETHODCALLTYPE Invoke(DISPID dispIdMember, REFIID riid, LCID lcid,
                                     WORD wFlags, DISPPARAMS* pDispParams,
                                     VARIANT* pVarResult, EXCEPINFO* pExcepInfo,
                                     UINT* puArgErr) override;

private:
    static constexpr DISPID kDispatchRequestId = 1;
    DispatchSyncFn m_dispatchSync;
};

} // namespace host

#endif // HOST_HOST_BRIDGE_PROXY_H
