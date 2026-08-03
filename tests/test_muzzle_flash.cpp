// Unit test for src/MuzzleFlashFilter.h -- IsMuzzleFlashMesh (hide fire-only
// muzzle quads on a reference object). Pure-header test (string only); links
// nothing -- the test_reference_world / test_gizmo_sizing precedent.
#include "../src/MuzzleFlashFilter.h"
#include <cstdio>

static int g_fail = 0;
static void ok(bool c, const char* m) { printf(c ? "  ok: %s\n" : "  FAIL: %s\n", m); if (!c) ++g_fail; }

int main()
{
    // Rule: hide iff the mesh name contains "flash" AND it is muzzle-associated
    // (its own name has "muzzle" OR its connected bone starts "muzzle"). Both args
    // are lower-cased by the caller; boneNameLower is "" when there's no connection.

    // Vanilla EV_AT-AT.ALO flash meshes (verified corpus).
    ok(IsMuzzleFlashMesh("muzzleb_00_flash", "muzzleb_00"), "MuzzleB_00_Flash on MuzzleB_00 -> hidden");
    ok(IsMuzzleFlashMesh("muzzleflash01",    "muzzlea_01"), "Muzzleflash01 on MuzzleA_01 -> hidden");
    ok(IsMuzzleFlashMesh("muzzleflash",      "muzzlea_00"), "Muzzleflash on MuzzleA_00 -> hidden");

    // Bone branch: a "*flash*"-named mesh on a muzzle bone is caught even without
    // "muzzle" in its own name.
    ok(IsMuzzleFlashMesh("fx_flash", "muzzle_l"), "fx_flash on a muzzle bone -> hidden (bone branch)");

    // Name branch: a "muzzle"+"flash" mesh with no connection (empty bone) is caught.
    ok(IsMuzzleFlashMesh("muzzleflash", ""), "muzzle+flash name with no bone -> hidden (name branch)");

    // Structural geometry is NEVER caught -- including real geometry mounted on a
    // "Muzzle*" bone (the false-positive the "flash"-in-name guard exists to prevent).
    ok(!IsMuzzleFlashMesh("barrel_00",    "muzzlepivot"), "barrel on a Muzzle* bone -> KEPT (no 'flash')");
    ok(!IsMuzzleFlashMesh("muzzle_mount", "muzzlepivot"), "muzzle_mount on a Muzzle* bone -> KEPT (no 'flash')");
    ok(!IsMuzzleFlashMesh("b_body",       "b_body"),      "B_Body -> kept");
    ok(!IsMuzzleFlashMesh("at-at_top_lod0", "root"),      "hull LOD mesh -> kept");
    ok(!IsMuzzleFlashMesh("b_turret",     "b_turret"),    "turret on B_Turret bone -> kept");
    ok(!IsMuzzleFlashMesh("muzzle_cover", "b_head"),      "muzzle_cover (no 'flash') -> kept");
    ok(!IsMuzzleFlashMesh("flash_panel", "b_hull"),       "flash_panel on a hull bone -> kept (not muzzle)");

    printf(g_fail ? "\n=== muzzle flash: %d FAIL ===\n" : "\n=== muzzle flash: ALL PASS ===\n", g_fail);
    return g_fail ? 1 : 0;
}
