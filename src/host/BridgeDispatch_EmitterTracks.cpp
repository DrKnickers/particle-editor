// Bridge request handlers for the emitters/* track kinds.
// BridgeDispatcher::DispatchInternal routes requests here via TryDispatchEmitterTracks.

#include "BridgeDispatcher.h"
#include "BridgeDispatchShared.h"
#include "BridgeRequestContext.h"

#include <cmath>                  // std::isfinite (track-key validation)

using nlohmann::json;

namespace host {

bool BridgeDispatcher::TryDispatchEmitterTracks(BridgeRequestContext& ctx)
{
    // Short local names for the request fields used by the handlers below.
    const json&        params = ctx.params;
    const std::string& kind   = ctx.kind;

    // -------- emitters/get-tracks ----------------
    //
    // Read-only. Serialises the named emitter's 7 tracks (Red, Green,
    // Blue, Alpha, Scale, Index, RotationSpeed in fixed order). Each
    // track's `keys` are emitted in ascending-time order (the source
    // `std::multiset<Key>` already orders by `time` via Key::operator<).
    // Interpolation enum maps as documented in
    // `bridge-schema/src/index.ts`:
    //   IT_LINEAR (0) → "linear"
    //   IT_SMOOTH (1) → "smooth"
    //   IT_STEP   (2) → "step"
    // IT_UNKNOWN (-1) is coerced to "linear" before sending so the
    // wire never carries the sentinel.
    //
    // Unknown id (or no system bound) returns 7 empty tracks rather
    // than ok:false — the React panel renders a "no data" stub instead
    // of an error toast on transient mismatches (e.g. selection lands
    // on an id that was just deleted).
    if (kind == "emitters/get-tracks")
    {
        static const char* kTrackNames[ParticleSystem::NUM_TRACKS] = {
            "red", "green", "blue", "alpha",
            "scale", "index", "rotationSpeed",
        };
        auto interpToString = [](ParticleSystem::Emitter::Track::InterpolationType it) -> const char* {
            switch (it)
            {
                case ParticleSystem::Emitter::Track::IT_LINEAR: return "linear";
                case ParticleSystem::Emitter::Track::IT_SMOOTH: return "smooth";
                case ParticleSystem::Emitter::Track::IT_STEP:   return "step";
                default: return "linear";
            }
        };

        int id = params.value("id", -1);
        json tracksArr = json::array();
        const ParticleSystem::Emitter* emit = nullptr;
        if (id >= 0 && m_pParticleSystem != nullptr && *m_pParticleSystem)
        {
            const auto& emitters = (*m_pParticleSystem)->getEmitters();
            if (static_cast<size_t>(id) < emitters.size() && emitters[id] != nullptr)
            {
                emit = emitters[id];
            }
        }
        for (int i = 0; i < ParticleSystem::NUM_TRACKS; i++)
        {
            json keysArr = json::array();
            const char* interp = "linear";
            if (emit != nullptr)
            {
                const ParticleSystem::Emitter::Track* t = emit->tracks[i];
                if (t != nullptr)
                {
                    // KeyMap is `std::multiset<Key>` ordered by time —
                    // a straight iteration emits keys in ascending
                    // time order.
                    for (const auto& k : t->keys)
                    {
                        keysArr.push_back(json{
                            {"time",  k.time},
                            {"value", k.value},
                        });
                    }
                    interp = interpToString(t->interpolation);
                }
            }
            // lockedTo: detect by pointer identity per the legacy
            // model — channel i is locked to channel j when
            // `tracks[i] == &trackContents[j]` (or transitively
            // `tracks[i] == tracks[j]`, which collapses to the same
            // pointer after the engine's file-load consolidation
            // pass). Only RGBA participate; other channels always
            // report null. Self-pointer (tracks[i] == &trackContents[i])
            // means "not locked" and also reports null.
            const char* lockedToName = nullptr;
            if (emit != nullptr && i < 4)
            {
                for (int j = 0; j < 4; j++)
                {
                    if (j == i) continue;
                    if (emit->tracks[i] == &emit->trackContents[j]
                        || emit->tracks[i] == emit->tracks[j])
                    {
                        // Only "earlier channel" locks are valid per
                        // the schema. If we matched a later channel
                        // via transitive equality, skip it — the
                        // earlier channel will be the canonical lock
                        // target when we hit it in this loop.
                        if (j < i)
                        {
                            lockedToName = kTrackNames[j];
                            break;
                        }
                    }
                }
            }
            tracksArr.push_back(json{
                {"name",          kTrackNames[i]},
                {"keys",          keysArr},
                {"interpolation", interp},
                {"lockedTo",      lockedToName == nullptr
                                      ? json(nullptr)
                                      : json(lockedToName)},
            });
        }
        ctx.SendOk(json{{"tracks", tracksArr}});
        return true;
    }


    // -------- emitters/delete-track-keys + set-track-interpolation --
    //
    // Track mutations. Both handlers
    // resolve the emitter by id, look up the named track on `tracks[]`
    // (the slot pointer aliasing — see the comment block in
    // ParticleSystem.h on Emitter::trackContents), then mutate the underlying multiset /
    // enum directly. Border keys (first + last in time order on the
    // multiset, which is already ordered by Key::operator<) are
    // silently skipped by delete-track-keys per legacy semantics;
    // they define the track's [0, 100] time range and aren't
    // deletable.
    //
    // Both mutations capture undo, mark the editor dirty, and emit
    // engine/state/changed + emitters/tree/changed so the React
    // panel re-fetches via `emitters/get-tracks`.
    auto trackNameToIndex = [](const std::string& name) -> int {
        if (name == "red")           return ParticleSystem::TRACK_RED_CHANNEL;
        if (name == "green")         return ParticleSystem::TRACK_GREEN_CHANNEL;
        if (name == "blue")          return ParticleSystem::TRACK_BLUE_CHANNEL;
        if (name == "alpha")         return ParticleSystem::TRACK_ALPHA_CHANNEL;
        if (name == "scale")         return ParticleSystem::TRACK_SCALE;
        if (name == "index")         return ParticleSystem::TRACK_INDEX;
        if (name == "rotationSpeed") return ParticleSystem::TRACK_ROTATION_SPEED;
        return -1;
    };

    if (kind == "emitters/delete-track-keys")
    {
        int id = params.value("id", -1);
        std::string trackName = params.value("track", std::string{});
        const json& timesJson = params.contains("times") ? params["times"] : json::array();

        ParticleSystem::Emitter* target = getEmitterById(id);
        if (target == nullptr)
        {
            ctx.SendErr("emitter not found");
            return true;
        }

        int trackIdx = trackNameToIndex(trackName);
        if (trackIdx < 0)
        {
            ctx.SendErr("unknown track");
            return true;
        }

        ParticleSystem::Emitter::Track* track = target->tracks[trackIdx];
        if (track == nullptr || track->keys.empty())
        {
            // Nothing to delete — return success silently to match the
            // mock's no-op semantics. Don't emit; nothing changed.
            ctx.SendOk(json::object());
            return true;
        }

        // Border keys = first + last in the multiset (ordered by
        // Key::operator< on `time`). std::multiset::begin / rbegin
        // are the cheapest way to grab them; cache the time values
        // for the skip check below.
        const float firstTime = track->keys.begin()->time;
        const float lastTime  = track->keys.rbegin()->time;

        // Capture undo BEFORE any erase — if every requested time is
        // a border-key no-op we'll discover that in the loop and
        // the capture is a wasted snapshot, but Undo coalescing
        // handles that gracefully and the alternative (capture-late)
        // can't restore the half-mutated multiset if iteration aborts.
        captureUndo();

        int removed = 0;
        for (const auto& t : timesJson)
        {
            if (!t.is_number()) continue;
            float timeVal = t.get<float>();
            // Silent-skip border keys.
            if (timeVal == firstTime || timeVal == lastTime) continue;
            // std::multiset::find takes a `Key` constructed from the
            // time alone; operator< compares only on time so the
            // probe value's `value` field is irrelevant.
            ParticleSystem::Emitter::Track::Key probe(timeVal, 0.0f);
            auto it = track->keys.find(probe);
            if (it != track->keys.end())
            {
                track->keys.erase(it);
                removed++;
            }
        }

        ctx.SendOk(json::object());
        if (removed > 0)
        {
            propagateLinkGroup(target); // sync link-group siblings
            // Re-seat live particle track cursors — the erase(s) above
            // invalidated any cursor pointing at a removed key (see the
            // set-track-key handler for the full rationale).
            if (m_engine != nullptr) m_engine->OnParticleSystemChanged(trackIdx);
            ctx.MarkDirty();
            EmitEmittersTreeChanged();
            EmitEngineStateChanged();
        }
        return true;
    }

    if (kind == "emitters/set-track-interpolation")
    {
        int id = params.value("id", -1);
        std::string trackName  = params.value("track",         std::string{});
        std::string interpName = params.value("interpolation", std::string{});

        ParticleSystem::Emitter* target = getEmitterById(id);
        if (target == nullptr)
        {
            ctx.SendErr("emitter not found");
            return true;
        }

        int trackIdx = trackNameToIndex(trackName);
        if (trackIdx < 0)
        {
            ctx.SendErr("unknown track");
            return true;
        }

        ParticleSystem::Emitter::Track* track = target->tracks[trackIdx];
        if (track == nullptr)
        {
            // No track slot bound — silent no-op (matches the wire
            // contract which never surfaces a refusal envelope).
            ctx.SendOk(json::object());
            return true;
        }

        ParticleSystem::Emitter::Track::InterpolationType next;
        if      (interpName == "linear") next = ParticleSystem::Emitter::Track::IT_LINEAR;
        else if (interpName == "smooth") next = ParticleSystem::Emitter::Track::IT_SMOOTH;
        else if (interpName == "step")   next = ParticleSystem::Emitter::Track::IT_STEP;
        else
        {
            ctx.SendErr("unknown interpolation");
            return true;
        }

        if (track->interpolation == next)
        {
            // No-op — don't capture undo or fire events.
            ctx.SendOk(json::object());
            return true;
        }

        captureUndo();
        track->interpolation = next;

        propagateLinkGroup(target); // sync link-group siblings
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        // Interpolation is read during particle update, and a PAUSED preview
        // skips its idle update unless something invalidates it — so choosing
        // Smooth / Step / Linear updated the curve panel while the placed
        // preview kept drawing the OLD interpolation until you unpaused or
        // stepped a frame. Same broadcast every other
        // track mutation already sends.
        if (m_engine) m_engine->OnParticleSystemChanged(-1);
        EmitEmittersTreeChanged();
        EmitEngineStateChanged();
        return true;
    }


    // -------- emitters/set-track-lock ------------------------------
    //
    // Per-channel track lock. The legacy editor's track-lock combo and the
    // file-load consolidation in ParticleSystem.cpp use the
    // *pointer identity* of `emit->tracks[i]` as the source of
    // truth for lock state: `tracks[i] == &trackContents[j]` (with
    // `i != j`) means channel `i` is read-only and displays
    // channel `j`'s key data.
    //
    // Locking rules (mirror legacy):
    //   - Only the first four channels (RGBA) participate.
    //   - Channel can only lock to an *earlier* channel (Green→Red,
    //     Blue→Red/Green, Alpha→Red/Green/Blue). Other combinations
    //     silently become "unlock" — UI surface already restricts
    //     options so this is defensive only.
    //   - `lockTo: null` restores `tracks[i] = &trackContents[i]`
    //     (channel owns its keys again; previous trackContents[i]
    //     is preserved because the lock didn't touch it).
    if (kind == "emitters/set-track-lock")
    {
        int id = params.value("id", -1);
        std::string channelName = params.value("channel", std::string{});
        // lockTo is `string | null` per the schema; nlohmann's
        // `value<string>` would throw on null, so check explicitly.
        std::string lockToName;
        bool lockToIsNull = !params.contains("lockTo") || params["lockTo"].is_null();
        if (!lockToIsNull) lockToName = params["lockTo"].get<std::string>();

        ParticleSystem::Emitter* target = getEmitterById(id);
        if (target == nullptr)
        {
            ctx.SendErr("emitter not found");
            return true;
        }

        int channelIdx = trackNameToIndex(channelName);
        if (channelIdx < 0)
        {
            ctx.SendErr("unknown channel");
            return true;
        }

        // Only RGBA participate. Silently accept and no-op for the
        // other three — keeps the React side simple (it can always
        // dispatch without first checking which channel it's on).
        if (channelIdx >= 4)
        {
            ctx.SendOk(json::object());
            return true;
        }

        ParticleSystem::Emitter::Track* desired = nullptr;
        if (lockToIsNull)
        {
            desired = &target->trackContents[channelIdx];
        }
        else
        {
            int targetIdx = trackNameToIndex(lockToName);
            // A non-null lockTo must resolve to a valid RGBA channel EARLIER than
            // us. Previously an invalid name silently fell back to unlock and still
            // returned OK — harmless for the React UI (its dropdown only offers
            // valid targets) but a silent-wrong-state trap for --record, where a
            // typo'd channel would record as success. Reject it so the record
            // dispatch aborts (ClassifyResponse sees the error).
            if (targetIdx >= 0 && targetIdx < 4 && targetIdx < channelIdx)
            {
                desired = &target->trackContents[targetIdx];
            }
            else
            {
                ctx.SendErr("invalid lockTo channel (must be an earlier RGBA channel)");
                return true;
            }
        }

        if (target->tracks[channelIdx] == desired)
        {
            // No-op — don't capture undo or fire events.
            ctx.SendOk(json::object());
            return true;
        }

        captureUndo();
        target->tracks[channelIdx] = desired;

        propagateLinkGroup(target); // sync link-group siblings
        // Re-seat live particle track cursors for this channel. The lock
        // repointed tracks[channelIdx] at a DIFFERENT KeyMap, so existing
        // cursors (iterators into the old container) would be compared
        // against the new container's end() next frame — undefined
        // behavior. Reloading re-seats them into the now-current container.
        if (m_engine != nullptr) m_engine->OnParticleSystemChanged(channelIdx);
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEmittersTreeChanged();
        EmitEngineStateChanged();
        return true;
    }


    // -------- emitters/set-track-key ----------
    //
    // Drag-to-move + Spinner edit commit. Erases the key at
    // `oldTime` from the multiset and inserts `(newTime, newValue)`.
    // Border keys (first + last in time order on the multiset) have
    // their `newTime` silently overridden to `oldTime` — only the
    // value moves. This mirrors the React-side drag clamping; the
    // host is the source of truth.
    //
    // `std::multiset<Key>::find` accepts a Key constructed from the
    // time alone — operator< compares only on `time`, so the probe
    // Key's `value` field is irrelevant. Erase + insert is the
    // ordered-key idiom (no in-place mutation of the ordering key).
    if (kind == "emitters/set-track-key")
    {
        int id = params.value("id", -1);
        std::string trackName = params.value("track", std::string{});
        float oldTime  = params.value("oldTime",  0.0f);
        float newTime  = params.value("newTime",  0.0f);
        float newValue = params.value("newValue", 0.0f);

        ParticleSystem::Emitter* target = getEmitterById(id);
        if (target == nullptr)
        {
            ctx.SendErr("emitter not found");
            return true;
        }

        int trackIdx = trackNameToIndex(trackName);
        if (trackIdx < 0)
        {
            ctx.SendErr("unknown track");
            return true;
        }

        ParticleSystem::Emitter::Track* track = target->tracks[trackIdx];
        if (track == nullptr || track->keys.empty())
        {
            // Nothing to move — silent ok.
            ctx.SendOk(json::object());
            return true;
        }

        // Identify border keys before any mutation.
        const float firstTime = track->keys.begin()->time;
        const float lastTime  = track->keys.rbegin()->time;
        const bool isBorder = (oldTime == firstTime || oldTime == lastTime);
        if (isBorder)
        {
            // Border keys: time fixed.
            newTime = oldTime;
        }

        ParticleSystem::Emitter::Track::Key probe(oldTime, 0.0f);
        auto it = track->keys.find(probe);
        if (it == track->keys.end())
        {
            // Key not found at oldTime — silent ok (matches the
            // overlay's read-modify-write semantics).
            ctx.SendOk(json::object());
            return true;
        }

        // Coalesce rapid same-track edits on the same emitter (a wheel/
        // hold-arrow/scrub Value or Time key spinner, plus a multi-key
        // group shift's N per-key calls) into a single undo step within the
        // window. Per-TRACK keying — legacy's exact choice
        // (track<<16|emitterIdx) and the only stable key for a Time spinner,
        // whose oldTime moves every tick. Mirrors the emitter-property layout
        // (set-properties, BridgeDispatch_EmitterProperties.cpp) with trackIdx in place of the field
        // hash; bit 31 set so the key is never 0 (= structural / never-fold).
        const DWORD coalesceKey =
            0x80000000u | ((static_cast<DWORD>(trackIdx) & 0x7FFFu) << 16)
                        | (static_cast<DWORD>(id) & 0xFFFFu);
        captureUndo(coalesceKey);
        track->keys.erase(it);
        track->keys.insert(ParticleSystem::Emitter::Track::Key(newTime, newValue));

        propagateLinkGroup(target); // sync link-group siblings
        // Re-seat the live per-particle track cursors. EmitterInstance
        // caches multiset iterators (prev/next) into tracks[trackIdx]->keys
        // for every live particle; the erase above invalidated any cursor
        // pointing at the moved key, so the next Engine::Update would
        // dereference a singular iterator (xtree assert). OnParticleSystemChanged
        // with the specific track index reloads those cursors — mirrors the
        // legacy editor's per-edit call.
        if (m_engine != nullptr) m_engine->OnParticleSystemChanged(trackIdx);
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEmittersTreeChanged();
        EmitEngineStateChanged();
        return true;
    }


    // -------- emitters/add-track-key ----------
    //
    // Click-to-add (Insert mode) commit. Inserts a new key into the
    // multiset. If a key at the exact `time` already exists, bumps
    // `time` by 0.001 until unique so the multiset doesn't accumulate
    // ambiguously-ordered duplicates (the dedupe is mirrored by the
    // mock at `addTrackKeyInOverlay`). Returns the actual inserted
    // (time, value) so the React side can auto-select the new key.
    if (kind == "emitters/add-track-key")
    {
        int id = params.value("id", -1);
        std::string trackName = params.value("track", std::string{});
        // `time`/`value` must be present + numeric — a missing field defaulting
        // to 0.0f would silently insert a wrong key (e.g. a typo'd param name
        // in a --record clip records as success, publishing a bad curve).
        if (!params.contains("time") || !params["time"].is_number()
            || !params.contains("value") || !params["value"].is_number())
        {
            ctx.SendErr("add-track-key requires numeric 'time' and 'value'");
            return true;
        }
        float time  = params["time"].get<float>();
        float value = params["value"].get<float>();
        if (!std::isfinite(time) || !std::isfinite(value))
        {
            ctx.SendErr("add-track-key 'time'/'value' must be finite");
            return true;
        }

        ParticleSystem::Emitter* target = getEmitterById(id);
        if (target == nullptr)
        {
            ctx.SendErr("emitter not found");
            return true;
        }

        int trackIdx = trackNameToIndex(trackName);
        if (trackIdx < 0)
        {
            ctx.SendErr("unknown track");
            return true;
        }

        ParticleSystem::Emitter::Track* track = target->tracks[trackIdx];
        if (track == nullptr)
        {
            // No track slot bound — fail loud (was a silent ok with the
            // request shape, which let a record clip "succeed" while no key
            // was ever inserted).
            ctx.SendErr("no track slot bound for this channel");
            return true;
        }

        // Dedupe-by-epsilon: bump until the time is unique. Bounded
        // by 1000 iterations as a defensive safety net so a pathological
        // dataset can't lock the dispatch thread.
        ParticleSystem::Emitter::Track::Key probe(time, 0.0f);
        int safety = 1000;
        while (track->keys.find(probe) != track->keys.end() && safety-- > 0)
        {
            time += 0.001f;
            probe = ParticleSystem::Emitter::Track::Key(time, 0.0f);
        }

        captureUndo();
        track->keys.insert(ParticleSystem::Emitter::Track::Key(time, value));

        propagateLinkGroup(target); // sync link-group siblings
        // Re-seat live particle track cursors. insert() doesn't invalidate
        // existing iterators, but a key added BETWEEN a particle's prev/next
        // cursors would be skipped (stale interpolation) until the cursors
        // reload — so re-seat here too, matching the legacy per-edit call.
        if (m_engine != nullptr) m_engine->OnParticleSystemChanged(trackIdx);
        ctx.SendOk(json{{"time", time}, {"value", value}});
        ctx.MarkDirty();
        EmitEmittersTreeChanged();
        EmitEngineStateChanged();
        return true;
    }


    // -------- emitters/add-track-keys --------------------------------
    //
    // The multi-key curve-paste gesture: every key inserted under ONE
    // captureUndo(), so a single Ctrl+Z reverses the whole paste. React
    // used to issue one add-track-key per clipboard key, each capturing
    // its own undo entry, so one Ctrl+Z removed one key of a paste.
    //
    // Per-key validation, the dedupe-by-epsilon bump and the returned
    // ACTUAL (time, value) are identical to the singular handler above;
    // only the undo + event boundary is shared across the batch.
    if (kind == "emitters/add-track-keys")
    {
        int id = params.value("id", -1);
        std::string trackName = params.value("track", std::string{});
        if (!params.contains("keys") || !params["keys"].is_array())
        {
            ctx.SendErr("add-track-keys requires a 'keys' array");
            return true;
        }

        // Validate the WHOLE batch before mutating anything. Rejecting
        // mid-loop would leave half a paste in the document behind an
        // error envelope — a state the caller never asked for and has no
        // single Ctrl+Z for.
        struct PendingKey { float time; float value; };
        std::vector<PendingKey> pending;
        for (const auto& k : params["keys"])
        {
            if (!k.is_object()
                || !k.contains("time")  || !k["time"].is_number()
                || !k.contains("value") || !k["value"].is_number())
            {
                ctx.SendErr("add-track-keys requires numeric 'time' and 'value' on every key");
                return true;
            }
            const float t = k["time"].get<float>();
            const float v = k["value"].get<float>();
            if (!std::isfinite(t) || !std::isfinite(v))
            {
                ctx.SendErr("add-track-keys 'time'/'value' must be finite");
                return true;
            }
            pending.push_back(PendingKey{t, v});
        }

        ParticleSystem::Emitter* target = getEmitterById(id);
        if (target == nullptr)
        {
            ctx.SendErr("emitter not found");
            return true;
        }

        int trackIdx = trackNameToIndex(trackName);
        if (trackIdx < 0)
        {
            ctx.SendErr("unknown track");
            return true;
        }

        ParticleSystem::Emitter::Track* track = target->tracks[trackIdx];
        if (track == nullptr)
        {
            // Fail loud, matching the singular handler — a silent ok here
            // lets a record clip "succeed" with no key inserted.
            ctx.SendErr("no track slot bound for this channel");
            return true;
        }

        if (pending.empty())
        {
            // Empty paste — no undo, no dirty, no events.
            ctx.SendOk(json{{"keys", json::array()}});
            return true;
        }

        captureUndo();

        json inserted = json::array();
        for (const PendingKey& pk : pending)
        {
            float time = pk.time;
            // Dedupe-by-epsilon per key, bounded by 1000 iterations as the
            // singular handler is. Earlier keys of this same batch are
            // already in the multiset, so a paste onto its own source
            // times bumps rather than aliasing.
            ParticleSystem::Emitter::Track::Key probe(time, 0.0f);
            int safety = 1000;
            while (track->keys.find(probe) != track->keys.end() && safety-- > 0)
            {
                time += 0.001f;
                probe = ParticleSystem::Emitter::Track::Key(time, 0.0f);
            }
            track->keys.insert(ParticleSystem::Emitter::Track::Key(time, pk.value));
            inserted.push_back(json{{"time", time}, {"value", pk.value}});
        }

        propagateLinkGroup(target); // sync link-group siblings
        // Re-seat live particle track cursors ONCE for the whole batch —
        // same rationale as the singular handler's per-insert re-seat.
        if (m_engine != nullptr) m_engine->OnParticleSystemChanged(trackIdx);
        ctx.SendOk(json{{"keys", inserted}});
        ctx.MarkDirty();
        EmitEmittersTreeChanged();
        EmitEngineStateChanged();
        return true;
    }


    return false;   // kind not in this domain
}

} // namespace host
