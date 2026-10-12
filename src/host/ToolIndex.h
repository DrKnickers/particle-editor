#ifndef TOOL_INDEX_H
#define TOOL_INDEX_H

// `--tool index`: every particle system a content stack reaches, and every
// texture its emitters name, resolved and hashed through the editor's own
// texture chain (AssetCache over FileManager). Window-free: textures are
// hashed, never decoded.

#include <string>
#include <vector>
#include "host/third_party/nlohmann/json.hpp"

namespace host
{
struct IndexStack
{
    std::vector<std::wstring> gameRoots;   // the installs, searched after the layers, with their MEGs
    std::vector<std::wstring> layers;      // the mod layers, front = highest precedence
};

// The content-stack hash covers the layer paths as given, as --observe's does:
// spell them as the editor's layer stack does to share its hash.
// Fills *records and returns kExitOk; or returns kExitUsage (a root or layer
// that is not a folder) or kExitSource (no root lists a MEG) with *error.
int RunIndex(const IndexStack& stack, nlohmann::json* records, std::string* error);
}

#endif // TOOL_INDEX_H
