#include "../src/RefLock.h"
#include <cstdio>
static int g_fail = 0;
static void ok(bool c, const char* m){ printf(c?"  ok: %s\n":"  FAIL: %s\n", m); if(!c) ++g_fail; }
int main(){
    // 1) Truth table: selection survives only when requested AND unlocked.
    ok(RefLockResolveSelected(true,  false) == true,  "want+unlocked -> selected");
    ok(RefLockResolveSelected(true,  true)  == false, "want+locked   -> NOT selected");
    ok(RefLockResolveSelected(false, false) == false, "no-want+unlocked -> not selected");
    ok(RefLockResolveSelected(false, true)  == false, "no-want+locked   -> not selected");

    // 2) Scenario: lock-while-selected deselects (SetReferenceLocked(true) re-resolves
    //    the CURRENT selection under the new lock).
    bool selected = true;
    selected = RefLockResolveSelected(selected, /*locked=*/true);
    ok(selected == false, "lock while selected -> deselected");

    // 3) Scenario: auto-select-on-load while locked stays deselected (the
    //    SetReferenceObject path computes !name.empty() through the SAME rule).
    bool nameNonEmpty = true;
    ok(RefLockResolveSelected(nameNonEmpty, /*locked=*/true) == false,
       "load object while locked -> stays deselected");
    ok(RefLockResolveSelected(nameNonEmpty, /*locked=*/false) == true,
       "load object while unlocked -> selected");

    // 4) Scenario: unlock re-resolves to the requested state (no change when unlocked).
    ok(RefLockResolveSelected(false, false) == false, "unlock a deselected object -> stays deselected");
    ok(RefLockResolveSelected(true,  false) == true,  "unlock with a pending select -> selectable");

    // 5) Body-click consume rule: a viewport LMB body hit is CONSUMED
    //    (select + swallow) only when the object is UNLOCKED. A locked object is
    //    navigation-transparent -- even a geometric hit passes through to the camera.
    ok(RefLockConsumeBodyClick(true,  false) == true,  "hit+unlocked -> consume (select)");
    ok(RefLockConsumeBodyClick(true,  true)  == false, "hit+locked   -> pass through (navigation)");
    ok(RefLockConsumeBodyClick(false, false) == false, "miss+unlocked -> nothing to consume");
    ok(RefLockConsumeBodyClick(false, true)  == false, "miss+locked   -> nothing to consume");

    // NOTE: these cases prove the PREDICATE only. The WM_LBUTTONDOWN routing that
    // consumes-vs-falls-through on its result has no boot-free test seam (--drive
    // can't inject mouse input); it is verified by the manual live editor check.

    printf(g_fail? "\n=== ref lock: %d FAIL ===\n":"\n=== ref lock: ALL PASS ===\n", g_fail);
    return g_fail?1:0;
}
