#pragma once
// The content stack's identity, shared by --observe (capture.json) and
// --tool index: its paths as forward-slashed UTF-8, the content-stack hash,
// and where in the stack an asset was found. Window-free.

#include "StringConv.h"
#include "common/sha256.h"
#include "gamedata/AssetLedger.h"
#include "host/third_party/nlohmann/json.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace host {

inline nlohmann::json ContentPaths(const std::vector<std::wstring>& paths)
{
    nlohmann::json result = nlohmann::json::array();
    for (const auto& path : paths) result.push_back(WideToUtf8(std::filesystem::path(path).generic_wstring()));
    return result;
}

// The SHA-256 of the ordered mod-layer paths' JSON (ContentPaths), including
// the empty list of an unmodded stack; "" if hashing fails.
inline std::string ContentStackHash(const std::vector<std::wstring>& layers)
{
    const std::string bytes = ContentPaths(layers).dump();
    return ::Sha256Hex(bytes.data(), bytes.size());   // ToolIO.h's host::Sha256Hex would hide it
}

inline nlohmann::json WinnerJson(const AssetWinner& winner)
{
    switch (winner.kind)
    {
    case AssetWinner::Kind::Loose: return {{"kind", "loose"}, {"path", winner.path}};
    case AssetWinner::Kind::Archive: return {{"kind", "archive"}, {"path", winner.path}, {"member", winner.member},
        {"start", winner.start}, {"size", winner.size}};
    case AssetWinner::Kind::Embedded: return {{"kind", "embedded"}, {"resourceId", winner.resourceId}};
    default: return nullptr;
    }
}

} // namespace host
