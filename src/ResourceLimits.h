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
// above (2026-07 audit, an-audit-finding). Depth alone leaves a shallow-but-enormous document
// unbounded, and kMaxXmlFileBytes does not stand in for it: every element
// becomes an XMLNode carrying a child vector and an attribute map, so a 64 MiB
// file of `<E/>` amplifies into something far larger than 64 MiB of heap. Real
// game XML runs to thousands of elements; the largest stock catalogs are well
// under 100k.
static const unsigned long kMaxXmlNodes          = 2u * 1000u * 1000u;  // 2M elements
static const unsigned long kMaxCatalogXmlFileCount = 4096u;
static const unsigned long kMaxCatalogXmlTotalBytes = kMaxXmlFileBytes;
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
// The rest of the aggregate caps the 2026-07 audit found missing. Each of these
// loops validated every ITEM it read and never the TOTAL, so a file of millions
// of individually-valid records passed every existing check while allocating
// without bound. Same defect shape in four places; the sibling loops in the very
// same files (bones, connections, emitters above) already had their caps.
//
// Top-level 0x0400 mesh containers in one .alo (audit an-audit-finding). Each is an
// emplace_back into model.meshes regardless of payload, so empty containers cost
// a full Mesh apiece. Real models have a handful; heavily-composed ones dozens.
static const unsigned long kMaxAloMeshes         = 4096u;
// Keys in ONE emitter track (audit an-audit-finding). Track::KeyMap is a std::multiset, so
// every key is a separate red-black-tree node — far more than the 8 bytes it
// occupies on disk. Bounded previously only by the enclosing chunk, i.e. up to
// ~32M keys under kMaxAloChunkBytes. A hand-authored curve has tens of keys.
static const unsigned long kMaxAloTrackKeys      = 65536u;
// Link-exempt entries in one 0x0003 chunk (audit an-audit-finding). The count was bounded
// only by the bytes remaining in the chunk — at ~3 bytes per entry that is still
// tens of millions of map inserts. Groups are per-particle-system and few.
static const unsigned long kMaxAloLinkExempts    = 65536u;
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
