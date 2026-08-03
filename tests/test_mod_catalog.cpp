// Unit test for src/ModScan.h (Win32 directory scan for mod-layer discovery).
#include <cstdio>
#include <string>
#include <vector>
#include <windows.h>
#include <shlobj.h>      // SHCreateDirectoryExW
#include "ModScan.h"

static int g_fail = 0;
#define CHECK(c) do { if(!(c)){ std::printf("FAIL %s:%d  %s\n",__FILE__,__LINE__,#c); ++g_fail; } } while(0)

// Build %TEMP%\mt21_<pid>\{Sub1\Data\Art, Sub2, Core\Data\Art}; return the root.
static std::wstring MakeTempTree()
{
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring root = std::wstring(tmp) + L"mt21_" + std::to_wstring(GetCurrentProcessId());
    SHCreateDirectoryExW(NULL, (root + L"\\Sub1\\Data\\Art").c_str(), NULL);
    SHCreateDirectoryExW(NULL, (root + L"\\Sub2").c_str(), NULL);            // no Data\Art
    SHCreateDirectoryExW(NULL, (root + L"\\Core\\Data\\Art").c_str(), NULL);
    return root;
}
static void RemoveTempTree(const std::wstring& root)
{
    // SHFileOperation requires a double-null-terminated path.
    std::wstring from = root; from.push_back(L'\0'); from.push_back(L'\0');
    SHFILEOPSTRUCTW op = {}; op.wFunc = FO_DELETE; op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI; SHFileOperationW(&op);
}

int main()
{
    const std::wstring root = MakeTempTree();
    std::vector<LayerRef> nested;
    ScanModNestedLayers(root, nested);
    // Sub1 + Core carry Data\Art; Sub2 does not. Sorted case-insensitively.
    CHECK(nested.size() == 2);
    if (nested.size() == 2) {
        CHECK(nested[0].label == L"Core");
        CHECK(nested[1].label == L"Sub1");
        CHECK(nested[0].path.find(L"\\Core") != std::wstring::npos);
        CHECK(nested[0].path.back() != L'\\');   // slash-free canonical
    }
    CHECK(ModRootHasArt(root) == false);                 // root itself has no Data\Art
    CHECK(ModRootHasArt(root + L"\\Sub1") == true);      // Sub1 does
    RemoveTempTree(root);

    if (g_fail == 0) std::printf("=== ModCatalog: ALL PASS ===\n");
    else             std::printf("=== ModCatalog: %d FAILURE(S) ===\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
