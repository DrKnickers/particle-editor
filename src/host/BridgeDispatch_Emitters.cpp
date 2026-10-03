// Bridge request handlers for the emitters/* tree and selection kinds.
// BridgeDispatcher::DispatchInternal routes requests here via TryDispatchEmitters.

#include "BridgeDispatcher.h"
#include "BridgeDispatchShared.h"
#include "BridgeRequestContext.h"

#include "StringConv.h"           // host::Utf8ToWide / WideToUtf8
#include "effect/ParticleSystemIO.h"  // LoadParticleSystem (preview/import-from-file)

using nlohmann::json;

namespace host {

// Duplicate `source` in `sys` (deep copy via the chunk writer/reader, with a
// fresh auto-suffixed name) and shift every TRACK_INDEX keyframe on the copy by
// `delta` — inserting a single t=0 key = delta when the source track is empty.
// Mirrors legacy EmitterList_DuplicateEmitter + ShiftIndexTrack. Returns the new
// emitter, or nullptr if the copy/insert failed. Shared by the single and the
// batch duplicate-with-index-increment handlers.
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

bool BridgeDispatcher::TryDispatchEmitters(BridgeRequestContext& ctx)
{
    // Short local names for the request fields used by the handlers below.
    const json&        params = ctx.params;
    const std::string& kind   = ctx.kind;

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
            // Nested-ok result (see BridgeRequestContext.h); the success
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
            // Like file/open, a load failure here is returned as a nested-ok
            // result so request() won't throw; success path is
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
            m_emit(env);
        }

        // engine/state/changed so the snapshot's selectedEmitterId is
        // observable through the standard snapshot channel too.
        EmitEngineStateChanged();
        return true;
    }


    // -------- emitters/import-from-file ------------------
    //
    // Clone the `selected` source emitters from another `.alo` into the
    // live system as new roots, via the shared data-layer core
    // ParticleSystem::ImportEmittersFrom (the same logic the legacy import
    // dialog uses). Atomic single undo; emits the tree-changed event.
    // Uses the shared captureUndo member for a single undo step.
    if (kind == "emitters/import-from-file")
    {
        // Hard failures go through sendErr (envelope ok:false) so the bridge
        // promise REJECTS and the dialog's catch surfaces the error + keeps the
        // modal open. (A nested sendOk{ok:false} would resolve as success and
        // close the dialog silently; see BridgeRequestContext.h.)
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
        // dirty the document (no dirty on a no-op). In-range picks
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
            // Structural change: reach already-placed instances.
            if (m_engine) m_engine->OnParticleSystemChanged(-1);
            EmitEngineStateChanged();
            EmitEmittersTreeChanged();
        }
        return true;
    }

    // -------- emitters/duplicate -------------------------------------
    //
    // Mirrors the legacy Win32 editor's `EmitterList_DuplicateEmitter`.
    // Round-trips the source through
    // the chunk serializer so the duplicate starts with empty
    // m_instances (a direct copy-construct would shallow-copy that
    // std::set and double-free on later deletion). The duplicate
    // becomes a root via `insertEmitterAfter`.
    if (kind == "emitters/duplicate")
    {
        int id = params.value("id", -1);
        ParticleSystem::Emitter* source = getEmitterById(id);
        if (source == nullptr)
        {
            // Nested-ok result (see BridgeRequestContext.h): the success path
            // returns ctx.SendOk({ok:true,newId}); caller reads nested ok, so all
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
            // legacy editor's convention (GenerateDuplicateName).
            cleanCopy.name = GenerateDuplicateName(sys, source->name);

            dup = sys->insertEmitterAfter(source, cleanCopy);
        }
        catch (...)
        {
            memfile->Release();
            // Nested-ok failure, matching the success payload the caller
            // inspects (see above).
            ctx.SendOk(json{{"ok", false}, {"error", "emitter copy failed"}});
            return true;
        }
        memfile->Release();

        if (dup == nullptr)
        {
            // Nested-ok failure, matching the success payload the caller
            // inspects (see above).
            ctx.SendOk(json{{"ok", false}, {"error", "insertEmitterAfter returned null"}});
            return true;
        }
        const int newId = static_cast<int>(dup->index);
        ctx.SendOk(json{{"ok", true}, {"newId", newId}});
        ctx.MarkDirty();
        // Structural change: reach already-placed instances.
        if (m_engine) m_engine->OnParticleSystemChanged(-1);
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
            // Nested-ok result (see BridgeRequestContext.h): the success path
            // returns ctx.SendOk({ok:true,newIds}); caller reads nested ok, so all
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
            // Nested-ok failure, matching the success payload the caller
            // inspects (see above).
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
                // Nested-ok failure, matching the success payload the caller
                // inspects (see above).
                ctx.SendOk(json{{"ok", false}, {"error", "emitter copy failed"}});
                return true;
            }
            memfile->Release();
            if (dup == nullptr)
            {
                // Nested-ok failure, matching the success payload the caller
                // inspects (see above).
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
        // Structural change: reach already-placed instances.
        if (m_engine) m_engine->OnParticleSystemChanged(-1);
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return true;
    }


    // -------- emitters/delete ---------------------------------------
    //
    // Mirrors the legacy Win32 editor's `EmitterList_DeleteEmitter`.
    // ParticleSystem::deleteEmitter
    // recursively deletes a subtree. Preserve a surviving selection by stable
    // identity because every positional id above the deletion can shift.
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
        const unsigned int selectedStableId = selectedEmitterStableId();
        sys->deleteEmitter(target);
        reconcileSelectionAfterDeletion(selectedStableId);

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


    // -------- emitters/delete-many ----------------------------------
    //
    // The multi-root delete gesture. Structurally the delete half of the
    // cut handler below: ONE captureUndo() around the whole loop, so a
    // single Ctrl+Z reverses the whole gesture. React used to issue N
    // separate emitters/delete requests, each capturing its own undo
    // entry, so one Ctrl+Z restored one emitter out of N.
    //
    // An emitter id is a POSITION that shifts down as earlier siblings
    // vanish, so — exactly as in cut — sort descending and re-resolve
    // each id via getEmitterById INSIDE the loop rather than caching raw
    // pointers across deleteEmitter calls.
    if (kind == "emitters/delete-many")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            ctx.SendOk(json::object());
            return true;
        }
        std::vector<int> ids;
        if (params.contains("ids") && params["ids"].is_array())
        {
            for (const auto& v : params["ids"])
            {
                if (v.is_number_integer()) ids.push_back(v.get<int>());
            }
        }
        if (ids.empty())
        {
            // Nothing asked for — no undo, no dirty, no events.
            ctx.SendOk(json::object());
            return true;
        }

        // Capture BEFORE any erase. Capturing late cannot restore a
        // half-deleted tree if the loop aborts; the wasted snapshot when
        // no id resolves is the same trade delete-track-keys makes.
        captureUndo();

        std::sort(ids.begin(), ids.end(), std::greater<int>());
        ParticleSystem* sys = m_pParticleSystem->get();
        const unsigned int selectedStableId = selectedEmitterStableId();
        int deleted = 0;
        for (int id : ids)
        {
            ParticleSystem::Emitter* target = getEmitterById(id);
            if (target == nullptr) continue;
            sys->deleteEmitter(target);
            deleted++;
        }

        if (deleted > 0)
            reconcileSelectionAfterDeletion(selectedStableId);
        ctx.SendOk(json::object());
        if (deleted > 0)
        {
            // The same post-delete sweep the single-delete handler runs;
            // the captureUndo() above covers the deletions AND the sweep
            // as one atomic step.
            EnforceSingleMemberLinkGroups();
            ctx.MarkDirty();
            EmitEngineStateChanged();
            EmitEmittersTreeChanged();
        }
        return true;
    }


    // -------- emitters/rename ---------------------------------------
    //
    // The legacy editor used an inline tree-view edit (EmitterList_RenameEmitter
    // → TreeView_EditLabel). The editor UI uses a modal,
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


    // -------- emitters/duplicate-with-index-increment ---------------
    //
    // Legacy `EmitterList_DuplicateEmitter(hWnd, indexDelta)`. Duplicate
    // first (same path as above), then shift the TRACK_INDEX track on the
    // duplicate by `delta` (the legacy editor's `ShiftIndexTrack`
    // behaviour). The shift adds `delta` to every
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
        // Structural change: reach already-placed instances.
        if (m_engine) m_engine->OnParticleSystemChanged(-1);
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return true;
    }

    // -------- emitters/duplicate-with-index-increment-many ----------
    //
    // Batch of `count` CHAINED duplicates in ONE undo step: each copy is made
    // from the PREVIOUS copy (not the original source), so its index track
    // climbs by `delta` per step (source 0, delta 1, count 3 -> +1, +2, +3).
    // response.newIds are the copies in creation order.
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
                    // Structural change: reach already-placed instances.
                    if (m_engine) m_engine->OnParticleSystemChanged(-1);
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
        // Structural change: reach already-placed instances.
        if (m_engine) m_engine->OnParticleSystemChanged(-1);
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
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
        // Structural change: reach already-placed instances.
        if (m_engine) m_engine->OnParticleSystemChanged(-1);
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
        // Structural change: reach already-placed instances.
        if (m_engine) m_engine->OnParticleSystemChanged(-1);
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
        // Structural change: reach already-placed instances.
        if (m_engine) m_engine->OnParticleSystemChanged(-1);
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
        // Every failure in this handler is a nested-ok result (see
        // BridgeRequestContext.h): the success path returns
        // ctx.SendOk({ok:true}) and the EmitterTree
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
                        m_emit(env);
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
            // Structural change: reach already-placed instances.
            if (m_engine) m_engine->OnParticleSystemChanged(-1);
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
        // Every failure in this handler is a nested-ok result (see
        // BridgeRequestContext.h): the success path returns
        // ctx.SendOk({ok:true,newIds}) and the JS caller
        // (lib/tree/emitter-reorder.ts reorderManyEmitters) reads nested ok as
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


    return false;   // kind not in this domain
}

} // namespace host
