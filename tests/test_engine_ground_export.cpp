// Checks map-editor normal leaves without a device or game install; never tests editor _bc maps.
#include "rendering/EngineGroundExport.h"
#include <cstdio>
#include <cstring>

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
    std::printf("test_engine_ground_export: all passed\n");
    return 0;
}
