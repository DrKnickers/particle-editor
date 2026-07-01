#ifndef RESOURCE_LIMITS_H
#define RESOURCE_LIMITS_H

#include <cstdint>

static const unsigned long kMaxXmlFileBytes      = 64u * 1024u * 1024u; // 64 MiB
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

#endif
