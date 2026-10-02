#pragma once
//
// BridgeWire.h — the ONE place a host->UI bridge envelope becomes wire text,
// and the one exception guard every request goes through on its way there.
//
// WHY (2026-09-30 audit H1). Emitter names, texture names and other strings
// read from .alo/.meg files are raw 8-bit bytes, not UTF-8. nlohmann's
// default dump() throws type_error 316 on the first invalid UTF-8 byte, and
// before this header there were ~40 independent `env.dump()` calls between
// the dispatcher and WebView2 — one of them sat outside every try, so a
// single emitter named "\xE9" threw straight through a WebView2 COM callback.
// Now:
//   - BridgeDispatcher::EmitFn takes the envelope as JSON, never as text, so
//     no emit call site can serialize on its own (scripts/
//     bridge-emit-chokepoint.test.mjs enforces this).
//   - The emit lambda (HostWindow), DispatchSync and HostBridgeProxy all
//     serialize through SerializeBridgeEnvelope, which replaces invalid UTF-8
//     with U+FFFD instead of throwing. Valid UTF-8 serializes byte-identically
//     to the default dump().
//   - RunGuardedDispatch turns ANY exception escaping a kind handler into a
//     well-formed ok:false envelope that keeps the request's correlation id.

#include <exception>
#include <string>

#include "third_party/nlohmann/json.hpp"

namespace host {

// Serialize a bridge envelope (res or evt) for the UI. Compact, like the
// default dump(); invalid UTF-8 bytes become U+FFFD rather than throwing.
inline std::string SerializeBridgeEnvelope(const nlohmann::json& env)
{
    return env.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// ok:false envelope for an exception that escaped a kind handler. Preserves
// the request's correlation id when it had a string one; `id: null`
// otherwise (the async door drops id-less responses, the sync door returns
// them so the caller still gets a parseable answer).
inline nlohmann::json BuildDispatchExceptionEnvelope(const nlohmann::json& parsed,
                                                     const char* what)
{
    nlohmann::json res = {
        {"type",  "res"},
        {"ok",    false},
        {"error", std::string("dispatch exception: ") + (what ? what : "(no message)")},
    };
    // Preserve correlation id if the request had one.
    const auto it = parsed.is_object() ? parsed.find("id") : parsed.end();
    if (it != parsed.end() && it->is_string())
        res["id"] = it->get<std::string>();
    else
        res["id"] = nullptr;
    return res;
}

// Run `handler` (returns the response envelope). Any exception it throws
// — nlohmann's type errors from a malformed param, a std::exception from the
// engine or file layer, or anything else — becomes the envelope above.
// `onError(category, what)` is told first, for logging and the perf span.
template <typename Handler, typename OnError>
nlohmann::json RunGuardedDispatch(const nlohmann::json& parsed,
                                  Handler&& handler, OnError&& onError)
{
    const char* category = nullptr;
    std::string what;
    try
    {
        return handler();
    }
    catch (const nlohmann::json::exception& e)
    {
        category = "type/conversion";
        what = e.what();
    }
    catch (const std::exception& e)
    {
        category = "std";
        what = e.what();
    }
    catch (...)
    {
        category = "unknown";
        what = "unknown exception";
    }
    onError(category, what.c_str());
    return BuildDispatchExceptionEnvelope(parsed, what.c_str());
}

} // namespace host
