#pragma once
//
// Per-request context threaded to every kind handler in the per-domain
// dispatch TUs (BridgeDispatch_*.cpp). Replaces DispatchInternal's local
// envelope lambdas verbatim — same envelope shapes, same "assign env, then
// patch id LAST" order in SetRes — so moved ladder blocks keep today's
// response semantics exactly. See tasks/2026-07-06-heavyweight-refactor-plan.md
// (Phase A) for the split design.

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

    // == the setRes lambda: assign env, then patch id LAST.
    void SetRes(nlohmann::json env)
    {
        res = std::move(env);
        if (!id.empty()) res["id"] = id; else res["id"] = nullptr;
    }
    // == the sendOk lambda.
    void SendOk(const nlohmann::json& data)
    {
        SetRes(nlohmann::json{{"type","res"},{"ok",true},{"data",data}});
    }
    // == the sendErr lambda.
    void SendErr(const std::string& msg)
    {
        SetRes(nlohmann::json{{"type","res"},{"ok",false},{"error",msg}});
    }

    // == the requireEngine / markDirty lambdas. Need BridgeDispatcher's
    // private members, so they're defined in BridgeDispatcher.cpp (the
    // struct is a friend of BridgeDispatcher).
    bool RequireEngine(const char* what);
    void MarkDirty();
};

} // namespace host
