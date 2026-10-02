// Bridge request handlers for the linkGroups/* kinds.
// BridgeDispatcher::DispatchInternal routes requests here via TryDispatchLinkGroups.

#include "BridgeDispatcher.h"
#include "BridgeDispatchShared.h"
#include "BridgeRequestContext.h"

using nlohmann::json;

namespace host {

bool BridgeDispatcher::TryDispatchLinkGroups(BridgeRequestContext& ctx)
{
    // Short local names for the request fields used by the handlers below.
    const json&        params = ctx.params;
    const std::string& kind   = ctx.kind;

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
    // ParticleSystem::setLinkExemptFlags); calling with the v1 default fields
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

        // LNK settings surface — faithful to the legacy editor's
        // settings-OK handler: when a field transitions exempt→shared and
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
    // Mirrors the legacy editor's settings-OK disagreement scan:
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


    // -------- linkGroups/set-membership ------------------------------
    //
    // Assign each emitter in `ids` to a link group:
    //   - groupId === null OR === 0 → leave the group
    //   - groupId  >  0             → join that existing group
    //   - groupId === -1            → create a new group
    //
    // This drives the LinkGroup.h API (Create/Join/Leave) rather
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


    return false;   // kind not in this domain
}

} // namespace host
