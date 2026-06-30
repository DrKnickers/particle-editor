// test_reference_transform_memory.cpp -- headless unit test for the per-object
// reference-transform memory (src/ReferenceTransformMemory.h), the fix for a land
// unit floating after a prior object left a non-zero transform. Pure (no engine /
// D3D), so it builds + runs in CI. Mirrors tests/test_ref_lock.cpp's ok() style.

#include "../src/ReferenceTransformMemory.h"
#include <cstdio>

using reftransform::Xform;
using reftransform::Store;
using reftransform::OnSwap;

static int g_fail = 0;
static void ok(bool c, const char* m) { printf(c ? "  ok: %s\n" : "  FAIL: %s\n", m); if (!c) ++g_fail; }

static Xform XF(float x, float y, float z) { return Xform{ D3DXVECTOR3(x, y, z), D3DXVECTOR3(0, 0, 0) }; }
static bool eqPos(const Xform& a, float x, float y, float z)
{ return a.pos.x == x && a.pos.y == y && a.pos.z == z; }

int main()
{
    // 1) Initial load ("" -> A): no memory, no outgoing object -> KEEP current. This
    //    is the startup restore (transform set THEN name) -- must not be clobbered.
    {
        Store s; bool changed = false;
        Xform r = OnSwap(s, "", "at_at_walker", XF(0, 0, 7.5f), changed);
        ok(changed && eqPos(r, 0, 0, 7.5f), "initial load keeps the (restored) transform");
        ok(s.empty(), "initial load stores nothing");
    }

    // 2) THE BUG: object A carries a non-zero Z; swapping to land unit B must NOT
    //    inherit it -- B (never moved) starts at origin = grounded.
    {
        Store s; bool changed = false;
        OnSwap(s, "", "generic_star_destroyer", XF(3, 0, 11.2f), changed);   // show SD at z=11.2
        Xform r = OnSwap(s, "generic_star_destroyer", "at_at_walker", XF(3, 0, 11.2f), changed);
        ok(changed && eqPos(r, 0, 0, 0), "swap space->land lands the land unit at origin (grounded)");
        ok(eqPos(s["generic_star_destroyer"], 3, 0, 11.2f), "outgoing object's transform is remembered");
    }

    // 2b) Regression for the deferred-catalog-rebuild defect: the LIVE transform can
    //     still hold a moved object's value (e.g. {0,0,9}) while a DIFFERENT object is
    //     picked. As long as the caller passes the outgoing object's key (non-empty),
    //     a never-moved incoming object must come in at origin -- the stale value must
    //     NOT leak. (The caller keys oldKey off the desired name, which survives the
    //     defer, precisely so oldKey is non-empty here.)
    {
        Store s; bool changed = false;
        Xform r = OnSwap(s, "at_at_walker", "at_st_walker", XF(0, 0, 9), changed);
        ok(changed && eqPos(r, 0, 0, 0), "stale live transform never leaks to a fresh object on a real swap");
        ok(eqPos(s["at_at_walker"], 0, 0, 9), "the stale value is preserved as the outgoing object's memory");
    }

    // 3) Per-object memory round-trips: move A, swap to B, swap BACK to A -> A's
    //    placement is restored (bug #2: a move is no longer lost on swap).
    {
        Store s; bool changed = false;
        OnSwap(s, "", "at_at_walker", XF(5, 0, 0), changed);                  // show A (moved to x=5)
        Xform toB = OnSwap(s, "at_at_walker", "at_st_walker", XF(5, 0, 0), changed);
        ok(eqPos(toB, 0, 0, 0), "B starts at origin");
        Xform back = OnSwap(s, "at_st_walker", "at_at_walker", XF(0, 0, 0), changed);
        ok(changed && eqPos(back, 5, 0, 0), "switching back restores A's remembered placement");
    }

    // 4) Entering None (A -> "") clears the live transform so a later None -> X can't
    //    inherit A's Z.
    {
        Store s; bool changed = false;
        OnSwap(s, "", "at_at_walker", XF(0, 0, 9), changed);
        Xform none = OnSwap(s, "at_at_walker", "", XF(0, 0, 9), changed);
        ok(changed && eqPos(none, 0, 0, 0), "entering None clears to origin");
        ok(eqPos(s["at_at_walker"], 0, 0, 9), "None still remembers the outgoing object");
        // None -> C (no memory): result is origin (no leak of the 9).
        Xform toC = OnSwap(s, "", "at_st_walker", XF(0, 0, 0), changed);
        ok(eqPos(toC, 0, 0, 0), "None -> fresh object stays grounded (no leak through None)");
        // None -> A (has memory): A's placement comes back even through a None hop.
        Xform toA = OnSwap(s, "", "at_at_walker", XF(0, 0, 0), changed);
        ok(eqPos(toA, 0, 0, 9), "None -> remembered object restores its placement");
    }

    // 5) Re-select the SAME object: no change, store untouched.
    {
        Store s; bool changed = true;
        Xform r = OnSwap(s, "at_at_walker", "at_at_walker", XF(2, 2, 2), changed);
        ok(!changed && eqPos(r, 2, 2, 2), "re-select same object -> no change");
        ok(s.empty(), "re-select same object stores nothing");
    }

    printf(g_fail ? "\n=== ref transform memory: %d FAIL ===\n" : "\n=== ref transform memory: ALL PASS ===\n", g_fail);
    return g_fail ? 1 : 0;
}
