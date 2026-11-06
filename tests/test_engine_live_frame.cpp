// Live-view frame layout and the torn-read check, over an in-memory section written the
// way the host's writer does. No shared memory, events or D3D.
#include "host/EngineLiveFrame.h"
#include <cstdio>

using namespace host::engine_live;
namespace
{
constexpr std::uint32_t kFrameWidthPx  = 4;
constexpr std::uint32_t kFrameHeightPx = 2;
constexpr std::uint64_t kCameraTag     = 77;
constexpr std::uint8_t  kFirstShade    = 10;
constexpr std::uint8_t  kSecondShade   = 200;
int                     g_failed       = 0;

void Check(bool passed, const char* message)
{
    if (passed) return;
    ++g_failed;
    std::printf("FAIL: %s\n", message);
}

template <class T> void StoreField(std::uint8_t* base, std::size_t offset, T value)
{
    std::memcpy(base + offset, &value, sizeof value);
}

std::vector<std::uint8_t> MakeSection()
{
    std::vector<std::uint8_t> section(kSectionBytes);
    StoreField(section.data(), kMagicOffset, kMagic);
    StoreField(section.data(), kVersionOffset, kVersion);
    StoreField(section.data(), kSlotCountOffset, kSlotCount);
    StoreField(section.data(), kSlotBytesOffset, static_cast<std::uint32_t>(kSlotBytes));
    StoreField(section.data(), kActiveOffset, std::uint32_t(1));
    return section;
}

std::uint8_t* SlotFor(std::vector<std::uint8_t>& section, std::int64_t sequence)
{
    return section.data() + kHeaderBytes + std::size_t(sequence % kSlotCount) * kSlotBytes;
}

// Mirrors the host writer: begin, fields, pixels, end, then publish `latest`.
void WriteFrame(std::vector<std::uint8_t>& section, std::int64_t sequence, std::uint8_t shade)
{
    std::uint8_t* slot = SlotFor(section, sequence);
    StoreField(slot, kSequenceBeginOffset, sequence);
    StoreField(slot, kWidthOffset, kFrameWidthPx);
    StoreField(slot, kHeightOffset, kFrameHeightPx);
    StoreField(slot, kPitchOffset, kFrameWidthPx * kBytesPerPixel);
    StoreField(slot, kFormatOffset, kFormatBgrx8);
    StoreField(slot, kCameraTagOffset, kCameraTag);
    std::memset(slot + kSlotHeaderBytes, shade, std::size_t(kFrameWidthPx) * kFrameHeightPx * kBytesPerPixel);
    StoreField(slot, kSequenceEndOffset, sequence);
    StoreField(section.data(), kLatestOffset, sequence);
}

void TestLayoutMatchesSpec()
{
    Check(kSlotBytes == 33177664, "slot is 64 header bytes plus 3840 x 2160 BGRX pixels");
    Check(kSectionBytes == 64 + 2 * kSlotBytes, "section is the header plus two slots");
}

void TestReadsNewestFrameOnce()
{
    std::vector<std::uint8_t> section = MakeSection();
    std::vector<std::uint8_t> pixels;
    FrameInfo                 info;
    Check(ReadNewestFrame(section.data(), section.size(), 0, pixels, info) == ReadOutcome::NoNewFrame,
          "an empty stream has no frame");
    WriteFrame(section, 1, kFirstShade);
    WriteFrame(section, 2, kSecondShade);
    Check(ReadNewestFrame(section.data(), section.size(), 0, pixels, info) == ReadOutcome::Copied,
          "the newest frame is copied");
    Check(info.sequence == 2 && info.width == kFrameWidthPx && info.height == kFrameHeightPx, "frame info is read");
    Check(info.cameraTag == kCameraTag, "the camera tag is read");
    Check(pixels.size() == std::size_t(kFrameWidthPx) * kFrameHeightPx * kBytesPerPixel && pixels[0] == kSecondShade,
          "pixels come from the newest slot");
    Check(ReadNewestFrame(section.data(), section.size(), info.sequence, pixels, info) == ReadOutcome::NoNewFrame,
          "a frame already held is not copied again");
}

void TestDropsTornFrames()
{
    std::vector<std::uint8_t> section = MakeSection();
    WriteFrame(section, 1, kFirstShade);
    // The writer has started frame 3 in frame 1's slot but not yet published it.
    StoreField(SlotFor(section, 3), kSequenceBeginOffset, std::int64_t(3));
    std::vector<std::uint8_t> pixels;
    FrameInfo                 info;
    Check(ReadNewestFrame(section.data(), section.size(), 0, pixels, info) == ReadOutcome::Torn,
          "a slot being rewritten is dropped");
    Check(info.sequence == 0, "a torn read leaves the caller's frame info alone");
}

void TestRefusesOtherLayouts()
{
    std::vector<std::uint8_t> section = MakeSection();
    WriteFrame(section, 1, kFirstShade);
    std::vector<std::uint8_t> pixels;
    FrameInfo                 info;
    Check(ReadNewestFrame(section.data(), section.size() - 1, 0, pixels, info) == ReadOutcome::Invalid,
          "a short section is refused");
    StoreField(section.data(), kVersionOffset, kVersion + 1);
    Check(ReadNewestFrame(section.data(), section.size(), 0, pixels, info) == ReadOutcome::Invalid,
          "another layout version is refused");
    StoreField(section.data(), kVersionOffset, kVersion);
    StoreField(SlotFor(section, 1), kPitchOffset, std::uint32_t(1));
    Check(ReadNewestFrame(section.data(), section.size(), 0, pixels, info) == ReadOutcome::Invalid,
          "a pitch that does not match the width is refused");
}
} // namespace

int main()
{
    TestLayoutMatchesSpec();
    TestReadsNewestFrameOnce();
    TestDropsTornFrames();
    TestRefusesOtherLayouts();
    if (g_failed) return 1;
    std::printf("test_engine_live_frame: all passed\n");
    return 0;
}
