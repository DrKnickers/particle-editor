#pragma once
#include <d3dx9math.h>
#include <map>
#include <string>

// Per-object reference-transform memory -- the pure decision logic extracted from
// Engine::SetReferenceObject so its swap edge-cases are unit-tested headlessly (the
// RefLock.h / ReferenceObjectWorld.h precedent; see
// tests/test_reference_transform_memory.cpp).
//
// The editor keeps a single LIVE reference transform (position + rotation). Left as
// one global state it leaks across object swaps: a Z set while framing object A
// (e.g. lifting a space unit) stays put and floats a land unit B picked after it.
// This remembers each object's transform by name, so a swap restores the incoming
// object's own placement (origin if it was never moved) and a freshly-picked unit
// starts grounded.
namespace reftransform {

struct Xform { D3DXVECTOR3 pos; D3DXVECTOR3 rot; };
using Store = std::map<std::string, Xform>;

inline Xform Origin() { return Xform{ D3DXVECTOR3(0,0,0), D3DXVECTOR3(0,0,0) }; }

// Decide the incoming transform for a pick of `newKey` while `oldKey` is shown, and
// update the per-object `store`. Keys MUST be pre-lowercased by the caller (the
// catalog folds names case-insensitively); an empty key means "no object" / None.
//
//   oldKey == newKey            -> no change (outChanged=false), store untouched.
//   real swap (oldKey!=newKey)  -> stash `current` under oldKey (when non-empty), then:
//     * newKey has stored memory          -> return that remembered transform
//     * newKey non-empty, had an outgoing  -> Origin() (a never-moved unit is grounded)
//     * newKey non-empty, NO outgoing      -> keep `current` (initial load: preserves a
//                                             startup-restored transform for the object)
//     * newKey empty (entering None)       -> Origin() (so a later None->X can't inherit)
//
// outChanged is true when the engine should overwrite its live transform with the result.
inline Xform OnSwap(Store& store, const std::string& oldKey, const std::string& newKey,
                    const Xform& current, bool& outChanged)
{
    if (oldKey == newKey) { outChanged = false; return current; }
    outChanged = true;

    if (!oldKey.empty())
        store[oldKey] = current;                 // remember the outgoing object

    if (newKey.empty())
        return Origin();                         // entering None

    auto it = store.find(newKey);
    if (it != store.end())
        return it->second;                       // restore this object's placement
    if (!oldKey.empty())
        return Origin();                         // never moved + a real swap -> grounded
    return current;                              // initial load, no memory -> keep current
}

} // namespace reftransform
