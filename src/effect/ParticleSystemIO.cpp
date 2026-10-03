// Bodies of the pure-IO helpers declared in ParticleSystemIO.h. They live in
// their own translation unit, apart from WinMain, so the standalone unit tests
// can link the real load and save wrappers.

#include <windows.h>   // GetCurrentProcessId; utils.h (via the header) needs it first
#include <memory>
#include <string>

#include "ParticleSystemIO.h"
#include "AtomicSave.h"
#include "ParticleSystem.h"
#include "common/exceptions.h"
#include "common/files.h"
#include "host/StringConv.h"          // host::WideToUtf8 — ParticleSystemIO errorOut is UTF-8

// ── Pure-IO ParticleSystem helpers ─────────
//
// These free functions are declared in `src/effect/ParticleSystemIO.h` and
// implemented here so the host's BridgeDispatcher can read/write .alo
// files without duplicating the PhysicalFile + ParticleSystem(IFile*)
// ctor dance.

std::unique_ptr<ParticleSystem> LoadParticleSystem(const std::wstring& path,
                                                   std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    PhysicalFile* file = NULL;
    try
    {
        file = new PhysicalFile(path);
    }
    catch (wexception& e)
    {
        if (errorOut) *errorOut = host::WideToUtf8(e.wwhat());
        return nullptr;
    }
    catch (...)
    {
        if (errorOut) *errorOut = "could not open file";
        return nullptr;
    }

    std::unique_ptr<ParticleSystem> system;
    try
    {
        system.reset(new ParticleSystem(file));
    }
    catch (wexception& e)
    {
        if (errorOut) *errorOut = host::WideToUtf8(e.wwhat());
        system.reset();
    }
    catch (...)
    {
        if (errorOut) *errorOut = "not a valid particle system";
        system.reset();
    }
    file->Release();
    return system;
}

bool SaveParticleSystem(ParticleSystem* system, const std::wstring& path,
                        std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    if (system == NULL)
    {
        if (errorOut) *errorOut = "null particle system";
        return false;
    }
    // Data-loss guard: write a flushed sibling temp, then rename it into place
    // (AtomicWriteParticleSystem, shared with the autosave tiers). Opening the
    // destination CREATE_ALWAYS and streaming chunks in place would let any
    // mid-write failure (disk full, removable drive, denied, throw) corrupt the
    // user's original .alo. A failure leaves the original untouched; only a
    // fully-written temp replaces it. The temp name carries the process id so
    // it can never collide with another editor instance's save of this file,
    // nor with an autosave tier's fixed `.tmp`.
    AtomicSaveOptions options;
    options.tmpPath = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    return AtomicWriteParticleSystem(*system, path, options, errorOut);
}
