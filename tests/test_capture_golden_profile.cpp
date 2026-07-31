// Regression test for the render-golden capture profile.
//
// The production header owns three small policy decisions, but the policy is
// useful only if the CLI, host restore, and CaptureRunner registry block call
// it. The source-binding checks below pin those three production call sites;
// a header-only predicate test cannot hide an unwired implementation.

#include "host/CaptureGoldenProfile.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

static int g_failed = 0;

#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

static std::string ReadSource(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

static size_t CountOccurrences(const std::string& text,
                               const std::string& needle)
{
    size_t count = 0;
    size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string::npos)
    {
        ++count;
        offset += needle.size();
    }
    return count;
}

static std::string WithoutWhitespace(const std::string& text)
{
    std::string compact;
    compact.reserve(text.size());
    for (unsigned char c : text)
    {
        if (!std::isspace(c))
            compact.push_back(static_cast<char>(c));
    }
    return compact;
}

static std::string ControlledBlock(const std::string& source,
                                   const std::string& call)
{
    const size_t callAt = source.find(call);
    if (callAt == std::string::npos)
        return {};

    // A semicolon before the opening brace means the predicate was merely
    // evaluated/assigned rather than directly controlling this block.
    const size_t open = source.find('{', callAt + call.size());
    const size_t semicolon = source.find(';', callAt + call.size());
    if (open == std::string::npos ||
        (semicolon != std::string::npos && semicolon < open))
    {
        return {};
    }

    int depth = 0;
    for (size_t i = open; i < source.size(); ++i)
    {
        if (source[i] == '{')
            ++depth;
        else if (source[i] == '}' && --depth == 0)
            return source.substr(open, i - open + 1);
    }
    return {};
}

int main()
{
    std::printf("test_capture_golden_profile\n");

    // --- 1. Golden profile acceptance is exact. This is the one supported
    // invocation: particle input + PNG output + no reference + skydome 1.
    CHECK(host::IsGoldenProfileRequestValid(true, true, true, false, 1),
          "golden particle capture with PNG and skydome 1 is accepted");

    // Each rejected row names the specific wrong value. These prevent a broad
    // "any capture" profile from silently changing another product surface.
    CHECK(!host::IsGoldenProfileRequestValid(true, false, true, false, 1),
          "golden request without particle input is rejected");
    CHECK(!host::IsGoldenProfileRequestValid(true, true, false, false, 1),
          "golden request without PNG output is rejected");
    CHECK(!host::IsGoldenProfileRequestValid(true, true, true, true, 1),
          "golden request with capture-ref input is rejected");
    CHECK(!host::IsGoldenProfileRequestValid(true, true, true, false, 0),
          "golden request with skydome off is rejected");
    CHECK(!host::IsGoldenProfileRequestValid(true, true, true, false, 2),
          "golden request with non-canonical skydome is rejected");

    // --- 2. OVERREACH GUARDS. The value that catches an over-broad fix is an
    // ordinary capture with the default skydome (0): it remains valid, restores
    // persisted view settings, and reads ShowGround/CaptureCam* from HKCU.
    CHECK(host::IsGoldenProfileRequestValid(false, true, true, false, 0),
          "ordinary capture with skydome 0 remains valid");
    CHECK(host::ShouldRestorePersistedViewSettings(false, false),
          "ordinary non-test-host run still restores persisted view settings");
    CHECK(host::ShouldReadCaptureRegistryOverrides(false),
          "ordinary capture still reads registry overrides");

    CHECK(!host::ShouldRestorePersistedViewSettings(true, false),
          "test-host continues to skip persisted view restore");
    CHECK(!host::ShouldRestorePersistedViewSettings(false, true),
          "golden capture skips persisted view restore");
    CHECK(!host::ShouldRestorePersistedViewSettings(true, true),
          "golden test-host combination also skips persisted view restore");
    CHECK(!host::ShouldReadCaptureRegistryOverrides(true),
          "golden capture skips capture-only registry overrides");

    // --- 3. CLI PRODUCTION BINDING. The validator must directly guard the
    // rejection block and receive all five real parsed values.
    {
        const std::string source = ReadSource(
            std::filesystem::current_path() / "src" / "main.cpp");
        const std::string compact = WithoutWhitespace(source);
        const std::string call = "IsGoldenProfileRequestValid(";
        const std::string block = ControlledBlock(source, call);

        CHECK(!source.empty(), "main.cpp production source is readable");
        CHECK(CountOccurrences(source, call) == 1,
              "main.cpp calls the golden request validator exactly once");
        CHECK(compact.find(
                  "if(!host::IsGoldenProfileRequestValid("
                  "captureGoldenProfile,!captureAlo.empty(),"
                  "!capturePng.empty(),!captureRef.empty(),"
                  "captureSkydome)){") != std::string::npos,
              "CLI validator receives the five real parsed capture values");
        CHECK(block.find("return 2;") != std::string::npos,
              "invalid golden request exits through the CLI error path");
    }

    // --- 4. HOST PRODUCTION BINDING. The predicate must directly contain the
    // persisted bloom/view/lighting restore block, not sit unused nearby.
    {
        const std::string source = ReadSource(
            std::filesystem::current_path() / "src" / "host" /
            "HostWindow.cpp");
        const std::string compact = WithoutWhitespace(source);
        const std::string call =
            "ShouldRestorePersistedViewSettings(";
        const std::string block = ControlledBlock(source, call);

        CHECK(!source.empty(), "HostWindow production source is readable");
        CHECK(CountOccurrences(source, call) == 1,
              "HostWindow calls the persisted-view predicate exactly once");
        CHECK(compact.find(
                  "if(ShouldRestorePersistedViewSettings("
                  "useTestHost,m_captureGoldenProfile)){") !=
                  std::string::npos,
              "persisted restore is directly controlled by the production predicate");
        CHECK(block.find("BloomEnabled") != std::string::npos &&
              block.find("ShowGround") != std::string::npos &&
              block.find("SkydomeIndex") != std::string::npos,
              "predicate contains the persisted bloom and view registry reads");
        CHECK(block.find("LightSunIntensity") != std::string::npos &&
              block.find("engine->SetAmbient") != std::string::npos,
              "predicate contains the persisted lighting restore");
    }

    // --- 5. CAPTURE PRODUCTION BINDING. Both ShowGround and CaptureCam* must
    // remain inside the one registry-read decision. Covering only the helper
    // would miss the original two-stage contamination path.
    {
        const std::string source = ReadSource(
            std::filesystem::current_path() / "src" / "host" /
            "CaptureRunner.cpp");
        const std::string compact = WithoutWhitespace(source);
        const std::string call =
            "ShouldReadCaptureRegistryOverrides(";
        const std::string block = ControlledBlock(source, call);
        const std::string goldenOnlyBlock =
            ControlledBlock(source, "if (m_captureGoldenProfile)");
        const size_t clearAt =
            source.find("engine->SetSkydomeEnvironment(");
        const size_t loadAt =
            source.find("LoadParticleSystem(m_captureAlo");
        const size_t seedAt = source.find("srand(0x5EEDu)");

        CHECK(!source.empty(), "CaptureRunner production source is readable");
        CHECK(CountOccurrences(source, call) == 1,
              "CaptureRunner calls the registry-read predicate exactly once");
        CHECK(compact.find(
                  "if(ShouldReadCaptureRegistryOverrides("
                  "m_captureGoldenProfile)){") != std::string::npos,
              "capture registry reads are directly controlled by the predicate");
        CHECK(block.find("\"ShowGround\"") != std::string::npos,
              "ShowGround registry read is inside the controlled block");
        CHECK(block.find("\"CaptureCamYaw\"") != std::string::npos &&
              block.find("\"CaptureCamPitch\"") != std::string::npos &&
              block.find("\"CaptureCamDist\"") != std::string::npos,
              "all CaptureCam registry reads are inside the controlled block");
        CHECK(compact.find(
                  "m_captureGoldenProfile?"
                  "engine->SetEmbeddedSkydomeSlotForCapture("
                  "m_captureSkydomeSlot):"
                  "engine->SetSkydomeSlot(m_captureSkydomeSlot)") !=
                  std::string::npos,
              "golden mode alone selects the embedded skydome source");
        CHECK(goldenOnlyBlock.find("engine->SetSkydomeEnvironment(") !=
                  std::string::npos,
              "golden-only block clears both game-dome selections");
        CHECK(clearAt != std::string::npos &&
              loadAt != std::string::npos &&
              seedAt != std::string::npos &&
              clearAt < loadAt &&
              loadAt < seedAt,
              "game-dome clear precedes fixture loading and the final PRNG seed");
        CHECK(source.find("IsSkydomeUsingEmbeddedResource") !=
                  std::string::npos &&
              source.find("SkydomePrimaryHasGpuBuffers") !=
                  std::string::npos &&
              source.find("SkydomeSecondaryHasGpuBuffers") !=
                  std::string::npos &&
              source.find("SkydomeMeshHasGpuBuffers") !=
                  std::string::npos,
              "runtime attestation verifies embedded source and live dome buffers");
    }

    // --- 6. ENGINE SOURCE IDENTITY. The capture selector must bypass the
    // FileManager-first route, survive Reset through ReloadSkydomeTexture, and
    // ordinary selection must explicitly leave embedded mode.
    {
        const std::filesystem::path root = std::filesystem::current_path();
        const std::string header = ReadSource(root / "src" / "engine.h");
        const std::string core = ReadSource(root / "src" / "engine.cpp");
        const std::string environment =
            ReadSource(root / "src" / "engine_environment.cpp");
        const std::string compactHeader = WithoutWhitespace(header);
        const std::string compactCore = WithoutWhitespace(core);
        const std::string compactEnvironment = WithoutWhitespace(environment);
        const std::string embeddedLoadBlock = ControlledBlock(
            environment, "static bool LoadEmbeddedSkydomeSlotOne(");
        const std::string resetReacquireBlock = ControlledBlock(
            core, "void Engine::ReacquireDeviceResourcesAfterReset()");

        CHECK(compactHeader.find(
                  "boolSetEmbeddedSkydomeSlotForCapture(intindex);") !=
                  std::string::npos &&
              compactHeader.find(
                  "boolIsSkydomeUsingEmbeddedResource()const"
                  "{returnm_skydomeUsesEmbeddedResource;}") !=
                  std::string::npos,
              "Engine exposes capture-only selection and source identity");
        CHECK(compactEnvironment.find(
                  "m_skydomeUsesEmbeddedResource=true;"
                  "if(!ReloadSkydomeTexture(index))") != std::string::npos,
              "embedded selector latches source identity before loading");
        CHECK(compactEnvironment.find(
                  "if(m_skydomeUsesEmbeddedResource){"
                  "if(slot==1&&LoadEmbeddedSkydomeSlotOne(") !=
                  std::string::npos,
              "reload bypasses FileManager when embedded identity is latched");
        CHECK(embeddedLoadBlock.find("FindResource") != std::string::npos &&
              embeddedLoadBlock.find("D3DXCreateTextureFromFileInMemory") !=
                  std::string::npos &&
              embeddedLoadBlock.find("LoadTextureViaFileManager") ==
                  std::string::npos,
              "embedded loader binds direct RCDATA decode without FileManager");
        CHECK(compactEnvironment.find(
                  "m_skydomeUsesEmbeddedResource=false;"
                  "if(!ReloadSkydomeTexture(newIndex))") != std::string::npos,
              "ordinary selection leaves embedded-source mode before reload");
        CHECK(compactCore.find("m_skydomeUsesEmbeddedResource=false;") !=
                  std::string::npos &&
              resetReacquireBlock.find(
                  "ReloadSkydomeTexture(m_skydomeIndex);") !=
                  std::string::npos &&
              resetReacquireBlock.find(
                  "m_skydomeUsesEmbeddedResource") == std::string::npos,
              "Engine initializes ordinary source mode and Reset preserves it");
    }

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
