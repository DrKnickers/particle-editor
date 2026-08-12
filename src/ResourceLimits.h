#ifndef RESOURCE_LIMITS_H
#define RESOURCE_LIMITS_H

#include <cstdint>

static const unsigned long kMaxXmlFileBytes      = 64u * 1024u * 1024u; // 64 MiB
// Element-nesting cap for untrusted mod XML. Real game XML nests ~6-10 deep; a
// crafted file nesting tens of thousands deep would otherwise build an
// arbitrarily tall XMLNode chain during the startup catalog prefetch (the
// recursion risk is in teardown, but the cap stops the tree existing at all).
static const unsigned long kMaxXmlDepth          = 512u;
// Total ELEMENT count for one document — the breadth companion to the depth cap
// above (2026-07 audit). Depth alone leaves a shallow-but-enormous document
// unbounded, and kMaxXmlFileBytes does not stand in for it: every element
// becomes an XMLNode carrying a child vector and an attribute map, so a 64 MiB
// file of `<E/>` amplifies into something far larger than 64 MiB of heap. Real
// game XML runs to thousands of elements; the largest stock catalogs are well
// under 100k.
static const unsigned long kMaxXmlNodes          = 2u * 1000u * 1000u;  // 2M elements
// Total ATTRIBUTE PAIRS for one document (2026-07 audit). A shallow
// element can carry a large Expat [name,value,...] array while paying for only
// one XMLNode; XMLNode then copies every pair into a std::map, amplifying each
// short on-disk attribute into map-node and string allocations. The approved
// resource budget is document-wide, not per element; a local seven-root corpus
// snapshot observed a maximum of 61,000 pairs.
static const unsigned long kMaxXmlAttributes     = 131072u;
static const unsigned long kMaxCatalogXmlFileCount = 4096u;
static const unsigned long kMaxCatalogXmlTotalBytes = kMaxXmlFileBytes;
// Accepted unique model-bearing picker entries for ONE skydome axis/load.
// The shared harvester aggregates across every routed file, so this bounds the
// published prefix without rejecting an otherwise readable manifest.
static const unsigned long kMaxSkydomeEntriesPerAxis = 1024u;
// Total fieldable-token PROCESSING work for one game-object catalog build
// (2026-07 audit). This counts temporary token-vector pushes, roster /
// build / spawn map applications, member-expansion pushes, and expansion replay.
// Repeated or inherited lists consume work again even when their final names
// deduplicate in the field-source map. The approved ceiling is aggregate across
// the build; a local seven-profile sample observed a maximum of 58,750
// operations. Exceeding the approved ceiling rejects rather than truncates.
static const unsigned long kMaxCatalogFieldableTokenWork = 131072u;
static const unsigned long kMaxMegNameTableBytes = 16u * 1024u * 1024u; // 16 MiB
static const unsigned long kMaxMegEntryCount     = 1u  * 1024u * 1024u; // 1M entries
static const uint16_t      kMaxFilenameLength    = 32768;               // 32 KiB
// Absolute caps for ALO chunk parsing (release-audit #13: "absolute maximum chunk
// and string sizes for ALO parsing") — a belt over the parent-relative bounds so a
// pathologically large file can't drive a huge allocation off a single chunk header.
static const unsigned long kMaxAloChunkBytes     = 256u * 1024u * 1024u; // 256 MiB
static const unsigned long kMaxAloStringBytes    = 16u  * 1024u * 1024u; // 16 MiB
static const unsigned long kMaxAloBones          = 4096u;
static const unsigned long kMaxAloConnections    = 4096u;
// Emitter-count cap for a particle system read. The 0x0700 read loop is
// otherwise bounded only by the file size, so a crafted .alo packed with tiny
// emitter chunks amplifies each ~8-byte header into a full Emitter allocation.
// Real systems have dozens of emitters; heavy modded ones a few hundred.
static const unsigned long kMaxAloEmitters       = 65536u;
// Spawn-chain DEPTH cap — the arrangement companion to the count cap above
// (2026-07 audit). kMaxAloEmitters bounds how MANY emitters a file may
// carry and says nothing about how they are wired: 65,536 emitters are harmless
// as a forest and fatal as a single chain, because ValidateEmitterGraph enforces
// single-parent and breaks cycles but never bounded depth. The graph is walked
// RECURSIVELY by BuildEmitterTreeNode (the bridge tree serializer, run on every
// open) and by deleteEmitter, so a deep-but-valid chain overflows the stack on a
// file the loader accepts. Real systems nest in the single digits — the in-game
// chain investigation used depth 3, and the engine dies of particle
// multiplication long before anything approaching this cap — so 256 is generous
// by two orders of magnitude while keeping the worst-case recursion far inside a
// 1 MiB thread stack.
static const unsigned long kMaxEmitterTreeDepth  = 256u;
// The rest of the aggregate caps the 2026-07 audit found missing. Each of these
// loops validated every ITEM it read and never the TOTAL, so a file of millions
// of individually-valid records passed every existing check while allocating
// without bound. Same defect shape in four places; the sibling loops in the very
// same files (bones, connections, emitters above) already had their caps.
//
// Top-level 0x0400 mesh containers in one .alo (2026-07 audit). Each is an
// emplace_back into model.meshes regardless of payload, so empty containers cost
// a full Mesh apiece. Real models have a handful; heavily-composed ones dozens.
static const unsigned long kMaxAloMeshes         = 4096u;
// Material containers across ALL meshes in one model (2026-07 audit). Each
// 0x10100 owns an AloSubMesh even if no geometry follows, so the top-level mesh
// cap does not bound this nested fan-out.
static const unsigned long kMaxAloSubMeshesTotal = 4096u;
// Recognized shader-parameter leaves across ALL materials in one model
// (2026-07 audit). Count raw 0x10102..0x10106 leaves, including duplicate names.
static const unsigned long kMaxAloShaderParamsTotal = 32768u;
// Keys in ONE emitter track (2026-07 audit). Track::KeyMap is a std::multiset, so
// every key is a separate red-black-tree node — far more than the 8 bytes it
// occupies on disk. Bounded previously only by the enclosing chunk, i.e. up to
// ~32M keys under kMaxAloChunkBytes. A hand-authored curve has tens of keys.
static const unsigned long kMaxAloTrackKeys      = 65536u;
// Link-exempt entries in one 0x0003 chunk (2026-07 audit). The count was bounded
// only by the bytes remaining in the chunk — at ~3 bytes per entry that is still
// tens of millions of map inserts. Groups are per-particle-system and few.
static const unsigned long kMaxAloLinkExempts    = 65536u;
// Raw link-exempt records across ALL sibling 0x0003 chunks. Repeated chunks are
// accepted for forward compatibility, so the per-chunk limit alone does not
// bound total parsing work. Count records before filtering zero/default entries
// or overwriting duplicate group IDs.
static const unsigned long kMaxAloLinkExemptRecordsTotal = 65536u;
// Roster names taken from one GameObjectList.lua (2026-07 audit). The reader already
// refuses a file over kMaxXmlFileBytes, but a byte cap is not a count cap: the
// `["NAME"]` scan is ~6 bytes per entry, so 64 MiB of `["a"]["b"]...` is ~10M
// std::set<std::string> inserts — each a red-black node plus a heap allocation.
// Same shape as the other aggregate-cap findings: every ITEM is checked, the TOTAL never is.
// Real rosters run a few hundred to ~900 entries (see readRosterLua), so 65536
// is ~70x the largest observed and can only ever bite content that is already
// pathological. Duplicates cannot inflate the set, so the cap is on distinct
// names — which is exactly the memory being bounded.
static const unsigned long kMaxRosterEntries     = 65536u;
// Inbound WebMessage cap, in UTF-16 CHARACTERS (2026-07 audit). Bridge
// ingress had no size limit: every message was handed to OnWebMessage and parsed
// whole, so one postMessage could drive an arbitrarily large allocation on the UI
// thread. Defence-in-depth rather than a live hole -- the origin check upstream
// means the only speaker is our own bundle -- but an unbounded parse behind a
// trust boundary should not be the thing keeping the window alive. Real bridge
// requests are a few KB; the largest (a multi-key curve paste) is far under a
// megabyte, so 16 Mi characters is generous by four orders of magnitude.
static const size_t        kMaxWebMessageChars  = 16u * 1024u * 1024u;
// Absolute caps for whole-file asset reads off a safe-but-adversarial name (#415):
// the #4 fail-closed gates block unsafe *names*, but a safe relative name can still
// resolve to a very large loose or MEG-backed asset, and the whole-file read then
// allocates from the raw file size. Check size() against the relevant cap before the
// read. Generous — real textures/models sit far below these; the ALO cap mirrors the
// per-chunk kMaxAloChunkBytes ceiling.
static const unsigned long kMaxTextureAssetBytes = 64u  * 1024u * 1024u; // 64 MiB
// Whole-file cap for a single ALO model read, intentionally set at (not above) the
// per-chunk kMaxAloChunkBytes ceiling: a real model sits far below either, so a
// whole file larger than one max chunk is treated as adversarial and rejected.
static const unsigned long kMaxAloModelBytes     = 256u * 1024u * 1024u; // 256 MiB
static const unsigned long kMaxShaderAssetBytes  = 16u  * 1024u * 1024u; // 16 MiB (compiled .fxo/.fx are tiny; generous)

#endif
