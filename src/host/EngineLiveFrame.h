// Live engine view: the host's shared-memory frame layout and the torn-read-safe copy of
// its newest frame. No handles, threads or D3D; EngineLiveReader maps the section.
// The layout is version 1 of the Alamo Preview host's live-frame format.
#ifndef HOST_ENGINE_LIVE_FRAME_H
#define HOST_ENGINE_LIVE_FRAME_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace host::engine_live
{
constexpr std::uint32_t kMagic           = 0x53465041; // bytes "APFS"
constexpr std::uint32_t kVersion         = 1;
constexpr std::uint32_t kSlotCount       = 2;
constexpr std::uint32_t kMaxWidthPx      = 3840;
constexpr std::uint32_t kMaxHeightPx     = 2160;
constexpr std::uint32_t kBytesPerPixel   = 4;
constexpr std::uint32_t kFormatBgrx8     = 1;
constexpr std::size_t   kHeaderBytes     = 64;
constexpr std::size_t   kSlotHeaderBytes = 64;
constexpr std::size_t   kSlotBytes       = kSlotHeaderBytes + std::size_t(kMaxWidthPx) * kMaxHeightPx * kBytesPerPixel;
constexpr std::size_t   kSectionBytes    = kHeaderBytes + kSlotCount * kSlotBytes;

// Header field offsets.
constexpr std::size_t kMagicOffset     = 0;
constexpr std::size_t kVersionOffset   = 4;
constexpr std::size_t kSlotCountOffset = 8;
constexpr std::size_t kSlotBytesOffset = 12;
constexpr std::size_t kLatestOffset    = 16;
constexpr std::size_t kActiveOffset    = 24;
// Slot header field offsets.
constexpr std::size_t kSequenceBeginOffset = 0;
constexpr std::size_t kWidthOffset         = 8;
constexpr std::size_t kHeightOffset        = 12;
constexpr std::size_t kPitchOffset         = 16;
constexpr std::size_t kFormatOffset        = 20;
constexpr std::size_t kCameraTagOffset     = 24;
constexpr std::size_t kFrameOffset         = 32;
constexpr std::size_t kSequenceEndOffset   = 48;

struct FrameInfo
{
    std::int64_t  sequence  = 0;
    std::uint32_t width     = 0;
    std::uint32_t height    = 0;
    std::uint64_t cameraTag = 0;
    std::uint64_t frame     = 0;
};

enum class ReadOutcome
{
    Copied,     // pixels and info hold a frame newer than the one asked about
    NoNewFrame, // the newest frame is the one the caller already has, or none yet
    Torn,       // the writer reused the slot during the copy; wait for the next frame
    Invalid     // the header or slot does not match layout version 1
};

template <class T> T LoadField(const std::uint8_t* base, std::size_t offset)
{
    T value;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

// The writer publishes a sequence with release semantics; these loads pair with it.
inline std::int64_t LoadSequence(const std::uint8_t* base, std::size_t offset)
{
    std::atomic_thread_fence(std::memory_order_acquire);
    const std::int64_t sequence = LoadField<std::int64_t>(base, offset);
    std::atomic_thread_fence(std::memory_order_acquire);
    return sequence;
}

inline bool IsValidHeader(const std::uint8_t* section, std::size_t sectionBytes)
{
    if (sectionBytes < kSectionBytes) return false;
    return LoadField<std::uint32_t>(section, kMagicOffset) == kMagic &&
           LoadField<std::uint32_t>(section, kVersionOffset) == kVersion &&
           LoadField<std::uint32_t>(section, kSlotCountOffset) == kSlotCount &&
           LoadField<std::uint32_t>(section, kSlotBytesOffset) == kSlotBytes;
}

inline bool IsStreamActive(const std::uint8_t* section)
{
    return LoadField<std::uint32_t>(section, kActiveOffset) != 0;
}

// Copies the newest frame if it is newer than haveSequence. The slot carries its sequence
// before and after the pixels; both must still equal `latest` after the copy, or the writer
// reused the slot mid-copy and the copy is dropped.
inline ReadOutcome ReadNewestFrame(const std::uint8_t*        section,
                                   std::size_t                sectionBytes,
                                   std::int64_t               haveSequence,
                                   std::vector<std::uint8_t>& pixels,
                                   FrameInfo&                 info)
{
    if (!IsValidHeader(section, sectionBytes)) return ReadOutcome::Invalid;
    const std::int64_t latest = LoadSequence(section, kLatestOffset);
    if (latest <= 0 || latest == haveSequence) return ReadOutcome::NoNewFrame;
    const std::uint8_t* slot = section + kHeaderBytes + std::size_t(latest % kSlotCount) * kSlotBytes;
    if (LoadSequence(slot, kSequenceBeginOffset) != latest) return ReadOutcome::Torn;
    FrameInfo next;
    next.sequence                   = latest;
    next.width                      = LoadField<std::uint32_t>(slot, kWidthOffset);
    next.height                     = LoadField<std::uint32_t>(slot, kHeightOffset);
    next.cameraTag                  = LoadField<std::uint64_t>(slot, kCameraTagOffset);
    next.frame                      = LoadField<std::uint64_t>(slot, kFrameOffset);
    const std::uint32_t pitchBytes  = LoadField<std::uint32_t>(slot, kPitchOffset);
    const std::uint32_t format      = LoadField<std::uint32_t>(slot, kFormatOffset);
    const bool          isSizeValid = next.width > 0 && next.height > 0 && next.width <= kMaxWidthPx &&
                                      next.height <= kMaxHeightPx && pitchBytes == next.width * kBytesPerPixel;
    if (!isSizeValid || format != kFormatBgrx8) return ReadOutcome::Invalid;
    pixels.resize(std::size_t(pitchBytes) * next.height);
    std::memcpy(pixels.data(), slot + kSlotHeaderBytes, pixels.size());
    const bool isUnchanged =
        LoadSequence(slot, kSequenceEndOffset) == latest && LoadSequence(slot, kSequenceBeginOffset) == latest;
    if (!isUnchanged) return ReadOutcome::Torn;
    info = next;
    return ReadOutcome::Copied;
}
} // namespace host::engine_live
#endif
