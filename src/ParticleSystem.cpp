#include <iostream>
#include <algorithm>
#include "ParticleSystem.h"
#include "EmitterInstance.h"
#include "ParticleSystemInstance.h"
#include "exceptions.h"
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
			case 0x05: nTriangles				= readInteger(reader) + 1; break;
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
		trackContents[i].interpolation = (Track::InterpolationType)readInteger(reader);
		Verify(reader.nextMini() == -1);

		trackContents[i].keys.insert(first);
		Verify(reader.next() == 0x01);

		ChunkType type;
		while ((type = reader.nextMini()) == 5)
		{
			Track::Key key;
			uint32_t value;
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
		trackContents[i].interpolation = (Emitter::Track::InterpolationType)readInteger(reader);
		Verify(reader.nextMini() == -1);

		trackContents[i].keys.insert(first);
		Verify(reader.next() == 0x01);
		ChunkType type;
		while ((type = reader.nextMini()) == 5)
		{
			Track::Key key;
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

	writer.beginChunk(0x0002);
	writeBool(writer, m_leaveParticles);
	writer.endChunk();

	writer.endChunk();
}

ParticleSystem::ParticleSystem()
{
	m_leaveParticles = true;
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

	    // Read emitters
	    Verify(reader.next() == 0x0800);
	    while ((type = reader.next()) == 0x0700)
	    {
            Emitter* emitter = new Emitter(reader);
            emitter->index = m_emitters.size();
		    m_emitters.push_back(emitter);
	    }
	    Verify(type == -1);

	    // Read leave particles chunk
	    type = reader.next();
	    if (type == 0x0002)
	    {
		    Verify(reader.size() == 1);
		    m_leaveParticles = readBool(reader);
		    type = reader.next();
	    }

	    // End of 0900h chunk
	    Verify(type == -1);

	    // Post-process. Validate spawn-field indices first: malformed files
	    // (saved by external tools or by an old version of the editor that
	    // didn't update cross-references on delete) can store an index that
	    // points past the end of the emitter list. The "!= -1" guard alone
	    // doesn't catch that — m_emitters[badIndex] then trips the debug
	    // assertion "vector subscript out of range" before the file finishes
	    // loading. Treat any out-of-range value as "no spawn".
	    for (unsigned int i = 0; i < m_emitters.size(); i++)
	    {
            Emitter* emitter = m_emitters[i];

            if (emitter->spawnOnDeath != (size_t)-1 && emitter->spawnOnDeath >= m_emitters.size())
            {
                printf("[Load] emitter %u '%s' has spawnOnDeath=%zu out of range (%zu emitters); clearing\n",
                       i, emitter->name.c_str(), emitter->spawnOnDeath, m_emitters.size()); fflush(stdout);
                emitter->spawnOnDeath = (size_t)-1;
            }
            if (emitter->spawnDuringLife != (size_t)-1 && emitter->spawnDuringLife >= m_emitters.size())
            {
                printf("[Load] emitter %u '%s' has spawnDuringLife=%zu out of range (%zu emitters); clearing\n",
                       i, emitter->name.c_str(), emitter->spawnDuringLife, m_emitters.size()); fflush(stdout);
                emitter->spawnDuringLife = (size_t)-1;
            }

		    if (emitter->spawnOnDeath    != (size_t)-1) m_emitters[emitter->spawnOnDeath]   ->parent = emitter;
		    if (emitter->spawnDuringLife != (size_t)-1) m_emitters[emitter->spawnDuringLife]->parent = emitter;
	    }
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
    // of its own. The user can re-link via the existing UI later.
    pEmitter->parent          = NULL;
    pEmitter->spawnOnDeath    = (size_t)-1;
    pEmitter->spawnDuringLife = (size_t)-1;

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
    // index it stored has shifted.
    for (size_t k = 0; k < reorder.size(); k++)
    {
        Emitter* e = reorder[k];
        if (e->parent == NULL) continue;
        if      (e->parent->spawnDuringLife == oldIndices[k]) e->parent->spawnDuringLife = e->index;
        else if (e->parent->spawnOnDeath    == oldIndices[k]) e->parent->spawnOnDeath    = e->index;
    }

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
    // index it stored has shifted.
    for (size_t k = 0; k < reordered.size(); k++)
    {
        Emitter* e = reordered[k];
        if (e->parent == NULL) continue;
        if      (e->parent->spawnDuringLife == oldIndices[k]) e->parent->spawnDuringLife = e->index;
        else if (e->parent->spawnOnDeath    == oldIndices[k]) e->parent->spawnOnDeath    = e->index;
    }

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