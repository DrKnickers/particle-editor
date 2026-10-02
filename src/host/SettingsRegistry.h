#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace host {

// HKCU key under which ALL editor state persists (recent files, view state,
// spawner config, lighting). Mirrors legacy main.cpp's registry layout.
constexpr const wchar_t* kRegistryKeyPath = L"Software\\AloParticleEditor";
// Per-mod display names, one REG_SZ per mod path, in a child of the key above.
constexpr const wchar_t* kRegModNicknamesPath = L"Software\\AloParticleEditor\\ModNicknames";
// REG_SZ: the game folder the user picked when the startup lookup failed.
constexpr const wchar_t* kRegGameDataPath = L"GameDataPath";

// Test seam (ALO_SETTINGS_LIVE=1): lifts the --test-host settings gate so a CDP
// test can exercise the real registry round-trip. The a11y harness never sets
// it, so its plain --test-host launch stays deterministic. Read by both the
// bridge dispatcher's settings gate and the host's mod-stack persistence gate,
// so the two can never disagree.
inline bool ReadSettingsLiveEnv()
{
    wchar_t buf[8] = {};
    DWORD n = GetEnvironmentVariableW(L"ALO_SETTINGS_LIVE", buf, 8);
    return n > 0 && n < 8 && buf[0] == L'1';
}

// One-value registry writers for the settings key above. Every Persist* used
// to hand-open/write/close the key with the identical RegCreateKeyExW
// boilerplate (14 copies across three files); these own the open flags in one
// place. Failure is deliberately silent, matching the old blocks: a failed
// settings write must never interrupt an edit. Empty string => delete the
// value (mirror legacy: clearing a slot deletes it).
inline HKEY OpenSettingsKeyForWrite(const wchar_t* path = kRegistryKeyPath)
{
    HKEY hKey = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr,
                    REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);
    return hKey;   // nullptr on failure; the writers below no-op
}
inline void WriteRegDword(const wchar_t* name, DWORD v,
                          const wchar_t* path = kRegistryKeyPath)
{
    if (HKEY k = OpenSettingsKeyForWrite(path))
    {
        RegSetValueExW(k, name, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&v), sizeof(v));
        RegCloseKey(k);
    }
}
inline void WriteRegSz(const wchar_t* name, const std::wstring& s,
                       const wchar_t* path = kRegistryKeyPath)
{
    if (HKEY k = OpenSettingsKeyForWrite(path))
    {
        if (s.empty())
            RegDeleteValueW(k, name);
        else
            RegSetValueExW(k, name, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(s.c_str()),
                           static_cast<DWORD>((s.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}
inline void WriteRegBinary(const wchar_t* name, const void* data, DWORD bytes,
                           const wchar_t* path = kRegistryKeyPath)
{
    if (HKEY k = OpenSettingsKeyForWrite(path))
    {
        RegSetValueExW(k, name, 0, REG_BINARY,
                       static_cast<const BYTE*>(data), bytes);
        RegCloseKey(k);
    }
}
// REG_MULTI_SZ from an already-serialized blob (each string NUL-terminated,
// plus the final extra NUL). Unlike the writers above this reports failure:
// a caller whose value is authoritative needs to know it was not saved.
inline bool WriteRegMultiSz(const wchar_t* name, const std::wstring& blob,
                            const wchar_t* path = kRegistryKeyPath)
{
    HKEY k = OpenSettingsKeyForWrite(path);
    if (!k) return false;
    const LONG rc = RegSetValueExW(k, name, 0, REG_MULTI_SZ,
                                   reinterpret_cast<const BYTE*>(blob.data()),
                                   static_cast<DWORD>(blob.size() * sizeof(wchar_t)));
    RegCloseKey(k);
    return rc == ERROR_SUCCESS;
}

inline HKEY OpenSettingsKeyForRead(const wchar_t* path = kRegistryKeyPath)
{
    HKEY hKey = nullptr;
    RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &hKey);
    return hKey;   // nullptr on failure/first-run; reads below all fail-soft
}

inline bool ReadRegDword(HKEY hKey, const wchar_t* name, DWORD& out)
{
    DWORD t = 0, s = sizeof(out), v = 0;
    if (RegQueryValueExW(hKey, name, nullptr, &t,
                         reinterpret_cast<LPBYTE>(&v), &s) == ERROR_SUCCESS
        && t == REG_DWORD && s == sizeof(out)) { out = v; return true; }
    return false;
}

// REG_BINARY 32-bit float; reject NaN/Inf so a corrupt blob can't poison state
// (preserves the legacy readF guard). Mutates out only on success.
inline bool ReadRegFloat(HKEY hKey, const wchar_t* name, float& out)
{
    float v = 0.0f; DWORD t = 0, s = sizeof(v);
    if (RegQueryValueExW(hKey, name, nullptr, &t,
                         reinterpret_cast<LPBYTE>(&v), &s) == ERROR_SUCCESS
        && t == REG_BINARY && s == sizeof(v)
        && v == v && (v - v) == 0.0f) { out = v; return true; }
    return false;
}

// Fixed-count REG_BINARY float array (ReferenceObjectTransform=6, GridSpacing=1),
// NaN/Inf-rejected per element. Writes out[0..count) only on full success.
inline bool ReadRegFloatArray(HKEY hKey, const wchar_t* name, float* out, DWORD count)
{
    DWORD t = 0, cb = count * sizeof(float);
    std::vector<float> tmp(count, 0.0f);
    if (RegQueryValueExW(hKey, name, nullptr, &t,
            reinterpret_cast<BYTE*>(tmp.data()), &cb) != ERROR_SUCCESS
        || t != REG_BINARY || cb != count * sizeof(float))
        return false;
    for (DWORD i = 0; i < count; ++i)
        if (!(tmp[i] == tmp[i] && (tmp[i] - tmp[i]) == 0.0f)) return false;
    for (DWORD i = 0; i < count; ++i) out[i] = tmp[i];
    return true;
}

// REG_SZ two-pass sized read (mirrors legacy ReadGroundSlotPath: the stored
// value may omit the trailing NUL). Empty string on any failure.
inline std::wstring ReadRegSz(HKEY hKey, const wchar_t* name)
{
    DWORD t = 0, cb = 0;
    if (RegQueryValueExW(hKey, name, nullptr, &t, nullptr, &cb) != ERROR_SUCCESS
        || t != REG_SZ || cb < sizeof(wchar_t))
        return std::wstring();
    std::vector<wchar_t> buf(cb / sizeof(wchar_t) + 1, 0);
    if (RegQueryValueExW(hKey, name, nullptr, &t,
            reinterpret_cast<LPBYTE>(buf.data()), &cb) != ERROR_SUCCESS)
        return std::wstring();
    buf.back() = 0;
    return std::wstring(buf.data());
}

// REG_MULTI_SZ two-pass sized read. Returns the raw characters (embedded NULs
// included) for the caller to split, since splitting rules belong to the
// value's owner. Empty on any failure.
inline std::vector<wchar_t> ReadRegMultiSz(HKEY hKey, const wchar_t* name)
{
    DWORD t = 0, cb = 0;
    if (RegQueryValueExW(hKey, name, nullptr, &t, nullptr, &cb) != ERROR_SUCCESS
        || t != REG_MULTI_SZ || cb < sizeof(wchar_t))
        return std::vector<wchar_t>();
    std::vector<wchar_t> buf(cb / sizeof(wchar_t));
    if (RegQueryValueExW(hKey, name, nullptr, &t,
            reinterpret_cast<LPBYTE>(buf.data()), &cb) != ERROR_SUCCESS)
        return std::vector<wchar_t>();
    return buf;
}

} // namespace host
