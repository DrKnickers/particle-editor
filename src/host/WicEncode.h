#ifndef HOST_WIC_ENCODE_H
#define HOST_WIC_ENCODE_H
//
// [R3b] WIC-based PNG encode for the --record frame pipeline
// (tasks/2026-07-07-perf-followups-plan.md §3). The GDI+ encoder
// (WindowCapture.cpp EncodeBgraToPng) measured ≈49 ms/frame at 3200×1460,
// back-pressuring the capture thread 8.9 s per f04 clip; WIC's PNG encoder
// with FilterOption=None is the measured-faster core. ONLY AsyncFrameEncoder
// consumes this — the GDI+ path stays for the preview/capture one-shots
// (its CLSID warm-up is tuned separately, #537).
//
// Output contract: non-interlaced, filter=None PNG from a top-down BGRA
// buffer with the X8 alpha byte IGNORED (32bppBGR source — parity with the
// GDI+ path's PixelFormat32bppRGB). Non-interlaced 8-bit stays inside what
// scripts/wiki-media/build.mjs's raw PNG-chunk parser supports.
//
// Thread contract: call from a thread with COM initialized (AsyncFrameEncoder's
// worker loop initializes MTA COM); the WIC factory is created per call —
// microseconds against a multi-ms encode.

#include <string>

namespace host {

bool EncodeBgraToPngWic(const unsigned char* bgra, int w, int h,
                        const std::wstring& path);

}  // namespace host

#endif  // HOST_WIC_ENCODE_H
