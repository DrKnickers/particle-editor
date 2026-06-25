// HostBridgeProxy — see HostBridgeProxy.h for the design overview.
//
// Implementation notes:
//   - GetTypeInfoCount returns 0 (no ITypeInfo). WebView2's marshaller
//     calls GetIDsOfNames + Invoke directly for late-bound dispatch, so
//     we don't need a type library. This is the same pattern Microsoft's
//     own WebView2 host-object samples use.
//   - GetIDsOfNames maps "dispatchRequest" → DISPID 1. Any other name
//     (including parameter-named args, which COM clients sometimes ask
//     for) returns DISPID_UNKNOWN with DISP_E_UNKNOWNNAME.
//   - Invoke validates wFlags has DISPATCH_METHOD, expects exactly one
//     BSTR arg in rgvarg[0], converts UTF-16 ↔ UTF-8 against m_dispatchSync,
//     and returns a freshly-allocated BSTR. WebView2's marshaller takes
//     ownership of the returned BSTR — we must NOT free it here.

#include "HostBridgeProxy.h"

#include <cstring>
#include <stdexcept>
#include "StringConv.h"   // host::Utf8ToWide / WideToUtf8 (consolidated, DRY audit cpp-host-0)
#include "third_party/nlohmann/json.hpp"

#pragma comment(lib, "OleAut32.lib")

namespace host {

HRESULT HostBridgeProxy::RuntimeClassInitialize(DispatchSyncFn dispatchSync)
{
    m_dispatchSync = std::move(dispatchSync);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE HostBridgeProxy::GetTypeInfoCount(UINT* pctinfo)
{
    if (pctinfo) *pctinfo = 0;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE HostBridgeProxy::GetTypeInfo(UINT /*iTInfo*/, LCID /*lcid*/,
                                                       ITypeInfo** ppTInfo)
{
    if (ppTInfo) *ppTInfo = nullptr;
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE HostBridgeProxy::GetIDsOfNames(REFIID /*riid*/, LPOLESTR* rgszNames,
                                                        UINT cNames, LCID /*lcid*/,
                                                        DISPID* rgDispId)
{
    if (!rgszNames || !rgDispId || cNames == 0) return E_INVALIDARG;

    HRESULT hr = S_OK;
    for (UINT i = 0; i < cNames; ++i)
    {
        if (i == 0 && rgszNames[i] && wcscmp(rgszNames[i], L"dispatchRequest") == 0)
        {
            rgDispId[i] = kDispatchRequestId;
        }
        else
        {
            rgDispId[i] = DISPID_UNKNOWN;
            hr = DISP_E_UNKNOWNNAME;
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HostBridgeProxy::Invoke(DISPID dispIdMember, REFIID /*riid*/,
                                                  LCID /*lcid*/, WORD wFlags,
                                                  DISPPARAMS* pDispParams,
                                                  VARIANT* pVarResult,
                                                  EXCEPINFO* /*pExcepInfo*/,
                                                  UINT* /*puArgErr*/)
{
    if (dispIdMember != kDispatchRequestId) return DISP_E_MEMBERNOTFOUND;
    if (!(wFlags & DISPATCH_METHOD)) return DISP_E_MEMBERNOTFOUND;
    if (!pDispParams || pDispParams->cArgs != 1 || !pDispParams->rgvarg)
        return DISP_E_BADPARAMCOUNT;

    // DISPPARAMS args are reverse-ordered (rightmost arg at index 0). We
    // have only one arg, so rgvarg[0] is unambiguous.
    VARIANT arg = pDispParams->rgvarg[0];
    if (arg.vt != VT_BSTR || arg.bstrVal == nullptr) return DISP_E_TYPEMISMATCH;

    // UTF-16 BSTR → UTF-8 std::string. SysStringLen excludes the terminator
    // (BSTRs are length-prefixed, not NUL-terminated); host::WideToUtf8 returns
    // {} for an empty input, matching the old srcLen==0 path.
    const std::string req = host::WideToUtf8(std::wstring(arg.bstrVal, SysStringLen(arg.bstrVal)));

    std::string res;
    if (!m_dispatchSync)
    {
        res = R"({"type":"res","ok":false,"error":"HostBridgeProxy: dispatch fn not set"})";
    }
    else
    {
        try
        {
            res = m_dispatchSync(req);
        }
        catch (const std::exception& e)
        {
            // Build a defensive error envelope so the JS side still
            // gets a well-formed response object to parse.
            // pre-fix this concatenated e.what() into a hand-rolled JSON
            // string, so quotes/backslashes/control chars in exception
            // text would malform the JSON — defeating the entire purpose
            // of the catch.
            res = nlohmann::json{
                {"type",  "res"},
                {"ok",    false},
                {"error", e.what() ? e.what() : "(no message)"},
            }.dump();
        }
        catch (...)
        {
            res = R"({"type":"res","ok":false,"error":"HostBridgeProxy: unknown exception"})";
        }
    }

    // UTF-8 → UTF-16 BSTR via the shared converter. host::Utf8ToWide returns {}
    // for empty input; SysAllocStringLen(data(), 0) yields a "" BSTR (data() on an
    // empty std::wstring is a valid pointer to the NUL terminator).
    const std::wstring wres = host::Utf8ToWide(res);
    BSTR bres = SysAllocStringLen(wres.data(), static_cast<UINT>(wres.size()));

    if (pVarResult)
    {
        VariantInit(pVarResult);
        pVarResult->vt = VT_BSTR;
        pVarResult->bstrVal = bres;
    }
    else
    {
        // No result slot from the caller — must free the BSTR ourselves
        // or it leaks.
        SysFreeString(bres);
    }
    return S_OK;
}

} // namespace host
