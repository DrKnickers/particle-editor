#ifndef REF_TRANSFORM_UNDO_KEY_H
#define REF_TRANSFORM_UNDO_KEY_H

#include <windows.h>     // DWORD
#include <cstdint>
#include <string_view>

// Pure helper for the reference-object-transform undo coalesce key.
//
// A `engine/set/reference-object-transform` request carries the WHOLE transform
// (6 floats) every time, so to give the SAME per-field undo granularity as the
// rest of the new UI we diff the incoming components against the current ones
// and key the coalesce on WHICH components changed:
//   - An X-spinner edit and a Y-spinner edit hash differently -> separate undo
//     steps; one spinner's wheel/arrow burst keeps the same hash -> one step.
//   - If NOTHING changed (a no-op Reset on an already-origin object, or a
//     clamped re-send) the fold is 0 and we return 0; the caller then pushes NO
//     undo entry. (A key-0 capture never coalesces, so capturing on a no-op
//     would leave a spurious, un-foldable undo step.)
//
// Key layout MIRRORS the emitter-props handler (BridgeDispatcher.cpp, the
// `0x80000000u | ((fieldHash & 0x7FFFu) << 16) | id` recipe): bit 31 set (never
// 0 = "structural / never coalesce"), bits 16..30 the FNV-1a fold of the
// changed component names, bits 0..15 a fixed discriminator in place of the
// emitter id. The FNV constants below MUST stay bit-identical to that handler's
// fold or per-field coalescing would silently diverge between the two paths.
//
// `inc` / `cur` order: posX, posY, posZ, rotX, rotY, rotZ.
inline DWORD RefTransformCoalesceKey(const float inc[6], const float cur[6])
{
    static const char* const kComp[6] = {"posX","posY","posZ","rotX","rotY","rotZ"};
    // 0xFEF0 sits well above any realistic emitter id (bits 0..15), so the ref
    // key can never collide with an emitter-props key.
    constexpr DWORD kRefTransformDiscriminator = 0xFEF0u;

    uint32_t fieldHash = 0;
    for (int i = 0; i < 6; ++i)
    {
        if (inc[i] == cur[i]) continue;          // unchanged component contributes nothing
        uint32_t h = 2166136261u;                // FNV-1a offset basis
        for (unsigned char c : std::string_view(kComp[i])) { h ^= c; h *= 16777619u; }
        fieldHash ^= h;                          // XOR-accumulate (order-independent)
    }
    if (fieldHash == 0) return 0;                // no detectable change -> caller skips the capture
    return 0x80000000u | ((fieldHash & 0x7FFFu) << 16) | kRefTransformDiscriminator;
}

#endif // REF_TRANSFORM_UNDO_KEY_H
