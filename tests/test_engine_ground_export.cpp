// Checks ground normal leaves and abandoned-export cleanup without a device or game install.
#include "rendering/EngineGroundExport.h"
#include "rendering/EngineGroundExportCleanup.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace
{
void TestAbandonedExportCleanup()
{
    constexpr std::chrono::hours          kCutoffAgeHours{24};
    constexpr std::chrono::hours          kOldAgeHours{48};
    const std::filesystem::file_time_type now = std::filesystem::file_time_type::clock::now();
    const std::filesystem::path           folder =
        std::filesystem::temp_directory_path() / ("pe-ground-export-" + std::to_string(now.time_since_epoch().count()));
    std::filesystem::create_directory(folder);
    try
    {
        const char* const leaves[] = {"123-456-1-color.dds", "123-456-1-normal.dds", "123-456-2-color.dds",
                                      "123-456-2-normal.dds", "unrelated.dds"};
        for (const char* leaf : leaves)
        {
            std::ofstream output(folder / leaf);
            output << "ground export fixture";
            output.close();
            if (!output) throw std::runtime_error("Cannot write ground export fixture.");
            const bool isFresh = std::strstr(leaf, "-2-") != nullptr;
            std::filesystem::last_write_time(folder / leaf, isFresh ? now : now - kOldAgeHours);
        }
        // Matching directories must survive too, even when old and empty.
        const std::filesystem::path directory = folder / "123-456-3-color.dds";
        std::filesystem::create_directory(directory);
        std::filesystem::last_write_time(directory, now - kOldAgeHours);
        const std::error_code error = engine_ground::RemoveAbandonedGroundExports(folder, now - kCutoffAgeHours);
        if (error) throw std::system_error(error, "Ground export cleanup failed");
        for (const char* leaf : leaves)
        {
            const bool isOldExport = std::strstr(leaf, "-1-") != nullptr;
            if (std::filesystem::exists(folder / leaf) == isOldExport)
                throw std::runtime_error(std::string("Wrong cleanup verdict for ") + leaf);
        }
        if (!std::filesystem::is_directory(directory)) throw std::runtime_error("Cleanup removed a directory.");
    }
    catch (...)
    {
        std::filesystem::remove_all(folder);
        throw;
    }
    std::filesystem::remove_all(folder);
}
} // namespace

int main()
{
    const char* const expected[] = {
        nullptr, "W_TEMPGRND00_B.DDS", "W_SAND00_B.DDS", "W_SNOW_RGH_B.DDS", nullptr, nullptr, nullptr, nullptr};
    constexpr int kSlotCount = sizeof(expected) / sizeof(expected[0]);
    for (int slot = 0; slot < kSlotCount; ++slot)
    {
        const char* actual = engine_ground::NormalLeafForSlot(slot);
        const bool  isSame = expected[slot] ? actual && std::strcmp(actual, expected[slot]) == 0 : !actual;
        if (isSame) continue;
        std::printf("FAIL: wrong map-editor normal for slot %d\n", slot);
        return 1;
    }
    if (engine_ground::NormalLeafForSlot(-1) || engine_ground::NormalLeafForSlot(kSlotCount)) return 1;
    try
    {
        TestAbandonedExportCleanup();
    }
    catch (const std::exception& error)
    {
        std::printf("FAIL: %s\n", error.what());
        return 1;
    }
    std::printf("test_engine_ground_export: all passed\n");
    return 0;
}
