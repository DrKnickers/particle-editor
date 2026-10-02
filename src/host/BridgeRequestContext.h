#pragma once
//
// Per-request context threaded to every kind handler in the per-domain
// dispatch TUs (BridgeDispatch_*.cpp). Holds the request fields and the
// response-envelope helpers every handler uses; SetRes assigns the envelope,
// then patches the id last.
//
// Nested-ok result contract. SendErr replies {ok:false,error}, which makes the
// web side's request() reject (throw). Some handlers instead report an
// expected failure (user cancel, load error, bad index) with
// SendOk({ok:false,...}) plus an error or reason field: the transport-level
// ok stays true, so request() resolves, and the caller reads the nested `ok`
// in the result to tell success from failure. Most such handlers also report
// success as {ok:true,...}; a few (file/new) return an empty object on
// success, so check each kind's result type in the bridge schema.

#include <string>

#include "third_party/nlohmann/json.hpp"

namespace host {

class BridgeDispatcher;

struct BridgeRequestContext
{
    BridgeDispatcher&     self;    // member access for moved handler blocks
    const std::string&    id;      // request id ("" when absent)
    const std::string&    kind;    // full kind string, exact-match dispatch
    const nlohmann::json& params;  // request params (object() when absent)
    nlohmann::json        res;     // response envelope, written via helpers

    // Assign the envelope, then patch the id LAST.
    void SetRes(nlohmann::json env)
    {
        res = std::move(env);
        if (!id.empty()) res["id"] = id; else res["id"] = nullptr;
    }
    // Transport-level success; `data` is the result payload.
    void SendOk(const nlohmann::json& data)
    {
        SetRes(nlohmann::json{{"type","res"},{"ok",true},{"data",data}});
    }
    // Transport-level failure; the web side's request() rejects with `msg`.
    void SendErr(const std::string& msg)
    {
        SetRes(nlohmann::json{{"type","res"},{"ok",false},{"error",msg}});
    }

    // RequireEngine / MarkDirty need BridgeDispatcher's
    // private members, so they're defined in BridgeDispatcher.cpp (the
    // struct is a friend of BridgeDispatcher).
    bool RequireEngine(const char* what);
    void MarkDirty();
};

} // namespace host
