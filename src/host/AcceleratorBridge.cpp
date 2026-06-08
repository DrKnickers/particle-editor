#include "AcceleratorBridge.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace host {

namespace {

// Map a textual key name to a Win32 VK code. Returns 0 if unknown.
// The caller has already upper-cased the input.
UINT VkFromName(const std::string& upper)
{
    // Single printable character — A..Z and 0..9 map directly to their
    // ASCII values in the Win32 VK table.
    if (upper.size() == 1)
    {
        char c = upper[0];
        if (c >= 'A' && c <= 'Z') return static_cast<UINT>(c);
        if (c >= '0' && c <= '9') return static_cast<UINT>(c);
    }

    if (upper == "DELETE" || upper == "DEL")     return VK_DELETE;
    if (upper == "BACKSPACE" || upper == "BACK") return VK_BACK;
    if (upper == "TAB")                          return VK_TAB;
    if (upper == "ENTER" || upper == "RETURN")   return VK_RETURN;
    if (upper == "ESC"   || upper == "ESCAPE")   return VK_ESCAPE;
    if (upper == "SPACE")                        return VK_SPACE;
    if (upper == "LEFT")                         return VK_LEFT;
    if (upper == "RIGHT")                        return VK_RIGHT;
    if (upper == "UP")                           return VK_UP;
    if (upper == "DOWN")                         return VK_DOWN;
    if (upper == "HOME")                         return VK_HOME;
    if (upper == "END")                          return VK_END;
    if (upper == "PAGEUP"   || upper == "PGUP")  return VK_PRIOR;
    if (upper == "PAGEDOWN" || upper == "PGDN")  return VK_NEXT;
    if (upper == "INSERT")                       return VK_INSERT;
    if (upper == "PRINTSCREEN")                  return VK_SNAPSHOT;
    if (upper == "PAUSE")                        return VK_PAUSE;

    // Function keys F1..F24.
    if (upper.size() >= 2 && upper.size() <= 3 && upper[0] == 'F')
    {
        int n = std::atoi(upper.c_str() + 1);
        if (n >= 1 && n <= 24) return static_cast<UINT>(VK_F1 + (n - 1));
    }

    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// ParseCombo
// ---------------------------------------------------------------------------
// Splits a combo string on '+' and classifies each segment as a modifier
// (Ctrl / Shift / Alt) or the key name. Order of modifiers does not matter:
// "Ctrl+S" == "S+Ctrl".
AcceleratorBridge::Parsed AcceleratorBridge::ParseCombo(const std::string& combo)
{
    Parsed p{ 0, false, false, false, combo };
    std::stringstream ss(combo);
    std::string part;
    while (std::getline(ss, part, '+'))
    {
        // Trim leading/trailing whitespace.
        while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front())))
            part.erase(part.begin());
        while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back())))
            part.pop_back();

        std::string upper = part;
        std::transform(upper.begin(), upper.end(), upper.begin(),
            [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

        if (upper == "CTRL" || upper == "CONTROL") { p.ctrl  = true; continue; }
        if (upper == "SHIFT")                       { p.shift = true; continue; }
        if (upper == "ALT")                         { p.alt   = true; continue; }

        // Treat as the key segment; upper-case it before the VK lookup.
        p.vk = VkFromName(upper);
    }
    return p;
}

// ---------------------------------------------------------------------------
// RegisterCombos
// ---------------------------------------------------------------------------
void AcceleratorBridge::RegisterCombos(const std::vector<std::string>& combos)
{
    m_combos = combos;
    m_parsed.clear();
    m_parsed.reserve(combos.size());
    for (const auto& c : combos)
    {
        Parsed p = ParseCombo(c);
        if (p.vk != 0)
            m_parsed.push_back(std::move(p));
        // Combos with vk==0 (unrecognised key name) are silently ignored;
        // they can never match an AcceleratorKeyPressed event.
    }
}

// ---------------------------------------------------------------------------
// TryDispatch
// ---------------------------------------------------------------------------
bool AcceleratorBridge::TryDispatch(UINT vk, bool ctrl, bool shift, bool alt,
                                     EmitFn emit) const
{
    for (const auto& p : m_parsed)
    {
        if (p.vk == vk && p.ctrl == ctrl && p.shift == shift && p.alt == alt)
        {
            emit(p.combo);
            return true;
        }
    }
    return false;
}

} // namespace host
