// Kind handlers for the emitters/* + linkGroups/* bridge domain(s), moved out of
// DispatchInternal's ladder (Phase A dispatch split --
// tasks/2026-07-06-heavyweight-refactor-plan.md).

#include "BridgeDispatcher.h"
#include "BridgeDispatchShared.h"
#include "BridgeRequestContext.h"

#include "StringConv.h"           // host::Utf8ToWide / WideToUtf8
#include "../ParticleSystemIO.h"  // LoadParticleSystem (preview/import-from-file)

using nlohmann::json;

namespace host {

// Duplicate `source` in `sys` (deep copy via the chunk writer/reader, with a
// fresh auto-suffixed name) and shift every TRACK_INDEX keyframe on the copy by
// `delta` — inserting a single t=0 key = delta when the source track is empty.
// Mirrors legacy EmitterList_DuplicateEmitter + ShiftIndexTrack. Returns the new
// emitter, or nullptr if the copy/insert failed. Shared by the single and the
// batch (#575) duplicate-with-index-increment handlers.
static ParticleSystem::Emitter* DuplicateEmitterWithIndexShift(
    ParticleSystem* sys, ParticleSystem::Emitter* source, float delta)
{
    ParticleSystem::Emitter* dup = nullptr;
    MemoryFile* memfile = new MemoryFile;
    try
    {
        ChunkWriter writer(memfile);
        source->copy(writer);
        memfile->seek(0);
        ChunkReader reader(memfile);
        ParticleSystem::Emitter cleanCopy(reader);
        cleanCopy.name = GenerateDuplicateName(sys, source->name);
        dup = sys->insertEmitterAfter(source, cleanCopy);
    }
    catch (...)
    {
        memfile->Release();
        return nullptr;
    }
    memfile->Release();
    if (dup == nullptr) return nullptr;

    if (delta != 0.0f)
    {
        ParticleSystem::Emitter::Track* track = dup->tracks[ParticleSystem::TRACK_INDEX];
        if (track->keys.empty())
        {
            track->keys.insert(ParticleSystem::Emitter::Track::Key(0.0f, delta));
        }
        else
        {
            std::vector<ParticleSystem::Emitter::Track::Key> tmp(
                track->keys.begin(), track->keys.end());
            track->keys.clear();
            for (size_t i = 0; i < tmp.size(); ++i)
            {
                track->keys.insert(ParticleSystem::Emitter::Track::Key(
                    tmp[i].time, tmp[i].value + delta));
            }
        }
    }
    return dup;
}

bool BridgeDispatcher::TryDispatchEmitters(BridgeRequestContext& ctx, const std::string& kind)
{
    // DispatchInternal-local aliases so the moved ladder blocks below stay
    // verbatim (plan #3A transforms only).
    const json&        params = ctx.params;
    const std::string& id     = ctx.id;

    // -------- emitters/preview-from-file ----------------------------
    //
    // actually load the .alo into a temporary ParticleSystem and
    // build the EmitterTreeNode tree. The temporary system drops at
    // scope exit. Note we wrap the real roots under a synthetic
    // `id: 0, name: "root"` node to match the MockBridge response
    // shape — the schema's EmitterTreeNode is single-rooted but a
    // ParticleSystem can have multiple root emitters.
    if (kind == "emitters/preview-from-file")
    {
        std::string path8 = params.value("path", std::string{});
        if (path8.empty())
        {
            // G3: intentional sendOk — file-load-result contract; the success
            // path returns ctx.SendOk({ok:true,tree}) and the Import Emitters
            // caller reads nested ok. Converting would split the contract.
            ctx.SendOk(json{{"ok", false}, {"error", "missing path"}});
            return true;
        }
        std::wstring path = Utf8ToWide(path8);
        std::string err;
        std::unique_ptr<ParticleSystem> tmp = LoadParticleSystem(path, &err);
        if (!tmp)
        {
            // G3: intentional sendOk — like file/open, a load failure here is
            // returned as nested-ok so request() won't throw; success path is
            // ctx.SendOk({ok:true,tree}). Caller inspects nested ok.
            ctx.SendOk(json{
                {"ok",    false},
                {"error", err.empty() ? std::string("could not load file") : err},
            });
            return true;
        }
        // Build the synthetic root + per-actual-root children.
        json children = json::array();
        const auto& emitters = tmp->getEmitters();
        for (size_t i = 0; i < emitters.size(); ++i)
        {
            if (emitters[i] != nullptr && emitters[i]->parent == nullptr)
            {
                children.push_back(BuildEmitterTreeNode(tmp.get(), i));
            }
        }
        // Synthetic root carries id 0 (legacy convention for preview)
        // but uses the new shape fields (role / linkGroup / visible)
        // so consumers don't see undefined when the schema is read
        // strictly.
        json tree = {
            {"id",        0},
            {"stableId",  0},  // synthetic root: 0 is reserved (real ids start at 1)
            {"name",      "root"},
            {"role",      "root"},
            {"linkGroup", 0},
            {"visible",   true},
            // Zero spawn: synthetic root never warns (= ZERO_SPAWN in
            // bridge-schema, the canonical zero shape).
            {"spawn", json{
                {"lifetime", 0.0}, {"useBursts", false}, {"nBursts", 0},
                {"burstDelay", 0.0}, {"nParticlesPerSecond", 0}, {"nParticlesPerBurst", 0},
            }},
            {"children",  children},
        };
        ctx.SendOk(json{{"ok", true}, {"tree", tree}});
        return true;
    }


    // -------- emitters/list -----------------------------------------
    //
    // Real implementation. Walks the live particle
    // system and returns a synthetic-root wrapper whose children are
    // the real top-level emitters. Returns an empty wrapper if no
    // system is bound (e.g. tests that haven't wired BindHostState).
    if (kind == "emitters/list")
    {
        json children = json::array();
        if (m_pParticleSystem != nullptr && *m_pParticleSystem)
        {
            const ParticleSystem* sys = m_pParticleSystem->get();
            const auto& emitters = sys->getEmitters();
            for (size_t i = 0; i < emitters.size(); ++i)
            {
                if (emitters[i] != nullptr && emitters[i]->parent == nullptr)
                {
                    children.push_back(BuildEmitterTreeNode(sys, i));
                }
            }
        }
        json tree = {
            {"id",        -1},
            {"stableId",  0},  // synthetic root: 0 is reserved (real ids start at 1)
            {"name",      ""},
            {"role",      "root"},
            {"linkGroup", 0},
            {"visible",   true},
            {"spawn", json{
                {"lifetime", 0.0}, {"useBursts", false}, {"nBursts", 0},
                {"burstDelay", 0.0}, {"nParticlesPerSecond", 0}, {"nParticlesPerBurst", 0},
            }},
            {"children",  children},
        };
        ctx.SendOk(json{{"root", tree}});
        return true;
    }


    // -------- emitters/select ---------------------------------------
    //
    // Selection state lives on the dispatcher (it's
    // editor state, not engine state). Update the scalar, emit the
    // narrow `emitters/selected` event (subscribed to by EmitterTree)
    // and a follow-up engine/state/changed so any snapshot consumer
    // sees the new selectedEmitterId.
    if (kind == "emitters/select")
    {
        // params.id is `number | null` on the wire. Null deserialises
        // to a JSON null; we store as -1 internally and re-serialise
        // as JSON null on the way out (in BuildEngineStateSnapshot).
        int newId = -1;
        if (params.contains("id") && !params["id"].is_null())
            newId = params["id"].get<int>();

        // Validate against the live tree when bound — selecting an
        // index that isn't a real emitter resets to no-selection. This
        // matches the MockBridge's behaviour and keeps the snapshot
        // honest. When no system is bound we accept the id as-is so
        // tests without BindHostState still round-trip.
        if (newId >= 0 && m_pParticleSystem != nullptr && *m_pParticleSystem)
        {
            const auto& emitters = (*m_pParticleSystem)->getEmitters();
            if (static_cast<size_t>(newId) >= emitters.size() || emitters[newId] == nullptr)
                newId = -1;
        }

        m_selectedEmitterId = newId;
        ctx.SendOk(json::object());

        // emitters/selected event — narrow payload for components that
        // care only about the selection scalar (EmitterTree).
        if (m_emit)
        {
            json env = {
                {"type",    "evt"},
                {"kind",    "emitters/selected"},
                {"payload", json{{"id", newId < 0 ? json(nullptr) : json(newId)}}},
            };
            m_emit(env.dump());
        }

        // engine/state/changed so the snapshot's selectedEmitterId is
        // observable through the standard snapshot channel too.
        EmitEngineStateChanged();
        return true;
    }


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


    // -------- emitter mutations -----------------
    //
    // Each handler validates the target emitter, captures a PRE-
    // mutation undo snapshot via captureUndo(), mutates via the
    // ParticleSystem API, then emits `emitters/tree/changed` + dirty.
    // The PRE-mutation timing pairs with undo/perform's head-of-
    // history auto-capture above (see lines ~1396) so Ctrl+Z restores
    // the state right before the mutation ran. Link-group sweeps
    // sit BETWEEN the mutation and the next captureUndo — covered by
    // the same snapshot atomically.

    // getEmitterById / captureUndo / propagateLinkGroup were lambdas defined
    // here; promoted to private members (defined above DispatchInternal) so
    // emitter/linkGroups handlers can move to a per-domain TU. Call sites
    // below are unchanged.

    // -------- emitters/get-properties ----
    //
    // Walks every editable Basic + Appearance + Physics field on the
    // named emitter and serialises into an EmitterPropertiesDto. The
    // `groups: GroupDto[]` field surfaces the 3 Group entries (NUM_GROUPS).
    // Unknown id / no system returns ok:false; the React panel
    // tolerates the failure by rendering the placeholder branch.
    if (kind == "emitters/get-properties")
    {
        int id = params.value("id", -1);
        const ParticleSystem::Emitter* emit = getEmitterById(id);
        if (emit == nullptr)
        {
            ctx.SendErr("emitter not found");
            return true;
        }

        // Helper: pack a Vec3 from three scalars.
        auto vec3 = [](float x, float y, float z) {
            return json::array({x, y, z});
        };

        json groupsArr = json::array();
        for (int g = 0; g < ParticleSystem::NUM_GROUPS; g++)
        {
            const auto& gr = emit->groups[g];
            groupsArr.push_back(json{
                {"type",            static_cast<int>(gr.type)},
                {"min",             vec3(gr.minX, gr.minY, gr.minZ)},
                {"max",             vec3(gr.maxX, gr.maxY, gr.maxZ)},
                {"sideLength",      gr.sideLength},
                {"sphereRadius",    gr.sphereRadius},
                {"sphereEdge",      static_cast<int>(gr.sphereEdge)},
                {"cylinderRadius",  gr.cylinderRadius},
                {"cylinderEdge",    static_cast<int>(gr.cylinderEdge)},
                {"cylinderHeight",  gr.cylinderHeight},
                {"val",             vec3(gr.valX, gr.valY, gr.valZ)},
            });
        }

        json props = {
            // ── Basic ───────────────────────────────────────────────
            {"name",                     emit->name},
            {"lifetime",                 emit->lifetime},
            {"initialDelay",             emit->initialDelay},
            {"useBursts",                emit->useBursts},
            {"nBursts",                  static_cast<int>(emit->nBursts)},
            {"burstDelay",               emit->burstDelay},
            {"nParticlesPerBurst",       static_cast<int>(emit->nParticlesPerBurst)},
            {"nParticlesPerSecond",      static_cast<int>(emit->nParticlesPerSecond)},
            {"randomLifetimePerc",       emit->randomLifetimePerc},
            {"randomScalePerc",          emit->randomScalePerc},
            {"randomRotation",           emit->randomRotation},
            {"randomRotationDirection",  emit->randomRotationDirection},
            {"randomRotationAverage",    emit->randomRotationAverage},
            {"randomRotationVariance",   emit->randomRotationVariance},
            {"freezeTime",               emit->freezeTime},
            {"skipTime",                 emit->skipTime},
            {"linkToSystem",             emit->linkToSystem},
            {"parentLinkStrength",       emit->parentLinkStrength},
            {"index",                    static_cast<int>(emit->index)},

            // ── Appearance ─────────────────────────────────────────
            {"colorTexture",             emit->colorTexture},
            {"normalTexture",            emit->normalTexture},
            {"blendMode",                static_cast<int>(emit->blendMode)},
            {"blendAlphaGated",          ParticleSystem::blendModeIsAlphaGated(static_cast<int>(emit->blendMode))},
            {"textureSize",              static_cast<int>(emit->textureSize)},
            {"nTriangles",               static_cast<int>(emit->nTriangles)},
            {"doColorAddGrayscale",      emit->doColorAddGrayscale},
            {"randomColors",             json::array({
                emit->randomColors[0], emit->randomColors[1],
                emit->randomColors[2], emit->randomColors[3],
            })},
            {"hasTail",                  emit->hasTail},
            {"tailSize",                 emit->tailSize},
            {"isHeatParticle",           emit->isHeatParticle},
            {"isWorldOriented",          emit->isWorldOriented},
            {"noDepthTest",              emit->noDepthTest},
            {"affectedByWind",           emit->affectedByWind},

            // ── Physics ────────────────────────────────────────────
            {"acceleration",             vec3(emit->acceleration[0],
                                              emit->acceleration[1],
                                              emit->acceleration[2])},
            {"gravity",                  emit->gravity},
            {"inwardSpeed",              emit->inwardSpeed},
            {"inwardAcceleration",       emit->inwardAcceleration},
            {"objectSpaceAcceleration",  emit->objectSpaceAcceleration},
            {"bounciness",               emit->bounciness},
            {"groundBehavior",           static_cast<int>(emit->groundBehavior)},
            {"emitFromMesh",             emit->emitFromMesh},
            {"emitFromMeshOffset",       emit->emitFromMeshOffset},
            {"isWeatherParticle",        emit->isWeatherParticle},
            {"weatherCubeSize",          emit->weatherCubeSize},
            {"weatherCubeDistance",      emit->weatherCubeDistance},
            {"weatherFadeoutDistance",   emit->weatherFadeoutDistance},

            {"groups",                   groupsArr},
        };
        ctx.SendOk(json{{"properties", props}});
        return true;
    }


    // -------- emitters/set-properties ----
    //
    // Batch patch: iterate over each key present in `patch` and apply
    // it directly to the target emitter's struct field. Captures undo
    // once, emits state/changed + tree/changed once, marks dirty once
    // per call regardless of how many fields the patch touched.
    //
    // Field type guards: nlohmann::json's `.value(key, fallback)` reads
    // through `get<T>`, which throws on type mismatch. Each branch uses
    // `is_*` checks before assignment so a stray null / wrong-type
    // field is a silent skip rather than a hard fault.
    if (kind == "emitters/set-properties")
    {
        int id = params.value("id", -1);
        ParticleSystem::Emitter* emit = getEmitterById(id);
        if (emit == nullptr)
        {
            ctx.SendErr("emitter not found");
            return true;
        }
        if (!params.contains("patch") || !params["patch"].is_object())
        {
            ctx.SendErr("missing patch");
            return true;
        }
        const json& patch = params["patch"];

        // Coalesce rapid edits to the SAME field(s) on the SAME emitter
        // (scroll-wheel ticks, held arrow) within the time window into one
        // undo step; switching field starts a fresh step. Finer than legacy's
        // per-emitter EP_CHANGE coalescing — a deliberate
        // design choice. Key layout: bit 31 set (never 0 = structural), bits
        // 16..30 an order-independent FNV-1a hash of the patch field names,
        // bits 0..15 the emitter id (so different emitters never fold).
        uint32_t fieldHash = 0;
        for (auto it = patch.begin(); it != patch.end(); ++it)
        {
            uint32_t h = 2166136261u; // FNV-1a offset basis
            for (unsigned char c : it.key()) { h ^= c; h *= 16777619u; }
            fieldHash ^= h; // XOR = order-independent across patch keys
        }
        const DWORD coalesceKey =
            0x80000000u | ((fieldHash & 0x7FFFu) << 16) | (static_cast<DWORD>(id) & 0xFFFFu);
        captureUndo(coalesceKey);

        // Helper macros — keep the per-field branch concise. Each
        // branch reads through `at()` only after a `contains()` check
        // so missing keys are a no-op.
        //
        // applied/skipped: the helpers classify each consumed key (type-ok →
        // applied, wrong type → skipped + fallback); the post-walk below adds
        // unconsumed keys (unknown field names) to skipped. The --record guard
        // (ClipRunner, pipeline spec §1.5) aborts on non-empty skipped so a
        // typo'd tutorial patch can't record a silent no-op as success. Live UI
        // callers ignore these extra response fields.
        json appliedArr = json::array();
        json skippedArr = json::array();
        auto getBool = [&](const char* key, bool fallback) -> bool {
            if (patch.contains(key) && patch.at(key).is_boolean()) { appliedArr.push_back(key); return patch.at(key).get<bool>(); }
            skippedArr.push_back(key);
            return fallback;
        };
        auto getFloat = [&](const char* key, float fallback) -> float {
            if (patch.contains(key) && patch.at(key).is_number()) { appliedArr.push_back(key); return patch.at(key).get<float>(); }
            skippedArr.push_back(key);
            return fallback;
        };
        auto getInt = [&](const char* key, int fallback) -> int {
            if (patch.contains(key) && patch.at(key).is_number_integer()) { appliedArr.push_back(key); return patch.at(key).get<int>(); }
            if (patch.contains(key) && patch.at(key).is_number()) { appliedArr.push_back(key); return static_cast<int>(patch.at(key).get<double>()); }
            skippedArr.push_back(key);
            return fallback;
        };
        auto getString = [&](const char* key, const std::string& fallback) -> std::string {
            if (patch.contains(key) && patch.at(key).is_string()) { appliedArr.push_back(key); return patch.at(key).get<std::string>(); }
            skippedArr.push_back(key);
            return fallback;
        };

        // ── Basic ───────────────────────────────────────────────────
        if (patch.contains("name"))                    emit->name = getString("name", emit->name);
        if (patch.contains("lifetime"))                emit->lifetime = getFloat("lifetime", emit->lifetime);
        if (patch.contains("initialDelay"))            emit->initialDelay = getFloat("initialDelay", emit->initialDelay);
        if (patch.contains("useBursts"))               emit->useBursts = getBool("useBursts", emit->useBursts);
        if (patch.contains("nBursts"))                 emit->nBursts = static_cast<unsigned long>(getInt("nBursts", static_cast<int>(emit->nBursts)));
        if (patch.contains("burstDelay"))              emit->burstDelay = getFloat("burstDelay", emit->burstDelay);
        if (patch.contains("nParticlesPerBurst"))      emit->nParticlesPerBurst = static_cast<unsigned long>(getInt("nParticlesPerBurst", static_cast<int>(emit->nParticlesPerBurst)));
        if (patch.contains("nParticlesPerSecond"))     emit->nParticlesPerSecond = static_cast<unsigned long>(getInt("nParticlesPerSecond", static_cast<int>(emit->nParticlesPerSecond)));
        if (patch.contains("randomLifetimePerc"))      emit->randomLifetimePerc = getFloat("randomLifetimePerc", emit->randomLifetimePerc);
        if (patch.contains("randomScalePerc"))         emit->randomScalePerc = getFloat("randomScalePerc", emit->randomScalePerc);
        if (patch.contains("randomRotation"))          emit->randomRotation = getBool("randomRotation", emit->randomRotation);
        if (patch.contains("randomRotationDirection")) emit->randomRotationDirection = getBool("randomRotationDirection", emit->randomRotationDirection);
        if (patch.contains("randomRotationAverage"))   emit->randomRotationAverage = getFloat("randomRotationAverage", emit->randomRotationAverage);
        if (patch.contains("randomRotationVariance"))  emit->randomRotationVariance = getFloat("randomRotationVariance", emit->randomRotationVariance);
        if (patch.contains("freezeTime"))              emit->freezeTime = getFloat("freezeTime", emit->freezeTime);
        if (patch.contains("skipTime"))                emit->skipTime = getFloat("skipTime", emit->skipTime);
        if (patch.contains("linkToSystem"))            emit->linkToSystem = getBool("linkToSystem", emit->linkToSystem);
        if (patch.contains("parentLinkStrength"))      emit->parentLinkStrength = getFloat("parentLinkStrength", emit->parentLinkStrength);
        if (patch.contains("index"))                   emit->index = static_cast<size_t>(getInt("index", static_cast<int>(emit->index)));

        // ── Appearance ─────────────────────────────────────────────
        if (patch.contains("colorTexture"))            emit->colorTexture = getString("colorTexture", emit->colorTexture);
        if (patch.contains("normalTexture"))           emit->normalTexture = getString("normalTexture", emit->normalTexture);
        if (patch.contains("blendMode"))               emit->blendMode = static_cast<unsigned long>(getInt("blendMode", static_cast<int>(emit->blendMode)));
        if (patch.contains("textureSize"))             emit->textureSize = static_cast<unsigned long>(getInt("textureSize", static_cast<int>(emit->textureSize)));
        if (patch.contains("nTriangles"))              emit->nTriangles = static_cast<unsigned long>(getInt("nTriangles", static_cast<int>(emit->nTriangles)));
        if (patch.contains("doColorAddGrayscale"))     emit->doColorAddGrayscale = getBool("doColorAddGrayscale", emit->doColorAddGrayscale);
        if (patch.contains("randomColors"))
        {
            const json& rc = patch.at("randomColors");
            // All-or-nothing: apply only if it's a 4-array of numbers. A wrong
            // shape or a non-numeric element leaves the field untouched and is
            // reported in `skipped` so the --record validator aborts rather than
            // recording a partial/garbage apply as success.
            bool ok = rc.is_array() && rc.size() == 4;
            for (size_t i = 0; ok && i < 4; i++) ok = rc[i].is_number();
            if (ok) { for (int i = 0; i < 4; i++) emit->randomColors[i] = rc[i].get<float>(); appliedArr.push_back("randomColors"); }
            else    { skippedArr.push_back("randomColors"); }
        }
        if (patch.contains("hasTail"))                 emit->hasTail = getBool("hasTail", emit->hasTail);
        if (patch.contains("tailSize"))                emit->tailSize = getFloat("tailSize", emit->tailSize);
        if (patch.contains("isHeatParticle"))          emit->isHeatParticle = getBool("isHeatParticle", emit->isHeatParticle);
        if (patch.contains("isWorldOriented"))         emit->isWorldOriented = getBool("isWorldOriented", emit->isWorldOriented);
        if (patch.contains("noDepthTest"))             emit->noDepthTest = getBool("noDepthTest", emit->noDepthTest);
        if (patch.contains("affectedByWind"))          emit->affectedByWind = getBool("affectedByWind", emit->affectedByWind);

        // ── Physics ────────────────────────────────────────────────
        if (patch.contains("acceleration"))
        {
            const json& ac = patch.at("acceleration");
            bool ok = ac.is_array() && ac.size() == 3;
            for (size_t i = 0; ok && i < 3; i++) ok = ac[i].is_number();
            if (ok) { for (int i = 0; i < 3; i++) emit->acceleration[i] = ac[i].get<float>(); appliedArr.push_back("acceleration"); }
            else    { skippedArr.push_back("acceleration"); }
        }
        if (patch.contains("gravity"))                 emit->gravity = getFloat("gravity", emit->gravity);
        if (patch.contains("inwardSpeed"))             emit->inwardSpeed = getFloat("inwardSpeed", emit->inwardSpeed);
        if (patch.contains("inwardAcceleration"))      emit->inwardAcceleration = getFloat("inwardAcceleration", emit->inwardAcceleration);
        if (patch.contains("objectSpaceAcceleration")) emit->objectSpaceAcceleration = getBool("objectSpaceAcceleration", emit->objectSpaceAcceleration);
        if (patch.contains("bounciness"))              emit->bounciness = getFloat("bounciness", emit->bounciness);
        if (patch.contains("groundBehavior"))          emit->groundBehavior = static_cast<unsigned long>(getInt("groundBehavior", static_cast<int>(emit->groundBehavior)));
        if (patch.contains("emitFromMesh"))            emit->emitFromMesh = getInt("emitFromMesh", emit->emitFromMesh);
        if (patch.contains("emitFromMeshOffset"))      emit->emitFromMeshOffset = getFloat("emitFromMeshOffset", emit->emitFromMeshOffset);
        if (patch.contains("isWeatherParticle"))       emit->isWeatherParticle = getBool("isWeatherParticle", emit->isWeatherParticle);
        if (patch.contains("weatherCubeSize"))         emit->weatherCubeSize = getFloat("weatherCubeSize", emit->weatherCubeSize);
        if (patch.contains("weatherCubeDistance"))     emit->weatherCubeDistance = getFloat("weatherCubeDistance", emit->weatherCubeDistance);
        if (patch.contains("weatherFadeoutDistance"))  emit->weatherFadeoutDistance = getFloat("weatherFadeoutDistance", emit->weatherFadeoutDistance);

        // ── Groups (NUM_GROUPS=3) ──────────────────────────────────
        if (patch.contains("groups") && patch.at("groups").is_array())
        {
            appliedArr.push_back("groups");
            const json& gs = patch.at("groups");
            const int n = std::min<int>(ParticleSystem::NUM_GROUPS,
                                        static_cast<int>(gs.size()));
            for (int gi = 0; gi < n; gi++)
            {
                const json& g = gs[gi];
                if (!g.is_object()) continue;
                auto& dst = emit->groups[gi];
                if (g.contains("type") && g.at("type").is_number_integer())
                    dst.type = static_cast<unsigned int>(g.at("type").get<int>());
                if (g.contains("min") && g.at("min").is_array() && g.at("min").size() == 3)
                {
                    dst.minX = g.at("min")[0].get<float>();
                    dst.minY = g.at("min")[1].get<float>();
                    dst.minZ = g.at("min")[2].get<float>();
                }
                if (g.contains("max") && g.at("max").is_array() && g.at("max").size() == 3)
                {
                    dst.maxX = g.at("max")[0].get<float>();
                    dst.maxY = g.at("max")[1].get<float>();
                    dst.maxZ = g.at("max")[2].get<float>();
                }
                if (g.contains("sideLength") && g.at("sideLength").is_number())
                    dst.sideLength = g.at("sideLength").get<float>();
                if (g.contains("sphereRadius") && g.at("sphereRadius").is_number())
                    dst.sphereRadius = g.at("sphereRadius").get<float>();
                if (g.contains("sphereEdge") && g.at("sphereEdge").is_number_integer())
                    dst.sphereEdge = static_cast<unsigned int>(g.at("sphereEdge").get<int>());
                if (g.contains("cylinderRadius") && g.at("cylinderRadius").is_number())
                    dst.cylinderRadius = g.at("cylinderRadius").get<float>();
                if (g.contains("cylinderEdge") && g.at("cylinderEdge").is_number_integer())
                    dst.cylinderEdge = static_cast<unsigned int>(g.at("cylinderEdge").get<int>());
                if (g.contains("cylinderHeight") && g.at("cylinderHeight").is_number())
                    dst.cylinderHeight = g.at("cylinderHeight").get<float>();
                if (g.contains("val") && g.at("val").is_array() && g.at("val").size() == 3)
                {
                    dst.valX = g.at("val")[0].get<float>();
                    dst.valY = g.at("val")[1].get<float>();
                    dst.valZ = g.at("val")[2].get<float>();
                }
            }
        }

        propagateLinkGroup(emit); // F4: keep link-group siblings in sync

        // Any patch key no branch consumed is an unknown field name → skipped.
        for (auto it = patch.begin(); it != patch.end(); ++it)
        {
            bool seen = false;
            for (const auto& k : appliedArr) if (k.get<std::string>() == it.key()) { seen = true; break; }
            if (!seen)
                for (const auto& k : skippedArr) if (k.get<std::string>() == it.key()) { seen = true; break; }
            if (!seen) skippedArr.push_back(it.key());
        }
        ctx.SendOk(json{{"applied", appliedArr}, {"skipped", skippedArr}});
        ctx.MarkDirty();
        // [D2] Property edits change particle appearance via fields read in
        // UpdateParticle (tailSize, linkToSystem, isHeatParticle, ...) but
        // don't flow through OnParticleSystemChanged — a paused preview
        // must still repaint this edit (review finding 1).
        if (m_engine) m_engine->InvalidatePausedIdleSkip();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return true;
    }


    // -------- emitters/duplicate -------------------------------------
    //
    // Mirrors legacy `EmitterList_DuplicateEmitter` at
    // [src/UI/EmitterList.cpp:4707]. Round-trips the source through
    // the chunk serializer so the duplicate starts with empty
    // m_instances (a direct copy-construct would shallow-copy that
    // std::set and double-free on later deletion). The duplicate
    // becomes a root via `insertEmitterAfter`.
    // -------- emitters/import-from-file (audit G1) ------------------
    //
    // Clone the `selected` source emitters from another `.alo` into the
    // live system as new roots, via the shared data-layer core
    // ParticleSystem::ImportEmittersFrom (the same logic the legacy import
    // dialog uses). Atomic single undo; emits the tree-changed event.
    // Placed after the captureUndo lambda (defined above) with the other
    // emitter-mutation handlers.
    if (kind == "emitters/import-from-file")
    {
        // Hard failures go through sendErr (envelope ok:false) so the bridge
        // promise REJECTS and the dialog's catch surfaces the error + keeps the
        // modal open. (A nested sendOk{ok:false} would resolve as success and
        // close the dialog silently — review finding, the G3 nested-ok trap.)
        if (!m_pParticleSystem || !*m_pParticleSystem)
        {
            ctx.SendErr("no particle system bound");
            return true;
        }
        std::string path8 = params.value("path", std::string{});
        if (path8.empty())
        {
            ctx.SendErr("missing path");
            return true;
        }
        std::vector<size_t> picks;
        if (params.contains("selected") && params["selected"].is_array())
        {
            for (const auto& v : params["selected"])
            {
                if (!v.is_number()) continue;
                long long n = v.get<long long>();
                if (n >= 0) picks.push_back(static_cast<size_t>(n));
            }
        }
        if (picks.empty())
        {
            ctx.SendErr("no emitters selected");
            return true;
        }
        std::wstring path = Utf8ToWide(path8);
        std::string err;
        std::unique_ptr<ParticleSystem> tmp = LoadParticleSystem(path, &err);
        if (!tmp)
        {
            ctx.SendErr(err.empty() ? std::string("could not load file") : err);
            return true;
        }
        // Drop out-of-range picks BEFORE capturing undo. An all-out-of-range
        // request imports nothing, so it must not push an undo snapshot or
        // dirty the document (audit-F1 'no dirty on a no-op'). In-range picks
        // keep their order.
        const size_t srcCount = tmp->getEmitters().size();
        std::vector<size_t> validPicks;
        for (size_t p : picks) if (p < srcCount) validPicks.push_back(p);
        if (validPicks.empty())
        {
            ctx.SendErr("no valid emitters to import");
            return true;
        }
        // Snapshot BEFORE mutating so the whole import is one undo unit; the
        // load + bound-check above ran first, so a failed/no-op import never
        // leaves a stray undo entry.
        captureUndo();
        ParticleSystem* sys = m_pParticleSystem->get();
        size_t n = sys->ImportEmittersFrom(
            *tmp, validPicks,
            [sys](const std::string& nm) { return GenerateDuplicateName(sys, nm); });
        ctx.SendOk(json{{"ok", true}, {"imported", static_cast<int>(n)}});
        // Only signal a mutation if something actually imported (n could be 0
        // only if every clone threw — a corrupt source that still loaded).
        if (n > 0)
        {
            ctx.MarkDirty();
            EmitEngineStateChanged();
            EmitEmittersTreeChanged();
        }
        return true;
    }

    if (kind == "emitters/duplicate")
    {
        int id = params.value("id", -1);
        ParticleSystem::Emitter* source = getEmitterById(id);
        if (source == nullptr)
        {
            // G3: intentional sendOk — handler success path returns
            // ctx.SendOk({ok:true,newId}); caller reads nested ok, so all
            // failures stay the same nested-ok shape to match.
            ctx.SendOk(json{{"ok", false}, {"error", "emitter not found"}});
            return true;
        }

        captureUndo();

        ParticleSystem* sys = m_pParticleSystem->get();
        ParticleSystem::Emitter* dup = nullptr;
        MemoryFile* memfile = new MemoryFile;
        try
        {
            ChunkWriter writer(memfile);
            source->copy(writer);

            memfile->seek(0);
            ChunkReader reader(memfile);
            ParticleSystem::Emitter cleanCopy(reader);

            // Auto-suffix the name to avoid collisions; mirrors the
            // legacy convention from EmitterList.cpp:4731.
            cleanCopy.name = GenerateDuplicateName(sys, source->name);

            dup = sys->insertEmitterAfter(source, cleanCopy);
        }
        catch (...)
        {
            memfile->Release();
            // G3: intentional sendOk — nested-ok failure to match the
            // success payload the caller inspects (see above).
            ctx.SendOk(json{{"ok", false}, {"error", "emitter copy failed"}});
            return true;
        }
        memfile->Release();

        if (dup == nullptr)
        {
            // G3: intentional sendOk — nested-ok failure to match the
            // success payload the caller inspects (see above).
            ctx.SendOk(json{{"ok", false}, {"error", "insertEmitterAfter returned null"}});
            return true;
        }
        const int newId = static_cast<int>(dup->index);
        ctx.SendOk(json{{"ok", true}, {"newId", newId}});
        ctx.MarkDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return true;
    }


    // -------- emitters/duplicate-many --------------------------------
    //
    // Batch duplicate: clone each selected emitter (the same single-emitter
    // copy emitters/duplicate does, looped). Returns `newIds` — the copies'
    // final indices, aligned to the input `ids` order — so the React side
    // re-selects the new copies. We keep the dup POINTERS and read their
    // ->index AFTER every insert, so the index shifts each insertEmitterAfter
    // causes don't corrupt the result.
    if (kind == "emitters/duplicate-many")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            // G3: intentional sendOk — handler success path returns
            // ctx.SendOk({ok:true,newIds}); caller reads nested ok, so all
            // failures stay the same nested-ok shape to match.
            ctx.SendOk(json{{"ok", false}, {"error", "no particle system bound"}});
            return true;
        }
        ParticleSystem* sys = m_pParticleSystem->get();

        std::vector<ParticleSystem::Emitter*> sources;
        if (params.contains("ids") && params["ids"].is_array())
        {
            for (const auto& j : params["ids"])
            {
                ParticleSystem::Emitter* e =
                    getEmitterById(j.is_number_integer() ? j.get<int>() : -1);
                if (e != nullptr) sources.push_back(e);
            }
        }
        if (sources.empty())
        {
            // G3: intentional sendOk — nested-ok failure to match the
            // success payload the caller inspects (see above).
            ctx.SendOk(json{{"ok", false}, {"error", "no emitters to duplicate"}});
            return true;
        }

        captureUndo();

        std::vector<ParticleSystem::Emitter*> dups;
        dups.reserve(sources.size());
        for (ParticleSystem::Emitter* src : sources)
        {
            ParticleSystem::Emitter* dup = nullptr;
            MemoryFile* memfile = new MemoryFile;
            try
            {
                ChunkWriter writer(memfile);
                src->copy(writer);
                memfile->seek(0);
                ChunkReader reader(memfile);
                ParticleSystem::Emitter cleanCopy(reader);
                cleanCopy.name = GenerateDuplicateName(sys, src->name);
                dup = sys->insertEmitterAfter(src, cleanCopy);
            }
            catch (...)
            {
                memfile->Release();
                // G3: intentional sendOk — nested-ok failure to match the
                // success payload the caller inspects (see above).
                ctx.SendOk(json{{"ok", false}, {"error", "emitter copy failed"}});
                return true;
            }
            memfile->Release();
            if (dup == nullptr)
            {
                // G3: intentional sendOk — nested-ok failure to match the
                // success payload the caller inspects (see above).
                ctx.SendOk(json{{"ok", false}, {"error", "insertEmitterAfter returned null"}});
                return true;
            }
            dups.push_back(dup);
        }

        json newIds = json::array();
        for (ParticleSystem::Emitter* dup : dups)
            newIds.push_back(static_cast<int>(dup->index));

        ctx.SendOk(json{{"ok", true}, {"newIds", newIds}});
        ctx.MarkDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return true;
    }


    // -------- emitters/delete ---------------------------------------
    //
    // Mirrors legacy `EmitterList_DeleteEmitter` at
    // [src/UI/EmitterList.cpp:4651]. ParticleSystem::deleteEmitter
    // recursively deletes a subtree. If the deleted id matches the
    // selection scalar, clear it and emit emitters/selected { id: null }.
    if (kind == "emitters/delete")
    {
        int id = params.value("id", -1);
        ParticleSystem::Emitter* target = getEmitterById(id);
        if (target == nullptr)
        {
            // No-op delete still returns success — matches the
            // mock's permissive behaviour.
            ctx.SendOk(json::object());
            return true;
        }

        captureUndo();

        ParticleSystem* sys = m_pParticleSystem->get();
        const bool wasSelected = (m_selectedEmitterId == id);
        sys->deleteEmitter(target);

        if (wasSelected)
        {
            m_selectedEmitterId = -1;
            if (m_emit)
            {
                json env = {
                    {"type",    "evt"},
                    {"kind",    "emitters/selected"},
                    {"payload", json{{"id", json(nullptr)}}},
                };
                m_emit(env.dump());
            }
        }

        // Demote any singleton groups left over from the
        // recursive subtree deletion (member of a 2-member group
        // deleted → survivor is now alone in the group → demote).
        // captureUndo() above covers both the deletion AND the
        // sweep atomically.
        EnforceSingleMemberLinkGroups();

        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return true;
    }


    // -------- emitters/rename ---------------------------------------
    //
    // Legacy uses an inline tree-view edit (EmitterList_RenameEmitter
    // at line 4814 → TreeView_EditLabel). The new-UI flow uses a modal,
    // dispatched here as a plain setName. Capture-undo guards against
    // mid-edit Ctrl-Z weirdness.
    if (kind == "emitters/rename")
    {
        int id = params.value("id", -1);
        std::string name = params.value("name", std::string{});
        ParticleSystem::Emitter* target = getEmitterById(id);
        if (target == nullptr)
        {
            ctx.SendErr("emitter not found");
            return true;
        }

        captureUndo();
        target->name = name;

        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEmittersTreeChanged();
        return true;
    }


    // -------- emitters/delete-track-keys + set-track-interpolation --
    //
    // Track mutations. Both handlers
    // resolve the emitter by id, look up the named track on `tracks[]`
    // (the slot pointer aliasing — see the comment block in
    // ParticleSystem.h:148), then mutate the underlying multiset /
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
            propagateLinkGroup(target); // F4: sync link-group siblings
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

        propagateLinkGroup(target); // F4: sync link-group siblings
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEmittersTreeChanged();
        EmitEngineStateChanged();
        return true;
    }


    // -------- emitters/set-track-lock ------------------------------
    //
    // Per-channel track lock. The legacy combo at
    // [TrackEditor.cpp:90-110](src/UI/TrackEditor.cpp:90) and the
    // file-load consolidation at [ParticleSystem.cpp:428] use the
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

        propagateLinkGroup(target); // F4: sync link-group siblings
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
        // (set-properties, this file) with trackIdx in place of the field
        // hash; bit 31 set so the key is never 0 (= structural / never-fold).
        const DWORD coalesceKey =
            0x80000000u | ((static_cast<DWORD>(trackIdx) & 0x7FFFu) << 16)
                        | (static_cast<DWORD>(id) & 0xFFFFu);
        captureUndo(coalesceKey);
        track->keys.erase(it);
        track->keys.insert(ParticleSystem::Emitter::Track::Key(newTime, newValue));

        propagateLinkGroup(target); // F4: sync link-group siblings
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

        propagateLinkGroup(target); // F4: sync link-group siblings
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


    // -------- emitters/duplicate-with-index-increment ---------------
    //
    // Legacy `EmitterList_DuplicateEmitter(hWnd, indexDelta)` at
    // [src/UI/EmitterList.cpp:4707]. Duplicate first (same path as
    // above), then shift the TRACK_INDEX track on the duplicate by
    // `delta` via `ShiftIndexTrack` (legacy helper at
    // [src/UI/EmitterList.cpp:2307]). The shift adds `delta` to every
    // keyframe value; if the track is empty, inserts a single key at
    // t=0 with value=delta.
    if (kind == "emitters/duplicate-with-index-increment")
    {
        int id = params.value("id", -1);
        float delta = params.value("delta", 0.0f);
        ParticleSystem::Emitter* source = getEmitterById(id);
        if (source == nullptr)
        {
            ctx.SendErr("emitter not found");
            return true;
        }

        captureUndo();

        ParticleSystem* sys = m_pParticleSystem->get();
        ParticleSystem::Emitter* dup = DuplicateEmitterWithIndexShift(sys, source, delta);
        if (dup == nullptr)
        {
            ctx.SendErr("emitter copy failed");
            return true;
        }

        const int newId = static_cast<int>(dup->index);
        ctx.SendOk(json{{"newId", newId}});
        ctx.MarkDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return true;
    }

    // -------- emitters/duplicate-with-index-increment-many ----------
    //
    // Batch of `count` CHAINED duplicates in ONE undo step: each copy is made
    // from the PREVIOUS copy (not the original source), so its index track
    // climbs by `delta` per step (source 0, delta 1, count 3 -> +1, +2, +3).
    // response.newIds are the copies in creation order. (#575)
    if (kind == "emitters/duplicate-with-index-increment-many")
    {
        int id = params.value("id", -1);
        float delta = params.value("delta", 0.0f);
        int count = params.value("count", 1);
        if (count < 1)   count = 1;
        if (count > 999) count = 999;   // match the dialog spinner's range

        ParticleSystem::Emitter* source = getEmitterById(id);
        if (source == nullptr)
        {
            ctx.SendErr("emitter not found");
            return true;
        }

        captureUndo();

        ParticleSystem* sys = m_pParticleSystem->get();
        std::vector<int> newIds;
        ParticleSystem::Emitter* cur = source;
        for (int i = 0; i < count; ++i)
        {
            ParticleSystem::Emitter* dup = DuplicateEmitterWithIndexShift(sys, cur, delta);
            if (dup == nullptr)
            {
                if (!newIds.empty())
                {
                    // A partial batch already mutated the tree; tell the web so
                    // it resyncs to the partial state (reversible in one undo via
                    // the single captureUndo above) instead of going stale.
                    ctx.MarkDirty();
                    EmitEngineStateChanged();
                    EmitEmittersTreeChanged();
                }
                ctx.SendErr("emitter copy failed");
                return true;
            }
            newIds.push_back(static_cast<int>(dup->index));
            cur = dup;   // chain: next copy is made from this one
        }

        ctx.SendOk(json{{"newIds", newIds}});
        ctx.MarkDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return true;
    }


    // -------- linkGroups/list-exempt-fields -------------------------
    //
    // Read the per-group LinkExemptFlags via
    // `ParticleSystem::getLinkExemptFlags`. Unknown groups return the
    // v1 default exempt set (handled inside getLinkExemptFlags).
    if (kind == "linkGroups/list-exempt-fields")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            ctx.SendErr("particle system not bound");
            return true;
        }
        uint32_t groupId =
            params.value("groupId", static_cast<uint32_t>(0));
        const LinkExemptFlags& flags =
            (*m_pParticleSystem)->getLinkExemptFlags(groupId);
        ctx.SendOk(json{{"fields", LinkExemptFlagsToJsonArray(flags)}});
        return true;
    }


    // -------- linkGroups/set-exempt-fields --------------------------
    //
    // Write the per-group exempt set. ParticleSystem normalises an
    // all-default value back out of the map (see
    // [src/ParticleSystem.h:351]); calling with the v1 default fields
    // therefore leaves the on-disk chunk untouched, matching legacy
    // save behaviour.
    if (kind == "linkGroups/set-exempt-fields")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            ctx.SendErr("particle system not bound");
            return true;
        }
        uint32_t groupId =
            params.value("groupId", static_cast<uint32_t>(0));
        const json& fieldsJson =
            params.contains("fields") ? params["fields"] : json::array();
        LinkExemptFlags flags = LinkExemptFlagsFromJsonArray(fieldsJson);

        ParticleSystem* sys = m_pParticleSystem->get();
        const LinkExemptFlags oldFlags = sys->getLinkExemptFlags(groupId);

        captureUndo();
        sys->setLinkExemptFlags(groupId, flags);

        // LNK settings surface — faithful to legacy settings-OK
        // (EmitterList.cpp:2841): when a field transitions exempt→shared and
        // members disagree, resolve it by copying the canonical (members[0],
        // first-in-tree-order) value to every sibling for exactly the
        // newly-shared fields (the diff mask). Only the newly-shared fields
        // are touched, so already-shared params are left as-is. captureUndo()
        // above already snapshotted, so flags + clobbered values fold into
        // ONE undo entry (matches legacy).
        bool resolved = false;
        std::vector<ParticleSystem::Emitter*> members =
            GetLinkGroupMembers(*sys, groupId);
        if (groupId != 0 && members.size() >= 2)
        {
            const LinkExemptFlags diffMask = MakeNewlySharedMask(oldFlags, flags);
            bool anyDisagree = false;
            for (size_t i = 1; i < members.size() && !anyDisagree; ++i)
                if (!DiffNonExemptParams(*members[i], *members[0], diffMask).empty())
                    anyDisagree = true;

            if (anyDisagree)
            {
                for (size_t i = 1; i < members.size(); ++i)
                    members[i]->copySharedParamsFrom(*members[0], diffMask);
                resolved = true;
            }
        }
        // copySharedParamsFrom reassigns each sibling's non-exempt
        // track multisets, orphaning live particles' cached cursor iterators.
        // Reseat every instance's cursors (the propagateLinkGroup chokepoint
        // pattern). Only fires when we actually copied.
        if (resolved && m_engine != nullptr)
            m_engine->OnParticleSystemChanged(-1);

        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEmittersTreeChanged();
        return true;
    }


    // -------- linkGroups/reset-exempt-fields ------------------------
    //
    // Reset = set to v1 defaults. ParticleSystem normalises that back
    // out of the map, so this effectively erases the per-group entry.
    if (kind == "linkGroups/reset-exempt-fields")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            ctx.SendErr("particle system not bound");
            return true;
        }
        uint32_t groupId =
            params.value("groupId", static_cast<uint32_t>(0));

        captureUndo();
        (*m_pParticleSystem)->setLinkExemptFlags(groupId, LinkExemptFlags{});

        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEmittersTreeChanged();
        return true;
    }


    // -------- linkGroups/diff-membership --------------------
    //
    // Read-only preview of a would-be join's non-exempt field
    // disagreements, so the UI can warn before set-membership silently
    // clobbers them. Mirrors the set-membership branches EXACTLY so the
    // warning lists precisely the fields that handler would overwrite:
    //   - groupId null/0 (leave): nothing is overwritten → no conflicts.
    //   - groupId  >  0 and the group EXISTS (join): canonical =
    //     members[0]; exempt = the group's flags; every target not
    //     already in the group is diffed against the canonical
    //     (matches JoinLinkGroup, which copies each joiner from members[0]
    //     under the group's exempt set).
    //   - groupId  >  0 but the group is empty, or groupId == -1 (new
    //     group): canonical = the first resolved target; exempt = v1
    //     defaults; the remaining targets are diffed against it (matches
    //     set-membership's create paths).
    // No mutation, no undo capture, no events fired.
    if (kind == "linkGroups/diff-membership")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            ctx.SendErr("particle system not bound");
            return true;
        }

        ParticleSystem* sys = m_pParticleSystem->get();

        int groupIdRaw = 0;
        if (params.contains("groupId") && !params["groupId"].is_null())
            groupIdRaw = params["groupId"].get<int>();

        const json& idsJson =
            params.contains("ids") ? params["ids"] : json::array();

        // Resolve (wire-id, emitter) pairs in caller order. The wire id
        // is echoed back in each conflict so the UI can attribute it.
        std::vector<std::pair<int, ParticleSystem::Emitter*>> targets;
        for (const auto& v : idsJson)
        {
            int id = v.get<int>();
            ParticleSystem::Emitter* e = getEmitterById(id);
            if (e != nullptr) targets.emplace_back(id, e);
        }

        // Determine the canonical member, the exempt set, and the joiners
        // to diff — exactly as set-membership would.
        ParticleSystem::Emitter* canonical = nullptr;
        const LinkExemptFlags*   exempt    = nullptr;
        std::vector<std::pair<int, ParticleSystem::Emitter*>> joiners;

        if (groupIdRaw > 0)
        {
            uint32_t target = static_cast<uint32_t>(groupIdRaw);
            std::vector<ParticleSystem::Emitter*> members =
                GetLinkGroupMembers(*sys, target);
            if (!members.empty())
            {
                canonical = members[0];
                exempt    = &sys->getLinkExemptFlags(target);
                for (const auto& t : targets)
                    if (t.second->linkGroup != target)
                        joiners.push_back(t);
            }
            else if (!targets.empty())
            {
                canonical = targets[0].second;
                exempt    = &GetDefaultLinkExemptFlags();
                for (size_t i = 1; i < targets.size(); ++i)
                    joiners.push_back(targets[i]);
            }
        }
        else if (groupIdRaw == -1 && !targets.empty())
        {
            canonical = targets[0].second;
            exempt    = &GetDefaultLinkExemptFlags();
            for (size_t i = 1; i < targets.size(); ++i)
                joiners.push_back(targets[i]);
        }
        // groupIdRaw == 0 (leave): canonical stays null → no conflicts.

        json conflicts = json::array();
        if (canonical != nullptr && exempt != nullptr)
        {
            for (const auto& j : joiners)
            {
                std::vector<std::string> fields =
                    DiffNonExemptParams(*j.second, *canonical, *exempt);
                if (!fields.empty())
                {
                    json entry;
                    entry["id"]     = j.first;
                    entry["fields"] = fields;
                    conflicts.push_back(entry);
                }
            }
        }

        ctx.SendOk(json{{"conflicts", conflicts}});
        return true;
    }


    // -------- linkGroups/diff-exempt-change (LNK settings surface) ---
    //
    // Read-only preview for the settings dialog: which existing members the
    // PROPOSED exempt set would overwrite to the canonical (members[0],
    // first-in-tree-order) value when a now-exempt field becomes SHARED.
    // Mirrors the legacy settings-OK disagreement scan (EmitterList.cpp:2841):
    // only fields transitioning exempt(stored)→shared(proposed) count, diffed
    // per non-canonical member. set-exempt-fields resolves it on commit.
    // No mutation, no undo, no events.
    if (kind == "linkGroups/diff-exempt-change")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            ctx.SendErr("particle system not bound");
            return true;
        }
        ParticleSystem* sys = m_pParticleSystem->get();

        uint32_t groupId = params.value("groupId", static_cast<uint32_t>(0));
        const json& exemptJson =
            params.contains("exempt") ? params["exempt"] : json::array();
        const LinkExemptFlags proposed = LinkExemptFlagsFromJsonArray(exemptJson);

        json conflicts = json::array();
        std::vector<ParticleSystem::Emitter*> members =
            GetLinkGroupMembers(*sys, groupId);
        if (groupId != 0 && members.size() >= 2)
        {
            const LinkExemptFlags  oldFlags = sys->getLinkExemptFlags(groupId);
            const LinkExemptFlags  diffMask = MakeNewlySharedMask(oldFlags, proposed);

            const auto& allEmitters = sys->getEmitters();
            auto wireIdOf = [&](ParticleSystem::Emitter* e) -> int {
                for (size_t i = 0; i < allEmitters.size(); ++i)
                    if (allEmitters[i] == e) return static_cast<int>(i);
                return -1;
            };

            ParticleSystem::Emitter* canonical = members[0];
            for (size_t i = 1; i < members.size(); ++i)
            {
                std::vector<std::string> fields =
                    DiffNonExemptParams(*members[i], *canonical, diffMask);
                if (!fields.empty())
                {
                    json entry;
                    entry["id"]     = wireIdOf(members[i]);
                    entry["fields"] = fields;
                    conflicts.push_back(entry);
                }
            }
        }

        ctx.SendOk(json{{"conflicts", conflicts}});
        return true;
    }


    // -------- add child / move / link-group memb -

    // -------- emitters/add-lifetime-child ---------------------------
    //
    // Wraps `ParticleSystem::addLifetimeEmitter(parent, Emitter())`.
    // The engine refuses (returns NULL) when the parent's lifetime
    // slot is already filled — surface that as `newId: -1`. Otherwise
    // return the new emitter's index in getEmitters().
    if (kind == "emitters/add-lifetime-child")
    {
        int parentId = params.value("parentId", -1);
        ParticleSystem::Emitter* parent = getEmitterById(parentId);
        if (parent == nullptr || m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            ctx.SendOk(json{{"newId", -1}});
            return true;
        }
        captureUndo();
        ParticleSystem::Emitter* child =
            (*m_pParticleSystem)->addLifetimeEmitter(parent);
        if (child == nullptr)
        {
            ctx.SendOk(json{{"newId", -1}});
            return true;
        }
        ctx.SendOk(json{{"newId", static_cast<int>(child->index)}});
        ctx.MarkDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return true;
    }


    // -------- emitters/add-root --------------------------------------
    //
    // Wraps `ParticleSystem::addRootEmitter()`
    // for the new top-level Emitters → New Emitter → Root menu item.
    // The engine always succeeds (no max-roots cap); the only failure
    // path is a missing particle-system pointer, surfaced as
    // `newId: -1` for parity with the add-child handlers.
    if (kind == "emitters/add-root")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            ctx.SendOk(json{{"newId", -1}});
            return true;
        }
        captureUndo();
        ParticleSystem::Emitter* child =
            (*m_pParticleSystem)->addRootEmitter();
        if (child == nullptr)
        {
            ctx.SendOk(json{{"newId", -1}});
            return true;
        }
        ctx.SendOk(json{{"newId", static_cast<int>(child->index)}});
        ctx.MarkDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return true;
    }


    // -------- emitters/add-death-child -------------------------------
    if (kind == "emitters/add-death-child")
    {
        int parentId = params.value("parentId", -1);
        ParticleSystem::Emitter* parent = getEmitterById(parentId);
        if (parent == nullptr || m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            ctx.SendOk(json{{"newId", -1}});
            return true;
        }
        captureUndo();
        ParticleSystem::Emitter* child =
            (*m_pParticleSystem)->addDeathEmitter(parent);
        if (child == nullptr)
        {
            ctx.SendOk(json{{"newId", -1}});
            return true;
        }
        ctx.SendOk(json{{"newId", static_cast<int>(child->index)}});
        ctx.MarkDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return true;
    }


    // -------- emitters/move ------------------------------------------
    //
    // Reorder the emitter among its siblings. Wraps
    // `ParticleSystem::moveEmitter(emitter, direction)`. The engine
    // already enforces "root-only" and "no-op at the edges" — a
    // refused move returns false and is a silent no-op here (the React
    // side disables the menu item at the edges; reaching this path with
    // a refusal is defensive). Always sends `{}` to match the schema.
    if (kind == "emitters/move")
    {
        int id = params.value("id", -1);
        std::string dirStr = params.value("direction", std::string{"up"});
        int dir = (dirStr == "down") ? +1 : -1;
        ParticleSystem::Emitter* target = getEmitterById(id);
        if (target == nullptr || m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            ctx.SendOk(json::object());
            return true;
        }
        captureUndo();
        const bool moved = (*m_pParticleSystem)->moveEmitter(target, dir);
        ctx.SendOk(json::object());
        if (moved)
        {
            ctx.MarkDirty();
            EmitEngineStateChanged();
            EmitEmittersTreeChanged();
        }
        return true;
    }


    // -------- emitters/move-many -------------------------------------
    //
    // Batch reorder: move the selected ROOT emitters up/down by one as a UNIT
    // (non-root selections are no-ops — moveEmitter is root-only). Order is
    // preserved: if the edge-most selected root is pinned at the edge, NOTHING
    // moves (the block doesn't deform by compacting trailing members past the
    // non-selected roots). Returns `newIds` (targets' final indices,
    // input-order-aligned) read from the stable pointers afterwards, so the
    // React selection follows the reorder.
    if (kind == "emitters/move-many")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            ctx.SendOk(json{{"newIds", json::array()}});
            return true;
        }
        const std::string dirStr = params.value("direction", std::string{"up"});
        const int dir = (dirStr == "down") ? +1 : -1;

        std::vector<ParticleSystem::Emitter*> targets;
        if (params.contains("ids") && params["ids"].is_array())
        {
            for (const auto& j : params["ids"])
            {
                ParticleSystem::Emitter* e =
                    getEmitterById(j.is_number_integer() ? j.get<int>() : -1);
                if (e != nullptr) targets.push_back(e);
            }
        }
        if (targets.empty())
        {
            ctx.SendOk(json{{"newIds", json::array()}});
            return true;
        }

        ParticleSystem* sys = m_pParticleSystem->get();
        auto isSel = [&](ParticleSystem::Emitter* e) {
            return std::find(targets.begin(), targets.end(), e) != targets.end();
        };

        // Roots in vector order.
        std::vector<ParticleSystem::Emitter*> roots;
        for (ParticleSystem::Emitter* e : sys->getEmitters())
            if (e != nullptr && e->parent == nullptr) roots.push_back(e);

        // Preserve order: the selection moves as a UNIT, or not at all. If the
        // edge-most root in the move direction is selected, the block is pinned
        // against the edge and NOTHING moves — rather than letting the trailing
        // members compact past the non-selected roots (order matters in a
        // particle system). Otherwise every selected root shifts by one,
        // processed ascending (up) / descending (down) so each one's neighbour
        // is already an unselected root when it swaps.
        std::vector<ParticleSystem::Emitter*> movable;
        const bool edgePinned =
            !roots.empty() && isSel(dir == -1 ? roots.front() : roots.back());
        if (!edgePinned)
        {
            if (dir == -1)
            {
                for (size_t i = 0; i < roots.size(); ++i)
                    if (isSel(roots[i])) movable.push_back(roots[i]);   // ascending
            }
            else
            {
                for (size_t i = roots.size(); i-- > 0; )
                    if (isSel(roots[i])) movable.push_back(roots[i]);   // descending
            }
        }

        if (!movable.empty()) captureUndo();
        bool anyMoved = false;
        for (ParticleSystem::Emitter* e : movable)
            if (sys->moveEmitter(e, dir)) anyMoved = true;

        json newIds = json::array();
        for (ParticleSystem::Emitter* e : targets)
            newIds.push_back(static_cast<int>(e->index));

        ctx.SendOk(json{{"newIds", newIds}});
        if (anyMoved)
        {
            ctx.MarkDirty();
            EmitEngineStateChanged();
            EmitEmittersTreeChanged();
        }
        return true;
    }


    // -------- emitters/set-visible -----------------------------------
    //
    // Per-emitter visibility toggle for the EmitterTree
    // panel toolbar's [👁] button. Sets `Emitter::visible` for the
    // target only — children are untouched. `visible` is editor-only
    // state (not persisted to the .alo file), so this handler does NOT
    // markDirty. Still emits tree-changed + state-changed so the engine
    // re-renders and any open inspector reflects the new flag.
    if (kind == "emitters/set-visible")
    {
        int  id      = params.value("id", -1);
        bool visible = params.value("visible", true);
        ParticleSystem::Emitter* target = getEmitterById(id);
        if (target == nullptr)
        {
            ctx.SendOk(json::object());
            return true;
        }
        target->visible = visible;
        ctx.SendOk(json::object());
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return true;
    }


    // -------- emitters/set-all-visible -------------------------------
    //
    // Bulk Show All / Hide All from the EmitterTree
    // panel toolbar. Walks the entire emitter array (the engine stores
    // all emitters flat with parent pointers — no recursion needed)
    // and sets `visible` uniformly. Same editor-only semantic as
    // set-visible above; no markDirty.
    if (kind == "emitters/set-all-visible")
    {
        bool visible = params.value("visible", true);
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            ctx.SendOk(json::object());
            return true;
        }
        const auto& emitters = (*m_pParticleSystem)->getEmitters();
        for (ParticleSystem::Emitter* e : emitters)
        {
            if (e != nullptr) e->visible = visible;
        }
        ctx.SendOk(json::object());
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return true;
    }


    // -------- linkGroups/set-membership ------------------------------
    //
    // Assign each emitter in `ids` to a link group:
    //   - groupId === null OR === 0 → leave the group
    //   - groupId  >  0             → join that existing group
    //   - groupId === -1            → create a new group
    //
    // F4: this drives the LinkGroup.h API (Create/Join/Leave) rather
    // than stamping `e->linkGroup` raw. That stamp set the membership ID
    // (so the bracket gutter drew) but NEVER synchronised the members'
    // non-exempt fields, so the group had no behavioural effect — the
    // root cause of "link groups don't work". Create/Join overwrite each
    // member's non-exempt params from the canonical member (first in
    // tree order for a new group; the group's canonical member for a
    // join), and Leave auto-dissolves a group left with one member.
    // Members already in a different group are detached first so the
    // operation always succeeds (CreateLinkGroup refuses if any member
    // is still grouped). The pre-mutation captureUndo() snapshots the
    // whole system, so one Ctrl+Z restores the prior membership AND the
    // pre-sync field values.
    if (kind == "linkGroups/set-membership")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            ctx.SendErr("particle system not bound");
            return true;
        }
        const json& idsJson =
            params.contains("ids") ? params["ids"] : json::array();
        // `groupId` may be a JSON number or null. Absent/null → 0 (leave).
        int groupIdRaw = 0;
        if (params.contains("groupId") && !params["groupId"].is_null())
        {
            groupIdRaw = params["groupId"].get<int>();
        }

        ParticleSystem* sys = m_pParticleSystem->get();

        // Resolve the target emitters once.
        std::vector<ParticleSystem::Emitter*> targets;
        for (const auto& v : idsJson)
        {
            ParticleSystem::Emitter* e = getEmitterById(v.get<int>());
            if (e != nullptr) targets.push_back(e);
        }

        captureUndo();

        if (groupIdRaw == 0)
        {
            // Leave / unlink.
            for (size_t i = 0; i < targets.size(); ++i)
                LeaveLinkGroup(*sys, targets[i]);
        }
        else if (groupIdRaw > 0)
        {
            // Explicit positive id. If the group already exists, JOIN each
            // target to it (joiners adopt the group's canonical values).
            // If it does NOT exist yet, CREATE it with this caller-chosen
            // id — `JoinLinkGroup` refuses a non-existent group, and the
            // contract (and the legacy stamp) is "assign to that group,
            // creating it if needed". The real dialog only sends positive
            // ids for existing groups, but bridge callers (and tests) rely
            // on create-if-needed.
            uint32_t target = static_cast<uint32_t>(groupIdRaw);
            const bool exists = !GetLinkGroupMembers(*sys, target).empty();
            // Detach any target currently in a *different* group first.
            for (size_t i = 0; i < targets.size(); ++i)
            {
                if (targets[i]->linkGroup != 0 && targets[i]->linkGroup != target)
                    LeaveLinkGroup(*sys, targets[i]);
            }
            if (exists)
            {
                for (size_t i = 0; i < targets.size(); ++i)
                    if (targets[i]->linkGroup != target)
                        JoinLinkGroup(*sys, targets[i], target);
            }
            else if (!targets.empty())
            {
                // New group with an explicit id: first target is canonical;
                // the rest sync their non-exempt params to it (mirrors
                // CreateLinkGroup, which only allocates max+1 ids).
                const LinkExemptFlags& exempt = sys->getLinkExemptFlags(target);
                targets[0]->linkGroup = target;
                for (size_t i = 1; i < targets.size(); ++i)
                {
                    targets[i]->copySharedParamsFrom(*targets[0], exempt);
                    targets[i]->linkGroup = target;
                }
            }
        }
        else // groupIdRaw == -1 : new group
        {
            for (size_t i = 0; i < targets.size(); ++i)
                if (targets[i]->linkGroup != 0)
                    LeaveLinkGroup(*sys, targets[i]);
            // Minimum group size is 2; a 1-id "new group" is a no-op.
            if (targets.size() >= 2)
                CreateLinkGroup(*sys, targets);
        }

        // Idempotent safety net — the API already preserves the
        // "no singleton groups" invariant, but a defensive sweep keeps
        // any future caller honest.
        EnforceSingleMemberLinkGroups();

        // Join/Create call `copySharedParamsFrom`, which REPLACES
        // each joining member's non-exempt track multisets with copies from
        // the canonical member. Any live particle of those members holds
        // cached cursor iterators into the OLD containers — now orphaned —
        // and the next Engine::Update would dereference a dangling iterator
        // (the xtree:181 "value-initialized iterator" assert). The legacy
        // key-edit handlers reseat per-track; a membership change can touch
        // EVERY non-exempt track on MULTIPLE members, so reseat all cursors
        // for all instances (-1). Cheap (re-finds cursors) and idempotent.
        if (m_engine != nullptr)
            m_engine->OnParticleSystemChanged(-1);

        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return true;
    }


    // -------- emitters/drop ----------------------
    //
    // Drag-and-drop reorder + reparent. Tagged-union on params.mode:
    //   - "reorder":  wraps `ParticleSystem::moveEmitterToRootIndex`.
    //                 `rootIndex` is the gap index in the rendered root
    //                 list (gap K = "land before position K"). The
    //                 engine refuses non-root sources, out-of-range
    //                 gaps, and no-op gaps (sourceIdx and sourceIdx+1)
    //                 by returning false; surface that as
    //                 `{ ok: false, error: "reorder refused" }`.
    //   - "reparent": wraps `ParticleSystem::reparentEmitter`. The
    //                 engine itself checks cycle, same-parent, and
    //                 slot-full — refusal returns false; surface as
    //                 `{ ok: false, error: "reparent refused" }`.
    //
    // React side resolves slot before calling (auto-pick: both free →
    // "lifetime"; only one free → that one; both filled → no bridge
    // call). The wire shape never carries "auto".
    if (kind == "emitters/drop")
    {
        // G3: every failure in this handler is an intentional sendOk —
        // the success path returns ctx.SendOk({ok:true}) and the EmitterTree
        // caller dispatches drops as `void bridge.request(...)`
        // (fire-and-forget). Converting to sendErr would both split this
        // handler's nested-ok contract and turn the fire-and-forget calls
        // into unhandled promise rejections. Out of scope to fix on the JS
        // side, so all failures stay nested-ok.
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            ctx.SendOk(json{{"ok", false}, {"error", "particle system not bound"}});
            return true;
        }
        std::string mode = params.value("mode", std::string{});
        int id = params.value("id", -1);
        ParticleSystem::Emitter* source = getEmitterById(id);
        if (source == nullptr)
        {
            ctx.SendOk(json{{"ok", false}, {"error", "source emitter not found"}});
            return true;
        }
        // After a successful drop the dragged emitter's positional index has
        // changed; re-select it (by its post-op index) so the highlight FOLLOWS
        // the moved emitter rather than sticking on the slot it left behind
        // (which now holds a different emitter). The web side leans on the
        // emitters/selected event this emits. `source` is the same Emitter
        // object across the move, so its new index is its position in the
        // (now-reordered) flat emitter vector.
        auto reselectMovedEmitter = [&]() {
            const auto& es = (*m_pParticleSystem)->getEmitters();
            for (size_t i = 0; i < es.size(); ++i)
            {
                if (es[i] == source)
                {
                    m_selectedEmitterId = static_cast<int>(i);
                    // emitters/selected event — same narrow payload the
                    // emitters/select handler emits; EmitterTree syncs primary.
                    if (m_emit)
                    {
                        json env = {
                            {"type",    "evt"},
                            {"kind",    "emitters/selected"},
                            {"payload", json{{"id", json(m_selectedEmitterId)}}},
                        };
                        m_emit(env.dump());
                    }
                    break;
                }
            }
        };
        captureUndo();
        if (mode == "reorder")
        {
            int rootIndex = params.value("rootIndex", -1);
            if (rootIndex < 0)
            {
                ctx.SendOk(json{{"ok", false}, {"error", "invalid rootIndex"}});
                return true;
            }
            const bool ok = (*m_pParticleSystem)->moveEmitterToRootIndex(
                source, static_cast<size_t>(rootIndex));
            if (!ok)
            {
                ctx.SendOk(json{{"ok", false}, {"error", "reorder refused"}});
                return true;
            }
            ctx.SendOk(json{{"ok", true}});
            ctx.MarkDirty();
            reselectMovedEmitter();
            EmitEngineStateChanged();
            EmitEmittersTreeChanged();
            return true;
        }
        if (mode == "reparent")
        {
            int targetId = params.value("targetId", -1);
            std::string slot = params.value("slot", std::string{"lifetime"});
            ParticleSystem::Emitter* target = getEmitterById(targetId);
            if (target == nullptr)
            {
                ctx.SendOk(json{{"ok", false}, {"error", "target emitter not found"}});
                return true;
            }
            const bool useDuringLife = (slot != "death");
            const bool ok = (*m_pParticleSystem)->reparentEmitter(
                source, target, useDuringLife);
            if (!ok)
            {
                ctx.SendOk(json{{"ok", false}, {"error", "reparent refused"}});
                return true;
            }
            ctx.SendOk(json{{"ok", true}});
            ctx.MarkDirty();
            reselectMovedEmitter();
            EmitEngineStateChanged();
            EmitEmittersTreeChanged();
            return true;
        }
        ctx.SendOk(json{{"ok", false}, {"error", "unknown mode"}});
        return true;
    }


    // -------- emitters/reorder-many (multi-select drag-reorder) ------
    //
    // Batch absolute-position root reorder. Moves the selected ROOT
    // emitters (params.ids, positional) to land contiguous at gap
    // params.rootIndex, preserving tree order; non-contiguous selections
    // collapse. Wraps ParticleSystem::reorderManyRootsToIndex, which refuses
    // out-of-range / non-root / empty / own-footprint no-op. Returns the
    // moved roots' final indices as newIds (a contiguous run).
    if (kind == "emitters/reorder-many")
    {
        // G3: every failure in this handler is an intentional sendOk — the
        // success path returns ctx.SendOk({ok:true,newIds}) and the JS caller
        // (lib/emitter-reorder.ts reorderManyEmitters) reads nested ok as
        // control flow: `const r = await request(...); if (!r.ok) return;`.
        // Converting to sendErr would make request() throw, defeating that
        // guard. Out of scope to fix on the JS side, so failures stay nested-ok.
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            ctx.SendOk(json{{"ok", false}, {"error", "particle system not bound"}});
            return true;
        }
        int rootIndex = params.value("rootIndex", -1);
        if (rootIndex < 0)
        {
            ctx.SendOk(json{{"ok", false}, {"error", "invalid rootIndex"}});
            return true;
        }
        std::vector<ParticleSystem::Emitter*> selection;
        if (params.contains("ids") && params["ids"].is_array())
        {
            for (const auto& j : params["ids"])
            {
                ParticleSystem::Emitter* e =
                    getEmitterById(j.is_number_integer() ? j.get<int>() : -1);
                if (e == nullptr)
                {
                    ctx.SendOk(json{{"ok", false}, {"error", "emitter not found"}});
                    return true;
                }
                if (e->parent != nullptr)
                {
                    ctx.SendOk(json{{"ok", false}, {"error", "non-root in selection"}});
                    return true;
                }
                selection.push_back(e);
            }
        }
        if (selection.empty())
        {
            ctx.SendOk(json{{"ok", false}, {"error", "empty selection"}});
            return true;
        }
        captureUndo();
        std::vector<size_t> outNewIds;
        const bool ok = (*m_pParticleSystem)->reorderManyRootsToIndex(
            selection, static_cast<size_t>(rootIndex), outNewIds);
        if (!ok)
        {
            ctx.SendOk(json{{"ok", false}, {"error", "reorder refused"}});
            return true;
        }
        json newIds = json::array();
        for (size_t v : outNewIds) newIds.push_back(static_cast<int>(v));
        ctx.SendOk(json{{"ok", true}, {"newIds", newIds}});
        ctx.MarkDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return true;
    }


    // -------- emitters/copy / cut / paste --------
    //
    // Process-local clipboard. We reuse the existing import-from-
    // file serialise pattern: per emitter, allocate a MemoryFile, wrap
    // it with a ChunkWriter, call `Emitter::copy(writer)` (which is
    // `write(writer, true)` — preserves identity-less form), then
    // snapshot the bytes into a `std::vector<uint8_t>`. Paste reverses
    // the round-trip via `Emitter(ChunkReader&)`. One buffer per copied
    // subtree so each can be deserialised independently (multi-id paste
    // produces multiple new roots).
    //
    // Cut = copy + delete. Single undo capture at the start, single
    // tree-changed at the end — the user sees one atomic step in undo.
    // Descending-id delete order keeps lower indices valid through the
    // loop (deleteEmitter shifts everything above the deleted index
    // down by one).
    if (kind == "emitters/copy" || kind == "emitters/cut")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            ctx.SendOk(json::object());
            return true;
        }
        // Pull the id list from params.ids (an array of numbers).
        std::vector<int> ids;
        if (params.contains("ids") && params["ids"].is_array())
        {
            for (const auto& v : params["ids"])
            {
                if (v.is_number_integer()) ids.push_back(v.get<int>());
            }
        }
        // Clear the clipboard before refilling — every copy/cut
        // replaces the entire contents.
        m_emitterClipboard.clear();
        for (int id : ids)
        {
            ParticleSystem::Emitter* source = getEmitterById(id);
            if (source == nullptr) continue;
            MemoryFile* memfile = new MemoryFile;
            try
            {
                ChunkWriter writer(memfile);
                source->copy(writer);
                std::vector<uint8_t> buf(memfile->size());
                memfile->seek(0);
                if (!buf.empty())
                {
                    memfile->read(buf.data(), static_cast<unsigned long>(buf.size()));
                }
                m_emitterClipboard.push_back(std::move(buf));
            }
            catch (...)
            {
                // Best-effort: skip this id and continue with the rest.
            }
            memfile->Release();
        }
        if (kind == "emitters/copy")
        {
            // Read-only — no undo, no dirty, no tree-changed.
            ctx.SendOk(json::object());
            return true;
        }

        // ---- cut: delete the originals atomically ----
        captureUndo();
        // Sort ids descending so the iteration is robust against any
        // mid-loop index reshuffling. We also re-resolve each id via
        // getEmitterById inside the loop because the legacy
        // `deleteEmitter` shifts subsequent slots down, invalidating
        // raw pointers across calls.
        std::sort(ids.begin(), ids.end(), std::greater<int>());
        ParticleSystem* sys = m_pParticleSystem->get();
        bool clearedSelection = false;
        for (int id : ids)
        {
            ParticleSystem::Emitter* target = getEmitterById(id);
            if (target == nullptr) continue;
            if (m_selectedEmitterId == id) clearedSelection = true;
            sys->deleteEmitter(target);
        }
        if (clearedSelection)
        {
            m_selectedEmitterId = -1;
            if (m_emit)
            {
                json env = {
                    {"type",    "evt"},
                    {"kind",    "emitters/selected"},
                    {"payload", json{{"id", json(nullptr)}}},
                };
                m_emit(env.dump());
            }
        }
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return true;
    }
    if (kind == "emitters/paste")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            ctx.SendOk(json{{"newIds", json::array()}});
            return true;
        }
        if (m_emitterClipboard.empty())
        {
            // Nothing to paste — silent no-op, no dirty, no undo.
            ctx.SendOk(json{{"newIds", json::array()}});
            return true;
        }
        // Optional `afterId` — the root the paste should land directly
        // after. When omitted or not a current root, paste at the end
        // of the root list.
        int afterId = -1;
        if (params.contains("afterId") && !params["afterId"].is_null())
        {
            afterId = params.value("afterId", -1);
        }
        captureUndo();
        ParticleSystem* sys = m_pParticleSystem->get();
        json newIds = json::array();
        ParticleSystem::Emitter* prevAnchor = (afterId >= 0)
            ? getEmitterById(afterId)
            : nullptr;
        // Track failure separately so a partial paste still emits one
        // tree-changed and returns the ids that *did* land.
        for (auto& buf : m_emitterClipboard)
        {
            if (buf.empty()) continue;
            MemoryFile* memfile = new MemoryFile;
            ParticleSystem::Emitter* pasted = nullptr;
            try
            {
                memfile->write(buf.data(),
                               static_cast<unsigned long>(buf.size()));
                memfile->seek(0);
                ChunkReader reader(memfile);
                ParticleSystem::Emitter staging(reader);
                staging.name = GenerateDuplicateName(sys, staging.name);
                if (prevAnchor != nullptr)
                {
                    pasted = sys->insertEmitterAfter(prevAnchor, staging);
                }
                else
                {
                    pasted = sys->addRootEmitter(staging);
                }
            }
            catch (...)
            {
                // Skip this entry; continue with the rest.
            }
            memfile->Release();
            if (pasted != nullptr)
            {
                newIds.push_back(static_cast<int>(pasted->index));
                // Chain subsequent pastes after this one so multi-id
                // paste keeps clipboard order.
                prevAnchor = pasted;
            }
        }
        ctx.SendOk(json{{"newIds", newIds}});
        if (!newIds.empty())
        {
            ctx.MarkDirty();
            EmitEngineStateChanged();
            EmitEmittersTreeChanged();
        }
        return true;
    }

    // -------- emitters/paste-as-child (legacy Paste As ▸) -----------
    //
    // Deserialise the FIRST clipboard buffer and attach it into the
    // parent's lifetime or death child slot — the splice of the
    // emitters/paste deser (above) and the add-lifetime/death-child
    // attach. `addLifetimeEmitter`/`addDeathEmitter` self-guard (return
    // NULL) when the slot is already filled, so a stale menu can't
    // double-occupy. One emitter per slot: a multi-buffer clipboard
    // pastes only buffer[0] (matches legacy's single-blob clipboard).
    if (kind == "emitters/paste-as-child")
    {
        int parentId = params.value("parentId", -1);
        std::string slot = params.value("slot", std::string());
        ParticleSystem::Emitter* parent = getEmitterById(parentId);
        if (parent == nullptr || m_pParticleSystem == nullptr || !*m_pParticleSystem
            || m_emitterClipboard.empty() || m_emitterClipboard.front().empty())
        {
            ctx.SendOk(json{{"newId", -1}});
            return true;
        }
        captureUndo();
        ParticleSystem* sys = m_pParticleSystem->get();
        ParticleSystem::Emitter* child = nullptr;
        MemoryFile* memfile = new MemoryFile;
        try
        {
            auto& buf = m_emitterClipboard.front();
            memfile->write(buf.data(), static_cast<unsigned long>(buf.size()));
            memfile->seek(0);
            ChunkReader reader(memfile);
            ParticleSystem::Emitter staging(reader);
            staging.name = GenerateDuplicateName(sys, staging.name);
            child = (slot == "death")
                ? sys->addDeathEmitter(parent, staging)
                : sys->addLifetimeEmitter(parent, staging);
        }
        catch (...)
        {
            // Deser failed — fall through to the null-child refusal.
        }
        memfile->Release();
        if (child == nullptr)
        {
            // Slot occupied or deser threw. captureUndo already ran —
            // parity with add-lifetime-child, which also captures before
            // this null-check.
            ctx.SendOk(json{{"newId", -1}});
            return true;
        }
        ctx.SendOk(json{{"newId", static_cast<int>(child->index)}});
        ctx.MarkDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return true;
    }


    return false;   // kind not in this domain
}

} // namespace host
