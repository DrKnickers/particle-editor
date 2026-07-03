#pragma once
#include <string>
#include <map>

// #494 follow-up: ${TOKEN} expansion for --record timeline paths, so a clip can
// reference install-relative locations (e.g. the active mod) without hardcoding
// an absolute machine path. The host supplies the token table — GAME resolves to
// the editor's configured game root (HKCU\Software\AloParticleEditor\GameDataPath,
// the same key the editor itself resolves mods against), so a timeline that says
//   "paths": ["${GAME}/Mods/Mod"]
// keeps working after the game is reinstalled elsewhere.
//
// Pure + header-only (no Windows/registry/json deps) so tests/test_clip_path_tokens.cpp
// can exercise it directly. FAIL-LOUD by contract: an unknown or unterminated token
// sets `err` (non-empty) and returns the input unchanged — the caller (ClipRunner)
// turns that into exit 2 rather than silently rendering with a broken path (which,
// for a mod layer, would quietly fall back to the unmodded look).

// Expand every `${NAME}` in `in` against `tokens`. On the FIRST unresolved or
// malformed token, sets `err` and returns `in` unchanged. `err` is empty on success.
// A bare `$` not followed by `{` is a literal. No nesting.
inline std::string ExpandPathTokens(const std::string& in,
                                    const std::map<std::string, std::string>& tokens,
                                    std::string& err)
{
    err.clear();
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();)
    {
        if (in[i] == '$' && i + 1 < in.size() && in[i + 1] == '{')
        {
            const size_t close = in.find('}', i + 2);
            if (close == std::string::npos)
            {
                err = "unterminated path token (missing '}') in \"" + in + "\"";
                return in;
            }
            const std::string name = in.substr(i + 2, close - (i + 2));
            if (name.empty())
            {
                err = "empty path token \"${}\" in \"" + in + "\"";
                return in;
            }
            const auto it = tokens.find(name);
            if (it == tokens.end())
            {
                err = "unknown path token \"${" + name + "}\" (known: ";
                bool first = true;
                for (const auto& kv : tokens) { err += (first ? "" : ", ") + kv.first; first = false; }
                if (first) err += "(none)";
                err += ")";
                return in;
            }
            out += it->second;
            i = close + 1;
        }
        else
        {
            out += in[i++];
        }
    }
    return out;
}
