// Registry round-trip regression for the engine-free persisted-settings read
// phase. Uses the production writers/readers against a throwaway HKCU child;
// no Engine, D3D9, or WebView2 code is linked.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "host/LightingSettings.h"
#include "host/RestoredSettings.h"
#include "host/SettingsRegistry.h"

#include <array>
#include <cstdio>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_fail = 0;

void Check(bool condition, const char* expression, int line)
{
    if (condition)
    {
        std::printf("  ok: %s\n", expression);
        return;
    }
    ++g_fail;
    std::printf("  FAIL line %d: %s\n", line, expression);
}

#define CHECK(condition) Check((condition), #condition, __LINE__)

class RegistryCleanup
{
public:
    explicit RegistryCleanup(const std::wstring& path) : m_path(path)
    {
        RegDeleteTreeW(HKEY_CURRENT_USER, m_path.c_str());
    }

    ~RegistryCleanup()
    {
        RegDeleteTreeW(HKEY_CURRENT_USER, m_path.c_str());
    }

private:
    std::wstring m_path;
};

bool PathsEqual(const std::vector<std::pair<int, std::wstring>>& actual,
                const std::vector<std::pair<int, std::wstring>>& expected)
{
    return actual == expected;
}

host::RestoredSettings Read(const std::wstring& path, bool inCaptureMode)
{
    HKEY key = host::OpenSettingsKeyForRead(path.c_str());
    host::RestoredSettings settings = host::ReadRestoredSettings(key, inCaptureMode);
    if (key) RegCloseKey(key);
    return settings;
}

bool HasRegistryType(HKEY key, const wchar_t* name, DWORD type)
{
    DWORD actualType = 0;
    return RegQueryValueExW(key, name, nullptr, &actualType, nullptr, nullptr)
            == ERROR_SUCCESS
        && actualType == type;
}

void CheckLightingSchema(HKEY key)
{
    struct Entry { const wchar_t* literal; const wchar_t* shared; DWORD type; };
    // Independent external schema pin: do not derive the literals from the
    // production constants, or a typo'd shared name would round-trip itself.
    const Entry entries[] = {
        { L"LightSunIntensity",       host::kLightSunIntensity,       REG_BINARY },
        { L"LightSunZAngle",          host::kLightSunZAngle,          REG_BINARY },
        { L"LightSunTilt",            host::kLightSunTilt,            REG_BINARY },
        { L"LightSunAmbientColor",    host::kLightSunAmbientColor,    REG_DWORD },
        { L"LightSunSpecularColor",   host::kLightSunSpecularColor,   REG_DWORD },
        { L"LightSunDiffuseColor",    host::kLightSunDiffuseColor,    REG_DWORD },
        { L"LightSunShadowColor",     host::kLightSunShadowColor,     REG_DWORD },
        { L"LightingForceFillAlignment", host::kLightForceFillAlignment, REG_DWORD },
        { L"LightFill1Intensity",     host::kLightFill1Intensity,     REG_BINARY },
        { L"LightFill1ZAngle",        host::kLightFill1ZAngle,        REG_BINARY },
        { L"LightFill1Tilt",          host::kLightFill1Tilt,          REG_BINARY },
        { L"LightFill1DiffuseColor",  host::kLightFill1DiffuseColor,  REG_DWORD },
        { L"LightFill2Intensity",     host::kLightFill2Intensity,     REG_BINARY },
        { L"LightFill2ZAngle",        host::kLightFill2ZAngle,        REG_BINARY },
        { L"LightFill2Tilt",          host::kLightFill2Tilt,          REG_BINARY },
        { L"LightFill2DiffuseColor",  host::kLightFill2DiffuseColor,  REG_DWORD },
    };
    for (const Entry& entry : entries)
    {
        CHECK(std::wstring(entry.shared) == entry.literal);
        CHECK(HasRegistryType(key, entry.shared, entry.type));
    }
}

void WriteLightingSentinels(const std::wstring& path)
{
    const float sunIntensity = 1.25f, sunZ = 12.5f, sunTilt = 33.5f;
    const float fill1Intensity = 2.25f, fill1Z = 132.5f, fill1Tilt = -12.5f;
    const float fill2Intensity = 3.25f, fill2Z = 222.5f, fill2Tilt = -22.5f;
    host::WriteRegBinary(host::kLightSunIntensity, &sunIntensity, sizeof(sunIntensity), path.c_str());
    host::WriteRegBinary(host::kLightSunZAngle, &sunZ, sizeof(sunZ), path.c_str());
    host::WriteRegBinary(host::kLightSunTilt, &sunTilt, sizeof(sunTilt), path.c_str());
    host::WriteRegDword(host::kLightSunAmbientColor, 0x00010203, path.c_str());
    host::WriteRegDword(host::kLightSunSpecularColor, 0x00040506, path.c_str());
    host::WriteRegDword(host::kLightSunDiffuseColor, 0x00070809, path.c_str());
    host::WriteRegDword(host::kLightSunShadowColor, 0x000A0B0C, path.c_str());
    host::WriteRegDword(host::kLightForceFillAlignment, 0, path.c_str());
    host::WriteRegBinary(host::kLightFill1Intensity, &fill1Intensity, sizeof(fill1Intensity), path.c_str());
    host::WriteRegBinary(host::kLightFill1ZAngle, &fill1Z, sizeof(fill1Z), path.c_str());
    host::WriteRegBinary(host::kLightFill1Tilt, &fill1Tilt, sizeof(fill1Tilt), path.c_str());
    host::WriteRegDword(host::kLightFill1DiffuseColor, 0x000D0E0F, path.c_str());
    host::WriteRegBinary(host::kLightFill2Intensity, &fill2Intensity, sizeof(fill2Intensity), path.c_str());
    host::WriteRegBinary(host::kLightFill2ZAngle, &fill2Z, sizeof(fill2Z), path.c_str());
    host::WriteRegBinary(host::kLightFill2Tilt, &fill2Tilt, sizeof(fill2Tilt), path.c_str());
    host::WriteRegDword(host::kLightFill2DiffuseColor, 0x00101112, path.c_str());
}

} // namespace

int main()
{
    std::printf("test_settings_restore\n");
    const std::wstring testPath = L"Software\\ParticleEditorTest-" +
                                  std::to_wstring(GetCurrentProcessId());
    RegistryCleanup cleanup(testPath);

    const float bloomStrength = 0.75f;
    const std::array<float, 6> transform = { 1, 2, 3, 4, 5, 6 };
    const float gridSpacing = 37.5f;
    host::WriteRegDword(L"BloomEnabled", 1, testPath.c_str());
    host::WriteRegBinary(L"BloomStrength", &bloomStrength, sizeof(bloomStrength), testPath.c_str());
    host::WriteRegDword(L"BackgroundColor", 0x00123456, testPath.c_str());
    host::WriteRegDword(L"ShowGround", 0, testPath.c_str());
    host::WriteRegSz(L"GroundTextureSlot2", L"ground-two", testPath.c_str());
    host::WriteRegSz(L"GroundTextureSlot0", L"ground-zero", testPath.c_str());
    host::WriteRegDword(L"GroundSolidColor", 0x00654321, testPath.c_str());
    host::WriteRegDword(L"GroundTexture", 2, testPath.c_str());
    host::WriteRegSz(L"SkydomeCustomSlot11", L"sky-eleven", testPath.c_str());
    host::WriteRegSz(L"SkydomeCustomSlot9", L"sky-nine", testPath.c_str());
    host::WriteRegDword(L"SkydomeIndex", 11, testPath.c_str());
    host::WriteRegSz(L"SkydomePrimaryName", L"PrimaryDome", testPath.c_str());
    host::WriteRegSz(L"SkydomeSecondaryName", L"SecondaryDome", testPath.c_str());
    host::WriteRegDword(L"SkydomeContext", 0, testPath.c_str());
    host::WriteRegBinary(L"ReferenceObjectTransform", transform.data(),
                          static_cast<DWORD>(sizeof(transform)), testPath.c_str());
    host::WriteRegDword(L"ReferenceObjectVisible", 0, testPath.c_str());
    host::WriteRegDword(L"GridVisible", 1, testPath.c_str());
    host::WriteRegBinary(L"GridSpacing", &gridSpacing, sizeof(gridSpacing), testPath.c_str());
    host::WriteRegDword(L"SnapEnabled", 1, testPath.c_str());
    host::WriteRegDword(L"ReferenceObjectLocked", 1, testPath.c_str());
    host::WriteRegSz(L"ReferenceObjectName", L"AT_AT", testPath.c_str());
    WriteLightingSentinels(testPath);

    HKEY testKey = host::OpenSettingsKeyForRead(testPath.c_str());
    CHECK(testKey != nullptr);
    if (testKey) CheckLightingSchema(testKey);
    if (testKey) RegCloseKey(testKey);

    const host::RestoredSettings settings = Read(testPath, false);
    CHECK(settings.bloomEnabled && *settings.bloomEnabled);
    CHECK(settings.bloomStrength && *settings.bloomStrength == bloomStrength);
    CHECK(!settings.bloomCutoff && !settings.bloomSize);
    CHECK(settings.backgroundColor && *settings.backgroundColor == 0x00123456);
    CHECK(settings.showGround && !*settings.showGround);
    CHECK(PathsEqual(settings.groundSlotPaths,
                     { { 0, L"ground-zero" }, { 2, L"ground-two" } }));
    CHECK(settings.groundSolidColor && *settings.groundSolidColor == 0x00654321);
    CHECK(settings.groundTexture && *settings.groundTexture == 2);
    CHECK(PathsEqual(settings.skydomeCustomPaths,
                     { { 9, L"sky-nine" }, { 10, L"" }, { 11, L"sky-eleven" } }));
    CHECK(settings.skydomeSlot && *settings.skydomeSlot == 11);
    CHECK(settings.hasSkydomeEnv && settings.skydomeContextRaw == 0);
    CHECK(settings.skydomePrimaryName == L"PrimaryDome" &&
          settings.skydomeSecondaryName == L"SecondaryDome");
    CHECK(settings.refTransform && *settings.refTransform == transform);
    CHECK(settings.refVisible && !*settings.refVisible);
    CHECK(settings.gridVisible && *settings.gridVisible);
    CHECK(settings.gridSpacing && *settings.gridSpacing == gridSpacing);
    CHECK(settings.snapEnabled && *settings.snapEnabled);
    CHECK(settings.refLocked && *settings.refLocked);
    CHECK(settings.refName && *settings.refName == L"AT_AT");
    CHECK(settings.sunIntensity == 1.25f && settings.sunZ == 12.5f && settings.sunTilt == 33.5f);
    CHECK(settings.sunAmbient == 0x00010203 && settings.sunSpecular == 0x00040506 &&
          settings.sunDiffuse == 0x00070809 && settings.sunShadow == 0x000A0B0C);
    CHECK(!settings.forceAlign);
    CHECK(settings.fill1Intensity == 2.25f && settings.fill1Zp == 132.5f &&
          settings.fill1Tiltp == -12.5f && settings.fill1Diffuse == 0x000D0E0F);
    CHECK(settings.fill2Intensity == 3.25f && settings.fill2Zp == 222.5f &&
          settings.fill2Tiltp == -22.5f && settings.fill2Diffuse == 0x00101112);

    const host::RestoredSettings captureSettings = Read(testPath, true);
    CHECK(!captureSettings.refName);

    const std::wstring missingPath = L"Software\\NoSuchKey-" +
                                     std::to_wstring(GetCurrentProcessId());
    RegDeleteTreeW(HKEY_CURRENT_USER, missingPath.c_str());
    HKEY missingKey = host::OpenSettingsKeyForRead(missingPath.c_str());
    CHECK(missingKey == nullptr);
    if (missingKey) RegCloseKey(missingKey);
    const host::RestoredSettings firstRun = host::ReadRestoredSettings(nullptr, false);
    CHECK(!firstRun.bloomEnabled && !firstRun.bloomStrength && !firstRun.bloomCutoff &&
          !firstRun.bloomSize && !firstRun.backgroundColor && !firstRun.showGround &&
          !firstRun.groundSolidColor && !firstRun.groundTexture && !firstRun.skydomeSlot &&
          !firstRun.refTransform && !firstRun.refVisible && !firstRun.gridVisible &&
          !firstRun.gridSpacing && !firstRun.snapEnabled && !firstRun.refLocked && !firstRun.refName);
    CHECK(firstRun.groundSlotPaths.empty());
    CHECK(!firstRun.hasSkydomeEnv && firstRun.skydomeContextRaw == 1 &&
          firstRun.skydomePrimaryName.empty() && firstRun.skydomeSecondaryName.empty());
    // Legacy always clears the three custom skydome slots before applying its
    // selected index, including on first run. Preserve those ordered empty calls.
    CHECK(PathsEqual(firstRun.skydomeCustomPaths,
                     { { 9, L"" }, { 10, L"" }, { 11, L"" } }));
    CHECK(firstRun.sunIntensity == host::kSunIntensityDefault &&
          firstRun.sunZ == host::kSunZAngleDefault && firstRun.sunTilt == host::kSunTiltDefault &&
          firstRun.sunAmbient == host::SunAmbientColorDefault() &&
          firstRun.sunSpecular == host::SunSpecularColorDefault() &&
          firstRun.sunDiffuse == host::SunDiffuseColorDefault() &&
          firstRun.sunShadow == host::SunShadowColorDefault() &&
          firstRun.forceAlign == host::kForceAlignDefault &&
          firstRun.fill1Intensity == host::kFill1IntensityDefault &&
          firstRun.fill1Zp == host::kFill1ZAngleDefault &&
          firstRun.fill1Tiltp == host::kFill1TiltDefault &&
          firstRun.fill1Diffuse == host::Fill1DiffuseColorDefault() &&
          firstRun.fill2Intensity == host::kFill2IntensityDefault &&
          firstRun.fill2Zp == host::kFill2ZAngleDefault &&
          firstRun.fill2Tiltp == host::kFill2TiltDefault &&
          firstRun.fill2Diffuse == host::Fill2DiffuseColorDefault());

    host::WriteRegSz(L"BloomStrength", L"wrong type", testPath.c_str());
    CHECK(!Read(testPath, false).bloomStrength);
    const std::array<float, 2> wrongSize = { 1.0f, 2.0f };
    host::WriteRegBinary(L"BloomStrength", wrongSize.data(),
                          static_cast<DWORD>(sizeof(wrongSize)), testPath.c_str());
    CHECK(!Read(testPath, false).bloomStrength);
    const DWORD nanBits = 0x7FC00000u;
    host::WriteRegBinary(L"BloomStrength", &nanBits, sizeof(nanBits), testPath.c_str());
    CHECK(!Read(testPath, false).bloomStrength);
    host::WriteRegSz(L"BloomEnabled", L"wrong type", testPath.c_str());
    CHECK(!Read(testPath, false).bloomEnabled);

    std::printf("%s\n", g_fail ? "=== FAILED ===" : "=== ALL PASS ===");
    return g_fail ? 1 : 0;
}
