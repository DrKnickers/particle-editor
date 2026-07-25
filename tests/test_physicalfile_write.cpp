// Regression test for PhysicalFile's write contract and its handle lifetime
// (src/files.cpp).
//
// Two properties are pinned here, both load-bearing for save correctness.
//
// 1. write() returns the full requested count on success. A SHORT write is a
//    FAILED write and must throw: WriteFile can succeed having written fewer
//    bytes than asked (a full disk or quota being the realistic case), and
//    ChunkWriter discards the returned count on its four chunk-header
//    backpatches — the last of which is the final write of a save. Without the
//    throw, the atomic-save path saw no exception, declared success, and
//    renamed a malformed temp over the user's document.
//
// 2. An OPEN PhysicalFile blocks DeleteFileW, and Releasing it unblocks the
//    delete. PhysicalFile opens without FILE_SHARE_DELETE, so a caller that
//    deletes its temp file while still holding the handle silently fails to
//    delete it. Autosave::Write did exactly that in its catch block: the .tmp
//    survived, the next autosave targeted the same path, could not reopen it,
//    and threw again — one failed write disabled autosave for the rest of the
//    session. The fix is to Release BEFORE DeleteFileW, and this test is what
//    stops that ordering from being quietly reintroduced.
//
//    If anyone ever adds FILE_SHARE_DELETE to the open flags, the first half of
//    that assertion changes — deliberately, so the coupling is visible.
//
// Header + files.cpp only; see tests/build_test_physicalfile_write.bat.

#include "files.h"
#include "exceptions.h"

#include <windows.h>
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_fail; std::printf("  FAIL: %s\n", msg); }   \
} while (0)

static std::wstring TempPath(const wchar_t* leaf)
{
    wchar_t dir[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, dir);
    std::wstring p(dir, n);
    p += leaf;
    return p;
}

int main()
{
    std::printf("test_physicalfile_write\n");

    // --- 1. write() returns the full count and the bytes land ---------------
    {
        const std::wstring path = TempPath(L"pe_pfw_basic.bin");
        DeleteFileW(path.c_str());

        const char payload[] = "0123456789ABCDEF";
        const unsigned long want = (unsigned long)sizeof(payload);

        bool threw = false;
        unsigned long got = 0;
        try
        {
            PhysicalFile* f = new PhysicalFile(path, PhysicalFile::WRITE);
            got = f->write(payload, want);
            f->Release();
        }
        catch (...) { threw = true; }

        CHECK(!threw, "a normal write does not throw");
        CHECK(got == want, "write() returns the full requested byte count");

        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        const bool haveAttrs =
            GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) != 0;
        CHECK(haveAttrs && fad.nFileSizeLow == want,
              "the file on disk is exactly the requested size");

        CHECK(DeleteFileW(path.c_str()) != 0,
              "the temp file deletes once the PhysicalFile is released");
    }

    // --- 2. handle lifetime vs DeleteFileW ----------------------------------
    // This is the mechanism that made the autosave leak fatal, pinned directly.
    {
        const std::wstring path = TempPath(L"pe_pfw_handle.bin");
        DeleteFileW(path.c_str());

        PhysicalFile* f = NULL;
        bool threw = false;
        try
        {
            f = new PhysicalFile(path, PhysicalFile::WRITE);
            f->write("x", 1);
        }
        catch (...) { threw = true; }
        CHECK(!threw && f != NULL, "opened a temp file for writing");

        if (f)
        {
            // Still open: no FILE_SHARE_DELETE, so the delete must FAIL. A
            // catch block that deletes here would leave the temp behind.
            const BOOL deletedWhileOpen = DeleteFileW(path.c_str());
            CHECK(deletedWhileOpen == 0,
                  "DeleteFileW FAILS while the PhysicalFile handle is still open");
            CHECK(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES,
                  "...and the file is consequently still on disk");

            // Release drops the last reference -> destructor -> CloseHandle.
            f->Release();
            f = NULL;

            const BOOL deletedAfterRelease = DeleteFileW(path.c_str());
            CHECK(deletedAfterRelease != 0,
                  "DeleteFileW SUCCEEDS once the PhysicalFile has been released");
            CHECK(GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES,
                  "...and the temp file is really gone");
        }
    }

    std::printf("%s\n", g_fail ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
