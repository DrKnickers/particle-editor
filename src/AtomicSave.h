#ifndef ATOMIC_SAVE_H
#define ATOMIC_SAVE_H
// Crash-durable .alo write shared by File > Save (SaveParticleSystem in
// main.cpp) and the autosave tiers (Autosave.cpp WriteTier).
//
// The destination is never truncated in place: the system is written to a temp
// file in the same directory, flushed to the device, optionally parsed back
// through the production loader, and only then renamed over the destination
// with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH. Any failure deletes
// the temp and leaves the destination exactly as it was.
#include <string>

class ParticleSystem;

struct AtomicSaveOptions
{
    // Parse the flushed temp back through the production ParticleSystem loader
    // before it may replace the destination.
    bool verify = false;
    // Temp file to write, in the destination's directory so the final rename
    // stays on one volume. Empty means dest + L".tmp".
    std::wstring tmpPath;
    // Test seam: called with the closed temp path just before verification.
    void (*beforeVerify)(const std::wstring& tmpPath) = nullptr;
};

// Returns true once the destination holds the new contents. On failure returns
// false, fills *err (UTF-8, may be null) with a reason, and removes the temp.
bool AtomicWriteParticleSystem(const ParticleSystem& sys, const std::wstring& dest,
                               const AtomicSaveOptions& options, std::string* err);

#endif // ATOMIC_SAVE_H
