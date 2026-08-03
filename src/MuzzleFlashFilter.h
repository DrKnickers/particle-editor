#pragma once
#include <string>
// Pure predicate: should the reference-object renderer hide this mesh as a
// muzzle flash? Extracted as a pure leaf so the naming convention is documented
// and unit-tested headlessly (the AloModel classifier-helper precedent).
//
// Muzzle-flash meshes (e.g. "MuzzleB_00_Flash" mounted on bone "MuzzleB_00", or
// "Muzzleflash" on "MuzzleA_00") are the short bright quads the game shows ONLY
// while a weapon FIRES -- toggled on by the fire event, never drawn at rest. They
// are NOT 0x402-hidden in the .alo (the game gates them at runtime), so the
// reference renderer -- which draws every non-hidden mesh -- shows them always-on,
// wrong for a static idle pose. Hide them so the reference object matches its
// in-game idle look.
//
// Convention (verified against the vanilla EV_AT-AT.ALO corpus -- e.g.
// "Muzzleflash" on bone "MuzzleA_00"): the mesh name MUST carry "flash" (a flash
// quad is always named "*flash*"; structural hull/barrel/turret geometry never is),
// AND it must be muzzle-associated -- either its own name also contains "muzzle" or
// its connected bone name starts with "muzzle". Requiring "flash" is the guard that
// keeps the muzzle-bone branch from deleting REAL geometry that happens to mount on
// a "Muzzle*" bone (e.g. a barrel named "barrel_00" connected to "MuzzlePivot").
//
// Acknowledged narrowness: a flash mesh named without "flash" (e.g. "FX_Fire") is
// NOT caught -- a deliberate trade to avoid hiding structural meshes on modded units.
//
// Both arguments must be lower-cased by the caller (the .alo stores mixed case);
// boneNameLower is "" when the mesh has no 0x602 connection.
inline bool IsMuzzleFlashMesh(const std::string& meshNameLower,
                              const std::string& boneNameLower)
{
    if (meshNameLower.find("flash") == std::string::npos)
        return false;                                  // not a flash quad -> never hidden
    return meshNameLower.find("muzzle") != std::string::npos ||
           boneNameLower.rfind("muzzle", 0) == 0;      // muzzle-associated by name or bone
}
