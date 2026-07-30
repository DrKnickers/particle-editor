// Production-linked regression test for the registry-backed recent-file MRU.
//
// RegOverridePredefKey redirects this process's HKEY_CURRENT_USER to a unique
// sandbox. The test exercises the exact production RecentFiles.cpp functions
// without reading or writing the developer's real AloParticleEditor values.
//
// Build (from repo root):
//   tests\build_test_recent_files.bat
// Run:
//   tests\test_recent_files.exe

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "host/BridgeDispatchShared.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace {

int g_passed = 0;
int g_failed = 0;

void Check(bool condition, const char* expression, int line)
{
    if (condition)
    {
        ++g_passed;
        return;
    }
    ++g_failed;
    std::printf("  FAIL line %d: %s\n", line, expression);
}

#define CHECK(condition) Check((condition), #condition, __LINE__)

void CheckVector(const std::vector<std::wstring>& actual,
                 const std::vector<std::wstring>& expected,
                 const char* label,
                 int line)
{
    if (actual == expected)
    {
        ++g_passed;
        return;
    }

    ++g_failed;
    std::printf("  FAIL line %d: %s (actual=%zu expected=%zu)\n",
                line, label, actual.size(), expected.size());
    const size_t count = (std::max)(actual.size(), expected.size());
    for (size_t i = 0; i < count; ++i)
    {
        const wchar_t* a = i < actual.size() ? actual[i].c_str() : L"<missing>";
        const wchar_t* e = i < expected.size() ? expected[i].c_str() : L"<missing>";
        std::wprintf(L"    [%zu] actual=%ls expected=%ls\n", i, a, e);
    }
}

#define CHECK_VECTOR(actual, expected, label) \
    CheckVector((actual), (expected), (label), __LINE__)

class RegistrySandbox
{
public:
    RegistrySandbox()
    {
        wchar_t suffix[160] = {};
        // Put the unique sandbox directly under the pre-existing Software key,
        // so deleting it leaves no test-owned parent key behind.
        swprintf_s(suffix, L"Software\\AloParticleEditor-RecentFilesTest-%lu-%llu",
                   GetCurrentProcessId(),
                   static_cast<unsigned long long>(GetTickCount64()));
        m_path = suffix;

        if (RegCreateKeyExW(HKEY_CURRENT_USER, m_path.c_str(), 0, nullptr,
                            REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr,
                            &m_root, nullptr) != ERROR_SUCCESS)
        {
            return;
        }

        if (RegOverridePredefKey(HKEY_CURRENT_USER, m_root) != ERROR_SUCCESS)
        {
            return;
        }
        m_overridden = true;

        if (RegCreateKeyExW(HKEY_CURRENT_USER, host::kRegistryKeyPath, 0, nullptr,
                            REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr,
                            &m_editorKey, nullptr) != ERROR_SUCCESS)
        {
            return;
        }
        m_ok = true;
    }

    RegistrySandbox(const RegistrySandbox&) = delete;
    RegistrySandbox& operator=(const RegistrySandbox&) = delete;

    ~RegistrySandbox()
    {
        if (m_editorKey != nullptr)
        {
            RegCloseKey(m_editorKey);
        }
        if (m_overridden)
        {
            RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        }
        if (m_root != nullptr)
        {
            RegCloseKey(m_root);
        }
        if (!m_path.empty())
        {
            RegDeleteTreeW(HKEY_CURRENT_USER, m_path.c_str());
        }
    }

    bool Ok() const { return m_ok; }
    HKEY EditorKey() const { return m_editorKey; }

private:
    std::wstring m_path;
    HKEY m_root = nullptr;
    HKEY m_editorKey = nullptr;
    bool m_overridden = false;
    bool m_ok = false;
};

FILETIME FileTimeFromTicks(ULONGLONG ticks)
{
    ULARGE_INTEGER value = {};
    value.QuadPart = ticks;
    FILETIME ft = {};
    ft.dwLowDateTime = value.LowPart;
    ft.dwHighDateTime = value.HighPart;
    return ft;
}

ULONGLONG CurrentFileTimeTicks()
{
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value = {};
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

bool SetRecentValue(HKEY key, const std::wstring& name, ULONGLONG ticks)
{
    const FILETIME ft = FileTimeFromTicks(ticks);
    return RegSetValueExW(key, name.c_str(), 0, REG_BINARY,
                          reinterpret_cast<const BYTE*>(&ft),
                          sizeof(ft)) == ERROR_SUCCESS;
}

bool SetRawValue(HKEY key, const wchar_t* name, DWORD type,
                 const void* data, DWORD size)
{
    return RegSetValueExW(key, name, 0, type,
                          reinterpret_cast<const BYTE*>(data),
                          size) == ERROR_SUCCESS;
}

std::vector<std::wstring> EnumeratePhysicalRecentNames(HKEY key)
{
    std::vector<std::wstring> names;
    for (DWORD index = 0;; ++index)
    {
        wchar_t name[1024] = {};
        DWORD nameLength = static_cast<DWORD>(std::size(name));
        DWORD type = 0;
        DWORD size = 0;
        const LONG result = RegEnumValueW(key, index, name, &nameLength, nullptr,
                                          &type, nullptr, &size);
        if (result == ERROR_NO_MORE_ITEMS)
        {
            break;
        }
        if (result != ERROR_SUCCESS)
        {
            return {};
        }
        const std::wstring valueName(name, nameLength);
        if (type == REG_BINARY && size == sizeof(FILETIME)
            && valueName.rfind(L"C:\\RecentFilesTest\\entry-", 0) == 0)
        {
            names.push_back(valueName);
        }
    }
    return names;
}

bool QueryExactValue(HKEY key, const wchar_t* name, DWORD expectedType,
                     const void* expectedData, DWORD expectedSize)
{
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS)
    {
        return false;
    }
    if (type != expectedType || size != expectedSize)
    {
        return false;
    }

    std::vector<BYTE> data(size);
    DWORD readSize = size;
    if (RegQueryValueExW(key, name, nullptr, &type, data.data(), &readSize)
        != ERROR_SUCCESS)
    {
        return false;
    }
    return type == expectedType
        && readSize == expectedSize
        && std::memcmp(data.data(), expectedData, expectedSize) == 0;
}

bool ValueIsMissing(HKEY key, const std::wstring& name)
{
    return RegQueryValueExW(key, name.c_str(), nullptr, nullptr, nullptr, nullptr)
        == ERROR_FILE_NOT_FOUND;
}

bool RecentValueExists(HKEY key, const std::wstring& name)
{
    DWORD type = 0;
    DWORD size = 0;
    return RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &size)
            == ERROR_SUCCESS
        && type == REG_BINARY
        && size == sizeof(FILETIME);
}

std::vector<std::wstring> Sorted(std::vector<std::wstring> values)
{
    std::sort(values.begin(), values.end());
    return values;
}

}  // namespace

int main()
{
    std::printf("test_recent_files_physical_mru_cap\n");

    RegistrySandbox sandbox;
    CHECK(sandbox.Ok());
    if (!sandbox.Ok())
    {
        std::printf("=== RecentFiles: sandbox setup failed ===\n");
        return 1;
    }

    // Overreach guards: all four values live under the same production key,
    // but none is a recent file. UnrelatedBinarySetting is deliberately the
    // exact size and type of a FILETIME; only its non-path name distinguishes
    // it from history. ReferenceObjectTransform guards the existing 24-byte
    // binary setting independently.
    const DWORD skydomeIndex = 0x13579BDFu;
    const ULONGLONG unrelatedBinary = 0x13579BDFULL;
    const std::array<float, 6> referenceTransform = { 1, 2, 3, 4, 5, 6 };
    const wchar_t customSkydome[] = L"C:\\Skies\\keep-me.alo";
    CHECK(SetRawValue(sandbox.EditorKey(), L"SkydomeIndex", REG_DWORD,
                      &skydomeIndex, sizeof(skydomeIndex)));
    CHECK(SetRawValue(sandbox.EditorKey(), L"UnrelatedBinarySetting", REG_BINARY,
                      &unrelatedBinary, sizeof(unrelatedBinary)));
    CHECK(SetRawValue(sandbox.EditorKey(), L"ReferenceObjectTransform", REG_BINARY,
                      referenceTransform.data(),
                      static_cast<DWORD>(sizeof(referenceTransform))));
    CHECK(SetRawValue(sandbox.EditorKey(), L"SkydomeCustomSlot4", REG_SZ,
                      customSkydome, sizeof(customSkydome)));

    // Seed ten valid MRU values. Their initial timestamps exist only to make
    // them recognizable as recents while the test discovers the registry's
    // physical enumeration order.
    for (int i = 0; i < 10; ++i)
    {
        const std::wstring path =
            L"C:\\RecentFilesTest\\entry-" + std::to_wstring(i) + L".alo";
        CHECK(SetRecentValue(sandbox.EditorKey(), path,
                             static_cast<ULONGLONG>(i + 1)));
    }

    const std::vector<std::wstring> physical =
        EnumeratePhysicalRecentNames(sandbox.EditorKey());
    CHECK(physical.size() == 10);
    if (physical.size() != 10)
    {
        std::printf("=== RecentFiles: seed enumeration failed ===\n");
        return 1;
    }

    // Make physical order the reverse of logical MRU order: physical[0] is
    // oldest and physical[9] newest. Deriving this from the observed order
    // avoids assuming any undocumented RegEnumValueW ordering.
    constexpr ULONGLONG kFileTimeTicksPerSecond = 10000000ULL;
    const ULONGLONG now = CurrentFileTimeTicks();
    const ULONGLONG base = now - 120ULL * kFileTimeTicksPerSecond;
    for (size_t i = 0; i < physical.size(); ++i)
    {
        CHECK(SetRecentValue(sandbox.EditorKey(), physical[i],
                             base + i * kFileTimeTicksPerSecond));
    }
    CHECK_VECTOR(EnumeratePhysicalRecentNames(sandbox.EditorKey()), physical,
                 "timestamp writes must not rename/reorder physical values");

    // Public presentation remains timestamp-sorted and capped to nine. The
    // oldest physical[0] is intentionally hidden before the touch.
    std::vector<std::wstring> expectedBefore;
    for (size_t i = physical.size(); i > 1; --i)
    {
        expectedBefore.push_back(physical[i - 1]);
    }
    CHECK_VECTOR(host::ReadRecentFiles(), expectedBefore,
                 "capped pre-touch MRU ordering");

    // Touch the physically first/oldest entry. It becomes the logical newest.
    // Correct trimming removes physical[1], which is now the logical oldest;
    // trimming raw enumeration order would instead remove physical[9].
    const std::vector<std::wstring> actualAfter =
        host::WriteRecentFile(physical[0]);
    std::vector<std::wstring> expectedAfter = { physical[0] };
    for (size_t i = physical.size(); i > 2; --i)
    {
        expectedAfter.push_back(physical[i - 1]);
    }
    CHECK_VECTOR(actualAfter, expectedAfter,
                 "post-touch returned MRU ordering");
    CHECK_VECTOR(host::ReadRecentFiles(), expectedAfter,
                 "post-touch persisted MRU ordering");

    const std::vector<std::wstring> physicalAfter =
        EnumeratePhysicalRecentNames(sandbox.EditorKey());
    CHECK(physicalAfter.size() == 9);
    CHECK_VECTOR(Sorted(physicalAfter), Sorted(expectedAfter),
                 "physical registry survivors");
    CHECK(ValueIsMissing(sandbox.EditorKey(), physical[1]));
    CHECK(RecentValueExists(sandbox.EditorKey(), physical[9]));

    // Exact unrelated-value oracle. An over-broad total-value or REG_BINARY
    // trim must fail at least one of these checks.
    CHECK(QueryExactValue(sandbox.EditorKey(), L"SkydomeIndex", REG_DWORD,
                          &skydomeIndex, sizeof(skydomeIndex)));
    CHECK(QueryExactValue(sandbox.EditorKey(), L"UnrelatedBinarySetting",
                          REG_BINARY, &unrelatedBinary,
                          sizeof(unrelatedBinary)));
    CHECK(QueryExactValue(sandbox.EditorKey(), L"ReferenceObjectTransform",
                          REG_BINARY, referenceTransform.data(),
                          static_cast<DWORD>(sizeof(referenceTransform))));
    CHECK(QueryExactValue(sandbox.EditorKey(), L"SkydomeCustomSlot4", REG_SZ,
                          customSkydome, sizeof(customSkydome)));

    if (g_failed == 0)
    {
        std::printf("=== RecentFiles: ALL PASS (%d checks) ===\n", g_passed);
    }
    else
    {
        std::printf("=== RecentFiles: %d FAILURE(S), %d passed ===\n",
                    g_failed, g_passed);
    }
    return g_failed == 0 ? 0 : 1;
}
