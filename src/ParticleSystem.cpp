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

// Process-monotonic counter for Emitter::stableId. UI-thread-only (every
// Emitter is constructed on the editor's UI thread), so a plain unsigned
// suffices. Starts at 1 so 0 stays free as the synthetic-root sentinel.
static unsigned int s_nextEmitterStableId = 1;

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

void ParticleSystem::Emitter::registerEmitterInstance(EmitterInstance* instance)
{
    m_instances.insert(instance);
}

void ParticleSystem::Emitter::unregisterEmitterInstance(EmitterInstance* instance)
{
    m_instances.erase(instance);
}

ParticleSystem::Emitter::Emitter()
{
	setDefaults();
}

ParticleSystem::Emitter::Emitter(const Emitter& emitter)
{
    // Copy all data
    *this = emitter;

    // Repoint the track pointers to our copy of the track contents
    for (int i = 0; i < NUM_TRACKS; i++)
    {
        tracks[i] = trackContents + (emitter.tracks[i] - emitter.trackContents);
    }

    // The default operator= just shallow-copied
    // m_instances (the set of live runtime EmitterInstance pointers),
    // leaving the cloned Emitter pointing at the source's live
    // instances. Subsequent destruction or mutation of the clone would
    // then affect instances that logically belong to the source. Clear
    // here so the clone starts with no runtime presence; the engine
    // re-populates m_instances as it spawns instances against the clone.
    m_instances.clear();

    // A clone is a NEW emitter: `*this = emitter` copied the source's
    // stableId, which would give two live rows the same React key (and the
    // FLIP pass an ambiguous identity). Issue a fresh one.
    stableId = s_nextEmitterStableId++;
}

void ParticleSystem::Emitter::detachFromLinkGroup()
{
#ifndef NDEBUG
    if (linkGroup != 0)
    {
        printf("[Link] detach '%s' (was group=%u)\n", name.c_str(), linkGroup);
        fflush(stdout);
    }
#endif
    linkGroup = 0;
}

void ParticleSystem::Emitter::copySharedParamsFrom(const Emitter&         src,
                                                    const LinkExemptFlags& exempt)
{
    if (&src == this) return;

    // 1) Snapshot every field on `*this` that the propagation must
    //    not touch.
    //
    //    Structural / private fields are always preserved (irrespective
    //    of any exempt flag), since "linkGroup membership", "parent",
    //    "tree position", and "active runtime instances" are intrinsic
    //    to an emitter's identity. Exempt-eligible fields are saved
    //    unconditionally — cheap (POD or short-string) and simpler than
    //    threading the flag check through each save. The conditional
    //    restore below decides what gets written back.
    std::set<EmitterInstance*> savedInstances = m_instances;
    Emitter*                   savedParent    = parent;
    size_t                     savedSpawnOD   = spawnOnDeath;
    size_t                     savedSpawnDL   = spawnDuringLife;
    size_t                     savedIndex     = index;
    uint32_t                   savedLinkGrp   = linkGroup;
    bool                       savedVisible   = visible;
    unsigned int               savedStableId  = stableId;

    // Strings + names.
    std::string savedName          = name;
    std::string savedColorTexture  = colorTexture;
    std::string savedNormalTexture = normalTexture;

    // Scalars + bools.
    bool          sav_linkToSystem            = linkToSystem;
    bool          sav_objectSpaceAcceleration = objectSpaceAcceleration;
    bool          sav_doColorAddGrayscale     = doColorAddGrayscale;
    bool          sav_affectedByWind          = affectedByWind;
    bool          sav_isHeatParticle          = isHeatParticle;
    bool          sav_isWeatherParticle       = isWeatherParticle;
    bool          sav_hasTail                 = hasTail;
    bool          sav_noDepthTest             = noDepthTest;
    bool          sav_randomRotation          = randomRotation;
    bool          sav_randomRotationDirection = randomRotationDirection;
    bool          sav_isWorldOriented         = isWorldOriented;
    bool          sav_useBursts               = useBursts;
    int           sav_emitFromMesh            = emitFromMesh;
    float         sav_gravity                 = gravity;
    float         sav_lifetime                = lifetime;
    float         sav_initialDelay            = initialDelay;
    float         sav_burstDelay              = burstDelay;
    float         sav_inwardSpeed             = inwardSpeed;
    float         sav_inwardAcceleration      = inwardAcceleration;
    float         sav_randomScalePerc         = randomScalePerc;
    float         sav_randomLifetimePerc      = randomLifetimePerc;
    float         sav_weatherCubeSize         = weatherCubeSize;
    float         sav_tailSize                = tailSize;
    float         sav_parentLinkStrength      = parentLinkStrength;
    float         sav_weatherCubeDistance     = weatherCubeDistance;
    float         sav_randomRotationAverage   = randomRotationAverage;
    float         sav_randomRotationVariance  = randomRotationVariance;
    float         sav_bounciness              = bounciness;
    float         sav_freezeTime              = freezeTime;
    float         sav_skipTime                = skipTime;
    float         sav_emitFromMeshOffset      = emitFromMeshOffset;
    float         sav_weatherFadeoutDistance  = weatherFadeoutDistance;
    unsigned long sav_nBursts                 = nBursts;
    unsigned long sav_blendMode               = blendMode;
    unsigned long sav_textureSize             = textureSize;
    unsigned long sav_nParticlesPerSecond     = nParticlesPerSecond;
    unsigned long sav_nTriangles              = nTriangles;
    unsigned long sav_nParticlesPerBurst      = nParticlesPerBurst;
    unsigned long sav_groundBehavior          = groundBehavior;

    // Arrays.
    float sav_acceleration[3];
    float sav_randomColors[4];
    memcpy(sav_acceleration, acceleration, sizeof(acceleration));
    memcpy(sav_randomColors, randomColors, sizeof(randomColors));

    // Random param groups (3 × sizeof(Group)).
    Group sav_groups[NUM_GROUPS];
    memcpy(sav_groups, groups, sizeof(groups));

    // Tracks — save the keymap + interpolation per slot. We snapshot
    // every slot unconditionally; the per-slot exempt check at restore
    // time decides what to write back.
    Track sav_tracks[NUM_TRACKS];
    for (int i = 0; i < NUM_TRACKS; i++) sav_tracks[i] = trackContents[i];

    // Unknown / undocumented fields. Saved for completeness so
    // copySharedParamsFrom is symmetric across the data model, even
    // though no UI surfaces them. If the user toggles their flag via
    // some future feature, save/restore is already wired.
    bool          sav_unknown15 = unknown15;
    bool          sav_unknown2b = unknown2b;
    bool          sav_unknown44 = unknown44;
    float         sav_unknown11 = unknown11;
    float         sav_unknown3f = unknown3f;
    unsigned long sav_unknown06 = unknown06;
    unsigned long sav_unknown49 = unknown49;

    // 2) Bulk-copy via default operator=. This clobbers m_instances
    //    and the tracks[] pointers, which we restore below.
    *this = src;

    // 3) Repoint tracks[] into our own trackContents[], mirroring
    //    src's aliasing structure (same pattern as Emitter(const
    //    Emitter&) at the file's existing copy constructor).
    for (int i = 0; i < NUM_TRACKS; i++)
    {
        tracks[i] = trackContents + (src.tracks[i] - src.trackContents);
    }

    // 4) Restore private + structural fields. stableId is identity — the
    //    propagation copies PARAMETERS between linked emitters, never which
    //    emitter a row IS (a clobber here would give two rows the same
    //    React key and corrupt the reorder-glide FLIP identity).
    m_instances     = savedInstances;
    parent          = savedParent;
    spawnOnDeath    = savedSpawnOD;
    spawnDuringLife = savedSpawnDL;
    index           = savedIndex;
    linkGroup       = savedLinkGrp;
    visible         = savedVisible;
    stableId        = savedStableId;

    // 5) Restore exempt fields per the flags.
    if (exempt.name)          name          = savedName;
    if (exempt.colorTexture)  colorTexture  = savedColorTexture;
    if (exempt.normalTexture) normalTexture = savedNormalTexture;

    if (exempt.linkToSystem)            linkToSystem            = sav_linkToSystem;
    if (exempt.objectSpaceAcceleration) objectSpaceAcceleration = sav_objectSpaceAcceleration;
    if (exempt.doColorAddGrayscale)     doColorAddGrayscale     = sav_doColorAddGrayscale;
    if (exempt.affectedByWind)          affectedByWind          = sav_affectedByWind;
    if (exempt.isHeatParticle)          isHeatParticle          = sav_isHeatParticle;
    if (exempt.isWeatherParticle)       isWeatherParticle       = sav_isWeatherParticle;
    if (exempt.hasTail)                 hasTail                 = sav_hasTail;
    if (exempt.noDepthTest)             noDepthTest             = sav_noDepthTest;
    if (exempt.randomRotation)          randomRotation          = sav_randomRotation;
    if (exempt.randomRotationDirection) randomRotationDirection = sav_randomRotationDirection;
    if (exempt.isWorldOriented)         isWorldOriented         = sav_isWorldOriented;
    if (exempt.useBursts)               useBursts               = sav_useBursts;
    if (exempt.emitFromMesh)            emitFromMesh            = sav_emitFromMesh;
    if (exempt.gravity)                 gravity                 = sav_gravity;
    if (exempt.lifetime)                lifetime                = sav_lifetime;
    if (exempt.initialDelay)            initialDelay            = sav_initialDelay;
    if (exempt.burstDelay)              burstDelay              = sav_burstDelay;
    if (exempt.inwardSpeed)             inwardSpeed             = sav_inwardSpeed;
    if (exempt.inwardAcceleration)      inwardAcceleration      = sav_inwardAcceleration;
    if (exempt.randomScalePerc)         randomScalePerc         = sav_randomScalePerc;
    if (exempt.randomLifetimePerc)      randomLifetimePerc      = sav_randomLifetimePerc;
    if (exempt.weatherCubeSize)         weatherCubeSize         = sav_weatherCubeSize;
    if (exempt.tailSize)                tailSize                = sav_tailSize;
    if (exempt.parentLinkStrength)      parentLinkStrength      = sav_parentLinkStrength;
    if (exempt.weatherCubeDistance)     weatherCubeDistance     = sav_weatherCubeDistance;
    if (exempt.randomRotationAverage)   randomRotationAverage   = sav_randomRotationAverage;
    if (exempt.randomRotationVariance)  randomRotationVariance  = sav_randomRotationVariance;
    if (exempt.bounciness)              bounciness              = sav_bounciness;
    if (exempt.freezeTime)              freezeTime              = sav_freezeTime;
    if (exempt.skipTime)                skipTime                = sav_skipTime;
    if (exempt.emitFromMeshOffset)      emitFromMeshOffset      = sav_emitFromMeshOffset;
    if (exempt.weatherFadeoutDistance)  weatherFadeoutDistance  = sav_weatherFadeoutDistance;
    if (exempt.nBursts)                 nBursts                 = sav_nBursts;
    if (exempt.blendMode)               blendMode               = sav_blendMode;
    if (exempt.textureSize)             textureSize             = sav_textureSize;
    if (exempt.nParticlesPerSecond)     nParticlesPerSecond     = sav_nParticlesPerSecond;
    if (exempt.nTriangles)              nTriangles              = sav_nTriangles;
    if (exempt.nParticlesPerBurst)      nParticlesPerBurst      = sav_nParticlesPerBurst;
    if (exempt.groundBehavior)          groundBehavior          = sav_groundBehavior;

    if (exempt.acceleration) memcpy(acceleration, sav_acceleration, sizeof(acceleration));
    if (exempt.randomColors) memcpy(randomColors, sav_randomColors, sizeof(randomColors));

    if (exempt.groupSpeed)    groups[0] = sav_groups[0];
    if (exempt.groupLifetime) groups[1] = sav_groups[1];
    if (exempt.groupPosition) groups[2] = sav_groups[2];

    // Tracks — for each exempt slot, restore the saved track AND
    // break any src-side aliasing by pointing tracks[i] at our own
    // trackContents[i]. The earlier code did this for TRACK_INDEX
    // only; this generalizes to any exempt track.
    const bool trackExempt[NUM_TRACKS] = {
        exempt.trackRed,
        exempt.trackGreen,
        exempt.trackBlue,
        exempt.trackAlpha,
        exempt.trackScale,
        exempt.trackIndex,
        exempt.trackRotationSpeed,
    };
    for (int i = 0; i < NUM_TRACKS; i++)
    {
        if (trackExempt[i])
        {
            trackContents[i] = sav_tracks[i];
            tracks[i]        = &trackContents[i];
        }
    }

    if (exempt.unknown06) unknown06 = sav_unknown06;
    if (exempt.unknown11) unknown11 = sav_unknown11;
    if (exempt.unknown15) unknown15 = sav_unknown15;
    if (exempt.unknown2b) unknown2b = sav_unknown2b;
    if (exempt.unknown3f) unknown3f = sav_unknown3f;
    if (exempt.unknown44) unknown44 = sav_unknown44;
    if (exempt.unknown49) unknown49 = sav_unknown49;

#ifndef NDEBUG
    // Assert that exempt fields hold their saved values.
    // Catches a forgotten restore if a future flag is added to
    // LinkExemptFlags but its restore line isn't added above.
    // Only asserts on the common easy-to-check fields (most-recent
    // flags); a full check would duplicate the restore code.
    if (exempt.lifetime)     assert(lifetime     == sav_lifetime);
    if (exempt.gravity)      assert(gravity      == sav_gravity);
    if (exempt.colorTexture) assert(colorTexture == savedColorTexture);
    if (exempt.acceleration)
        assert(memcmp(acceleration, sav_acceleration, sizeof(acceleration)) == 0);
    // stableId is IDENTITY (React row keys + FLIP maps key by it) — a clobber
    // here gives two live emitters the same id. Unconditional, unlike the
    // exempt checks above.
    assert(stableId == savedStableId);
#endif
}

ParticleSystem::Emitter::~Emitter()
{
    // Remove all instances of this emitter type. The instances are owned by
    // unique_ptr's in their respective ParticleSystemInstance::m_emitters
    // lists, so we route the deletion through the owning PSI rather than
    // raw-deleting them here (which would leave a dangling unique_ptr —
    // crash on the next render/update).
    while (!m_instances.empty())
    {
        EmitterInstance* inst = *m_instances.begin();
        // RemoveEmitter erases the owning unique_ptr -> ~EmitterInstance ->
        // unregisterEmitterInstance(this), which removes inst from m_instances.
        inst->GetSystem().RemoveEmitter(inst);
    }
}

void ParticleSystem::Emitter::setDefaults()
{
	spawnOnDeath    = -1;
	spawnDuringLife = -1;
	parent          = NULL;
    visible         = true;
    linkGroup       = 0;
    stableId        = s_nextEmitterStableId++;

	name          = "default";
	colorTexture  = "p_particle_master.tga";
	normalTexture = "p_particle_depth_master.tga";

	groups[0].type = 0;
	groups[0].minX = 0.0f; groups[0].minY = 0.0f; groups[0].minZ = 0.0f;
	groups[0].maxX = 0.0f; groups[0].maxY = 0.0f; groups[0].maxZ = 0.0f;
	groups[0].valX = 0.0f; groups[0].valY = 0.0f; groups[0].valZ = 0.0f;
	groups[0].cylinderEdge   = false;
	groups[0].cylinderHeight = 0.0f;
	groups[0].cylinderRadius = 0.0f;
	groups[0].sphereEdge     = false;
	groups[0].sphereRadius   = 0.0f;
	groups[0].sideLength     = 0.0f;
	groups[2] = groups[1] = groups[0];

	for (int i = 0; i < NUM_TRACKS; i++)
	{
        float value;
        switch (i)
        {
            case TRACK_RED_CHANNEL:
            case TRACK_GREEN_CHANNEL:
            case TRACK_BLUE_CHANNEL:
            case TRACK_ALPHA_CHANNEL:   value =  1.0f; break;
            case TRACK_SCALE:           value = 20.0f; break;
            case TRACK_INDEX:           value =  0.0f; break;
            case TRACK_ROTATION_SPEED:  value =  0.0f; break;
        }
        trackContents[i].interpolation = (i == TRACK_INDEX) ? Track::IT_STEP : Track::IT_LINEAR;
		trackContents[i].keys.insert(Track::Key(  0.0f, value));
		trackContents[i].keys.insert(Track::Key(100.0f, value));
        tracks[i] = &trackContents[i];
	}
    // Point Green, Blue and Alpha tracks to Red
    tracks[1] = tracks[0];
    tracks[2] = tracks[0];
    tracks[3] = tracks[0];

    linkToSystem            = false;
	randomRotationDirection	= false;
	doColorAddGrayscale		= false;
	affectedByWind			= false;
	isHeatParticle			= false;
	isWeatherParticle		= false;
	hasTail					= false;
	noDepthTest				= false;
	randomRotation			= false;
	isWorldOriented			= false;
	useBursts				= false;
	objectSpaceAcceleration = false;
	gravity                =   0.0f;
	lifetime			   =   1.0f;
	initialDelay		   =   0.0f;
	burstDelay			   =   1.0f;
	inwardAcceleration     =   0.0f;
	inwardSpeed			   =   0.0f;
	acceleration[0]		   =   0.0f;
	acceleration[1]		   =   0.0f;
	acceleration[2]		   =   0.0f;
	randomScalePerc		   =   0.0f;
	randomLifetimePerc	   =   0.0f;
	weatherCubeSize        = 500.0f;
	tailSize               =  50.0f;
	parentLinkStrength    =    0.0f;
	weatherCubeDistance    =   0.0f;
	randomRotationVariance =   0.0f;
	randomRotationAverage  =   0.0f;
	randomColors[0]		   =   0.0f;
	randomColors[1]		   =   0.0f;
	randomColors[2]		   =   0.0f;
	randomColors[3]		   =   0.0f;
	bounciness			   =   0.2f;
	freezeTime			   =   0.0f;
	skipTime			   =   0.0f;
	emitFromMeshOffset     =   0.50f;
	weatherFadeoutDistance = 100.00f;
	emitFromMesh        = EMIT_DISABLE;
	index               = 0;
	blendMode           = 1;
	textureSize         = 64;
	nBursts             = 0;
	nParticlesPerSecond = 1;
	nTriangles          = 2;
	nParticlesPerBurst  = 1;
	groundBehavior      = 0;

	// These have an unknown function
	unknown15 = true;
	unknown44 = false;
	unknown11 =   0.0f;
	unknown3f =  50.00f;
	unknown06 = 0;
}

//
// ParticleSystem class
//
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

ParticleSystem::ParticleSystem()
{
	m_leaveParticles = true;
}

const LinkExemptFlags& ParticleSystem::getLinkExemptFlags(uint32_t groupId) const
{
    std::map<uint32_t, LinkExemptFlags>::const_iterator it = m_linkExempts.find(groupId);
    if (it != m_linkExempts.end()) return it->second;
    return GetDefaultLinkExemptFlags();
}

void ParticleSystem::setLinkExemptFlags(uint32_t                 groupId,
                                         const LinkExemptFlags&   flags)
{
    if (groupId == 0) return;                                  // not a valid group
    // Normalize: don't store an entry that equals the v1 defaults.
    // Keeps files without customisation byte-identical to the earlier output.
    if (flags == GetDefaultLinkExemptFlags())
    {
        m_linkExempts.erase(groupId);
        return;
    }
    m_linkExempts[groupId] = flags;
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

void ParticleSystem::ValidateEmitterGraph()
{
    const size_t n = m_emitters.size();

    // Pass 1: sanitise spawn indices so the graph is a single-parent forest.
    //  (a) out-of-range index -> "no spawn". A malformed file (external tool,
    //      or an old editor that didn't fix cross-references on delete) can
    //      store an index past the end of the list; m_emitters[bad] would
    //      otherwise trip the debug "vector subscript out of range" assert.
    //  (b) self-link (an emitter spawning itself) -> clear.
    //  (c) a child already claimed by an earlier parent slot -> clear, so no
    //      child has two parents (otherwise deleteEmitter()'s recursion and
    //      the EmitterList tree rebuild visit it twice -> double-free).
    std::vector<bool> claimed(n, false);
    for (size_t i = 0; i < n; i++)
    {
        Emitter* e = m_emitters[i];
        size_t* slots[2] = { &e->spawnOnDeath, &e->spawnDuringLife };
        for (size_t s = 0; s < 2; s++)
        {
            const size_t c = *slots[s];
            if (c == (size_t)-1) continue;
            const char* reason = NULL;
            if      (c >= n)      reason = "out of range";
            else if (c == i)      reason = "self-link";
            else if (claimed[c])  reason = "already parented";
            if (reason != NULL)
            {
                printf("[Load] emitter %zu '%s' has invalid spawn index %zu (%s); clearing\n",
                       i, e->name.c_str(), c, reason); fflush(stdout);
                *slots[s] = (size_t)-1;
            }
            else
            {
                claimed[c] = true;
            }
        }
    }

    // Pass 2: break cycles. After pass 1 every node has in-degree <= 1, so any
    // remaining cycle is a simple loop; an iterative DFS clears the back-edge
    // that closes it. color: 0 = unvisited, 1 = on the current path, 2 = done.
    // Iterative (not recursive) so a deep chain can't overflow the stack.
    std::vector<int> color(n, 0);
    for (size_t root = 0; root < n; root++)
    {
        if (color[root] != 0) continue;
        std::vector< std::pair<size_t, int> > stack;
        color[root] = 1;
        stack.push_back(std::make_pair(root, 0));
        while (!stack.empty())
        {
            const size_t u       = stack.back().first;
            const int    slotIdx = stack.back().second;
            if (slotIdx >= 2)
            {
                color[u] = 2;
                stack.pop_back();
                continue;
            }
            stack.back().second = slotIdx + 1;   // advance before descending
            Emitter* e = m_emitters[u];
            const size_t v = (slotIdx == 0) ? e->spawnOnDeath : e->spawnDuringLife;
            if (v == (size_t)-1) continue;
            if (color[v] == 1)
            {
                // Back-edge into the active path: this link closes a cycle.
                printf("[Load] emitter %zu '%s' closes a spawn cycle back to %zu; clearing\n",
                       u, e->name.c_str(), v); fflush(stdout);
                if (slotIdx == 0) e->spawnOnDeath    = (size_t)-1;
                else              e->spawnDuringLife = (size_t)-1;
            }
            else if (color[v] == 0)
            {
                color[v] = 1;
                stack.push_back(std::make_pair(v, 0));
            }
            // color[v] == 2 (already finished) can't occur after pass 1's
            // in-degree<=1 guarantee, and would be a harmless cross-edge.
        }
    }

    // Pass 3: rebuild parent pointers from the now acyclic, single-parent
    // spawn links. Reset to NULL (root) first so a stale parent from a link
    // we just cleared can't survive.
    for (size_t i = 0; i < n; i++)
        m_emitters[i]->parent = NULL;
    for (size_t i = 0; i < n; i++)
    {
        Emitter* e = m_emitters[i];
        if (e->spawnOnDeath    != (size_t)-1) m_emitters[e->spawnOnDeath]   ->parent = e;
        if (e->spawnDuringLife != (size_t)-1) m_emitters[e->spawnDuringLife]->parent = e;
    }
}

size_t ParticleSystem::ImportEmittersFrom(
    ParticleSystem& source,
    const std::vector<size_t>& picks,
    const std::function<std::string(const std::string&)>& makeUniqueName)
{
    if (picks.empty()) return 0;

    // Pass 1: clone each pick as a root in THIS system; build the
    // source→destination index map for Pass 2. A clone is a deep copy via
    // the chunk serialiser in copy=true mode (strips runtime / link state).
    std::map<size_t, size_t> srcToDest;
    std::vector<Emitter*> destEmitters(picks.size(), NULL);
    size_t imported = 0;
    for (size_t i = 0; i < picks.size(); ++i)
    {
        size_t srcIdx = picks[i];
        if (srcIdx >= source.getEmitters().size()) continue;
        Emitter& srcEmit = source.getEmitter(srcIdx);

        MemoryFile* mf = new MemoryFile;
        Emitter* placed = NULL;
        try
        {
            ChunkWriter w(mf);
            srcEmit.copy(w);
            mf->seek(0);
            ChunkReader r(mf);
            // Braced init avoids the most-vexing-parse on `Emitter clone(r);`.
            Emitter clone{r};
            clone.name = makeUniqueName(clone.name);
            placed = addRootEmitter(clone);
        }
        catch (...)
        {
            placed = NULL;
            // A clone failure silently dropped the emitter. Keep the
            // existing behaviour (skip the pick) but make it observable -- the
            // returned `imported` count already lets callers detect partial
            // imports (imported < picks.size()); log the specific drop in debug.
#ifndef NDEBUG
            printf("[ImportEmittersFrom] dropped pick %zu (src idx %zu): "
                   "serialize/clone failed\n", i, srcIdx); fflush(stdout);
#endif
        }
        mf->Release();

        if (placed != NULL)
        {
            srcToDest[srcIdx] = placed->index;
            destEmitters[i]   = placed;
            ++imported;
        }
    }

    // Pass 2: re-map spawn fields. Children whose source-index isn't in the
    // picks set stay -1 (copy=true already cleared them).
    for (size_t i = 0; i < picks.size(); ++i)
    {
        Emitter* dst = destEmitters[i];
        if (dst == NULL) continue;
        const Emitter& src = source.getEmitter(picks[i]);
        auto rebind = [&](size_t srcChildIdx) -> size_t {
            if (srcChildIdx == (size_t)-1) return (size_t)-1;
            auto it = srcToDest.find(srcChildIdx);
            return (it != srcToDest.end()) ? it->second : (size_t)-1;
        };
        dst->spawnDuringLife = rebind(src.spawnDuringLife);
        dst->spawnOnDeath    = rebind(src.spawnOnDeath);
    }

    // Rebuild parent pointers + drop any self / duplicate-parent / cyclic link
    // the re-map could have introduced. Pre-existing emitters revalidate
    // harmlessly (their links are unchanged and already valid).
    ValidateEmitterGraph();

    // Pass 3: re-create source link groups. Bucket imports by source
    // linkGroup; ≥2-member buckets get a fresh destination group. Single-
    // member buckets arrive unlinked (copy=true stripped linkGroup).
    std::map<uint32_t, std::vector<Emitter*>> byGroup;
    for (size_t i = 0; i < picks.size(); ++i)
    {
        if (destEmitters[i] == NULL) continue;
        uint32_t srcGroup = source.getEmitter(picks[i]).linkGroup;
        if (srcGroup != 0)
            byGroup[srcGroup].push_back(destEmitters[i]);
    }
    for (auto& kv : byGroup)
    {
        if (kv.second.size() >= 2)
            CreateLinkGroup(*this, kv.second);
    }

    return imported;
}

ParticleSystem::Emitter* ParticleSystem::addRootEmitter(const ParticleSystem::Emitter& emitter)
{
    Emitter* pEmitter = new Emitter(emitter);
    pEmitter->index = m_emitters.size();
	m_emitters.push_back(pEmitter);
	return pEmitter;
}

ParticleSystem::Emitter* ParticleSystem::addLifetimeEmitter(Emitter* parent, const ParticleSystem::Emitter& emitter)
{
    Emitter* pEmitter = NULL;
    if (parent->spawnDuringLife == -1)
    {
        pEmitter = new Emitter(emitter);
        pEmitter->index  = m_emitters.size();
        pEmitter->parent = parent;
        pEmitter->useBursts = false;  // Life emitter is never bursts
        parent->spawnDuringLife = pEmitter->index;
	    m_emitters.push_back(pEmitter);
    }
    return pEmitter;
}

ParticleSystem::Emitter* ParticleSystem::addDeathEmitter(Emitter* parent, const ParticleSystem::Emitter& emitter)
{
    Emitter* pEmitter = NULL;
    if (parent->spawnOnDeath == -1)
    {
        pEmitter = new Emitter(emitter);
        pEmitter->index  = m_emitters.size();
        pEmitter->parent = parent;
        pEmitter->useBursts = true;  // Death emitter is always infinite bursts
        pEmitter->nBursts   = 0;
        parent->spawnOnDeath = pEmitter->index;
	    m_emitters.push_back(pEmitter);
    }
    return pEmitter;
}

ParticleSystem::Emitter* ParticleSystem::insertEmitterAfter(const Emitter* reference, const Emitter& source)
{
    if (reference == NULL) return NULL;

    Emitter* pEmitter = new Emitter(source);
    // The duplicate is independent: not yet a child of anything, no children
    // of its own. The user can re-link via the existing UI later. The
    // duplicate is also detached from any link group the source belongs
    // to — copy makes a fresh emitter, consistent with "Duplicate" semantics.
    pEmitter->parent          = NULL;
    pEmitter->spawnOnDeath    = (size_t)-1;
    pEmitter->spawnDuringLife = (size_t)-1;
    pEmitter->detachFromLinkGroup();

    const size_t insertAt = reference->index + 1;

    // Shift indices of every emitter at insertAt and above up by one. Walk
    // back-to-front so each emitter's parent spawn-field is checked against
    // its current (pre-shift) index — mirrors deleteEmitter's forward shift.
    for (size_t i = m_emitters.size(); i > insertAt; i--)
    {
        Emitter* e = m_emitters[i - 1];
        if (e->parent != NULL)
        {
            if      (e->parent->spawnDuringLife == e->index) e->parent->spawnDuringLife = e->index + 1;
            else if (e->parent->spawnOnDeath    == e->index) e->parent->spawnOnDeath    = e->index + 1;
        }
        e->index = e->index + 1;
    }

    pEmitter->index = insertAt;
    m_emitters.insert(m_emitters.begin() + insertAt, pEmitter);
    return pEmitter;
}

namespace {

// Rewrite each moved emitter's parent spawn-field index to the emitter's new
// `index`, after a root reorder has permuted m_emitters and reassigned indices.
//
// Two-pass: snapshot every affected parent's two slot fields (spawnDuringLife /
// spawnOnDeath) BEFORE writing any of them, then match each child's pre-move
// index (`oldIndices[k]`) against the SNAPSHOT — never the live field. The
// single-pass read-modify-write this replaces compared against the live field
// and silently SWAPPED a parent's life/death children whenever one child's new
// index aliased a sibling's old index. `moved[k]->index` is
// already the new index; `moved` and `oldIndices` are parallel, new-layout order.
//
// Shared by moveEmitter / moveEmitterToRootIndex / reorderManyRootsToIndex so
// the rewrite lives in exactly one place (the three were hand-copied KEEP-IN-
// SYNC loops, which is how the swap escaped into all three).
void rewriteParentSpawnIndices(const std::vector<ParticleSystem::Emitter*>& moved,
                               const std::vector<size_t>& oldIndices)
{
    std::unordered_map<ParticleSystem::Emitter*, std::pair<size_t, size_t>> origSlots;
    for (ParticleSystem::Emitter* e : moved)
        if (e->parent != NULL)
            origSlots.emplace(e->parent,
                std::make_pair(e->parent->spawnDuringLife, e->parent->spawnOnDeath));

    for (size_t k = 0; k < moved.size(); k++)
    {
        ParticleSystem::Emitter* e = moved[k];
        if (e->parent == NULL) continue;
        const std::pair<size_t, size_t>& orig = origSlots[e->parent];
        if      (orig.first  == oldIndices[k]) e->parent->spawnDuringLife = e->index;
        else if (orig.second == oldIndices[k]) e->parent->spawnOnDeath    = e->index;
    }
}

} // namespace

bool ParticleSystem::moveEmitter(Emitter* emitter, int direction)
{
    if (emitter == NULL || emitter->parent != NULL) return false;
    if (direction != -1 && direction != 1) return false;

    // Walk m_emitters once to find the neighbor root in the requested
    // direction. Tracks `prev` only across roots so a non-root child sitting
    // between two roots in vector order doesn't get mistaken for a neighbor.
    Emitter* neighbor = NULL;
    Emitter* prevRoot = NULL;
    bool sawSelf = false;
    for (size_t i = 0; i < m_emitters.size(); i++)
    {
        Emitter* e = m_emitters[i];
        if (e->parent != NULL) continue;
        if (e == emitter)
        {
            sawSelf = true;
            if (direction == -1) { neighbor = prevRoot; break; }
        }
        else if (sawSelf && direction == 1)
        {
            neighbor = e;
            break;
        }
        prevRoot = e;
    }
    if (neighbor == NULL) return false;

    // Collect each subtree in vector order. Pre-order via spawn-field
    // traversal would also work, but iterating m_emitters preserves the
    // current intra-subtree ordering — important when a subtree isn't
    // contiguous and the user has implicitly chosen which order siblings
    // appear by some earlier reorder step.
    auto markSubtree = [&](Emitter* root, std::vector<bool>& mark) {
        // Iterative DFS to avoid recursion stack worries on large systems.
        std::vector<Emitter*> stack;
        stack.push_back(root);
        while (!stack.empty())
        {
            Emitter* e = stack.back(); stack.pop_back();
            mark[e->index] = true;
            if (e->spawnDuringLife != (size_t)-1) stack.push_back(m_emitters[e->spawnDuringLife]);
            if (e->spawnOnDeath    != (size_t)-1) stack.push_back(m_emitters[e->spawnOnDeath]);
        }
    };

    std::vector<bool> inA(m_emitters.size(), false);
    std::vector<bool> inB(m_emitters.size(), false);
    markSubtree(emitter,  inA);
    markSubtree(neighbor, inB);

    // Vector-ordered member lists, plus combined occupied positions.
    std::vector<Emitter*> subtreeA, subtreeB;
    std::vector<size_t>   occupied;
    for (size_t i = 0; i < m_emitters.size(); i++)
    {
        if (inA[i]) { subtreeA.push_back(m_emitters[i]); occupied.push_back(i); }
        if (inB[i]) { subtreeB.push_back(m_emitters[i]); occupied.push_back(i); }
    }
    std::sort(occupied.begin(), occupied.end());

    // direction = +1 (down):  B-subtree fills the lower positions, A-subtree the upper.
    // direction = -1 (up):    A-subtree fills the lower, B-subtree the upper.
    std::vector<Emitter*> reorder;
    reorder.reserve(occupied.size());
    if (direction == 1)
    {
        for (Emitter* e : subtreeB) reorder.push_back(e);
        for (Emitter* e : subtreeA) reorder.push_back(e);
    }
    else
    {
        for (Emitter* e : subtreeA) reorder.push_back(e);
        for (Emitter* e : subtreeB) reorder.push_back(e);
    }

    // Capture old indices before overwriting m_emitters slots, so we can
    // identify which spawn-field on each parent referenced this child.
    std::vector<size_t> oldIndices(reorder.size());
    for (size_t k = 0; k < reorder.size(); k++) oldIndices[k] = reorder[k]->index;

    // Place each moved emitter at its new occupied slot and update the
    // emitter's own index field.
    for (size_t k = 0; k < occupied.size(); k++)
    {
        m_emitters[occupied[k]] = reorder[k];
        reorder[k]->index       = occupied[k];
    }

    // Rewrite parent spawn-field indices that referenced any moved emitter.
    // The parent pointer is stable across this operation; only the integer
    // index it stored has shifted. (Two-pass; see rewriteParentSpawnIndices.)
    rewriteParentSpawnIndices(reorder, oldIndices);

    return true;
}

bool ParticleSystem::moveEmitterToRootIndex(Emitter* emitter, size_t targetRootIndex)
{
    if (emitter == NULL || emitter->parent != NULL) return false;

    // Build the current root order. Root index = position in this list.
    std::vector<Emitter*> roots;
    roots.reserve(m_emitters.size());
    size_t sourceRootIdx = (size_t)-1;
    for (size_t i = 0; i < m_emitters.size(); i++)
    {
        if (m_emitters[i]->parent == NULL)
        {
            if (m_emitters[i] == emitter) sourceRootIdx = roots.size();
            roots.push_back(m_emitters[i]);
        }
    }
    if (sourceRootIdx == (size_t)-1) return false;     // not actually a root in this system
    if (targetRootIndex > roots.size()) return false;  // out of range

    // No-op detection: dropping at gap [sourceRootIdx, sourceRootIdx+1]
    // leaves the layout unchanged. (Gap K means "before root K"; gap == N
    // means "below last root".)
    if (targetRootIndex == sourceRootIdx || targetRootIndex == sourceRootIdx + 1) return false;

    // Compute the new root order. Remove the source first, then re-insert
    // at the target index, adjusting for the removal-shift if the target
    // is below the source.
    std::vector<Emitter*> newRoots;
    newRoots.reserve(roots.size());
    for (size_t i = 0; i < roots.size(); i++)
    {
        if (i != sourceRootIdx) newRoots.push_back(roots[i]);
    }
    size_t insertAt = (targetRootIndex > sourceRootIdx) ? targetRootIndex - 1 : targetRootIndex;
    newRoots.insert(newRoots.begin() + insertAt, emitter);

    // Collect each root's subtree in m_emitters vector order. Iterating
    // m_emitters preserves intra-subtree ordering (matches the convention
    // moveEmitter already established for adjacent swaps); pre-order via
    // spawn-field traversal would also work but produces a different
    // ordering when a subtree's emitters aren't contiguous in m_emitters.
    auto rootOf = [this](Emitter* e) -> Emitter* {
        while (e->parent != NULL) e = e->parent;
        return e;
    };

    std::vector<std::vector<Emitter*>> subtrees(roots.size());
    std::vector<size_t> rootOrderIdx(m_emitters.size(), (size_t)-1);
    for (size_t i = 0; i < roots.size(); i++) rootOrderIdx[roots[i]->index] = i;

    for (size_t i = 0; i < m_emitters.size(); i++)
    {
        Emitter* e = m_emitters[i];
        Emitter* r = rootOf(e);
        size_t   k = rootOrderIdx[r->index];
        subtrees[k].push_back(e);
    }

    // Reassemble m_emitters by walking the new root order and concatenating
    // each root's subtree.
    std::vector<Emitter*> reordered;
    reordered.reserve(m_emitters.size());
    std::vector<size_t>   oldIndices;
    oldIndices.reserve(m_emitters.size());
    for (Emitter* r : newRoots)
    {
        // Find the original index of this root in `roots` so we can copy
        // its already-collected subtree.
        for (size_t k = 0; k < roots.size(); k++)
        {
            if (roots[k] == r)
            {
                for (Emitter* e : subtrees[k])
                {
                    oldIndices.push_back(e->index);
                    reordered.push_back(e);
                }
                break;
            }
        }
    }

    // Install the new layout and reassign indices.
    m_emitters = reordered;
    for (size_t i = 0; i < m_emitters.size(); i++) m_emitters[i]->index = i;

    // Rewrite parent spawn-field indices that referenced any moved emitter.
    // The parent pointer is stable across this operation; only the integer
    // index it stored has shifted. (Two-pass; see rewriteParentSpawnIndices.)
    rewriteParentSpawnIndices(reordered, oldIndices);

    return true;
}

bool ParticleSystem::reorderManyRootsToIndex(
        const std::vector<Emitter*>& selection, size_t gap,
        std::vector<size_t>& outNewIds)
{
    // 1. Current root order (root index = position in this list).
    std::vector<Emitter*> roots;
    roots.reserve(m_emitters.size());
    for (size_t i = 0; i < m_emitters.size(); i++)
        if (m_emitters[i]->parent == NULL) roots.push_back(m_emitters[i]);
    const size_t N = roots.size();
    if (gap > N) return false;                          // out of range (gap is 0..N)

    // 2. Map selection -> ascending source root indices. Reject a null /
    //    missing / non-root id, or an empty selection.
    std::vector<char> selFlag(N, 0);
    std::set<size_t> uniq;                              // dedupe + ascending
    {
        std::unordered_map<Emitter*, size_t> rootPos;
        for (size_t r = 0; r < N; r++) rootPos[roots[r]] = r;
        for (Emitter* e : selection)
        {
            auto it = rootPos.find(e);
            if (it == rootPos.end()) return false;      // null, missing, or non-root
            uniq.insert(it->second);
        }
    }
    if (uniq.empty()) return false;
    std::vector<size_t> selRootIdx(uniq.begin(), uniq.end());
    const size_t M = selRootIdx.size();
    for (size_t r : selRootIdx) selFlag[r] = 1;
    const size_t first = selRootIdx.front();
    const size_t last  = selRootIdx.back();

    // 3. No-op: an already-contiguous block dropped anywhere on its own
    //    footprint [first, last+1] (edges AND interior gaps). Mirrors the
    //    mock reorderManyRoots; generalises moveEmitterToRootIndex's rule.
    if (last - first + 1 == M && gap >= first && gap <= last + 1) return false;

    // 4. New root order: unselected roots (in order) with the selected block
    //    spliced in at the shift-corrected insertion point. `removedBeforeGap`
    //    is the count of selected roots strictly before the gap (the multi
    //    generalisation of moveEmitterToRootIndex's single -1 shift).
    std::vector<Emitter*> rest, block;
    rest.reserve(N - M);
    block.reserve(M);
    for (size_t r = 0; r < N; r++) (selFlag[r] ? block : rest).push_back(roots[r]);
    size_t removedBeforeGap = 0;
    for (size_t r : selRootIdx) if (r < gap) removedBeforeGap++;
    const size_t insertAt = gap - removedBeforeGap;
    std::vector<Emitter*> newRoots;
    newRoots.reserve(N);
    newRoots.insert(newRoots.end(), rest.begin(), rest.begin() + insertAt);
    newRoots.insert(newRoots.end(), block.begin(), block.end());
    newRoots.insert(newRoots.end(), rest.begin() + insertAt, rest.end());

    // 5. Reassemble m_emitters by subtree + reassign indices + rewrite parent
    //    spawn fields. Copied from moveEmitterToRootIndex (KEEP IN SYNC):
    //    collect each root's subtree in m_emitters order, concatenate per the
    //    new root order, reassign index = position, fix spawnDuringLife /
    //    spawnOnDeath references that pointed at a moved emitter.
    auto rootOf = [this](Emitter* e) -> Emitter* {
        while (e->parent != NULL) e = e->parent;
        return e;
    };
    std::vector<std::vector<Emitter*>> subtrees(N);
    std::vector<size_t> rootOrderIdx(m_emitters.size(), (size_t)-1);
    for (size_t i = 0; i < N; i++) rootOrderIdx[roots[i]->index] = i;
    for (size_t i = 0; i < m_emitters.size(); i++)
    {
        Emitter* e = m_emitters[i];
        Emitter* r = rootOf(e);
        size_t   k = rootOrderIdx[r->index];
        subtrees[k].push_back(e);
    }
    std::vector<Emitter*> reordered;
    reordered.reserve(m_emitters.size());
    std::vector<size_t> oldIndices;
    oldIndices.reserve(m_emitters.size());
    for (Emitter* r : newRoots)
    {
        for (size_t k = 0; k < N; k++)
        {
            if (roots[k] == r)
            {
                for (Emitter* e : subtrees[k])
                {
                    oldIndices.push_back(e->index);
                    reordered.push_back(e);
                }
                break;
            }
        }
    }
    m_emitters = reordered;
    for (size_t i = 0; i < m_emitters.size(); i++) m_emitters[i]->index = i;
    // Two-pass spawn-field rewrite (see rewriteParentSpawnIndices).
    rewriteParentSpawnIndices(reordered, oldIndices);

    // 6. newIds = the block roots' final positional indices (contiguous run,
    //    in the block's tree order).
    outNewIds.clear();
    outNewIds.reserve(M);
    for (Emitter* r : block) outNewIds.push_back(r->index);
    return true;
}

// Walk `candidate`'s parent chain looking for `ancestor`. Returns true
// if candidate IS ancestor or appears anywhere in ancestor's subtree.
//
// Used by reparentEmitter for cycle detection: dropping source onto
// target is a cycle iff target is in source's subtree, which by the
// equivalence above is the same as "ancestor=source is reachable
// from candidate=target by walking up parent pointers."
//
// Bottom-up walk (via parent pointers) rather than top-down (via
// spawn fields) so this function itself can't recurse into a
// malformed cycle. Bounded by tree depth, which in practice is in
// the single digits.
static bool IsInSubtreeOf(const ParticleSystem::Emitter* candidate,
                          const ParticleSystem::Emitter* ancestor)
{
    if (candidate == NULL || ancestor == NULL) return false;
    const ParticleSystem::Emitter* p = candidate;
    while (p != NULL)
    {
        if (p == ancestor) return true;
        p = p->parent;
    }
    return false;
}

bool ParticleSystem::reparentEmitter(Emitter* source, Emitter* target, bool useSpawnDuringLife)
{
    // Validation. Bail before any mutation if any check fails — the
    // system stays untouched on a refused reparent.
    if (source == NULL || target == NULL || source == target) return false;

    // Slot-switching under the same parent is out of scope for the v1
    // reparent gesture; refuse so the drag-drop UX doesn't confuse
    // users with a "what just happened?" no-op visual.
    if (source->parent == target) return false;

    // Cycle: target must not be in source's subtree. If it were,
    // putting source under target would make source a descendant of
    // itself.
    if (IsInSubtreeOf(target, source)) return false;

    size_t& targetSlot = useSpawnDuringLife ? target->spawnDuringLife
                                            : target->spawnOnDeath;
    if (targetSlot != (size_t)-1) return false;     // chosen slot occupied

    // Detach from old parent (if any). Compare each slot to source's
    // index explicitly — don't assume Lifetime vs Death; a malformed
    // file or a future bug elsewhere could leave only one of the two
    // referencing source.
    if (source->parent != NULL)
    {
        if      (source->parent->spawnDuringLife == source->index) source->parent->spawnDuringLife = (size_t)-1;
        else if (source->parent->spawnOnDeath    == source->index) source->parent->spawnOnDeath    = (size_t)-1;
    }

    // Attach to new parent. m_emitters position and source's index
    // are unchanged — addLifetimeEmitter / addDeathEmitter establish
    // that vector layout doesn't follow tree layout, so leaving
    // source in place avoids unrelated index churn (and the index
    // itself is what the new spawn slot has to store).
    targetSlot     = source->index;
    source->parent = target;

    return true;
}

void ParticleSystem::deleteEmitter(Emitter* emitter)
{
    // Invalidate its parent references to it
    if (emitter->parent != NULL)
    {
        if (emitter->parent->spawnDuringLife == emitter->index) {
            emitter->parent->spawnDuringLife = -1;
        } else {
            emitter->parent->spawnOnDeath = -1;
        }
    }

    // Delete its children
    if (emitter->spawnDuringLife != -1) deleteEmitter(m_emitters[emitter->spawnDuringLife]);
    if (emitter->spawnOnDeath    != -1) deleteEmitter(m_emitters[emitter->spawnOnDeath]);

    // Adjust indices of the emitters that follow it
    for (size_t i = emitter->index; i < m_emitters.size() - 1; i++)
    {
        Emitter* e = m_emitters[i + 1];
        if (e->parent != NULL) {
            if (e->parent->spawnDuringLife == e->index) {
                e->parent->spawnDuringLife = i;
            } else {
                e->parent->spawnOnDeath = i;
            }
        }
        e->index = i;
    }

    // Remove emitter from list. The Emitter destructor walks m_instances and
    // routes deletion through the owning ParticleSystemInstance, so live
    // EmitterInstance objects don't leave a dangling unique_ptr behind.
    m_emitters.erase( m_emitters.begin() + emitter->index );
    delete emitter;
}

ParticleSystem::~ParticleSystem()
{
    for (size_t i = 0; i < m_emitters.size(); i++)
    {
        delete m_emitters[i];
    }
}
