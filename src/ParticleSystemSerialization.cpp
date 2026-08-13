// Serialization split from ParticleSystem.cpp; mutation and validation stay there.

#include <iostream>
#include <algorithm>
#include <cassert>
#include <set>
#include <unordered_map>
#include "ParticleSystem.h"
#include "EmitterInstance.h"
#include "ParticleSystemInstance.h"
#include "LinkGroup.h"
#include "exceptions.h"
#include "ResourceLimits.h"
using namespace std;

static const int NUM_BLEND_MODES = 14;

static void Verify(int expr)
{
	if (!expr)
	{
		throw BadFileException();
	}
}

static uint8_t readByte(ChunkReader& reader)
{
	uint8_t value;
	Verify(reader.size() == sizeof(uint8_t));
	reader.read(&value, sizeof(value));
	return value;
}

static bool readBool(ChunkReader& reader)
{
	return readByte(reader) != 0;
}

static float readFloat(ChunkReader& reader)
{
	float value;
	Verify(reader.size() == sizeof(float));
	reader.read(&value, sizeof(value));
	return value;
}

static unsigned long readInteger(ChunkReader& reader)
{
	uint32_t value;
	Verify(reader.size() == sizeof(uint32_t));
	reader.read(&value, sizeof(value));
	return letohl(value);
}

static unsigned long readPackedInteger(ChunkReader& reader, long& remaining)
{
	uint32_t value;
	Verify(remaining >= (long)sizeof(uint32_t));
	reader.read(&value, sizeof(value));
	remaining -= (long)sizeof(value);
	return letohl(value);
}

static void readPackedBytes(ChunkReader& reader, void* buffer, long size, long& remaining)
{
	Verify(size >= 0 && remaining >= size);
	if (size > 0)
	{
		reader.read(buffer, size);
		remaining -= size;
	}
}

static void skipPackedBytes(ChunkReader& reader, long size, long& remaining)
{
	Verify(size >= 0 && remaining >= size);
	char buffer[256];
	while (size > 0)
	{
		const long n = std::min<long>(size, (long)sizeof(buffer));
		reader.read(buffer, n);
		size -= n;
		remaining -= n;
	}
}

// Map a raw file integer to a valid InterpolationType, falling
// back to the linear default for anything outside {IT_UNKNOWN..IT_STEP}.
static ParticleSystem::Emitter::Track::InterpolationType clampInterpolation(unsigned long raw)
{
	typedef ParticleSystem::Emitter::Track Track;
	int v = (int)raw;
	if (v < Track::IT_UNKNOWN || v > Track::IT_STEP)
		return Track::IT_LINEAR;
	return (Track::InterpolationType)v;
}

static void writeByte(ChunkWriter& writer, uint8_t value)
{
	writer.write(&value, sizeof(value));
}

static void writeBool(ChunkWriter& writer, bool value)
{
	writeByte(writer, value);
}

static void writeFloat(ChunkWriter& writer, float value)
{
	writer.write(&value, sizeof(value));
}

static void writeInteger(ChunkWriter& writer, unsigned long value)
{
	uint32_t leValue = htolel(value);
	writer.write(&leValue, sizeof(leValue));
}

//
// Writing
//
static void writeMiniBool(ChunkWriter& writer, ChunkType type, bool value)
{
	writer.beginMiniChunk(type);
	writeBool(writer, value);
	writer.endChunk();
}

static void writeMiniFloat(ChunkWriter& writer, ChunkType type, float value)
{
	writer.beginMiniChunk(type);
	writeFloat(writer, value);
	writer.endChunk();
}

static void writeMiniInteger(ChunkWriter& writer, ChunkType type, unsigned long value)
{
	writer.beginMiniChunk(type);
	writeInteger(writer, value);
	writer.endChunk();
}

//
// Emitter class
//

void ParticleSystem::Emitter::writeProperties(ChunkWriter& writer) const
{
	writer.beginChunk(0x0002);

	writeMiniInteger(writer, 0x04, blendMode);
	writeMiniInteger(writer, 0x05, max(1,nTriangles) - 1);
	writeMiniInteger(writer, 0x06, unknown06);
	writeMiniBool   (writer, 0x07, useBursts);
	writeMiniBool   (writer, 0x43, parentLinkStrength != 0.0);
	writeMiniBool   (writer, 0x08, linkToSystem);
	writeMiniFloat  (writer, 0x09, -inwardSpeed);

	writer.beginMiniChunk(0x0a);
	writer.write(acceleration, 3 * sizeof(float));
	writer.endChunk();

	writeMiniFloat  (writer, 0x0C, gravity);
	writeMiniFloat  (writer, 0x0F, lifetime);
	writeMiniFloat  (writer, 0x12, randomScalePerc);
	writeMiniFloat  (writer, 0x13, randomLifetimePerc);
	writeMiniInteger(writer, 0x49, unknown49);
	writeMiniInteger(writer, 0x10, textureSize);
	writeMiniInteger(writer, 0x14, (unsigned long)index);
	writeMiniBool   (writer, 0x15, unknown15);
	writeMiniFloat  (writer, 0x17, randomRotationVariance);
	writeMiniFloat  (writer, 0x0B, -inwardAcceleration);
	writeMiniBool   (writer, 0x23, randomRotationDirection);
	writeMiniFloat  (writer, 0x24, initialDelay);
	writeMiniFloat  (writer, 0x25, burstDelay);
	writeMiniInteger(writer, 0x26, nParticlesPerBurst);
	writeMiniInteger(writer, 0x27, nBursts == 0 ? -1 : nBursts);
	writeMiniFloat  (writer, 0x28, parentLinkStrength);
	writeMiniInteger(writer, 0x2a, nParticlesPerSecond);
	writeMiniBool   (writer, 0x2b, unknown2b);

	writer.beginMiniChunk(0x2c);
	writer.write(randomColors, 4 * sizeof(float));
	writer.endChunk();

	writeMiniBool   (writer, 0x2d, doColorAddGrayscale);
	writeMiniBool   (writer, 0x2e, isWorldOriented);
	writeMiniInteger(writer, 0x2f, groundBehavior);
	writeMiniFloat  (writer, 0x30, bounciness);
	writeMiniBool   (writer, 0x31, affectedByWind);
	writeMiniFloat  (writer, 0x32, freezeTime);
	writeMiniFloat  (writer, 0x33, skipTime);
	writeMiniInteger(writer, 0x34, emitFromMesh);
	writeMiniBool   (writer, 0x35, objectSpaceAcceleration);
	writeMiniBool   (writer, 0x3b, isHeatParticle);
	writeMiniFloat  (writer, 0x3c, emitFromMeshOffset);
	writeMiniBool   (writer, 0x3d, isWeatherParticle);
	writeMiniFloat  (writer, 0x3e, weatherCubeSize);
	writeMiniFloat  (writer, 0x3f, unknown3f);
	writeMiniFloat  (writer, 0x40, weatherFadeoutDistance);
	writeMiniBool   (writer, 0x41, hasTail);
	writeMiniFloat  (writer, 0x42, tailSize);
	writeMiniBool   (writer, 0x44, unknown44);
	writeMiniBool   (writer, 0x46, noDepthTest);
	writeMiniFloat  (writer, 0x47, weatherCubeDistance);
	writeMiniBool   (writer, 0x48, randomRotation);

	writer.endChunk();
}

void ParticleSystem::Emitter::writeTracks(ChunkWriter& writer) const
{
	writer.beginChunk(0x0001);

	// Write channel tracks
	for (int i = 0; i < 4; i++)
	{
		writer.beginChunk(0x00);
		writer.beginMiniChunk(0x02);
		writeByte(writer, (uint8_t)(int)(tracks[i]->keys.begin()->value * 255));
		writer.endChunk();
		writer.beginMiniChunk(0x03);
		writeByte(writer, (uint8_t)(int)(tracks[i]->keys.rbegin()->value * 255));
		writer.endChunk();
		writer.beginMiniChunk(0x04);
		writeInteger(writer, tracks[i]->interpolation);
		writer.endChunk();
		writer.endChunk();

		writer.beginChunk(0x01);
		for (multiset<Track::Key>::const_iterator key = ++tracks[i]->keys.begin(); key != --tracks[i]->keys.end(); key++)
		{
			writer.beginMiniChunk(0x05);
			uint32_t value = htolel((unsigned long)(key->value * 255));
			float    time  = key->time / 100.0f;
			writer.write(&value, sizeof(uint32_t));
			writer.write(&time,  sizeof(float));
			writer.endChunk();
		}
		writer.endChunk();
	}

	// Write other tracks
	for (int i = 4; i < 7; i++)
	{
		float first = tracks[i]->keys.begin()->value;
		float last  = tracks[i]->keys.rbegin()->value;

		if (randomRotation && i == TRACK_ROTATION_SPEED)
		{
			// If we use random rotation, this track is special
            first = randomRotationAverage;
			last  = 0;
		}

		writer.beginChunk(0x00);
		writer.beginMiniChunk(0x02);
		writeFloat(writer, first);
		writer.endChunk();
		writer.beginMiniChunk(0x03);
		writeFloat(writer, last);
		writer.endChunk();
		writer.beginMiniChunk(0x04);
		writeInteger(writer, tracks[i]->interpolation);
		writer.endChunk();
		writer.endChunk();

		writer.beginChunk(0x01);
		if (!randomRotation || i != TRACK_ROTATION_SPEED)
		{
			// Don't store the rotation speed track for random rotations
			for (multiset<Track::Key>::const_iterator key = ++tracks[i]->keys.begin(); key != --tracks[i]->keys.end(); key++)
			{
				writer.beginMiniChunk(0x05);
				float value = key->value;
				float time  = key->time / 100.0f;
				writer.write(&value, sizeof(float));
				writer.write(&time,  sizeof(float));
				writer.endChunk();
			}
		}
		writer.endChunk();
	}

	writer.endChunk();
}

void ParticleSystem::Emitter::writeGroups(ChunkWriter& writer) const
{
	writer.beginChunk(0x0029);

	for (int i = 0; i < NUM_GROUPS; i++)
	{
		writer.beginChunk(0x1100);
		writer.beginChunk(0x1101);
		writer.write(&groups[i], sizeof(Group));
		writer.endChunk();
		writer.endChunk();
	}

	writer.endChunk();
}

void ParticleSystem::Emitter::write(ChunkWriter& writer, bool copy)
{
	// Set second group
	groups[1].type = 1;
	groups[1].minX = 0.0f;
	groups[1].maxX = 0.0f;
	groups[1].minY = lifetime * (1 - randomLifetimePerc);
	groups[1].maxY = lifetime;
	groups[1].minZ = 0.0f;
	groups[1].maxZ = 0.0f;

	writeProperties(writer);

	writer.beginChunk(0x0003);
	writer.writeString(colorTexture);
	writer.endChunk();

	writer.beginChunk(0x0016);
	writer.writeString(name);
	writer.endChunk();

	writeGroups(writer);
	writeTracks(writer);

	writer.beginChunk(0x0036);
    writer.beginMiniChunk(0x37); writeInteger(writer, (unsigned long)(copy ? -1 : spawnOnDeath));    writer.endChunk();
    writer.beginMiniChunk(0x39); writeInteger(writer, (unsigned long)(copy ? -1 : spawnDuringLife)); writer.endChunk();
	writer.endChunk();
	
	if (normalTexture != "")
	{
		writer.beginChunk(0x0045);
		writer.writeString(normalTexture);
		writer.endChunk();
	}

	// Editor-only link-group chunk. Game engine readers skip
	// unknown chunks at the emitter level (the existing optional
	// 0x36 / 0x45 chunks rely on the same behaviour). Only emitted
	// when this emitter actually belongs to a group, so files
	// without link groups remain byte-identical to pre-feature
	// output. Also suppressed when serialising for clipboard copy —
	// link-group IDs are local to a particle system, so cross-file
	// paste arrives unlinked by design.
	if (!copy && linkGroup != 0)
	{
		writer.beginChunk(0x0100);
		writeInteger(writer, linkGroup);
		writer.endChunk();
	}
}

//
// Reading
//
void ParticleSystem::Emitter::readProperties(ChunkReader& reader)
{
	bool useLinkStrength = false;

	ChunkType type;
	while ((type = reader.nextMini()) != -1)
	{
		switch (type)
		{
			case 0x04: blendMode				= readInteger(reader) % NUM_BLEND_MODES; break;
			case 0x05:
			{
				// A crafted 0xFFFFFFFF would wrap `+1` to 0,
				// producing a zero-triangle emitter. Clamp so the loaded
				// value never wraps and stays >= 1.
				unsigned long raw = readInteger(reader);
				nTriangles = (raw == 0xFFFFFFFFul) ? 1 : raw + 1;
				break;
			}
			case 0x07: useBursts				= readBool(reader); break;
			case 0x08: linkToSystem 			= readBool(reader); break;
			case 0x09: inwardSpeed				= -readFloat(reader); break;
			case 0x0A: reader.read(acceleration, 3 * sizeof(float)); break;
			case 0x0B: inwardAcceleration       = -readFloat(reader);   break;
			case 0x0C: gravity					= readFloat(reader); break;
			case 0x0F: lifetime					= readFloat(reader); break;
			case 0x10: textureSize				= readInteger(reader); break;
			case 0x12: randomScalePerc			= readFloat(reader); break;
			case 0x13: randomLifetimePerc		= readFloat(reader); break;
			case 0x14: readInteger(reader); break; // Read but ignore index
            case 0x17: randomRotationVariance   = readFloat(reader); randomRotationVariance = max(0.0f, min(1.0f, randomRotationVariance)); break;
			case 0x23: randomRotationDirection	= readBool(reader); break;
			case 0x24: initialDelay				= readFloat(reader); break;
			case 0x25: burstDelay				= readFloat(reader); break;
			case 0x26: nParticlesPerBurst		= readInteger(reader); break;
			case 0x27: nBursts					= readInteger(reader); if (nBursts == -1) nBursts = 0; break;
			case 0x28: parentLinkStrength		= readFloat(reader); break;
			case 0x2A: nParticlesPerSecond		= readInteger(reader); break;
			case 0x2B: unknown2b				= readBool(reader); break;
			case 0x2C: reader.read(randomColors, 4 * sizeof(float)); break;
			case 0x2D: doColorAddGrayscale		= readBool(reader); break;
			case 0x2E: isWorldOriented			= readBool(reader); break;
			case 0x2F: groundBehavior			= readInteger(reader); break;
			case 0x30: bounciness				= readFloat(reader); break;
			case 0x31: affectedByWind			= readBool(reader); break;
			case 0x32: freezeTime				= readFloat(reader); break;
			case 0x33: skipTime					= readFloat(reader); break;
			case 0x34: emitFromMesh             = readInteger(reader); break;
			case 0x35: objectSpaceAcceleration  = readBool(reader); break;
			case 0x3B: isHeatParticle			= readBool(reader); break;
			case 0x3C: emitFromMeshOffset       = readFloat(reader); break;
			case 0x3D: isWeatherParticle		= readBool(reader); break;
			case 0x3E: weatherCubeSize			= readFloat(reader); break;
			case 0x40: weatherFadeoutDistance = readFloat(reader);   break;
			case 0x41: hasTail					= readBool(reader); break;
			case 0x42: tailSize					= readFloat(reader); break;
			case 0x43: useLinkStrength			= readBool(reader); break;
			case 0x46: noDepthTest				= readBool(reader); break;
			case 0x47: weatherCubeDistance		= readFloat(reader); break;
			case 0x48: randomRotation			= readBool(reader); break;

			case 0x06: unknown06 = readInteger(reader); break;
			case 0x11: unknown11 = readFloat(reader);   break;
			case 0x15: unknown15 = readBool(reader);    break;
			case 0x3F: unknown3f = readFloat(reader);   break;
			case 0x44: unknown44 = readBool(reader);    break;
			case 0x49: unknown49 = readInteger(reader); break;

			default:
				throw BadFileException();
		}
	}

	if (!useLinkStrength) parentLinkStrength  = 0.0f;
}

void ParticleSystem::Emitter::readGroups(ChunkReader& reader)
{
	for (int i = 0; i < NUM_GROUPS; i++)
	{
		Verify(reader.next() == 0x1100);
		Verify(reader.next() == 0x1101);
		reader.read(&groups[i], sizeof(Group));
		// `type` is blitted raw from the file but is consumed as a
		// group-type selector (GT_EXACT..GT_CYLINDER). Reject out-of-range
		// values. sphereEdge/cylinderEdge stay unvalidated -- they are used
		// as flags, not bounded enums.
		Verify(groups[i].type < NUM_GROUP_TYPES);
		Verify(reader.next() == -1);
	}

	Verify(reader.next() == -1);
}

void ParticleSystem::Emitter::readTracks(ChunkReader& reader)
{
	// Read channel tracks
	for (int i = 0; i < 4; i++)
	{
		trackContents[i].keys.clear();
        tracks[i] = &trackContents[i];

		Verify(reader.next() == 0x00);
		Verify(reader.nextMini() == 0x02);
		Track::Key first(0.0f, readByte(reader) / 255.0f);
		Verify(reader.nextMini() == 0x03);
		Track::Key last(100.0f, readByte(reader) / 255.0f);
		Verify(reader.nextMini() == 0x04);
		// The file int is cast straight to the enum; clamp an
		// out-of-range value to the linear default rather than store a
		// bogus interpolation mode.
		trackContents[i].interpolation = clampInterpolation(readInteger(reader));
		Verify(reader.nextMini() == -1);

		trackContents[i].keys.insert(first);
		Verify(reader.next() == 0x01);

		ChunkType type;
		while ((type = reader.nextMini()) == 5)
		{
			Track::Key key;
			uint32_t value;
			// This mini-chunk is a 4-byte value + 4-byte time;
			// guard the exact size before the raw reads, mirroring how
			// readFloat/readInteger validate reader.size().
			Verify(reader.size() == sizeof(uint32_t) + sizeof(float));
			reader.read(&value, sizeof(uint32_t));
			key.value = letohl(value) / 255.0f;
			reader.read(&key.time, sizeof(float));
			key.time *= 100.0f;	// Transform to percentage
			Verify(key.value >= 0.0f && key.value <= 1.0f && key.time <= 100.0f && key.time >= trackContents[i].keys.rbegin()->time);
			// Aggregate cap (2026-07 audit). Every key above is individually
			// validated; the TOTAL never was, and KeyMap is a multiset, so each
			// 8-byte on-disk key becomes a tree node many times that size.
			Verify(trackContents[i].keys.size() < kMaxAloTrackKeys);
			trackContents[i].keys.insert(key);
		}
		Verify(type == -1);
		trackContents[i].keys.insert(last);
	}

    // See if any of the first four are identical
    for (int i = 0; i < 4; i++)
    for (int j = i + 1; j < 4; j++)
    {
        if (tracks[i] == &trackContents[i] &&
            trackContents[i].interpolation == trackContents[j].interpolation &&
            trackContents[i].keys.size() == trackContents[j].keys.size() &&
            equal(trackContents[i].keys.begin(), trackContents[i].keys.end(), trackContents[j].keys.begin()))
        {
            // Identical, point them to the same contents
            tracks[j] = tracks[i];
        }
    }

	// Read other tracks
	for (int i = 4; i < 7; i++)
	{
		trackContents[i].keys.clear();

		Track::Key first, last;
		Verify(reader.next() == 0x00);
		Verify(reader.nextMini() == 0x02);
		first.time  = 0.0;
		first.value = readFloat(reader);
		Verify(reader.nextMini() == 0x03);
		last.time  = 100.0;
		last.value = readFloat(reader);
		Verify(reader.nextMini() == 0x04);
		// Clamp an out-of-range interpolation int to the linear default.
		trackContents[i].interpolation = clampInterpolation(readInteger(reader));
		Verify(reader.nextMini() == -1);

		trackContents[i].keys.insert(first);
		Verify(reader.next() == 0x01);
		ChunkType type;
		while ((type = reader.nextMini()) == 5)
		{
			Track::Key key;
			// Guard the exact key size (value + time) before the
			// raw blit, mirroring the channel-track loop above.
			Verify(reader.size() == sizeof(Track::Key));
			reader.read(&key, sizeof(Track::Key));
			key.time *= 100.0f; // Transform to percentage
			Verify(key.time <= 100.0f && trackContents[i].keys.rbegin()->time <= key.time);
			// Mirror the channel-track rule exactly: the first endpoint already
			// occupies one slot, intermediates stop before the cap, and the final
			// endpoint is appended after this loop.
			Verify(trackContents[i].keys.size() < kMaxAloTrackKeys);
			trackContents[i].keys.insert(key);
		}
		Verify(type == -1);
		trackContents[i].keys.insert(last);
	}

	Verify(reader.next() == -1);
}

ParticleSystem::Emitter::Emitter(ChunkReader& reader)
{
	setDefaults();

	Verify(reader.next() == 0x02); readProperties(reader);
	Verify(reader.next() == 0x03); colorTexture = reader.readString();
	Verify(reader.next() == 0x16); name         = reader.readString();
	Verify(reader.next() == 0x29); readGroups(reader);
	Verify(reader.next() == 0x01); readTracks(reader);

	if (randomRotation)
	{
		randomRotationAverage = this->tracks[TRACK_ROTATION_SPEED]->keys.begin()->value;
	}

	ChunkType type = reader.next();
	if (type == 0x36)
	{
		Verify(reader.nextMini() == 0x37); spawnOnDeath    = readInteger(reader); if (spawnOnDeath    == 0xFFFFFFFF) spawnOnDeath    = (size_t)-1;
		Verify(reader.nextMini() == 0x39); spawnDuringLife = readInteger(reader); if (spawnDuringLife == 0xFFFFFFFF) spawnDuringLife = (size_t)-1;
		Verify(reader.nextMini() == -1);
		type = reader.next();
	}

	if (type == 0x45)
	{
		normalTexture = reader.readString();
		type = reader.next();
	}

	// Editor-only link-group chunk. Optional; absent in
	// pre-feature files and in files where this emitter is unlinked.
	if (type == 0x100)
	{
		linkGroup = readInteger(reader);
		type = reader.next();
	}

	Verify(type == -1);
}

void ParticleSystem::write(IFile* file)
{
	ChunkWriter writer(file);

	writer.beginChunk(0x0900);

	writer.beginChunk(0x0000);
	writer.writeString(m_name);
	writer.endChunk();

	writer.beginChunk(0x0001);
	writeInteger(writer, 0);	// Irrelevant value
	writer.endChunk();

	writer.beginChunk(0x0800);
	for (size_t i = 0; i < m_emitters.size(); i++)
	{
		writer.beginChunk(0x0700);
		m_emitters[i]->write(writer);
		writer.endChunk();
	}
	writer.endChunk();

	// Per-group exempt-flags chunk. Editor-only — game engine
	// skips unknown system-level chunks (same pattern as 0x0002
	// leaveParticles below). Only emitted when at least one group has
	// a non-default exempt set; files without customization remain
	// byte-identical to the earlier output.
	//
	// Layout:
	//   uint32_t count
	//   for each entry:
	//     uint32_t groupId
	//     uint32_t flagsByteCount        // sizeof(LinkExemptFlags) at write time
	//     uint8_t  flags[flagsByteCount] // raw POD blob
	//
	// The flagsByteCount prefix lets future versions add fields to
	// LinkExemptFlags without breaking older readers — they read what
	// they know, skip the rest. Older-saved-by-newer readers see a
	// smaller blob and default the missing tail to false.
	if (!m_linkExempts.empty())
	{
		writer.beginChunk(0x0003);
		writeInteger(writer, (unsigned long)m_linkExempts.size());
		for (std::map<uint32_t, LinkExemptFlags>::const_iterator it
		         = m_linkExempts.begin();
		     it != m_linkExempts.end(); ++it)
		{
			writeInteger(writer, (unsigned long)it->first);
			writeInteger(writer, (unsigned long)sizeof(LinkExemptFlags));
			writer.write(&it->second, sizeof(LinkExemptFlags));
		}
		writer.endChunk();
	}

	writer.beginChunk(0x0002);
	writeBool(writer, m_leaveParticles);
	writer.endChunk();

	writer.endChunk();
}

ParticleSystem::ParticleSystem(IFile* file)
{
    try
    {
	    ChunkType   type;
	    ChunkReader reader(file);

	    if ((type = reader.next()) != 0x0900)
	    {
		    throw WrongFileException();
	    }
    	
	    // Read name
	    Verify(reader.next() == 0x0000);
	    m_name = reader.readString();

	    // Ignore 0001 chunk
	    Verify(reader.next() == 0x0001 && reader.size() == sizeof(uint32_t));

	    // Read emitters. Cap the count: the loop is otherwise bounded only by the
	    // file size, so a crafted .alo packed with tiny 0x0700 headers amplifies
	    // each ~8-byte chunk into a full Emitter allocation (2026-07 audit).
	    Verify(reader.next() == 0x0800);
	    while ((type = reader.next()) == 0x0700)
	    {
            Verify(m_emitters.size() < kMaxAloEmitters);
            Emitter* emitter = new Emitter(reader);
            emitter->index = m_emitters.size();
		    m_emitters.push_back(emitter);
	    }
	    Verify(type == -1);

	    // Read optional system-body sibling chunks. Earlier readers
	    // only handled 0x0002 (leaveParticles); this extends to
	    // 0x0003 (per-group link-exempt flags). The loop tolerantly
	    // skips any unrecognized chunk so future additions don't
	    // require touching this code path.
	    uint32_t linkExemptRecordsTotal = 0;
	    type = reader.next();
	    while (type != -1)
	    {
	        if (type == 0x0002)
	        {
		        Verify(reader.size() == 1);
		        m_leaveParticles = readBool(reader);
	        }
	        else if (type == 0x0003)
	        {
	            // Per-group link-exempt flags.
	            long remaining = reader.size();
	            uint32_t count = (uint32_t)readPackedInteger(reader, remaining);
	            // Aggregate cap (2026-07 audit). `remaining` bounds the BYTES,
	            // not the entries — at ~3 bytes apiece that still admits tens of
	            // millions of map inserts from one chunk. Reject up front rather
	            // than part-way through, so a refused file leaves no half-built
	            // exempt table behind.
	            Verify(count <= kMaxAloLinkExempts);
	            // Count raw records across every sibling before filtering zero
	            // IDs/default flags or overwriting duplicate group IDs. The
	            // maintained total<=cap invariant makes the subtraction safe.
	            Verify(count <= kMaxAloLinkExemptRecordsTotal - linkExemptRecordsTotal);
	            linkExemptRecordsTotal += count;
	            for (uint32_t i = 0; i < count; ++i)
	            {
	                uint32_t groupId      = (uint32_t)readPackedInteger(reader, remaining);
	                uint32_t flagsBytes   = (uint32_t)readPackedInteger(reader, remaining);
	                Verify(remaining >= 0 && flagsBytes <= (uint32_t)remaining);
	                LinkExemptFlags flags = GetDefaultLinkExemptFlags();
	                uint32_t toRead = (flagsBytes <= sizeof(LinkExemptFlags))
	                                ? flagsBytes
	                                : (uint32_t)sizeof(LinkExemptFlags);
	                if (toRead > 0)
	                    readPackedBytes(reader, &flags, (long)toRead, remaining);
	                // Drain any trailing bytes from a future-version blob.
	                if (flagsBytes > sizeof(LinkExemptFlags))
	                {
	                    long discardSize = (long)(flagsBytes - sizeof(LinkExemptFlags));
	                    skipPackedBytes(reader, discardSize, remaining);
	                }
	                // Defensive: drop entries for groupId 0 (invalid)
	                // or entries equal to v1 defaults (the writer
	                // shouldn't emit them, but be lenient if a hand-
	                // crafted file does).
	                if (groupId != 0 && flags != GetDefaultLinkExemptFlags())
	                    m_linkExempts[groupId] = flags;
	            }
#ifndef NDEBUG
	            printf("[Link] read chunk 0x0003: count=%u entries=%zu\n",
	                   count, m_linkExempts.size());
	            fflush(stdout);
#endif
	        }
	        else
	        {
	            // Unknown chunk — drain its bytes and continue.
	            reader.skip();
	        }
	        type = reader.next();
	    }

	    // Post-process: make the loaded spawn-graph well-formed before any
	    // emitter is parented or recursed over. ValidateEmitterGraph clears
	    // out-of-range / self / duplicate-parent links, breaks cycles, and
	    // rebuilds parent pointers. (Also covers autosave restore, which
	    // loads through this same ParticleSystem(IFile*) constructor.)
	    ValidateEmitterGraph();
    }
    catch (...)
    {
	    for (size_t i = 0; i < m_emitters.size(); i++)
	    {
            delete m_emitters[i];
        }
        throw;
    }
}

