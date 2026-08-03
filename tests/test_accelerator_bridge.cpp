// Unit test for host::AcceleratorBridge (src/host/AcceleratorBridge.cpp) --
// the combo parser + matcher that pre-translates accelerator keys before
// WebView2 swallows them.
//
// Covers, per the source:
//   - every named key in VkFromName's table incl. aliases (AcceleratorBridge.cpp:14-51)
//   - single-char A..Z / 0..9 keys and the F1..F24 range (+F0/F25 rejects)
//   - modifier parsing: order-insensitive, whitespace-trimmed, case-insensitive,
//     Ctrl/Control alias (ParseCombo, AcceleratorBridge.cpp:61-86)
//   - unrecognised key names are silently dropped at registration and can
//     never match (RegisterCombos, AcceleratorBridge.cpp:99-103)
//   - TryDispatch requires EXACT modifier equality (AcceleratorBridge.cpp:112-118):
//     near-misses (extra/missing/different modifiers) return false, emit nothing
//   - the emitted payload is the ORIGINAL combo string, verbatim
//   - RegisterCombos REPLACES the previous list entirely (AcceleratorBridge.cpp:91-104)
//
// Mirrors tests/test_webview_modal_policy.cpp's shape. Standalone console exe;
// see tests/build_test_accelerator_bridge.bat.

#include "host/AcceleratorBridge.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

using host::AcceleratorBridge;

// Emission recorder: TryDispatch + capture what (if anything) was emitted.
struct Dispatch
{
    bool        handled;
    int         emits;
    std::string combo;
};
static Dispatch dispatch(const AcceleratorBridge& b, UINT vk,
                         bool ctrl, bool shift, bool alt)
{
    Dispatch d{ false, 0, "" };
    d.handled = b.TryDispatch(vk, ctrl, shift, alt,
        [&](const std::string& combo) { ++d.emits; d.combo = combo; });
    return d;
}

// One registered combo must match exactly one (vk, mods) tuple.
static bool matchesOnly(const char* combo, UINT vk, bool ctrl, bool shift, bool alt)
{
    AcceleratorBridge b;
    b.RegisterCombos({ combo });
    Dispatch hit = dispatch(b, vk, ctrl, shift, alt);
    if (!hit.handled || hit.emits != 1 || hit.combo != combo) return false;
    // The same vk with every OTHER modifier combination must NOT match.
    for (int m = 0; m < 8; ++m)
    {
        bool c = (m & 1) != 0, s = (m & 2) != 0, a = (m & 4) != 0;
        if (c == ctrl && s == shift && a == alt) continue;
        Dispatch miss = dispatch(b, vk, c, s, a);
        if (miss.handled || miss.emits != 0) return false;
    }
    return true;
}

int main()
{
    std::printf("test_accelerator_bridge\n");

    // ---- A: the documented React set (AcceleratorBridge.h:4-5) -------------
    {
        AcceleratorBridge b;
        b.RegisterCombos({ "Ctrl+S", "Ctrl+Z", "Ctrl+Shift+Z", "Delete", "F5" });

        Dispatch d = dispatch(b, 'S', true, false, false);
        CHECK(d.handled && d.emits == 1 && d.combo == "Ctrl+S", "Ctrl+S matches and echoes its combo");
        d = dispatch(b, 'Z', true, false, false);
        CHECK(d.handled && d.combo == "Ctrl+Z", "Ctrl+Z matches");
        d = dispatch(b, 'Z', true, true, false);
        CHECK(d.handled && d.combo == "Ctrl+Shift+Z", "Ctrl+Shift+Z matches");
        d = dispatch(b, VK_DELETE, false, false, false);
        CHECK(d.handled && d.combo == "Delete", "bare Delete matches");
        d = dispatch(b, VK_F5, false, false, false);
        CHECK(d.handled && d.combo == "F5", "bare F5 matches");

        // Near-misses: wrong/extra/missing modifiers must NOT match or emit.
        d = dispatch(b, 'S', true, false, true);
        CHECK(!d.handled && d.emits == 0, "Ctrl+Alt+S does NOT match Ctrl+S");
        d = dispatch(b, 'S', false, false, false);
        CHECK(!d.handled && d.emits == 0, "bare S does NOT match Ctrl+S");
        d = dispatch(b, 'S', true, true, false);
        CHECK(!d.handled && d.emits == 0, "Ctrl+Shift+S does NOT match Ctrl+S");
        d = dispatch(b, 'Z', false, true, false);
        CHECK(!d.handled && d.emits == 0, "Shift+Z alone matches neither Z combo");
        d = dispatch(b, VK_DELETE, true, false, false);
        CHECK(!d.handled && d.emits == 0, "Ctrl+Delete does NOT match bare Delete");
        d = dispatch(b, VK_F5, false, false, true);
        CHECK(!d.handled && d.emits == 0, "Alt+F5 does NOT match bare F5");
        d = dispatch(b, 'Q', true, false, false);
        CHECK(!d.handled && d.emits == 0, "unregistered key returns false, emits nothing");
    }

    // ---- B: the full named-key table (AcceleratorBridge.cpp:25-41) ---------
    {
        struct { const char* name; UINT vk; } table[] = {
            { "Delete", VK_DELETE }, { "Del", VK_DELETE },
            { "Backspace", VK_BACK }, { "Back", VK_BACK },
            { "Tab", VK_TAB },
            { "Enter", VK_RETURN }, { "Return", VK_RETURN },
            { "Esc", VK_ESCAPE }, { "Escape", VK_ESCAPE },
            { "Space", VK_SPACE },
            { "Left", VK_LEFT }, { "Right", VK_RIGHT },
            { "Up", VK_UP }, { "Down", VK_DOWN },
            { "Home", VK_HOME }, { "End", VK_END },
            { "PageUp", VK_PRIOR }, { "PgUp", VK_PRIOR },
            { "PageDown", VK_NEXT }, { "PgDn", VK_NEXT },
            { "Insert", VK_INSERT },
            { "PrintScreen", VK_SNAPSHOT },
            { "Pause", VK_PAUSE },
        };
        bool all = true;
        for (const auto& row : table)
        {
            if (!matchesOnly(row.name, row.vk, false, false, false))
            {
                all = false;
                std::printf("  FAIL detail: named key '%s' broken\n", row.name);
            }
        }
        CHECK(all, "every named key + alias maps to its VK and matches exactly");
    }

    // ---- C: single characters and function keys ----------------------------
    {
        CHECK(matchesOnly("Ctrl+A", 'A', true, false, false), "letter A maps to VK 'A'");
        CHECK(matchesOnly("Ctrl+7", '7', true, false, false), "digit 7 maps to VK '7'");
        CHECK(matchesOnly("Ctrl+s", 'S', true, false, false), "lowercase key upper-cased before VK lookup");
        CHECK(matchesOnly("F1",  VK_F1,      false, false, false), "F1 boundary");
        CHECK(matchesOnly("F12", VK_F1 + 11, false, false, false), "F12 mid-range");
        CHECK(matchesOnly("F24", VK_F1 + 23, false, false, false), "F24 boundary");
        // "5" is the digit, not F5.
        AcceleratorBridge b;
        b.RegisterCombos({ "5" });
        CHECK(dispatch(b, '5', false, false, false).handled
              && !dispatch(b, VK_F5, false, false, false).handled,
              "digit '5' is VK '5', not F5");
    }

    // ---- D: parse edges -- whitespace, order, case, Control alias ----------
    {
        AcceleratorBridge b;
        b.RegisterCombos({ " shift + CONTROL + z ", "s+CTRL" });
        Dispatch d = dispatch(b, 'Z', true, true, false);
        CHECK(d.handled && d.combo == " shift + CONTROL + z ",
              "modifiers order/case/space-insensitive; ORIGINAL string echoed verbatim");
        d = dispatch(b, 'S', true, false, false);
        CHECK(d.handled && d.combo == "s+CTRL", "key-before-modifier order accepted");
        CHECK(matchesOnly("Ctrl+Shift+Alt+X", 'X', true, true, true),
              "triple-modifier combo matches only the exact triple");
        CHECK(matchesOnly("Alt+Enter", VK_RETURN, false, false, true),
              "Alt-only combo requires exactly Alt");
    }

    // ---- E: unrecognised keys are dropped at registration ------------------
    // vk==0 combos are silently ignored (AcceleratorBridge.cpp:99-103); a
    // valid combo registered alongside must keep working.
    {
        AcceleratorBridge b;
        b.RegisterCombos({ "Ctrl+F25", "F0", "Ctrl+Foo", "Ctrl+", "Ctrl+Shift", "", "Ctrl+K" });
        CHECK(!dispatch(b, VK_F1 + 24, true, false, false).handled, "F25 out of range -> never matches");
        CHECK(!dispatch(b, 0, true, false, false).handled, "vk 0 never matches (dropped combos unreachable)");
        CHECK(!dispatch(b, 0, false, false, false).handled, "empty / modifier-only combos never match");
        Dispatch d = dispatch(b, 'K', true, false, false);
        CHECK(d.handled && d.combo == "Ctrl+K", "valid combo still works alongside dropped ones");
    }

    // ---- F: RegisterCombos replaces the previous list ----------------------
    {
        AcceleratorBridge b;
        b.RegisterCombos({ "Ctrl+S" });
        CHECK(dispatch(b, 'S', true, false, false).handled, "first registration active");
        b.RegisterCombos({ "Ctrl+D" });
        CHECK(!dispatch(b, 'S', true, false, false).handled, "re-registration drops the old list");
        CHECK(dispatch(b, 'D', true, false, false).handled, "re-registration installs the new list");
        b.RegisterCombos({});
        CHECK(!dispatch(b, 'D', true, false, false).handled, "empty registration clears everything");
    }

    // ---- G: duplicates -- first registered match wins, single emission -----
    {
        AcceleratorBridge b;
        b.RegisterCombos({ "Ctrl+S", "Control+S" });
        Dispatch d = dispatch(b, 'S', true, false, false);
        CHECK(d.handled && d.emits == 1 && d.combo == "Ctrl+S",
              "duplicate combos: first match wins, exactly one emission");
    }

    // ---- H: an empty bridge (nothing registered) never matches -------------
    {
        AcceleratorBridge b;
        CHECK(!dispatch(b, 'S', true, false, false).handled
              && !dispatch(b, VK_DELETE, false, false, false).handled,
              "fresh bridge matches nothing");
    }

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
