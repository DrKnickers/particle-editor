// Disk-level regression test for the ParticleSystemIO contract
// (src/ParticleSystemIO.h): load/save of .alo files through real files on
// disk, plus the error taxonomy the wrappers translate into
// `false + errorOut`.
//
// SCOPE NOTE (same constraint test_alo_roundtrip.cpp:5-12 documents): the
// wrapper BODIES -- LoadParticleSystem / SaveParticleSystem -- live in
// src/main.cpp (main.cpp:465-568) inside the composed-app TU (WinMain +
// WebView2 host + engine), so they cannot be compiled or linked into a
// standalone device-free exe without extracting them into their own TU (a src
// change this test deliberately does not make). What IS linkable -- and what
// this test pins -- is the exact machinery those wrappers are a thin
// try/catch veneer over:
//   - load  = PhysicalFile(path, READ) + ParticleSystem(IFile*)   (main.cpp:470-506)
//   - save  = PhysicalFile(path, WRITE) + ParticleSystem::write   (main.cpp:508-568)
// and the exception taxonomy the wrappers convert to false+errorOut:
//   - nonexistent path       -> FileNotFoundException  (files.cpp:53-59)
//   - unwritable path        -> FileNotFoundException / IOException (files.cpp:56-63)
//   - corrupt / non-.alo file -> BadFileException family / ReadException
// Every failure below must surface as a wexception-derived throw (what the
// wrapper reports) and NEVER as a crash or a silent success.
//
// Temp-file handling mirrors tests/test_palette_store.cpp (GetTempPathW + a
// unique per-process directory, deleted on exit). Standalone console exe; see
// tests/build_test_particle_system_io.bat.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ParticleSystemIO.h"   // compile contract: the header must stand alone
#include "ParticleSystem.h"
#include "ParticleSystemInstance.h"
#include "ResourceLimits.h"     // kMaxEmitterTreeDepth (section G)
#include "files.h"
#include "exceptions.h"

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Link stub -- see test_alo_roundtrip.cpp:36-39. ~Emitter calls
// ParticleSystemInstance::RemoveEmitter whose real body is D3D-coupled; no
// EmitterInstance is registered here.
void ParticleSystemInstance::RemoveEmitter(EmitterInstance*) {}

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

using Emitter = ParticleSystem::Emitter;

// ---- unique temp dir (pattern from test_palette_store.cpp:51-74) -----------
static std::wstring g_tempDir;
static std::vector<std::wstring> g_tempFiles;

static bool createTempDir()
{
    wchar_t tempRoot[MAX_PATH];
    DWORD n = GetTempPathW(_countof(tempRoot), tempRoot);
    if (n == 0 || n >= _countof(tempRoot)) return false;
    for (DWORD attempt = 0; attempt < 100; ++attempt)
    {
        wchar_t candidate[MAX_PATH];
        swprintf_s(candidate, L"%sAloPsIoTest-%lu-%llu-%lu",
                   tempRoot, GetCurrentProcessId(),
                   (unsigned long long)GetTickCount64(), attempt);
        if (CreateDirectoryW(candidate, NULL)) { g_tempDir = candidate; return true; }
        if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    return false;
}

static std::wstring tempPath(const wchar_t* name)
{
    std::wstring p = g_tempDir + L"\\" + name;
    g_tempFiles.push_back(p);
    return p;
}

static void cleanupTempDir()
{
    for (const std::wstring& f : g_tempFiles) DeleteFileW(f.c_str());
    RemoveDirectoryW(g_tempDir.c_str());
}

// ---- helpers over the real disk path ----------------------------------------
// Save `ps` to `path` the way SaveParticleSystem's happy path does
// (PhysicalFile WRITE + ParticleSystem::write). Returns false if any
// wexception surfaced; `other` flags a non-wexception escape.
static bool saveTo(ParticleSystem& ps, const std::wstring& path, bool& other)
{
    other = false;
    PhysicalFile* f = NULL;
    try
    {
        f = new PhysicalFile(path, PhysicalFile::WRITE);
        ps.write(f);
        f->Release();
        return true;
    }
    catch (wexception&)
    {
        if (f) f->Release();
        return false;
    }
    catch (...)
    {
        other = true;
        if (f) f->Release();
        return false;
    }
}

// Load `path` the way LoadParticleSystem does (PhysicalFile READ +
// ParticleSystem(IFile*)). nullptr on wexception; `other` flags anything else.
static std::unique_ptr<ParticleSystem> loadFrom(const std::wstring& path, bool& other)
{
    other = false;
    PhysicalFile* f = NULL;
    try
    {
        f = new PhysicalFile(path);   // READ is the default mode (files.h:59)
    }
    catch (wexception&) { return nullptr; }
    catch (...)         { other = true; return nullptr; }

    std::unique_ptr<ParticleSystem> ps;
    try         { ps.reset(new ParticleSystem(f)); }
    catch (wexception&) { ps.reset(); }
    catch (...)         { other = true; ps.reset(); }
    f->Release();
    return ps;
}

static void writeRawBytes(const std::wstring& path, const void* data, unsigned long n)
{
    PhysicalFile* f = new PhysicalFile(path, PhysicalFile::WRITE);
    if (n) f->write(data, n);
    f->Release();
}

// ---- section-G helpers: spawn-graph shape ---------------------------------
// Both walk ITERATIVELY on purpose. The whole point of kMaxEmitterTreeDepth is
// that a recursive walk over an unclamped graph is unsafe, so the oracle that
// measures the clamp must not share the defect it is measuring -- with the cap
// reverted these run over the full un-clamped chain and must still return.

// Length (in nodes) of the longest spawn chain. Roots count as depth 1.
static size_t maxChainDepth(const ParticleSystem& ps)
{
    const std::vector<Emitter*>& es = ps.getEmitters();
    size_t best = 0;
    for (size_t r = 0; r < es.size(); ++r)
    {
        if (es[r]->parent != NULL) continue;              // start at roots only
        std::vector<std::pair<size_t, size_t> > stack;    // (index, depth)
        stack.push_back(std::make_pair(r, (size_t)1));
        while (!stack.empty())
        {
            const size_t u = stack.back().first;
            const size_t d = stack.back().second;
            stack.pop_back();
            if (d > best) best = d;
            // A chain longer than the emitter count means a cycle survived
            // validation; bail loudly rather than spin forever.
            if (d > es.size()) return (size_t)-1;
            const Emitter* e = es[u];
            if (e->spawnDuringLife != (size_t)-1)
                stack.push_back(std::make_pair(e->spawnDuringLife, d + 1));
            if (e->spawnOnDeath != (size_t)-1)
                stack.push_back(std::make_pair(e->spawnOnDeath, d + 1));
        }
    }
    return best;
}

static size_t rootCount(const ParticleSystem& ps)
{
    size_t n = 0;
    const std::vector<Emitter*>& es = ps.getEmitters();
    for (size_t i = 0; i < es.size(); ++i) if (es[i]->parent == NULL) ++n;
    return n;
}

// Author a single lifetime-spawn chain `nodes` long. Returns what it built.
static size_t buildChain(ParticleSystem& ps, size_t nodes)
{
    Emitter* cur = ps.addRootEmitter();
    if (cur == NULL) return 0;
    size_t built = 1;
    while (built < nodes)
    {
        Emitter* next = ps.addLifetimeEmitter(cur);
        if (next == NULL) break;
        cur = next;
        ++built;
    }
    return built;
}

int main()
{
    std::printf("test_particle_system_io\n");
    if (!createTempDir())
    {
        std::printf("FATAL: could not create a temp directory\n");
        return 2;
    }

    // ---- A: save -> load round-trip through real files ----------------------
    {
        ParticleSystem ps;
        ps.setName("IoRoundTrip");
        Emitter* e = ps.addRootEmitter();
        e->name          = "IoEm0";
        e->colorTexture  = "FX\\Io.TGA";
        e->lifetime      = 3.25f;
        e->nTriangles    = 7;

        const std::wstring path = tempPath(L"roundtrip.alo");
        bool other = false;
        CHECK(saveTo(ps, path, other) && !other, "save to a temp .alo succeeds");

        std::unique_ptr<ParticleSystem> rp = loadFrom(path, other);
        CHECK(rp != nullptr && !other, "load of the saved .alo succeeds");
        if (rp)
        {
            CHECK(rp->getName() == "IoRoundTrip", "round-trip: system name survives");
            CHECK(rp->getEmitters().size() == 1, "round-trip: emitter count survives");
            if (rp->getEmitters().size() == 1)
            {
                const Emitter* re = rp->getEmitters()[0];
                CHECK(re->name == "IoEm0", "round-trip: emitter name survives");
                CHECK(re->colorTexture == "FX\\Io.TGA", "round-trip: colorTexture survives");
                CHECK(re->lifetime == 3.25f, "round-trip: lifetime survives");
                CHECK(re->nTriangles == 7, "round-trip: nTriangles survives");
            }
        }

        // Saving over an existing file truncates + rewrites (CREATE_ALWAYS,
        // files.cpp:51) -- the second image must still load cleanly.
        ps.setName("IoRoundTrip2");
        CHECK(saveTo(ps, path, other) && !other, "overwrite-save succeeds");
        rp = loadFrom(path, other);
        CHECK(rp != nullptr && !other && rp->getName() == "IoRoundTrip2",
              "overwritten file loads the NEW image");
    }

    // ---- B: load of a nonexistent path -> FileNotFoundException -------------
    {
        const std::wstring missing = g_tempDir + L"\\does-not-exist.alo";
        bool fnf = false, otherExc = false;
        try { PhysicalFile* f = new PhysicalFile(missing); f->Release(); }
        catch (FileNotFoundException&) { fnf = true; }
        catch (...) { otherExc = true; }
        CHECK(fnf && !otherExc,
              "nonexistent path -> FileNotFoundException (wrapper reports errorOut, files.cpp:53-59)");

        bool other = false;
        std::unique_ptr<ParticleSystem> rp = loadFrom(missing, other);
        CHECK(rp == nullptr && !other, "load path returns null (never a crash) for a missing file");
    }

    // ---- C: save to an unwritable path -> throws, never silent --------------
    // A path inside a directory that does not exist -> ERROR_PATH_NOT_FOUND.
    {
        const std::wstring bad = g_tempDir + L"\\no-such-subdir\\out.alo";
        bool threw = false, otherExc = false;
        try { PhysicalFile* f = new PhysicalFile(bad, PhysicalFile::WRITE); f->Release(); }
        catch (FileNotFoundException&) { threw = true; }   // ERROR_PATH_NOT_FOUND branch
        catch (IOException&)           { threw = true; }   // the generic WRITE-open failure
        catch (...) { otherExc = true; }
        CHECK(threw && !otherExc,
              "unwritable path -> IOException family (wrapper reports errorOut, files.cpp:56-63)");

        ParticleSystem ps;
        ps.setName("NeverWritten");
        ps.addRootEmitter();
        bool other = false;
        CHECK(!saveTo(ps, bad, other) && !other, "save path returns false (never a crash)");
    }

    // ---- D: corrupt file -> BadFileException family, no silent success ------
    {
        const std::wstring garbage = tempPath(L"garbage.alo");
        static const char junk[] = "This is definitely not an Alamo particle system file.";
        writeRawBytes(garbage, junk, (unsigned long)sizeof(junk));

        bool badFile = false, otherExc = false;
        try
        {
            PhysicalFile* f = new PhysicalFile(garbage);
            try { ParticleSystem ps(f); }
            catch (...) { f->Release(); throw; }
            f->Release();
        }
        catch (BadFileException&) { badFile = true; }   // incl. WrongFileException
        catch (ReadException&)    { badFile = true; }
        catch (...) { otherExc = true; }
        CHECK(badFile && !otherExc,
              "garbage bytes -> BadFileException/ReadException, no other escape");

        bool other = false;
        std::unique_ptr<ParticleSystem> rp = loadFrom(garbage, other);
        CHECK(rp == nullptr && !other, "load path returns null for a corrupt file");
    }

    // ---- E: empty file -> rejected via the documented path ------------------
    {
        const std::wstring empty = tempPath(L"empty.alo");
        writeRawBytes(empty, NULL, 0);
        bool other = false;
        std::unique_ptr<ParticleSystem> rp = loadFrom(empty, other);
        CHECK(rp == nullptr && !other, "zero-byte file -> null, no foreign exception");
    }

    // ---- F: truncated valid file -> rejected, never silent garbage ----------
    {
        ParticleSystem ps;
        ps.setName("TruncateMe");
        ps.addRootEmitter();
        const std::wstring whole = tempPath(L"whole.alo");
        bool other = false;
        CHECK(saveTo(ps, whole, other) && !other, "truncation setup: full save succeeds");

        // Read the image back and rewrite only the first half.
        {
            PhysicalFile* f = new PhysicalFile(whole);
            std::vector<unsigned char> img(f->size());
            if (!img.empty()) f->read(img.data(), (unsigned long)img.size());
            f->Release();
            const std::wstring half = tempPath(L"half.alo");
            writeRawBytes(half, img.data(), (unsigned long)(img.size() / 2));
            std::unique_ptr<ParticleSystem> rp = loadFrom(half, other);
            CHECK(rp == nullptr && !other,
                  "half-truncated .alo -> null via the documented error path");
        }
    }

    // ---- G: spawn-chain depth is capped on load (2026-07 audit) ------
    //
    // kMaxAloEmitters (65536) bounds how MANY emitters a file may carry and
    // says nothing about their ARRANGEMENT, so a single chain of tens of
    // thousands passes every pre-existing check. ValidateEmitterGraph enforces
    // single-parent and breaks cycles but did not bound depth -- and the graph
    // is walked RECURSIVELY downstream (BuildEmitterTreeNode on every open,
    // deleteEmitter on every delete), so an over-deep-but-valid file overflowed
    // the stack. The cap clears the offending link, which re-roots the tail
    // rather than discarding it.
    //
    // The save path writes the in-memory graph verbatim (ParticleSystem::write
    // is a flat emitter loop, no validation), so authoring over-deep in memory
    // and reloading is the honest way to produce the adversarial file.
    {
        const size_t cap  = static_cast<size_t>(kMaxEmitterTreeDepth);
        const size_t over = cap + 40;

        ParticleSystem ps;
        ps.setName("DeepChain");
        CHECK(buildChain(ps, over) == over, "deep-chain setup: authored cap+40 emitters");
        // The editing path is deliberately uncapped -- ValidateEmitterGraph
        // runs on load and import only. If this ever fails, the cap moved to
        // the wrong layer and the rest of this section proves nothing.
        CHECK(maxChainDepth(ps) == over,
              "deep-chain setup: in-memory chain is cap+40 deep (editing path is uncapped)");

        const std::wstring path = tempPath(L"deepchain.alo");
        bool other = false;
        CHECK(saveTo(ps, path, other) && !other, "deep-chain: over-deep system saves");

        std::unique_ptr<ParticleSystem> rp = loadFrom(path, other);
        CHECK(rp != nullptr && !other, "deep-chain: over-deep file LOADS (clamped, not rejected)");
        if (rp)
        {
            CHECK(rp->getEmitters().size() == over,
                  "deep-chain: every emitter survives the clamp (nothing dropped)");
            // The discriminating assertions. Without the cap these read
            // `over` and `1` -- the specific wrong values, not merely "changed".
            CHECK(maxChainDepth(*rp) == cap,
                  "deep-chain: reloaded chain is clamped to exactly kMaxEmitterTreeDepth");
            CHECK(rootCount(*rp) == 2,
                  "deep-chain: the over-deep tail is re-rooted (cap+40 -> 2 roots)");
        }
    }

    // ---- G2: a chain exactly AT the cap is untouched ------------------------
    // The boundary control for G. A cap that clamps legal depth is a data-loss
    // bug of its own, so G alone is not enough: G would still pass if the guard
    // fired one node early. These two together pin the off-by-one from both
    // sides.
    {
        const size_t cap = static_cast<size_t>(kMaxEmitterTreeDepth);

        ParticleSystem ps;
        ps.setName("AtCapChain");
        CHECK(buildChain(ps, cap) == cap, "at-cap setup: authored exactly cap emitters");

        const std::wstring path = tempPath(L"atcapchain.alo");
        bool other = false;
        CHECK(saveTo(ps, path, other) && !other, "at-cap: save succeeds");

        std::unique_ptr<ParticleSystem> rp = loadFrom(path, other);
        CHECK(rp != nullptr && !other, "at-cap: load succeeds");
        if (rp)
        {
            CHECK(maxChainDepth(*rp) == cap,
                  "at-cap: a chain exactly at the cap survives at full depth");
            CHECK(rootCount(*rp) == 1,
                  "at-cap: no spurious re-rooting at the boundary");
        }
    }

    // ---- H: DeriveParticleSystemName — legacy DoSaveFile name parity --------
    // The game engine registers particle systems under the internal name
    // (chunk 0x0000). The legacy editor re-stamped it from the destination
    // filename on EVERY save; the host's file/save + file/save-as do the same
    // via this helper. Pin the exact derivation: basename, extension
    // stripped, lowercased, '_' for unrepresentable characters.
    {
        CHECK(DeriveParticleSystemName(L"C:\\Mods\\Art\\PP_Proton_Torpedo_R.alo")
                  == "pp_proton_torpedo_r",
              "derive: basename + strip .alo + lowercase (backslash path)");
        CHECK(DeriveParticleSystemName(L"C:/Mods/Art/P_Explosion.ALO")
                  == "p_explosion",
              "derive: forward-slash path + uppercase extension");
        CHECK(DeriveParticleSystemName(L"noext") == "noext",
              "derive: no extension leaves the name whole");
        CHECK(DeriveParticleSystemName(L"a.b\\P_Fire.v2.alo") == "p_fire.v2",
              "derive: only the LAST dot is an extension; dotted dirs ignored");
        CHECK(DeriveParticleSystemName(L"D:\\out\\P_Sm\x2603ke.alo") == "p_sm_ke",
              "derive: unrepresentable wide char (U+2603) narrows to '_'");

        // Integration: the derived name survives a real save->load cycle.
        ParticleSystem ps;
        ps.addRootEmitter();
        const std::wstring path = tempPath(L"Derived_Name_Check.ALO");
        ps.setName(DeriveParticleSystemName(path));
        bool other = false;
        CHECK(saveTo(ps, path, other) && !other, "derive: stamped save succeeds");
        std::unique_ptr<ParticleSystem> rp = loadFrom(path, other);
        CHECK(rp != nullptr && !other &&
                  rp->getName() == "derived_name_check",
              "derive: reloaded file carries the filename-derived name");
    }

    cleanupTempDir();

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
