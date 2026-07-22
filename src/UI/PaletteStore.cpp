// PaletteStore (data layer only).
//
// Split out from TexturePalette.cpp so the data layer can be tested in
// isolation (no d3dx9 / GDI dependencies). Popup window, content
// control, and thumbnail decoder live in TexturePalette.cpp.

#include "TexturePalette.h"
#include "../crc32.h"
#include "../utils.h"

#include <windows.h>
#include <shlobj.h>      // SHGetFolderPathW
#include <algorithm>
#include <cwctype>
#include <cstdio>
#include <cstring>
#include <cstdarg>

#pragma comment(lib, "Shell32.lib")

using namespace TexturePalette;
using std::wstring;
using std::vector;
using std::unordered_map;

namespace {

// Lowercase a wstring in-place using the C locale. INI section keys
// derive from the lowercased mod path so that the user opening
// "C:\Mods\RaW" and "C:\mods\raw" share one palette.
wstring LowercaseCopy(const wstring& s)
{
    wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) out.push_back((wchar_t)towlower(c));
    return out;
}

// Encode/decode the slot mask as "color", "bump", or "color,bump".
// Reading a value not in this set returns SLOT_NONE so corrupt INI
// lines fail closed.
wstring EncodeSlotMask(uint8_t mask)
{
    if ((mask & (SLOT_COLOR | SLOT_BUMP)) == (SLOT_COLOR | SLOT_BUMP)) return L"color,bump";
    if (mask & SLOT_COLOR) return L"color";
    if (mask & SLOT_BUMP)  return L"bump";
    return L"";
}

uint8_t DecodeSlotMask(const wstring& s)
{
    uint8_t m = SLOT_NONE;
    if (s.find(L"color") != wstring::npos) m |= SLOT_COLOR;
    if (s.find(L"bump")  != wstring::npos) m |= SLOT_BUMP;
    return m;
}

// Reject filenames containing INI metacharacters or control chars
// — defensive sanity-check at TouchRecent / TogglePin boundaries.
bool FilenameOk(const wstring& f)
{
    // A texture filename is a basename; nothing legitimate needs > 260 chars, and
    // an unbounded value would attempt an oversized WritePrivateProfileStringW.
    if (f.empty() || f.size() > 260) return false;
    for (wchar_t c : f)
    {
        if (c < 0x20) return false;
        // '=' is the INI key/value separator; '|' is this store's own recent-entry
        // field delimiter (filename|mask|timestamp) — either would corrupt the
        // round-trip, so reject both alongside CR/LF.
        if (c == L'=' || c == L'|' || c == L'\r' || c == L'\n') return false;
    }
    return true;
}

// QueryPerformanceCounter wrapper — monotonically-increasing stamp
// for recents LRU sort.
uint64_t Now()
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (uint64_t)li.QuadPart;
}

wstring AppDataDir()
{
    wchar_t buf[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, buf) == S_OK)
        return wstring(buf);
    return wstring();
}

wstring PaletteDir()
{
    // Test seam only; production leaves ALO_PALETTE_DIR unset. The override
    // directory must already exist (IniPath's CreateDirectoryW is single-level;
    // the test pre-creates its temp dir).
    wchar_t overrideBuf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ALO_PALETTE_DIR", overrideBuf, _countof(overrideBuf));
    if (n > 0 && n < _countof(overrideBuf)) return wstring(overrideBuf, n);
    if (n >= _countof(overrideBuf))
    {
        vector<wchar_t> dynamicBuf(n);
        n = GetEnvironmentVariableW(L"ALO_PALETTE_DIR", dynamicBuf.data(), (DWORD)dynamicBuf.size());
        if (n > 0 && n < dynamicBuf.size()) return wstring(dynamicBuf.data(), n);
    }

    const wstring appData = AppDataDir();
    if (appData.empty()) return wstring();
    return appData + L"\\AloParticleEditor";
}

wstring IniGetString(const wstring& iniPath, const wstring& section,
                     const wstring& key, const wstring& defaultValue)
{
    wchar_t buf[2048];
    DWORD n = GetPrivateProfileStringW(section.c_str(), key.c_str(), defaultValue.c_str(),
                                       buf, _countof(buf), iniPath.c_str());
    return wstring(buf, n);
}

int IniGetInt(const wstring& iniPath, const wstring& section, const wstring& key, int defaultValue)
{
    return (int)GetPrivateProfileIntW(section.c_str(), key.c_str(), defaultValue, iniPath.c_str());
}

void IniSet(const wstring& iniPath, const wstring& section, const wstring& key, const wstring& value)
{
    WritePrivateProfileStringW(section.c_str(), key.c_str(), value.c_str(), iniPath.c_str());
}

void IniSetInt(const wstring& iniPath, const wstring& section, const wstring& key, int value)
{
    wchar_t buf[16];
    swprintf_s(buf, L"%d", value);
    WritePrivateProfileStringW(section.c_str(), key.c_str(), buf, iniPath.c_str());
}

void IniEraseSection(const wstring& iniPath, const wstring& section)
{
    WritePrivateProfileStringW(section.c_str(), NULL, NULL, iniPath.c_str());
}

#ifndef NDEBUG
void DbgPrintf(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsprintf_s(buf, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
}
#else
void DbgPrintf(const char*, ...) {}
#endif

} // anonymous namespace

// ============================================================================
// Store

Store::Store()
    : m_iniPathChecked(false)
{
}

Store::~Store()
{
}

Store& Store::Instance()
{
    static Store s_instance;
    return s_instance;
}

void Store::SetEphemeral(bool ephemeral)
{
    if (m_ephemeral == ephemeral) return;

    // The cache can contain either real user state or capture-local seeded
    // state. Never carry either across the automation boundary: record/drive
    // must start empty, and interactive mode must reload the persisted palette.
    m_ephemeral = ephemeral;
    m_activeMod.clear();
    m_byMod.clear();
}

wstring Store::IniPath() const
{
    if (m_iniPathChecked) return m_iniPathCache;
    m_iniPathChecked = true;

    const wstring dir = PaletteDir();
    if (dir.empty()) { m_iniPathCache.clear(); return m_iniPathCache; }

    CreateDirectoryW(dir.c_str(), NULL);
    m_iniPathCache = dir + L"\\texture-palettes.ini";
    return m_iniPathCache;
}

wstring Store::SectionFor(const wstring& modPath) const
{
    const wstring lowered = LowercaseCopy(modPath);
    const std::string utf8 = WideToAnsi(lowered);
    const unsigned long h = crc32(utf8.c_str(), utf8.size());
    wchar_t buf[24];
    swprintf_s(buf, L"mod=%08lx", h);
    return wstring(buf);
}

void Store::SetActiveMod(const wstring& modPath)
{
    if (modPath.empty())
    {
        // Switching to "unmodded" — just deactivate the current mod.
        // Critically, do NOT wipe the previous mod's INI section: a
        // user toggling between Unmodded and ModA should still find
        // ModA's palette intact when they switch back. Section wiping
        // is reserved for Reset View Settings (ClearActiveMod).
        DbgPrintf("[Palette] mod switch from='%s' to=unmodded\n",
                  WideToAnsi(m_activeMod).c_str());
        m_activeMod.clear();
        return;
    }

    const wstring oldMod = m_activeMod;
    const wstring newKey = LowercaseCopy(modPath);

    if (LowercaseCopy(oldMod) == newKey)
    {
        m_activeMod = modPath;
        return;
    }

    m_activeMod = modPath;
    LoadOrInit(modPath);

    DbgPrintf("[Palette] mod switch from='%s' to='%s' loadedEntries=%zu\n",
              WideToAnsi(oldMod).c_str(),
              WideToAnsi(modPath).c_str(),
              m_byMod[newKey].entries.size());
}

void Store::ClearActiveMod()
{
    if (m_activeMod.empty()) return;

    const wstring section = SectionFor(m_activeMod);
    const wstring iniPath = m_ephemeral ? wstring() : IniPath();
    if (!iniPath.empty()) IniEraseSection(iniPath, section);

    const wstring key = LowercaseCopy(m_activeMod);
    m_byMod.erase(key);
    DbgPrintf("[Palette] cleared mod='%s'\n", WideToAnsi(m_activeMod).c_str());
}

Store::ModPalette& Store::LoadOrInit(const wstring& modPath)
{
    const wstring key = LowercaseCopy(modPath);
    auto it = m_byMod.find(key);
    if (it != m_byMod.end()) return it->second;

    ModPalette mp;
    mp.filter = SLOT_COLOR;

    const wstring iniPath = m_ephemeral ? wstring() : IniPath();
    if (!iniPath.empty())
    {
        const wstring section = SectionFor(modPath);
        const wstring filterStr = IniGetString(iniPath, section, L"Filter", L"Color");
        if (filterStr == L"Bump") mp.filter = SLOT_BUMP;

        int pinCount = IniGetInt(iniPath, section, L"PinCount", 0);
        if (pinCount < 0) pinCount = 0;
        if (pinCount > (int)MAX_PINS) pinCount = (int)MAX_PINS;
        for (int i = 0; i < pinCount; ++i)
        {
            wchar_t key2[32];
            swprintf_s(key2, L"Pin%d", i);
            wstring v = IniGetString(iniPath, section, key2, L"");
            size_t pipe = v.find(L'|');
            if (pipe == wstring::npos) continue;
            const wstring name = v.substr(0, pipe);
            const wstring mask = v.substr(pipe + 1);
            if (!FilenameOk(name))
            {
                DbgPrintf("[Palette] dropped malformed pin entry '%s'\n", WideToAnsi(name).c_str());
                continue;
            }
            const uint8_t m = DecodeSlotMask(mask);
            if (m == SLOT_NONE)
            {
                DbgPrintf("[Palette] dropped pin '%s' with bad slot mask '%s'\n",
                          WideToAnsi(name).c_str(), WideToAnsi(mask).c_str());
                continue;
            }
            Entry e;
            e.filename   = name;
            e.isPinned   = true;
            e.slotMask   = m;
            e.lastUsedNs = 0;
            mp.entries.push_back(e);
        }

        int recentCount = IniGetInt(iniPath, section, L"RecentCount", 0);
        if (recentCount < 0) recentCount = 0;
        if (recentCount > (int)MAX_RECENTS) recentCount = (int)MAX_RECENTS;
        for (int i = 0; i < recentCount; ++i)
        {
            wchar_t key2[32];
            swprintf_s(key2, L"Recent%d", i);
            wstring v = IniGetString(iniPath, section, key2, L"");
            size_t pipe1 = v.find(L'|');
            if (pipe1 == wstring::npos) continue;
            size_t pipe2 = v.find(L'|', pipe1 + 1);
            if (pipe2 == wstring::npos) continue;
            const wstring name = v.substr(0, pipe1);
            const wstring mask = v.substr(pipe1 + 1, pipe2 - pipe1 - 1);
            if (!FilenameOk(name))
            {
                DbgPrintf("[Palette] dropped malformed recent entry '%s'\n", WideToAnsi(name).c_str());
                continue;
            }
            const uint8_t m = DecodeSlotMask(mask);
            if (m == SLOT_NONE)
            {
                DbgPrintf("[Palette] dropped recent '%s' with bad slot mask '%s'\n",
                          WideToAnsi(name).c_str(), WideToAnsi(mask).c_str());
                continue;
            }
            Entry e;
            e.filename   = name;
            e.isPinned   = false;
            e.slotMask   = m;
            e.lastUsedNs = (uint64_t)(recentCount - i);
            mp.entries.push_back(e);
        }
    }

    auto inserted = m_byMod.emplace(key, std::move(mp));
    return inserted.first->second;
}

SlotMask Store::ActiveFilter() const
{
    if (m_activeMod.empty()) return SLOT_COLOR;
    const wstring key = LowercaseCopy(m_activeMod);
    auto it = m_byMod.find(key);
    if (it == m_byMod.end()) return SLOT_COLOR;
    return it->second.filter;
}

void Store::SetActiveFilter(SlotMask filter)
{
    if (m_activeMod.empty()) return;
    if (filter != SLOT_COLOR && filter != SLOT_BUMP) return;

    ModPalette& mp = LoadOrInit(m_activeMod);
    if (mp.filter == filter) return;
    mp.filter = filter;
    FlushMod(m_activeMod, mp);
}

void Store::TouchRecent(const wstring& filename, SlotMask usedAs)
{
    if (m_activeMod.empty()) return;
    if (!FilenameOk(filename))
    {
        DbgPrintf("[Palette] rejected TouchRecent for malformed name\n");
        return;
    }
    if (usedAs != SLOT_COLOR && usedAs != SLOT_BUMP) return;

    ModPalette& mp = LoadOrInit(m_activeMod);

    for (Entry& e : mp.entries)
    {
        if (e.filename == filename)
        {
            e.slotMask  |= (uint8_t)usedAs;
            e.lastUsedNs = Now();
            FlushMod(m_activeMod, mp);
            DbgPrintf("[Palette] touch recent name='%s' slot=%s (existing)\n",
                      WideToAnsi(filename).c_str(),
                      usedAs == SLOT_COLOR ? "Color" : "Bump");
            return;
        }
    }

    Entry e;
    e.filename   = filename;
    e.isPinned   = false;
    e.slotMask   = (uint8_t)usedAs;
    e.lastUsedNs = Now();

    size_t matchingRecents = 0;
    for (const Entry& cand : mp.entries)
    {
        if (!cand.isPinned && (cand.slotMask & usedAs)) ++matchingRecents;
    }
    if (matchingRecents >= MAX_RECENTS)
    {
        size_t   oldestIdx = (size_t)-1;
        uint64_t oldestNs  = UINT64_MAX;
        for (size_t i = 0; i < mp.entries.size(); ++i)
        {
            const Entry& cand = mp.entries[i];
            if (cand.isPinned) continue;
            if (!(cand.slotMask & usedAs)) continue;
            if (cand.lastUsedNs < oldestNs)
            {
                oldestNs  = cand.lastUsedNs;
                oldestIdx = i;
            }
        }
        if (oldestIdx != (size_t)-1)
        {
            DbgPrintf("[Palette] evicted oldest recent '%s'\n",
                      WideToAnsi(mp.entries[oldestIdx].filename).c_str());
            mp.entries.erase(mp.entries.begin() + oldestIdx);
        }
    }

    mp.entries.push_back(e);
    FlushMod(m_activeMod, mp);
    DbgPrintf("[Palette] touch recent name='%s' slot=%s (new)\n",
              WideToAnsi(filename).c_str(),
              usedAs == SLOT_COLOR ? "Color" : "Bump");
}

bool Store::TogglePin(const wstring& filename)
{
    if (m_activeMod.empty()) return false;
    if (!FilenameOk(filename)) return false;

    ModPalette& mp = LoadOrInit(m_activeMod);

    for (Entry& e : mp.entries)
    {
        if (e.filename == filename)
        {
            if (e.isPinned)
            {
                e.isPinned   = false;
                e.lastUsedNs = Now();
                FlushMod(m_activeMod, mp);
                DbgPrintf("[Palette] toggle pin name='%s' newState=false\n",
                          WideToAnsi(filename).c_str());
                return true;
            }
            else
            {
                size_t pinCount = 0;
                for (const Entry& cand : mp.entries)
                {
                    if (cand.isPinned) ++pinCount;
                }
                if (pinCount >= MAX_PINS)
                {
                    DbgPrintf("[Palette] pin rejected (full): '%s'\n",
                              WideToAnsi(filename).c_str());
                    return false;
                }
                e.isPinned = true;
                FlushMod(m_activeMod, mp);
                DbgPrintf("[Palette] toggle pin name='%s' newState=true\n",
                          WideToAnsi(filename).c_str());
                return true;
            }
        }
    }
    return false;
}

vector<Entry> Store::Pins(SlotMask filter) const
{
    vector<Entry> out;
    if (m_activeMod.empty()) return out;
    const wstring key = LowercaseCopy(m_activeMod);
    auto it = m_byMod.find(key);
    if (it == m_byMod.end()) return out;
    for (const Entry& e : it->second.entries)
    {
        if (e.isPinned && (e.slotMask & filter)) out.push_back(e);
    }
    return out;
}

vector<Entry> Store::Recents(SlotMask filter) const
{
    vector<Entry> out;
    if (m_activeMod.empty()) return out;
    const wstring key = LowercaseCopy(m_activeMod);
    auto it = m_byMod.find(key);
    if (it == m_byMod.end()) return out;
    for (const Entry& e : it->second.entries)
    {
        if (!e.isPinned && (e.slotMask & filter)) out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        return a.lastUsedNs > b.lastUsedNs;
    });
    return out;
}

void Store::FlushMod(const wstring& modPath, const ModPalette& mp) const
{
    // Automation isolation: in --record / --drive, in-memory recents/pins are
    // already updated by the caller, but the persistent INI must not be touched —
    // FlushMod erases + rewrites the whole real mod section (evicting user recents),
    // which an unattended tutorial/CI run must never do. Skip the disk write.
    if (m_ephemeral) return;
    const wstring iniPath = IniPath();
    if (iniPath.empty()) return;

    const wstring section = SectionFor(modPath);
    IniEraseSection(iniPath, section);

    IniSet(iniPath, section, L"Path", modPath);
    IniSet(iniPath, section, L"Filter", mp.filter == SLOT_BUMP ? L"Bump" : L"Color");

    int pinIdx = 0, recentIdx = 0;
    vector<const Entry*> recentSorted;
    for (const Entry& e : mp.entries)
    {
        if (e.isPinned)
        {
            wchar_t key[32]; swprintf_s(key, L"Pin%d", pinIdx++);
            wstring val = e.filename + L"|" + EncodeSlotMask(e.slotMask);
            IniSet(iniPath, section, key, val);
        }
        else
        {
            recentSorted.push_back(&e);
        }
    }
    std::sort(recentSorted.begin(), recentSorted.end(),
              [](const Entry* a, const Entry* b) { return a->lastUsedNs > b->lastUsedNs; });
    for (const Entry* e : recentSorted)
    {
        wchar_t key[32]; swprintf_s(key, L"Recent%d", recentIdx++);
        wchar_t tsBuf[32]; swprintf_s(tsBuf, L"%llu", (unsigned long long)e->lastUsedNs);
        wstring val = e->filename + L"|" + EncodeSlotMask(e->slotMask) + L"|" + tsBuf;
        IniSet(iniPath, section, key, val);
    }
    IniSetInt(iniPath, section, L"PinCount",    pinIdx);
    IniSetInt(iniPath, section, L"RecentCount", recentIdx);
}
