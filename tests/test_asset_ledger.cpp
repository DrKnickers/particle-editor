// The asset ledger, device-free: the managers' real lookup, retry, cache and
// fallback chain (AssetCache) over a real FileManager, a scratch content root
// and a MEG written here, with a stub decoder in place of D3DX.
#include "gamedata/managers.h"
#include "common/crc32.h"
#include "common/sha256.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

using Result = AssetAttempt::Result;
using Kind = AssetWinner::Kind;
using Outcome = AssetRecord::Outcome;

// A resource: the bytes it was decoded from.
struct Fake : RefCounted
{
    std::string bytes;
    explicit Fake(const std::string& b) : bytes(b) {}
};

// Accepts bytes starting with "GOOD"; the fallback's bytes are "FALLBACK".
class FakeDecoder : public IAssetDecoder<Fake>
{
public:
    int decodes = 0, fallbacks = 0;
    std::string resource = "IDB_MISSING";
    Fake* Decode(const std::vector<unsigned char>& b) override
    {
        ++decodes;
        const std::string s(b.begin(), b.end());
        return s.compare(0, 4, "GOOD") == 0 ? new Fake(s) : nullptr;
    }
    Fake* CreateFallback(std::vector<unsigned char>& bytes, std::string& resourceId) override
    {
        ++fallbacks;
        const std::string s = "FALLBACK";
        bytes.assign(s.begin(), s.end());
        resourceId = resource;
        return new Fake(s);
    }
};

static std::wstring g_root;   // scratch content root, slash-terminated

static void WriteBytes(const std::wstring& path, const std::string& bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << bytes;
}

static std::string Hash(const std::string& s) { return Sha256Hex(s.data(), s.size()); }

static std::string Utf8(const std::wstring& w) { return AssetPathUtf8(w); }

// A MEG v1 archive: counts, the name table, the CRC-sorted index, then the data.
struct MegEntry { std::string name, bytes; unsigned long start = 0; };
static void WriteMeg(const std::wstring& path, std::vector<MegEntry>& entries)
{
    auto u32 = [](std::string& out, unsigned long v) { out.append(reinterpret_cast<const char*>(&v), 4); };
    std::string names;
    for (auto& e : entries)
    {
        const unsigned short length = static_cast<unsigned short>(e.name.size());
        names.append(reinterpret_cast<const char*>(&length), 2);
        names += e.name;
    }
    unsigned long start = 8 + static_cast<unsigned long>(names.size()) + 20 * static_cast<unsigned long>(entries.size());
    std::vector<size_t> order(entries.size());
    for (size_t i = 0; i < order.size(); ++i) { order[i] = i; entries[i].start = start; start += (unsigned long)entries[i].bytes.size(); }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b)
    {
        return crc32(entries[a].name.c_str(), entries[a].name.size()) < crc32(entries[b].name.c_str(), entries[b].name.size());
    });
    std::string out;
    u32(out, (unsigned long)entries.size());
    u32(out, (unsigned long)entries.size());
    out += names;
    for (size_t index = 0; index < order.size(); ++index)
    {
        const MegEntry& e = entries[order[index]];
        u32(out, crc32(e.name.c_str(), e.name.size()));
        u32(out, (unsigned long)index);
        u32(out, (unsigned long)e.bytes.size());
        u32(out, e.start);
        u32(out, (unsigned long)order[index]);
    }
    for (auto& e : entries) out += e.bytes;
    WriteBytes(path, out);
}

static const AssetRecord* Find(const AssetLedger& ledger, const std::string& identity)
{
    return ledger.Find("texture", identity);
}

// A file manager whose one file reports more bytes than it delivers.
class ShortFile : public IFile
{
public:
    bool eof() override { return false; }
    unsigned long size() override { return 16; }
    void seek(unsigned long) override {}
    unsigned long tell() override { return 0; }
    unsigned long read(void* buffer, unsigned long) override { memset(buffer, 'G', 4); return 4; }
    unsigned long write(const void*, unsigned long) override { return 0; }
};
class ShortFileManager : public IFileManager
{
public:
    IFile* getFile(const std::string& path) override
    {
        return path.find("SHORT") != std::string::npos ? new ShortFile() : NULL;
    }
};

int main()
{
    std::printf("test_asset_ledger\n");

    CHECK(Hash("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 of the empty input");
    CHECK(Hash("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 of abc");
    CHECK(Hash(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "sha256 of one million a");

    // Scratch content root: Data\MegaFiles.xml, one MEG, and loose textures.
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    g_root = std::wstring(temp) + L"pe-asset-ledger-" + std::to_wstring(GetCurrentProcessId()) + L"\\";
    CreateDirectoryW(g_root.c_str(), NULL);
    CreateDirectoryW((g_root + L"Data").c_str(), NULL);
    CreateDirectoryW((g_root + L"Data\\Art").c_str(), NULL);
    CreateDirectoryW((g_root + L"Data\\Art\\Textures").c_str(), NULL);
    WriteBytes(g_root + L"Data\\MegaFiles.xml", "<?xml version=\"1.0\"?>\n<Mega_Files>\n<File>Data\\Test.meg</File>\n</Mega_Files>\n");
    std::vector<MegEntry> meg = {
        { "DATA\\ART\\TEXTURES\\MEG_ONLY.TGA", "GOOD-meg" },
        { "DATA\\ART\\TEXTURES\\RETRY.DDS", "GOOD-retry-dds" },
        { "DATA\\ART\\TEXTURES\\BAD.DDS", "GOOD-bad-dds" },
        { "DATA\\ART\\TEXTURES\\EMPTY.DDS", "GOOD-empty-dds" },
        { "DATA\\ART\\TEXTURES\\BIG.DDS", "GOOD-big-dds" },
    };
    WriteMeg(g_root + L"Data\\Test.meg", meg);
    const std::wstring textures = g_root + L"Data\\Art\\Textures\\";
    WriteBytes(textures + L"LOOSE.TGA", "GOOD-loose");
    WriteBytes(textures + L"BAD.TGA", "BAD-bytes");
    WriteBytes(textures + L"EMPTY.TGA", "");
    WriteBytes(textures + L"BIG.TGA", std::string(100, 'G'));
    // The literal path is relative to the working directory.
    SetCurrentDirectoryW(g_root.c_str());
    WriteBytes(g_root + L"LIT.TGA", "GOOD-literal");
    WriteBytes(g_root + L"LITBAD.TGA", "BAD-literal");
    WriteBytes(textures + L"LITBAD.TGA", "GOOD-litbad-content");

    FileManager files({ g_root });
    AssetCache<Fake> cache("texture", &files, "Data\\Art\\Textures\\", ".DDS", 64);
    FakeDecoder decoder;
    auto get = [&](const char* name) { Fake* f = cache.Get(decoder, name); return f; };

    {
        Fake* f = get("lit.tga");
        const AssetRecord* r = Find(cache.Ledger(), "LIT.TGA");
        CHECK(f && r && r->attempts.size() == 1 && r->attempts[0].result == Result::Resolved &&
              r->winner.kind == Kind::Loose && r->winner.path == Utf8(g_root + L"LIT.TGA") &&
              r->sha256 == Hash("GOOD-literal") && r->outcome == Outcome::Resolved,
              "a literal-path texture is resolved from its loose file and hashed");
        SAFE_RELEASE(f);
    }
    {
        Fake* f = get("meg_only.tga");
        const AssetRecord* r = Find(cache.Ledger(), "MEG_ONLY.TGA");
        const MegEntry& e = meg[0];
        CHECK(f && r && r->attempts.size() == 2 && r->attempts[0].result == Result::NotFound &&
              r->attempts[0].sha256.empty() && r->attempts[0].winner.kind == Kind::None &&
              r->winner.kind == Kind::Archive && r->winner.path == Utf8(g_root + L"Data\\Test.meg") &&
              r->winner.member == e.name && r->winner.start == e.start && r->winner.size == e.bytes.size() &&
              r->sha256 == Hash(e.bytes),
              "a MEG member is the winner with its archive, member, byte range and the slice's hash");
        SAFE_RELEASE(f);
    }
    {
        Fake* f = get("loose.tga");
        const AssetRecord* r = Find(cache.Ledger(), "LOOSE.TGA");
        CHECK(f && r && r->winner.kind == Kind::Loose && r->winner.path == Utf8(textures + L"LOOSE.TGA") &&
              r->sha256 == Hash("GOOD-loose"), "a loose file in the content root wins before the MEG");
        SAFE_RELEASE(f);
    }
    {
        Fake* f = get("retry.tga");
        const AssetRecord* r = Find(cache.Ledger(), "RETRY.TGA");
        CHECK(f && r && r->attempts.size() == 3 && r->attempts[1].result == Result::NotFound &&
              r->attempts[2].result == Result::Resolved && r->attempts[2].name == "Data\\Art\\Textures\\RETRY.DDS" &&
              r->winner.member == "DATA\\ART\\TEXTURES\\RETRY.DDS" && r->sha256 == Hash("GOOD-retry-dds"),
              "a missing original resolves through the .DDS retry, recorded under the requested identity");
        SAFE_RELEASE(f);
    }
    {
        Fake* f = get("bad.tga");
        const AssetRecord* r = Find(cache.Ledger(), "BAD.TGA");
        CHECK(f && r && r->attempts.size() == 3 && r->attempts[1].result == Result::DecodeFailed &&
              r->attempts[1].winner.kind == Kind::Loose && r->attempts[1].sha256 == Hash("BAD-bytes") &&
              r->outcome == Outcome::Resolved && r->sha256 == Hash("GOOD-bad-dds"),
              "a failed decode keeps its winner and hash, and the .DDS retry is the terminal resource, not a fallback");
        SAFE_RELEASE(f);
    }
    {
        Fake* empty = get("empty.tga");
        Fake* big = get("big.tga");
        const AssetRecord* e = Find(cache.Ledger(), "EMPTY.TGA");
        const AssetRecord* b = Find(cache.Ledger(), "BIG.TGA");
        CHECK(empty && e && e->attempts[1].result == Result::ReadRejected && e->attempts[1].reportedSize == 0 &&
              e->attempts[1].sha256.empty() && e->attempts[1].winner.kind == Kind::Loose && e->sha256 == Hash("GOOD-empty-dds"),
              "an empty winner is read-rejected with its size and no hash, then the retry resolves");
        CHECK(big && b && b->attempts[1].result == Result::ReadRejected && b->attempts[1].reportedSize == 100 &&
              b->attempts[1].reason == "over the size cap" && b->sha256 == Hash("GOOD-big-dds"),
              "an oversized winner is read-rejected with its reported size");
        SAFE_RELEASE(empty);
        SAFE_RELEASE(big);
    }
    {
        Fake* a = get("none.tga");
        Fake* b = get("none2.tga");
        const AssetRecord* r = Find(cache.Ledger(), "NONE.TGA");
        CHECK(a && a == b && a == cache.Default() && decoder.fallbacks == 1 && r && r->attempts.size() == 3 &&
              std::all_of(r->attempts.begin(), r->attempts.end(), [](const AssetAttempt& x) { return x.result == Result::NotFound; }) &&
              r->outcome == Outcome::DefaultSubstituted && r->winner.kind == Kind::Embedded &&
              r->winner.resourceId == "IDB_MISSING" && r->sha256 == Hash("FALLBACK"),
              "when every attempt fails, the embedded fallback is substituted with its resource id and hash, created once");
        SAFE_RELEASE(a);
        SAFE_RELEASE(b);
    }
    {
        Fake* f = get("litbad.tga");
        const AssetRecord* r = Find(cache.Ledger(), "LITBAD.TGA");
        CHECK(f && r && r->attempts.size() == 2 && r->attempts[0].result == Result::DecodeFailed &&
              r->attempts[0].winner.path == Utf8(g_root + L"LITBAD.TGA") && r->attempts[0].sha256 == Hash("BAD-literal") &&
              r->winner.path == Utf8(textures + L"LITBAD.TGA") && r->sha256 == Hash("GOOD-litbad-content"),
              "a failed literal-path decode is kept, and the content-root file is the terminal resource");
        SAFE_RELEASE(f);
    }
    {
        // The shader chain: a literal shader, a failed original then the .FXO
        // retry, and exhausted retries ending in the embedded default shader.
        CreateDirectoryW((g_root + L"Data\\Art\\Shaders").c_str(), NULL);
        const std::wstring shaders = g_root + L"Data\\Art\\Shaders\\";
        WriteBytes(g_root + L"LITSHADER.FX", "GOOD-literal-shader");
        WriteBytes(shaders + L"PRIM.FX", "BAD-shader");
        WriteBytes(shaders + L"PRIM.FXO", "GOOD-compiled");
        AssetCache<Fake> shaderCache("shader", &files, "Data\\Art\\Shaders\\", ".FXO", 64);
        FakeDecoder shaderDecoder;
        shaderDecoder.resource = "IDR_DEFAULT_SHADER";
        Fake* lit = shaderCache.Get(shaderDecoder, "litshader.fx");
        Fake* prim = shaderCache.Get(shaderDecoder, "Prim.fx");
        Fake* none = shaderCache.Get(shaderDecoder, "none.fx");
        const AssetRecord* l = shaderCache.Ledger().Find("shader", "LITSHADER.FX");
        const AssetRecord* p = shaderCache.Ledger().Find("shader", "PRIM.FX");
        const AssetRecord* n = shaderCache.Ledger().Find("shader", "NONE.FX");
        CHECK(lit && l && l->type == "shader" && l->winner.path == Utf8(g_root + L"LITSHADER.FX") &&
              l->sha256 == Hash("GOOD-literal-shader"), "a literal-path shader is recorded although it bypasses the file manager");
        CHECK(prim && p && p->attempts.size() == 3 && p->attempts[1].result == Result::DecodeFailed &&
              p->attempts[2].name == "Data\\Art\\Shaders\\PRIM.FXO" && p->outcome == Outcome::Resolved &&
              p->sha256 == Hash("GOOD-compiled"), "a failed shader decode is followed by a successful .FXO retry");
        CHECK(none && n && n->outcome == Outcome::DefaultSubstituted && n->winner.resourceId == "IDR_DEFAULT_SHADER" &&
              n->sha256 == Hash("FALLBACK"), "exhausted shader retries end in the embedded default shader");
        SAFE_RELEASE(lit); SAFE_RELEASE(prim); SAFE_RELEASE(none);
    }
    {
        // A shader annotation asks first, then two emitters with different casing.
        const int before = decoder.decodes;
        WriteBytes(textures + L"SMOKE.TGA", "GOOD-smoke");
        Fake* a = get("smoke.tga");
        Fake* b = get("smoke.tga");
        Fake* c = get("SMOKE.TGA");
        const AssetRecord* r = Find(cache.Ledger(), "SMOKE.TGA");
        CHECK(a == b && b == c && decoder.decodes == before + 1 && r &&
              r->requests == std::set<std::string>({ "smoke.tga", "SMOKE.TGA" }),
              "cache hits share one decode and one entry that lists every original name");
        SAFE_RELEASE(a); SAFE_RELEASE(b); SAFE_RELEASE(c);
    }
    {
        // retry.tga was cleared, so its .DDS retry finds RETRY.DDS in the cache.
        cache.Clear();
        Fake* dds = get("retry.dds");
        const int before = decoder.decodes;
        Fake* tga = get("retry.tga");
        const AssetRecord* r = Find(cache.Ledger(), "RETRY.TGA");
        CHECK(tga == dds && decoder.decodes == before && r && r->attempts.back().reason == "cached" &&
              r->attempts.back().result == Result::Resolved && r->sha256 == Hash("GOOD-retry-dds"),
              "a retry that hits another entry records that entry's winner and hash, without decoding");
        SAFE_RELEASE(dds); SAFE_RELEASE(tga);
    }
    {
        const size_t count = cache.Ledger().Snapshot().size();
        cache.Clear();
        Fake* f = get("lit.tga");
        CHECK(cache.Ledger().Snapshot().size() == count, "a reload of identical bytes after Clear merges into the catalog");
        SAFE_RELEASE(f);
        cache.Clear();
        WriteBytes(g_root + L"LIT.TGA", "GOOD-literal-changed");
        f = get("lit.tga");
        const AssetRecord* r = Find(cache.Ledger(), "LIT.TGA");
        const auto all = cache.Ledger().Snapshot();
        const auto lits = std::count_if(all.begin(), all.end(), [](const AssetRecord& x) { return x.identity == "LIT.TGA"; });
        CHECK(all.size() == count + 1 && lits == 2 && r && r->sha256 == Hash("GOOD-literal-changed"),
              "changed bytes after Clear add a load, and the catalog keeps the earlier one");
        SAFE_RELEASE(f);
    }
    {
        const auto all = cache.Ledger().Snapshot();
        bool sorted = true;
        for (size_t i = 1; i < all.size(); ++i) sorted = sorted && !(all[i].identity < all[i - 1].identity);
        CHECK(sorted && cache.Ledger().Unverifiable().empty(), "the snapshot is sorted by identity and nothing is unverifiable");
    }
    {
        ShortFileManager shortFiles;
        AssetCache<Fake> shortCache("texture", &shortFiles, "Short\\", ".DDS", 0);
        FakeDecoder shortDecoder;
        Fake* f = shortCache.Get(shortDecoder, "short.tga");
        const AssetRecord* r = shortCache.Ledger().Find("texture", "SHORT.TGA");
        // Only the literal miss is recorded: both content reads came up short.
        CHECK(shortCache.Ledger().Unverifiable() == "short read: Short\\SHORT.TGA" && r && r->attempts.size() == 1 &&
              r->attempts[0].name == "SHORT.TGA" && r->outcome == Outcome::DefaultSubstituted,
              "a short read marks the ledger unverifiable and is not recorded as an attempt");
        SAFE_RELEASE(f);
    }

    SetCurrentDirectoryW(temp);
    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
