// Regression test for SubFile::read clamping (src/files.cpp).
//
// SubFile is a bounded view into a parent file (used for MEG-packed entries:
// MegaFile::getFile returns `new SubFile(parent, start, size)`). A read request
// LARGER than the sub-view's remaining bytes must NOT spill into whatever bytes
// follow in the parent (the next packed entry). Before the clamp fix, a read of
// BUFFER_SIZE on a small packed XML (XMLTree::parse reads 32 KB chunks) returned
// the adjacent entry's bytes as trailing garbage -> expat parse failed -> the
// skydome XML lists "resolved" but parsed to zero entries. This test pins the
// boundary behaviour against that regression. Standalone console exe; see
// tests/build_test_subfile_read.bat.

#include "files.h"
#include "exceptions.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

typedef std::vector<unsigned char> Bytes;

// Lay out a parent file as [prefix 'P'][inner payload][suffix 0xEE], so a
// SubFile over the inner region is flanked by sentinel bytes on both sides. Any
// byte that leaks past the sub-view shows up as 0xEE (suffix) or 'P' (prefix).
static const size_t kPrefix = 37;   // arbitrary, non-aligned start offset
static const size_t kSuffix = 4096; // big enough that an unclamped 32 KB-ish read would grab it

static std::wstring writeParentFile(const Bytes& inner)
{
    wchar_t dir[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, dir);
    std::wstring path = std::wstring(dir) + L"subfile_read_test.bin";

    Bytes file;
    file.insert(file.end(), kPrefix, (unsigned char)'P');
    file.insert(file.end(), inner.begin(), inner.end());
    file.insert(file.end(), kSuffix, (unsigned char)0xEE);

    PhysicalFile* w = new PhysicalFile(path, PhysicalFile::WRITE);  // rc=1
    w->write(file.data(), (unsigned long)file.size());
    w->Release();                                                   // rc=0 -> close
    return path;
}

int main()
{
    std::printf("test_subfile_read\n");

    // A small inner payload: smaller than a typical read buffer, like a packed
    // skydome XML vs XMLTree::parse's 32 KB chunk.
    Bytes inner;
    for (int i = 0; i < 123; ++i) inner.push_back((unsigned char)('A' + (i % 26)));
    const unsigned long innerLen = (unsigned long)inner.size();

    const std::wstring path = writeParentFile(inner);

    PhysicalFile* parent = new PhysicalFile(path, PhysicalFile::READ);     // rc=1
    SubFile*      sub    = new SubFile(parent, (unsigned long)kPrefix, innerLen); // parent rc=2
    parent->Release();                                                    // parent rc=1 (held by sub)

    CHECK(sub->size() == innerLen, "SubFile::size == inner length");

    // --- A: oversized read must return exactly innerLen and NOT spill suffix ---
    {
        sub->seek(0);
        std::vector<unsigned char> buf(innerLen + kSuffix, 0x00);  // way bigger than the sub-view
        unsigned long n = sub->read(buf.data(), (unsigned long)buf.size());
        CHECK(n == innerLen, "oversized read returns exactly inner length (no over-read)");
        CHECK(std::memcmp(buf.data(), inner.data(), innerLen) == 0, "oversized read content == inner payload");
        bool spilled = false;
        for (unsigned long i = 0; i < kSuffix; ++i)
            if (buf[innerLen + i] == 0xEE) { spilled = true; break; }
        CHECK(!spilled, "no suffix (adjacent-entry) bytes leaked into the buffer");
        CHECK(sub->eof(), "eof() true after consuming the whole sub-view");
    }

    // --- B: chunked reads reconstruct the payload exactly, then EOF ---
    {
        sub->seek(0);
        Bytes got;
        unsigned char chunk[16];
        for (;;)
        {
            unsigned long n = sub->read(chunk, sizeof(chunk));
            if (n == 0) break;
            got.insert(got.end(), chunk, chunk + n);
            if (got.size() > innerLen + 64) break;  // runaway guard
        }
        CHECK(got.size() == innerLen, "chunked reads total exactly inner length");
        CHECK(got.size() == innerLen && std::memcmp(got.data(), inner.data(), innerLen) == 0,
              "chunked reads reconstruct inner payload");
    }

    // --- C: read at/after end returns 0 ---
    {
        sub->seek(innerLen);
        unsigned char tmp[8];
        unsigned long n = sub->read(tmp, sizeof(tmp));
        CHECK(n == 0, "read at end returns 0");
    }

    // --- D: exact-size read (the ReadAndRelease path) is unchanged ---
    {
        sub->seek(0);
        std::vector<unsigned char> buf(innerLen, 0x00);
        unsigned long n = sub->read(buf.data(), innerLen);
        CHECK(n == innerLen, "exact-size read returns inner length");
        CHECK(std::memcmp(buf.data(), inner.data(), innerLen) == 0, "exact-size read content == inner payload");
    }

    sub->Release();              // sub rc=0 -> ~SubFile releases parent -> both freed
    DeleteFileW(path.c_str());

    std::printf("%s (%d failure%s)\n", g_failed ? "FAILED" : "PASSED",
                g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
