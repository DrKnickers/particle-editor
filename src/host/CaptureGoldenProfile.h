#pragma once

namespace host
{

// The render-golden profile is deliberately narrower than general capture.
// It is an internal particle-capture contract with one canonical skydome; it
// must never silently change normal launches or reference-object captures.
inline bool IsGoldenProfileRequestValid(bool goldenProfile,
                                        bool hasCaptureAlo,
                                        bool hasCapturePng,
                                        bool hasCaptureRef,
                                        int captureSkydomeSlot)
{
    return !goldenProfile ||
           (hasCaptureAlo &&
            hasCapturePng &&
            !hasCaptureRef &&
            captureSkydomeSlot == 1);
}

// Test-host and render-golden runs both need constructor defaults rather than
// the interactive user's persisted view state. Ordinary interactive/capture
// runs retain the existing restore behavior.
inline bool ShouldRestorePersistedViewSettings(bool useTestHost,
                                               bool goldenProfile)
{
    return !useTestHost && !goldenProfile;
}

// CaptureRunner historically honors ShowGround and CaptureCam* from HKCU.
// Golden captures replace those inputs with an explicit canonical state, while
// every ordinary capture continues to honor them.
inline bool ShouldReadCaptureRegistryOverrides(bool goldenProfile)
{
    return !goldenProfile;
}

// The persisted mod-layer stack is a machine-local input: whichever layer the
// interactive editor last had active silently redirects content resolution. A
// golden capture must resolve against the unmodded install so the same commit
// yields the same pixels on another box. Ordinary captures keep the restore —
// the write-back is already suppressed for them (ModManager ephemeral).
inline bool ShouldRestorePersistedModLayers(bool goldenProfile)
{
    return !goldenProfile;
}

} // namespace host
