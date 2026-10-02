// Bridge request handlers for the emitters/* clipboard kinds.
// BridgeDispatcher::DispatchInternal routes requests here via TryDispatchEmitterClipboard.

#include "BridgeDispatcher.h"
#include "BridgeDispatchShared.h"
#include "BridgeRequestContext.h"

#include <algorithm>              // std::sort (cut delete order)
#include <utility>                // std::move (clipboard buffers)

using nlohmann::json;

namespace host {

bool BridgeDispatcher::TryDispatchEmitterClipboard(BridgeRequestContext& ctx)
{
    // Short local names for the request fields used by the handlers below.
    const json&        params = ctx.params;
    const std::string& kind   = ctx.kind;

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
            // Structural change: reach already-placed instances.
            if (m_engine) m_engine->OnParticleSystemChanged(-1);
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
        // Structural change: reach already-placed instances.
        if (m_engine) m_engine->OnParticleSystemChanged(-1);
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return true;
    }


    return false;   // kind not in this domain
}

} // namespace host
