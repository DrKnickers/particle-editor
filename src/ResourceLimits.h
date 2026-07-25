#ifndef RESOURCE_LIMITS_H
#define RESOURCE_LIMITS_H

#include <cstdint>

static const unsigned long kMaxXmlFileBytes      = 64u * 1024u * 1024u; // 64 MiB
// Element-nesting cap for untrusted mod XML. Real game XML nests ~6-10 deep; a
// crafted file nesting tens of thousands deep would otherwise build an
// arbitrarily tall XMLNode chain during the startup catalog prefetch (the
// recursion risk is in teardown, but the cap stops the tree existing at all).
static const unsigned long kMaxXmlDepth          = 512u;
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
