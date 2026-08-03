// Standalone unit test for src/host/SavePathConfine.h.
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <vector>
#include <windows.h>
#include <shlobj.h>      // SHCreateDirectoryExW, SHFileOperationW
#include "host/SavePathConfine.h"

static int g_fail = 0;

static std::wstring JoinPath(const std::wstring& a, const std::wstring& b)
{
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

static bool IsDir(const std::wstring& p)
{
    const DWORD attrs = GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool EnsureDir(const std::wstring& p)
{
    if (IsDir(p)) return true;
    SHCreateDirectoryExW(nullptr, p.c_str(), nullptr);
    return IsDir(p);
}

static bool TouchFile(const std::wstring& p)
{
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

static void RemoveTree(const std::wstring& root)
{
    if (root.empty()) return;
    std::wstring from = root;
    from.push_back(L'\0');
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION | FOF_SILENT;
    SHFileOperationW(&op);
}

static std::wstring SafeSuffix(const wchar_t* name)
{
    std::wstring out;
    for (const wchar_t* p = name; *p; ++p) {
        const wchar_t ch = *p;
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
            (ch >= L'0' && ch <= L'9')) {
            out.push_back(ch);
        } else {
            out.push_back(L'_');
        }
    }
    return out;
}

struct TempTree {
    explicit TempTree(const wchar_t* name)
    {
        wchar_t tmp[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, tmp);
        if (n == 0 || n >= MAX_PATH) return;
        static int s_counter = 0;
        root = std::wstring(tmp) + L"clip_save_confine_" +
               std::to_wstring(GetCurrentProcessId()) + L"_" +
               std::to_wstring(++s_counter) + L"_" + SafeSuffix(name);
        ok = EnsureDir(root);
    }

    ~TempTree()
    {
        RemoveTree(root);
    }

    std::wstring root;
    bool ok = false;
};

static bool Report(const char* name, bool ok)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
    return ok;
}

static bool Skip(const char* name, const std::string& reason)
{
    std::printf("SKIP: %s (%s)\n", name, reason.c_str());
    return true;
}

static bool Allows(const std::wstring& root, const std::wstring& target,
                   std::string* errOut = nullptr)
{
    std::wstring resolved;
    std::string err;
    const bool ok = clip::ConfineSavePath(root, target, &resolved, err);
    if (errOut) *errOut = err;
    return ok;
}

static bool Rejects(const std::wstring& root, const std::wstring& target,
                    std::string* errOut = nullptr)
{
    return !Allows(root, target, errOut);
}

static std::wstring ToggleAsciiCase(std::wstring s)
{
    for (wchar_t& ch : s) {
        if (ch >= L'a' && ch <= L'z') ch = wchar_t(ch - L'a' + L'A');
        else if (ch >= L'A' && ch <= L'Z') ch = wchar_t(ch - L'A' + L'a');
    }
    return s;
}

static std::wstring LeafOf(const std::wstring& p)
{
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? p : p.substr(slash + 1);
}

static std::wstring CmdQuote(const std::wstring& p)
{
    return L"\"" + p + L"\"";
}

static std::string LastErrorReason(const char* prefix)
{
    return std::string(prefix) + " GetLastError=" + std::to_string(GetLastError());
}

static void CaseHappyPath()
{
    TempTree tmp(L"happy");
    const std::wstring nested = JoinPath(tmp.root, L"nested");
    const std::wstring directTarget = JoinPath(tmp.root, L"direct.alo");
    const std::wstring nestedTarget = JoinPath(nested, L"nested.alo");

    bool ok = tmp.ok;
    ok = ok && EnsureDir(nested);
    ok = ok && TouchFile(directTarget);
    ok = ok && TouchFile(nestedTarget);
    ok = ok && Allows(tmp.root, directTarget);
    ok = ok && Allows(tmp.root, nestedTarget);
    Report("happy path", ok);
}

static void CaseTraversal()
{
    TempTree tmp(L"traversal");
    const std::wstring root = JoinPath(tmp.root, L"stage");
    const std::wstring evil = JoinPath(tmp.root, L"evil");
    const std::wstring target = root + L"\\..\\evil\\x.alo";

    bool ok = tmp.ok;
    ok = ok && EnsureDir(root);
    ok = ok && EnsureDir(evil);
    ok = ok && TouchFile(JoinPath(evil, L"x.alo"));
    ok = ok && Rejects(root, target);
    Report("traversal", ok);
}

static void CasePrefixTrap()
{
    TempTree tmp(L"prefix");
    const std::wstring root = JoinPath(tmp.root, L"stage");
    const std::wstring evil = JoinPath(tmp.root, L"stage-evil");
    const std::wstring target = JoinPath(evil, L"x.alo");

    bool ok = tmp.ok;
    ok = ok && EnsureDir(root);
    ok = ok && EnsureDir(evil);
    ok = ok && TouchFile(target);
    ok = ok && Rejects(root, target);
    Report("prefix trap", ok);
}

static void CaseCaseInsensitivity()
{
    TempTree tmp(L"case");
    const std::wstring target = JoinPath(ToggleAsciiCase(tmp.root), L"x.alo");

    bool ok = tmp.ok;
    ok = ok && TouchFile(JoinPath(tmp.root, L"x.alo"));
    ok = ok && Allows(tmp.root, target);
    Report("case-insensitivity", ok);
}

static void CaseTrailingSeparators()
{
    TempTree tmp(L"trailing");
    const std::wstring target = JoinPath(tmp.root, L"x.alo");
    const std::wstring rootWithSeparators = tmp.root + L"\\\\\\";

    bool ok = tmp.ok;
    ok = ok && TouchFile(target);
    ok = ok && Allows(rootWithSeparators, target);
    Report("trailing separators", ok);
}

static void CaseMissingParent()
{
    TempTree tmp(L"missing_parent");
    const std::wstring target = JoinPath(JoinPath(tmp.root, L"missing"), L"x.alo");
    std::string err;

    bool ok = tmp.ok;
    ok = ok && Rejects(tmp.root, target, &err);
    ok = ok && err == "target parent directory does not exist or is not resolvable";
    Report("missing parent", ok);
}

static void CasePathlessEmptyInputs()
{
    TempTree tmp(L"pathless_empty");
    const std::wstring target = JoinPath(tmp.root, L"x.alo");

    bool ok = tmp.ok;
    ok = ok && TouchFile(target);
    ok = ok && Rejects(L"", target);
    ok = ok && Rejects(tmp.root, L"");
    ok = ok && Rejects(L"", L"");
    ok = ok && Rejects(tmp.root, L"x.alo");
    Report("pathless/empty inputs", ok);
}

static void CaseDotFinalComponent()
{
    TempTree tmp(L"dot_final");

    bool ok = tmp.ok;
    ok = ok && Rejects(tmp.root, JoinPath(tmp.root, L"."));
    ok = ok && Rejects(tmp.root, JoinPath(tmp.root, L".."));
    Report("dot final component", ok);
}

static void CaseAdsAndTrailing()
{
    // An NTFS alternate-data-stream leaf (out.alo:hidden) resolves its parent
    // under root but writes to a stream, not the plain .alo — must be rejected.
    // Trailing dot/space leaves (Win32 strips them) must be rejected too.
    TempTree tmp(L"ads_trail");

    bool ok = tmp.ok;
    ok = ok && Rejects(tmp.root, JoinPath(tmp.root, L"out.alo:hidden"));
    ok = ok && Rejects(tmp.root, JoinPath(tmp.root, L"out.alo:$DATA"));
    ok = ok && Rejects(tmp.root, JoinPath(tmp.root, L"out.alo."));
    ok = ok && Rejects(tmp.root, JoinPath(tmp.root, L"out.alo "));
    // A plain leaf under root still passes.
    ok = ok && Allows(tmp.root, JoinPath(tmp.root, L"out.alo"));
    Report("ADS + trailing dot/space", ok);
}

static void CaseJunction()
{
    TempTree tmp(L"junction");
    const std::wstring root = JoinPath(tmp.root, L"stage");
    const std::wstring outside = JoinPath(tmp.root, L"outside");
    const std::wstring link = JoinPath(root, L"jump");
    const std::wstring target = JoinPath(link, L"x.alo");

    bool ok = tmp.ok;
    ok = ok && EnsureDir(root);
    ok = ok && EnsureDir(outside);
    ok = ok && TouchFile(JoinPath(outside, L"x.alo"));
    if (!ok) {
        Report("junction", false);
        return;
    }

    const std::wstring command = L"cmd /c mklink /J " + CmdQuote(link) + L" " +
                                 CmdQuote(outside) + L" >NUL 2>&1";
    const int rc = _wsystem(command.c_str());
    if (rc != 0 || !IsDir(link)) {
        RemoveDirectoryW(link.c_str());
        Skip("junction", "mklink /J failed with exit code " + std::to_string(rc));
        return;
    }

    ok = Rejects(root, target);
    RemoveDirectoryW(link.c_str());
    Report("junction", ok);
}

static void CaseShortName()
{
    TempTree tmp(L"short_name");
    const std::wstring longDir = JoinPath(tmp.root, L"Long Directory Name For Save Confinement");

    bool ok = tmp.ok;
    ok = ok && EnsureDir(longDir);
    ok = ok && TouchFile(JoinPath(longDir, L"x.alo"));
    if (!ok) {
        Report("8.3", false);
        return;
    }

    const DWORD need = GetShortPathNameW(longDir.c_str(), nullptr, 0);
    if (need == 0) {
        Skip("8.3", LastErrorReason("GetShortPathNameW failed"));
        return;
    }

    std::wstring shortDir(need, L'\0');
    const DWORD got = GetShortPathNameW(longDir.c_str(), &shortDir[0], need);
    if (got == 0 || got >= need) {
        Skip("8.3", LastErrorReason("GetShortPathNameW failed"));
        return;
    }
    shortDir.resize(got);

    if (LeafOf(shortDir) == LeafOf(longDir)) {
        Skip("8.3", "short path lookup is identical to long path");
        return;
    }

    ok = Allows(tmp.root, JoinPath(shortDir, L"x.alo"));
    Report("8.3", ok);
}

int main()
{
    CaseHappyPath();
    CaseTraversal();
    CasePrefixTrap();
    CaseCaseInsensitivity();
    CaseTrailingSeparators();
    CaseMissingParent();
    CasePathlessEmptyInputs();
    CaseDotFinalComponent();
    CaseAdsAndTrailing();
    CaseJunction();
    CaseShortName();
    return g_fail == 0 ? 0 : 1;
}
