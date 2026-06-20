// Stage 3 gate — prove the resource migration landed in the BUILT binary.
//
// When the legacy localized resource files (ParticleEditor.en.rc / .de.rc,
// resource.en.h / .de.h) were deleted, the shared IDS_* error/query strings the
// surviving engine/IO/host code LoadString()s had to be migrated into the kept
// Resources\resource.h + a new STRINGTABLE in ParticleEditor.rc. A clean compile
// does NOT prove the strings resolve at runtime: a missing STRINGTABLE entry
// still compiles (the IDS_* macro is just a number) but ::LoadString returns 0 →
// an empty error message. That silent failure is exactly what a cold launch
// misses and what this test catches.
//
// Approach: load the built ParticleEditor.exe as a data module and, for each of
// the 11 migrated ids, assert ::LoadStringW returns the EXACT expected English
// string (non-empty). Also assert the live non-string resources the .rc trim was
// supposed to KEEP (shader/ground/skydome RCDATA, IDI_LOGO icon, IDB_MISSING
// placeholder bitmap) still exist, so a future over-eager trim is caught too.
//
// Usage: test_resource_strings.exe [path-to-ParticleEditor.exe]
//   With no arg it searches x64\Release then x64\Debug relative to the repo root.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cwchar>

#include "Resources\resource.h"   // pure #defines — IDS_* / IDR_* / IDB_* / IDI_*

static int g_pass = 0;
static int g_fail = 0;

static void check_string(HMODULE hMod, UINT id, const wchar_t* name, const wchar_t* expected)
{
    wchar_t buf[1024];
    int len = ::LoadStringW(hMod, id, buf, (int)(sizeof(buf) / sizeof(buf[0])));
    if (len <= 0)
    {
        wprintf(L"  FAIL  %-28s (id %u): LoadString returned %d (empty/missing string)\n", name, id, len);
        g_fail++;
        return;
    }
    if (wcscmp(buf, expected) != 0)
    {
        wprintf(L"  FAIL  %-28s (id %u): got \"%ls\" expected \"%ls\"\n", name, id, buf, expected);
        g_fail++;
        return;
    }
    wprintf(L"  ok    %-28s (id %u)\n", name, id);
    g_pass++;
}

static void check_resource(HMODULE hMod, LPCWSTR type, UINT id, const wchar_t* name)
{
    HRSRC hRes = ::FindResourceW(hMod, MAKEINTRESOURCEW(id), type);
    if (hRes == NULL)
    {
        wprintf(L"  FAIL  %-28s (id %u): resource not found in binary\n", name, id);
        g_fail++;
        return;
    }
    wprintf(L"  ok    %-28s (id %u)\n", name, id);
    g_pass++;
}

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* candidates[] = {
        L"x64\\Release\\ParticleEditor.exe",
        L"x64\\Debug\\ParticleEditor.exe",
    };

    const wchar_t* path = NULL;
    if (argc > 1)
    {
        path = argv[1];
    }
    else
    {
        for (const wchar_t* c : candidates)
        {
            if (::GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES) { path = c; break; }
        }
    }

    if (path == NULL)
    {
        wprintf(L"FATAL: ParticleEditor.exe not found (pass a path, or build x64 Release/Debug first)\n");
        return 2;
    }

    // LOAD_LIBRARY_AS_DATAFILE: map the exe's resources without running it.
    HMODULE hMod = ::LoadLibraryExW(path, NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (hMod == NULL)
    {
        wprintf(L"FATAL: LoadLibraryEx failed for %ls (err %lu)\n", path, ::GetLastError());
        return 2;
    }
    wprintf(L"Loaded: %ls\n\n", path);

    wprintf(L"Migrated IDS_* strings (must resolve to their real English text):\n");
    check_string(hMod, IDS_ERROR_FILE_READ,         L"IDS_ERROR_FILE_READ",         L"Unable to read file");
    check_string(hMod, IDS_ERROR_FILE_FIND,         L"IDS_ERROR_FILE_FIND",         L"Unable to find file:\n%ls");
    check_string(hMod, IDS_ERROR_FILE_WRITE,        L"IDS_ERROR_FILE_WRITE",        L"Unable to write file");
    check_string(hMod, IDS_ERROR_FILE_CORRUPT,      L"IDS_ERROR_FILE_CORRUPT",      L"Bad or corrupted file");
    check_string(hMod, IDS_ERROR_FILE_FORMAT,       L"IDS_ERROR_FILE_FORMAT",       L"The file is not a particle file");
    check_string(hMod, IDS_ERROR_XML_PARSER_CREATE, L"IDS_ERROR_XML_PARSER_CREATE", L"Unable to create XML parser");
    check_string(hMod, IDS_ERROR_XML,               L"IDS_ERROR_XML",               L"%ls at line %d");
    check_string(hMod, IDS_QUERY_DATA_PATH,         L"IDS_QUERY_DATA_PATH",         L"Please select the Star Wars Empire at War or Forces of Corruption folder that contains the Data folder.");
    check_string(hMod, IDS_ERROR_FILE_OPEN,         L"IDS_ERROR_FILE_OPEN",         L"Unable to open file:\n%ls");
    check_string(hMod, IDS_ERROR_FILE_CREATE,       L"IDS_ERROR_FILE_CREATE",       L"Unable to create file:\n%ls");
    check_string(hMod, IDS_ERROR_RENDERER_RESET,    L"IDS_ERROR_RENDERER_RESET",    L"Unable to reset rendering window");

    wprintf(L"\nKept live resources (the .rc trim must NOT have dropped these):\n");
    check_resource(hMod, RT_RCDATA, IDS_SCENEHEAT,         L"IDS_SCENEHEAT (SceneHeat.fx)");
    check_resource(hMod, RT_RCDATA, IDR_DEFAULT_SHADER,    L"IDR_DEFAULT_SHADER");
    check_resource(hMod, RT_RCDATA, IDR_SHADER_SKYDOME,    L"IDR_SHADER_SKYDOME");
    check_resource(hMod, RT_RCDATA, IDR_SHADER_GROUND_LIT, L"IDR_SHADER_GROUND_LIT");
    check_resource(hMod, RT_RCDATA, IDB_GROUND,            L"IDB_GROUND (dirt)");
    check_resource(hMod, RT_RCDATA, IDB_GROUND_GRASS,      L"IDB_GROUND_GRASS");
    check_resource(hMod, RT_RCDATA, IDB_GROUND_SAND,       L"IDB_GROUND_SAND");
    check_resource(hMod, RT_RCDATA, IDB_GROUND_SNOW,       L"IDB_GROUND_SNOW");
    check_resource(hMod, RT_RCDATA, IDR_SKYDOME_SPACE,     L"IDR_SKYDOME_SPACE");
    check_resource(hMod, RT_RCDATA, IDR_SKYDOME_ATMOSPHERE,L"IDR_SKYDOME_ATMOSPHERE");
    check_resource(hMod, RT_RCDATA, IDR_SKYDOME_SUNSET,    L"IDR_SKYDOME_SUNSET");
    check_resource(hMod, RT_RCDATA, IDR_SKYDOME_DAWN,      L"IDR_SKYDOME_DAWN");
    check_resource(hMod, RT_RCDATA, IDR_SKYDOME_NIGHT,     L"IDR_SKYDOME_NIGHT");
    check_resource(hMod, RT_RCDATA, IDR_SKYDOME_OVERCAST,  L"IDR_SKYDOME_OVERCAST");
    check_resource(hMod, RT_RCDATA, IDR_SKYDOME_STUDIO,    L"IDR_SKYDOME_STUDIO");
    check_resource(hMod, RT_RCDATA, IDR_SKYDOME_INDOOR,    L"IDR_SKYDOME_INDOOR");
    check_resource(hMod, RT_GROUP_ICON, IDI_LOGO,          L"IDI_LOGO (app icon)");
    check_resource(hMod, RT_BITMAP, IDB_MISSING,           L"IDB_MISSING (placeholder)");

    ::FreeLibrary(hMod);

    wprintf(L"\n=== ResourceStrings: %d passed, %d failed ===\n", g_pass, g_fail);
    if (g_fail == 0)
        wprintf(L"=== ResourceStrings: ALL PASS ===\n");
    return g_fail == 0 ? 0 : 1;
}
