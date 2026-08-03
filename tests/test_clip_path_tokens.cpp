// Unit test for ExpandPathTokens (src/host/ClipPathTokens.h) — the ${TOKEN}
// expander behind --record's install-relative timeline paths (#494 follow-up).
// Pure header, no Windows/json/DX. Contract:
//   - ${NAME} -> tokens[NAME]; multiple/positional tokens all expand
//   - no ${...}     -> unchanged, err empty (even with an empty table)
//   - unknown NAME  -> err set (FAIL LOUD), input returned unchanged
//   - unterminated / empty ${} -> err set
//   - bare '$' or '${' -less text is literal
#include "ClipPathTokens.h"
#include <cstdio>
#include <map>
#include <string>

static int g_fail = 0;

static void ok(const char* what, const std::string& in,
               const std::map<std::string, std::string>& tok, const std::string& want)
{
    std::string err;
    const std::string got = ExpandPathTokens(in, tok, err);
    if (!err.empty() || got != want) {
        printf("  FAIL[%s]: ExpandPathTokens(\"%s\") = \"%s\" err=\"%s\", expected \"%s\" (no err)\n",
               what, in.c_str(), got.c_str(), err.c_str(), want.c_str());
        ++g_fail;
    }
}

static void fails(const char* what, const std::string& in,
                  const std::map<std::string, std::string>& tok)
{
    std::string err;
    const std::string got = ExpandPathTokens(in, tok, err);
    if (err.empty() || got != in) {
        printf("  FAIL[%s]: ExpandPathTokens(\"%s\") = \"%s\" err=\"%s\", expected fail-loud "
               "(non-empty err + input returned unchanged)\n",
               what, in.c_str(), got.c_str(), err.c_str());
        ++g_fail;
    }
}

int main()
{
    const std::map<std::string, std::string> T = {
        {"GAME", "C:/Games/Steam/steamapps/common/Star Wars Empire at War/corruption"},
        {"ASSETS", "C:/EaWModding/DATA"},
    };

    // happy path — the real use case
    ok("mod-path", "${GAME}/Mods/ExampleMod", T,
       "C:/Games/Steam/steamapps/common/Star Wars Empire at War/corruption/Mods/ExampleMod");
    ok("open-path", "${ASSETS}/ART/MODELS/P_PLANET_EXPLODE_GALACTIC.ALO", T,
       "C:/EaWModding/DATA/ART/MODELS/P_PLANET_EXPLODE_GALACTIC.ALO");
    // no tokens -> untouched (with a populated AND an empty table)
    ok("literal",       "C:/EaWModding/DATA/x.alo", T, "C:/EaWModding/DATA/x.alo");
    ok("literal-empty", "C:/EaWModding/DATA/x.alo", {}, "C:/EaWModding/DATA/x.alo");
    // multiple + adjacent + positional
    ok("two-tokens", "${GAME}::${ASSETS}", T,
       "C:/Games/Steam/steamapps/common/Star Wars Empire at War/corruption::C:/EaWModding/DATA");
    ok("token-tail", "prefix/${GAME}", T,
       "prefix/C:/Games/Steam/steamapps/common/Star Wars Empire at War/corruption");
    // a bare '$' is literal
    ok("bare-dollar", "cost is $5 for ${ASSETS}", T, "cost is $5 for C:/EaWModding/DATA");
    ok("dollar-eol",  "trailing $", T, "trailing $");
    // a lone '}' with no opening ${ is an ordinary char
    ok("stray-brace", "a}b/${GAME}", T,
       "a}b/C:/Games/Steam/steamapps/common/Star Wars Empire at War/corruption");
    // substituted text is NOT re-scanned: a token whose VALUE contains ${...}
    // expands once and the ${B} in the value is left literal (no recursion/loop)
    ok("no-rescan", "${SELF}", {{"SELF", "${B}"}}, "${B}");

    // fail-loud cases
    fails("unknown",       "${NOPE}/x", T);
    fails("unknown-empty", "${GAME}/x", {});          // right syntax, empty table -> still fails
    fails("unterminated",  "${GAME/Mods", T);
    fails("trailing-open", "path${", T);         // bare ${ at end -> unterminated
    fails("empty-token",   "${}/x", T);

    if (g_fail == 0) { printf("clip-path-tokens: ALL PASS\n"); return 0; }
    printf("clip-path-tokens: %d FAILED\n", g_fail);
    return 1;
}
