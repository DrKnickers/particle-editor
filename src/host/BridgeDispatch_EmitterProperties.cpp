// Bridge request handlers for the emitters/* property kinds.
// BridgeDispatcher::DispatchInternal routes requests here via TryDispatchEmitterProperties.

#include "BridgeDispatcher.h"
#include "BridgeDispatchShared.h"
#include "BridgeRequestContext.h"

#include <algorithm>              // std::min (group patch)
#include <cfloat>                 // FLT_MAX (bounciness range check)
#include <climits>                // INT_MIN / INT_MAX (getInt range check)
#include <cmath>                  // std::isfinite / std::fabs

using nlohmann::json;

namespace host {

bool BridgeDispatcher::TryDispatchEmitterProperties(BridgeRequestContext& ctx)
{
    // Short local names for the request fields used by the handlers below.
    const json&        params = ctx.params;
    const std::string& kind   = ctx.kind;

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
            if (patch.contains(key) && patch.at(key).is_number())
            {
                // A double outside int's range (or NaN) makes the cast undefined;
                // report it as skipped, like a wrong-typed value.
                const double d = patch.at(key).get<double>();
                if (d >= INT_MIN && d <= INT_MAX) { appliedArr.push_back(key); return static_cast<int>(d); }
            }
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
        if (patch.contains("bounciness"))
        {
            // Values outside [0,1] are allowed (the Physics tab doesn't clamp),
            // but a non-finite one would poison the GROUND_BOUNCE simulation and
            // the saved file. Skip it like a wrong-typed value.
            const json& b = patch.at("bounciness");
            if (!b.is_number() || (std::isfinite(b.get<double>()) && std::fabs(b.get<double>()) <= FLT_MAX))
                emit->bounciness = getFloat("bounciness", emit->bounciness);
            else
                skippedArr.push_back("bounciness");
        }
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

        propagateLinkGroup(emit); // keep link-group siblings in sync

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
        // Broadcast the definition change to every LIVE instance. Composite
        // values (m_spawnDelay from nParticlesPerSecond, burst counts,
        // acceleration/gravity, texture bindings) are cached per instance and
        // only recomputed in onParticleSystemChanged(track==-1) — without this
        // call an already-placed instance (Shift+click spawn) kept its
        // creation-time spawn rate forever while the auto-respawning main
        // preview read fresh values, masking the gap (v0.3.0 cold-launch
        // finding). Subsumes the previous InvalidatePausedIdleSkip() call:
        // OnParticleSystemChanged also busts the paused-idle skip, so a
        // paused preview still repaints the edit (review finding).
        // m_spawnDelay is a PERIOD (next round schedules time+delay), so a
        // mid-flight recompute is safe; it also deliberately resets the
        // overload-guard backoff, which is correct after an intentional edit.
        if (m_engine) m_engine->OnParticleSystemChanged(-1);
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return true;
    }


    return false;   // kind not in this domain
}

} // namespace host
