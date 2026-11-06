// Ground companion policy for the map editor. Window-free; never selects the editor's _bc maps.
#ifndef RENDERING_ENGINE_GROUND_EXPORT_H
#define RENDERING_ENGINE_GROUND_EXPORT_H

namespace engine_ground
{
inline const char* NormalLeafForSlot(int slot)
{
    constexpr int kGrassSlot = 1;
    constexpr int kSandSlot  = 2;
    constexpr int kSnowSlot  = 3;
    switch (slot)
    {
        case kGrassSlot: return "W_TEMPGRND00_B.DDS";
        case kSandSlot:  return "W_SAND00_B.DDS";
        case kSnowSlot:  return "W_SNOW_RGH_B.DDS";
        default:         return nullptr;
    }
}
} // namespace engine_ground
#endif
