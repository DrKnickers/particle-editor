// Regression test for MegaFile header validation (src/MegaFiles.cpp, audit G9).
//
// A .meg is: [uint32 numStrings][uint32 numFiles]
//            numStrings x [uint16 len][len bytes]
//            numFiles   x FileInfo{uint32 crc, index, size, start, nameIndex}
// A forged header could drive an OOM allocation loop (huge counts) or index
// filenames[] out of bounds (nameIndex >= numStrings is UB on std::vector
// operator[], NOT a throw), or point an entry's start/size past EOF. The G9 fix
// caps the counts against the file size up front and validates every entry's
// nameIndex/start/size, throwing BadFileException instead of crashing. This test
// feeds crafted byte images through the MegaFile(IFile*) ctor (via a MemoryFile)
// and asserts BadFileException is thrown -- never a crash -- and that a
// well-formed tiny .meg constructs cleanly. See tests/build_test_meg_fuzz.bat.

#include "MegaFiles.h"
#include "files.h"
#include "exceptions.h"

#include <cstdint>
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

// FileInfo on disk = 5 x uint32 = 20 bytes (must match MegaFile::FileInfo).
static const uint32_t kFileInfoSize = 20;
static const uint32_t kMaxMegEntryCountForTest = 1u * 1024u * 1024u;

static void putU32(Bytes& b, uint32_t v)
{
    b.push_back((unsigned char)(v & 0xFF));
    b.push_back((unsigned char)((v >> 8) & 0xFF));
    b.push_back((unsigned char)((v >> 16) & 0xFF));
    b.push_back((unsigned char)((v >> 24) & 0xFF));
}
static void putU16(Bytes& b, uint16_t v)
{
    b.push_back((unsigned char)(v & 0xFF));
    b.push_back((unsigned char)((v >> 8) & 0xFF));
}
static void putStr(Bytes& b, const std::string& s)
{
    putU16(b, (uint16_t)s.size());
    b.insert(b.end(), s.begin(), s.end());
}
static void putFileInfo(Bytes& b, uint32_t crc, uint32_t index, uint32_t size,
                        uint32_t start, uint32_t nameIndex)
{
    putU32(b, crc); putU32(b, index); putU32(b, size); putU32(b, start); putU32(b, nameIndex);
}

// Seed a MemoryFile with the bytes and rewind to 0 for the ctor's reads.
static MemoryFile* makeFile(const Bytes& b)
{
    MemoryFile* f = new MemoryFile();   // rc=1
    if (!b.empty()) f->write(b.data(), (unsigned long)b.size());
    f->seek(0);
    return f;
}

// Construct a MegaFile from the byte image; report whether it threw
// BadFileException (true), constructed cleanly (false), or threw something
// unexpected (recorded as a failure with `other`).
static bool throwsBadFile(const Bytes& b, bool& other)
{
    other = false;
    MemoryFile* f = makeFile(b);   // rc=1
    try
    {
        MegaFile mega(f);          // ctor AddRef's -> rc=2; mega dtor Release -> rc=1
        f->Release();              // rc=0
        return false;              // constructed cleanly, no throw
    }
    catch (BadFileException&)
    {
        f->Release();              // ctor AddRef'd then the partial object unwound; balance our ref
        return true;
    }
    catch (...)
    {
        other = true;
        f->Release();
        return false;
    }
}

// Assert the image is REJECTED with BadFileException (not a crash, not silent).
static void expectBadFile(const Bytes& b, const char* label)
{
    bool other = false;
    bool threw = throwsBadFile(b, other);
    CHECK(threw && !other, label);
}

struct ScriptedMegFile : IFile
{
    Bytes data;
    unsigned long reportedSize;
    unsigned long pos = 0;
    bool largeNameReadAttempted = false;
    bool fileInfoReadAttempted = false;
    bool filenameLengthReadAttempted = false;

    ScriptedMegFile(const Bytes& bytes, unsigned long fakeSize)
        : data(bytes), reportedSize(fakeSize) {}

    bool eof() override { return pos >= reportedSize; }
    unsigned long size() override { return reportedSize; }
    void seek(unsigned long offset) override { pos = offset; }
    unsigned long tell() override { return pos; }
    unsigned long read(void* buffer, unsigned long n) override
    {
        if (pos >= 8 && n == sizeof(uint16_t)) filenameLengthReadAttempted = true;
        if (n > 32768u) largeNameReadAttempted = true;
        if (pos >= 8 && n == kFileInfoSize) fileInfoReadAttempted = true;
        if (pos + n > data.size()) return 0;
        if (n) std::memcpy(buffer, data.data() + pos, n);
        pos += n;
        return n;
    }
    unsigned long write(const void*, unsigned long) override { return 0; }
};

static bool scriptedThrowsBadFile(ScriptedMegFile* f, bool& other,
                                  bool* largeNameRead = nullptr,
                                  bool* fileInfoRead = nullptr,
                                  bool* filenameLengthRead = nullptr)
{
    other = false;
    auto capture = [&]() {
        if (largeNameRead) *largeNameRead = f->largeNameReadAttempted;
        if (fileInfoRead) *fileInfoRead = f->fileInfoReadAttempted;
        if (filenameLengthRead) *filenameLengthRead = f->filenameLengthReadAttempted;
    };
    try
    {
        MegaFile mega(f);
        capture();
        f->Release();
        return false;
    }
    catch (BadFileException&)
    {
        capture();
        f->Release();
        return true;
    }
    catch (...)
    {
        other = true;
        capture();
        f->Release();
        return false;
    }
}

int main()
{
    std::printf("test_meg_fuzz\n");

    // ---- A: a WELL-FORMED tiny .meg constructs OK ----
    // 1 string "A.TGA", 1 FileInfo whose nameIndex=0 and start/size fit in-file.
    {
        Bytes b;
        putU32(b, 1);                 // numStrings
        putU32(b, 1);                 // numFiles
        putStr(b, "A.TGA");           // filenames[0]
        // header-end offset is where the FileInfo table starts; place the entry's
        // payload region wholly inside the file. With size=0/start=0 the
        // (start > fsize) and (size > fsize-start) checks both pass trivially.
        putFileInfo(b, /*crc*/0, /*index*/0, /*size*/0, /*start*/0, /*nameIndex*/0);

        bool other = false;
        bool threw = throwsBadFile(b, other);
        CHECK(!threw && !other, "well-formed tiny .meg constructs cleanly (no throw)");
    }

    // ---- B: numStrings absurdly large (> filesize/2) ----
    {
        Bytes b;
        putU32(b, 0xFFFFFFFFul);      // numStrings way past fsize/2
        putU32(b, 0);                 // numFiles
        // (no string/file payload; the cap fires before any read loop)
        expectBadFile(b, "numStrings > filesize/2 -> BadFileException (no OOM loop)");
    }

    // ---- C: numFiles absurdly large (> filesize/sizeof(FileInfo)) ----
    {
        Bytes b;
        putU32(b, 0);                 // numStrings
        putU32(b, 0xFFFFFFFFul);      // numFiles way past fsize/20
        expectBadFile(b, "numFiles > filesize/20 -> BadFileException (no OOM loop)");
    }

    // ---- D: a FileInfo with nameIndex >= numStrings ----
    // counts pass the cap, strings read fine, but the entry's nameIndex points
    // off the end of filenames[] -> must throw (not UB index later).
    {
        Bytes b;
        putU32(b, 1);                 // numStrings = 1 (valid indices: {0})
        putU32(b, 1);                 // numFiles = 1
        putStr(b, "A.TGA");
        putFileInfo(b, 0, 0, /*size*/0, /*start*/0, /*nameIndex*/5);  // 5 >= 1
        expectBadFile(b, "FileInfo.nameIndex >= numStrings -> BadFileException");
    }

    // ---- E: a FileInfo with start past EOF ----
    {
        Bytes b;
        putU32(b, 1);                 // numStrings
        putU32(b, 1);                 // numFiles
        putStr(b, "A.TGA");
        // start is wildly past the file size -> (start > fsize) fires.
        putFileInfo(b, 0, 0, /*size*/0, /*start*/0xFFFFFF00ul, /*nameIndex*/0);
        expectBadFile(b, "FileInfo.start past EOF -> BadFileException");
    }

    // ---- F: a FileInfo with size past EOF (start in range, size overshoots) ----
    {
        Bytes b;
        putU32(b, 1);                 // numStrings
        putU32(b, 1);                 // numFiles
        putStr(b, "A.TGA");
        // start=0 is in range, but size > fsize-start -> (size > fsize - start) fires.
        putFileInfo(b, 0, 0, /*size*/0xFFFFFF00ul, /*start*/0, /*nameIndex*/0);
        expectBadFile(b, "FileInfo.size past EOF -> BadFileException");
    }

    // ---- G: per-filename length cap rejects before attempting a huge name read ----
    {
        Bytes b;
        putU32(b, 1);                 // numStrings
        putU32(b, 0);                 // numFiles
        putU16(b, 65535);             // length > kMaxFilenameLength
        ScriptedMegFile* f = new ScriptedMegFile(b, 128u * 1024u * 1024u);
        bool other = false;
        bool largeNameRead = false;
        bool threw = scriptedThrowsBadFile(f, other, &largeNameRead);
        CHECK(threw && !other, "filename length > 32 KiB -> BadFileException");
        CHECK(!largeNameRead, "oversized filename rejected before large read");
    }

    // ---- H: aggregate name table cap rejects before reading the over-cap entry ----
    {
        Bytes b;
        const uint32_t n = 513;        // 512 * 32768 == 16 MiB; 513rd exceeds cap
        putU32(b, n);
        putU32(b, 0);
        for (uint32_t i = 0; i < n; ++i)
        {
            putU16(b, 32768);
            if (i < 512)
                b.insert(b.end(), 32768, (unsigned char)'A');
        }
        ScriptedMegFile* f = new ScriptedMegFile(b, 128u * 1024u * 1024u);
        bool other = false;
        bool largeNameRead = false;
        bool threw = scriptedThrowsBadFile(f, other, &largeNameRead);
        CHECK(threw && !other, "aggregate MEG name table > 16 MiB -> BadFileException");
        CHECK(!largeNameRead, "aggregate overflow entry rejected before reading its filename bytes");
    }

    // ---- I: entry-count hard cap rejects before FileInfo loop ----
    {
        Bytes b;
        putU32(b, 0);
        putU32(b, kMaxMegEntryCountForTest + 1u);
        ScriptedMegFile* f = new ScriptedMegFile(b, 128u * 1024u * 1024u);
        bool other = false;
        bool fileInfoRead = false;
        bool threw = scriptedThrowsBadFile(f, other, nullptr, &fileInfoRead);
        CHECK(threw && !other, "numFiles > 1M -> BadFileException");
        CHECK(!fileInfoRead, "entry-count cap fires before FileInfo reads");
    }

    // ---- J: filename-count hard cap rejects before filename loop ----
    {
        Bytes b;
        putU32(b, kMaxMegEntryCountForTest + 1u);
        putU32(b, 0);
        ScriptedMegFile* f = new ScriptedMegFile(b, 128u * 1024u * 1024u);
        bool other = false;
        bool largeNameRead = false;
        bool filenameLengthRead = false;
        bool threw = scriptedThrowsBadFile(f, other, &largeNameRead, nullptr, &filenameLengthRead);
        CHECK(threw && !other, "numStrings > 1M -> BadFileException");
        CHECK(!filenameLengthRead, "numStrings cap fires before filename length reads");
        CHECK(!largeNameRead, "numStrings cap fires before filename byte reads");
    }

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
