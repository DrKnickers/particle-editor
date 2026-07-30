#include <cassert>
#include <cstdio>
#include <cstdarg>
#include <windows.h>
#include "EmitterInstance.h"
#include "ParticleCompaction.h"
#include "SpawnSchedule.h"   // ReconcileNextSpawnTime (an-audit-finding rate-edit clamp)
#include "ParticleSystemInstance.h"
using namespace std;

// [norm-dbg] Gated behind the ALO_SHADER_DIAG env var (matches the repo's ALO_*
// test hooks) so normal interactive use stays silent and skips the diagnostic
// block's per-draw texture LockRect; set it for a capture run.
static bool NormDiagEnabled()
{
	static int s_diag = -1;
	if (s_diag < 0) { char b[8]; s_diag = (GetEnvironmentVariableA("ALO_SHADER_DIAG", b, sizeof(b)) > 0) ? 1 : 0; }
	return s_diag != 0;
}

// [norm-dbg] one-shot diagnostic logger for the shaded-smoke s1 normal investigation.
static void NormDbg(const char* fmt, ...)
{
	if (!NormDiagEnabled()) return;
	char buf[1024];
	va_list ap; va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap); va_end(ap);
	OutputDebugStringA(buf); fputs(buf, stdout); fflush(stdout);
}

struct EmitterInstance::Particle : public Object3D
{
	struct TrackCursor
	{
		// The cursor is always between these two keys
		ParticleSystem::Emitter::Track::KeyMap::const_iterator prev;
		ParticleSystem::Emitter::Track::KeyMap::const_iterator next;
	};

	Particle*	   m_next;
	Particle*	   m_prev;
    ParticleBlock* m_block;
    size_t         m_index;

	D3DXVECTOR3 m_initialPosition;
	D3DXVECTOR3 m_systemSpawnPosition;
    D3DXVECTOR3 m_parentSpawnPosition;
	D3DXVECTOR3 m_initialSpeed;
	D3DXVECTOR3	m_acceleration;
	D3DXVECTOR4	m_baseColor;
	float		m_baseScale;
	float       m_rotationDirection;
	float       m_baseRotation;
    TimeF       m_positionTime;
    TimeF       m_bounceTime;
	TimeF		m_spawnTime;
	TimeF		m_deathTime;
    EmitterInstance* m_childEmitter;
	
	TrackCursor m_cursors[ParticleSystem::NUM_TRACKS];

	size_t      m_verticesIndex;
	size_t      m_indicesIndex;

	// Make the (inherited) position public
	void setPosition(const D3DXVECTOR3& position) { m_position = position; }
	void setVelocity(const D3DXVECTOR3& velocity) { m_velocity = velocity; }

    Particle() : Object3D(NULL), m_childEmitter(NULL)
    {
    }

    ~Particle()
    {
        // If we have a child attached, detach it first
        if (m_childEmitter != NULL)
        {
            m_childEmitter->Detach();
            m_childEmitter->StopSpawning();
        }
    }
};

//
// This class holds and manages a block of particles.
// From this block, particles can be allocated and freed. Particles are
// guaranteed not to move in memory after allocated.
// Create ParticleBlocks with new, never on the stack
//
class EmitterInstance::ParticleBlock
{
    uint32_t* m_freeMap;
    Particle* m_particles;
    size_t    m_size;
    size_t    m_base;

public:
    // Allocate a particle. Returns NULL if there are no free particles
    // in this block
    Particle* AllocateParticle()
    {
	    // Find a free group in the map
	    for (size_t i = 0; i < m_size / 32; i++)
	    {
        	size_t   c = 0;
		    uint32_t x = m_freeMap[i];
		    if (x != 0)
		    {
			    if ((x & 0xFFFF) == 0) { c += 16; x >>= 16; }
			    if ((x & 0x00FF) == 0) { c +=  8; x >>=  8; }
			    if ((x & 0x000F) == 0) { c +=  4; x >>=  4; }
			    if ((x & 0x0003) == 0) { c +=  2; x >>=  2; }
			    if ((x & 0x0001) == 0) { c +=  1; }
	            m_freeMap[i] &= ~(1 << c);
	            return &m_particles[i * 32 + c];
		    }
	    }
        return NULL;
    }
    
    // Free the particle
    void FreeParticle(Particle* particle)
    {
        assert(particle->m_block == this);
        size_t index = particle->m_index - m_base;
    	m_freeMap[index / 32] |= (1 << (index % 32));
    }

    // Creates a particle block with the specified size.
    // Allocated particles use the specified base as index into
    // the vertex and index arrays.
    ParticleBlock(size_t base, size_t size)
    {
        m_base      = base;
        m_size      = (size + 31) & -32;
        m_freeMap   = new uint32_t[m_size / 32];
        m_particles = new Particle[m_size];

        for (size_t i = 0; i < m_size / 32; i++)
        {
            m_freeMap[i] = 0xFFFFFFFF;
        }

        for (size_t i = 0; i < m_size; i++)
        {
            m_particles[i].m_block = this;
            m_particles[i].m_index = m_base + i;
        }
    }

    ~ParticleBlock()
    {
        delete[] m_freeMap;
        delete[] m_particles;
    }
};

EmitterInstance::Particle& EmitterInstance::AllocateParticle()
{
    Particle* particle = NULL;
    for (size_t i = 0; i < m_blocks.size(); i++)
    {
        particle = m_blocks[i]->AllocateParticle();
        if (particle != NULL)
        {
            break;
        }
    }

    if (particle == NULL)
    {
	    // We couldn't find a free spot, allocate new particles
        ParticleBlock* block = new ParticleBlock(m_primitives.capacity(), m_primitives.capacity());
        m_blocks.push_back(block);

        m_vertices     .resize (m_vertices     .size()     * 2);
	    m_primitives   .reserve(m_primitives   .capacity() * 2);
	    m_particleIndex.reserve(m_particleIndex.capacity() * 2);

        particle = block->AllocateParticle();
    }

	// Link the particle into the list
	particle->m_next = m_particleList;
	particle->m_prev = NULL;
	if (particle->m_next != NULL)
	{
		particle->m_next->m_prev = particle;
	}
	m_particleList = particle;
    return *particle;
}

void EmitterInstance::FreeParticle(Particle& particle)
{
	// Unlink from list (keep m_next intact though)
	if (particle.m_next != NULL) particle.m_next->m_prev = particle.m_prev;
	if (particle.m_prev != NULL) particle.m_prev->m_next = particle.m_next;
	else                         m_particleList = particle.m_next;

	particle.m_block->FreeParticle(&particle);
}

static void GenerateRandomProperty(const ParticleSystem::Emitter::Group& group, D3DXVECTOR3& value)
{
	switch (group.type)
	{
		case ParticleSystem::GT_EXACT:
			value.x = group.valX;
			value.y = group.valY;
			value.z = group.valZ;
			break;

		case ParticleSystem::GT_BOX:
			value.x = GetRandom(group.minX, group.maxX);
			value.y = GetRandom(group.minY, group.maxY);
			value.z = GetRandom(group.minZ, group.maxZ);
			break;

		case ParticleSystem::GT_CUBE:
			value.x = GetRandom(-group.sideLength, group.sideLength) / 2;
			value.y = GetRandom(-group.sideLength, group.sideLength) / 2;
			value.z = GetRandom(-group.sideLength, group.sideLength) / 2;
			break;

		case ParticleSystem::GT_SPHERE:
		{
			float angleXY = GetRandom(D3DXToRadian(-180), D3DXToRadian(180));
			float angleZ  = GetRandom(D3DXToRadian(- 90), D3DXToRadian( 90));
			float radius  = (group.sphereEdge ? 1.0f : GetRandom(0.0f, 1.0f)) * group.sphereRadius;
			value.x = radius * cosf(angleZ) * cosf(angleXY);
			value.y = radius * cosf(angleZ) * sinf(angleXY);
			value.z = radius * sinf(angleZ);
			break;
		}

		case ParticleSystem::GT_CYLINDER:
		{
			float angleXY = GetRandom(D3DXToRadian(-180), D3DXToRadian(180));
			float radius  = (group.cylinderEdge ? 1.0f : GetRandom(0.0f, 1.0f)) * group.cylinderRadius;
			value.x = radius * cosf(angleXY);
			value.y = radius * sinf(angleXY);
			value.z = GetRandom(0.0f, group.cylinderHeight);
			break;
		}
	}
}

// Resets a particle's appearance and lifetime
void EmitterInstance::ResetParticle(Particle& particle, TimeF currentTime)
{
    particle.m_positionTime = 0;
	particle.m_spawnTime    = currentTime;
	particle.m_deathTime    = particle.m_spawnTime + m_emitter.lifetime * GetRandom(1.0f - m_emitter.randomLifetimePerc, 1.0f);

    particle.m_initialPosition   = particle.GetPosition();

	particle.m_baseScale         = GetRandom(1.0f - m_emitter.randomScalePerc, 1.0f);
	particle.m_rotationDirection = (!m_emitter.randomRotationDirection || GetRandom(0.0, 1.0f) < 0.5) ? 1.0f : -1.0f;
	particle.m_baseRotation      = m_emitter.randomRotation ? m_emitter.randomRotationAverage * (1 + GetRandom(-m_emitter.randomRotationVariance, m_emitter.randomRotationVariance)) : 0.0f;
	if (m_emitter.doColorAddGrayscale)
	{
		particle.m_baseColor.x = particle.m_baseColor.y = particle.m_baseColor.z =
		particle.m_baseColor.w = GetRandom(0.0f, m_emitter.randomColors[0]);
	}
	else
	{
		particle.m_baseColor.x = GetRandom(0.0f, m_emitter.randomColors[0]);
		particle.m_baseColor.y = GetRandom(0.0f, m_emitter.randomColors[1]);
		particle.m_baseColor.z = GetRandom(0.0f, m_emitter.randomColors[2]);
		particle.m_baseColor.w = GetRandom(0.0f, m_emitter.randomColors[3]);
	}

	// Initialize track 'cursors'
	for (int i = 0; i < ParticleSystem::NUM_TRACKS; i++)
	{
		particle.m_cursors[i].next =
		particle.m_cursors[i].prev = m_emitter.tracks[i]->keys.begin();
	}
}

// Spawn a single particle. Returns false when the per-instance uint16
// index cap refused the spawn — callers must NOT count a refused spawn
// (a phantom +1 would permanently inflate Engine::m_numParticles, since
// only REAL particles ever decrement it on death, which would in turn
// eat the overload guard's budget headroom forever).
bool EmitterInstance::SpawnParticle(TimeF currentTime)
{
	Particle& particle = AllocateParticle();

	// Hard index cap. Vertex indices are uint16 (the draw call uses
	// D3DFMT_INDEX16) and m_verticesIndex = m_index * NUM_VERTICES_PER_PARTICLE
	// feeds the (uint16_t) casts that build the index buffer below. Once
	// m_index * NUM_VERTICES_PER_PARTICLE exceeds 0xFFFF those casts wrap and
	// the triangles reference the wrong vertices -- silent render corruption.
	// nParticlesPerBurst / nParticlesPerSecond are read from file without a
	// clamp (weather emitters can instantiate a whole second's worth at once),
	// so refuse to spawn past the ceiling: free the slot and bail. The real
	// fix is 32-bit indexing; this keeps a pathological emitter from
	// corrupting the frame in the meantime.
	const size_t kMaxParticleIndex = 0xFFFF / NUM_VERTICES_PER_PARTICLE;
	if (particle.m_index > kMaxParticleIndex)
	{
#ifndef NDEBUG
		printf("[Particle] uint16 index cap reached at %zu live particles; refusing spawn\n",
		       particle.m_index); fflush(stdout);
#endif
		FreeParticle(particle);
		return false;
	}

	particle.m_verticesIndex = particle.m_index * NUM_VERTICES_PER_PARTICLE;

    // Set and generate properties
    particle.m_systemSpawnPosition = m_system.GetPosition();
    particle.m_parentSpawnPosition = GetPosition();

    GenerateRandomProperty(m_emitter.groups[ParticleSystem::GROUP_SPEED], particle.m_initialSpeed);
	if (m_emitter.affectedByWind)
	{
		particle.m_initialSpeed += m_engine.GetWind();
	}

    if (m_emitter.isWeatherParticle)
    {
        particle.m_acceleration = D3DXVECTOR3(0, 0, 0);
        particle.m_initialPosition.x = GetRandom(-m_emitter.weatherCubeSize / 2, m_emitter.weatherCubeSize / 2);
        particle.m_initialPosition.y = GetRandom(-m_emitter.weatherCubeSize / 2, m_emitter.weatherCubeSize / 2);
        particle.m_initialPosition.z = GetRandom(-m_emitter.weatherCubeSize / 2, m_emitter.weatherCubeSize / 2);

        // Move to weather cube center
        const Engine::Camera& camera = m_engine.GetCamera();
        D3DXVECTOR3 looking = camera.Target - camera.Position;
        D3DXVec3Normalize(&looking, &looking);
        particle.m_initialPosition += camera.Position + looking * m_emitter.weatherCubeDistance;
    }
    else
    {
        D3DXVECTOR3 normpos;
	    GenerateRandomProperty(m_emitter.groups[ParticleSystem::GROUP_POSITION], particle.m_initialPosition);
	    D3DXVec3Normalize(&normpos, &particle.m_initialPosition);

	    particle.m_initialSpeed    -= normpos * m_emitter.inwardSpeed;
	    particle.m_acceleration     = m_acceleration - normpos * m_emitter.inwardAcceleration;
    	particle.m_initialPosition += particle.m_parentSpawnPosition;
    }

    if (m_emitter.groundBehavior == ParticleSystem::GROUND_BOUNCE)
    {
        if (particle.m_acceleration.z != 0)
        {
            // Parabola;
            // Solve x(t) = x(0) + v(0) * t + 0.5 * a * t * t = 0 for t:
            // t = (-b +/- sqrt(b^2 - 4ac)) / 2a =>
            // t = (-v + sqrt(v*v - 2*a*x)) / a
            float D  = sqrtf(particle.m_initialSpeed.z * particle.m_initialSpeed.z - 2 * particle.m_acceleration.z * particle.m_initialPosition.z);
            float t0 = (-particle.m_initialSpeed.z - D) / particle.m_acceleration.z;
            float t1 = (-particle.m_initialSpeed.z + D) / particle.m_acceleration.z;
            particle.m_bounceTime = max(t0, t1);
        }
        else if (particle.m_initialSpeed.z != 0)
        {
            // Linear system;
            // Solve x(t) = x(0) + v(0) * t = 0 for t
            particle.m_bounceTime = -particle.m_initialPosition.z / particle.m_initialSpeed.z;
            if (particle.m_bounceTime < 0)
            {
                // Never bounces
                particle.m_bounceTime = FLT_MAX;
            }
        }
        else
        {
            // Never bounces
            particle.m_bounceTime = FLT_MAX;
        }
    }

    particle.setPosition(particle.m_initialPosition);
    ResetParticle(particle, currentTime);

    // Spawn the child emitter
    particle.m_childEmitter = NULL;
    if (m_emitter.spawnDuringLife != -1)
    {
        particle.m_childEmitter = m_system.SpawnEmitter(currentTime, m_emitter.spawnDuringLife, &particle);
    }

	// Create index
	Primitive prim;
	prim.index[0] = (uint16_t)particle.m_verticesIndex + 0;
	prim.index[1] = (uint16_t)particle.m_verticesIndex + 2;
	prim.index[2] = (uint16_t)particle.m_verticesIndex + 3;
	prim.index[3] = (uint16_t)particle.m_verticesIndex + 2;
	prim.index[4] = (uint16_t)particle.m_verticesIndex + 0;
	prim.index[5] = (uint16_t)particle.m_verticesIndex + 1;
	particle.m_indicesIndex = m_primitives.size();
	m_primitives.push_back(prim);
	m_particleIndex.push_back(&particle);
	return true;
}

void EmitterInstance::UpdateTrackCursors(Particle& particle, float relTime) const
{
	for (int i = 0; i < ParticleSystem::NUM_TRACKS; i++)
	{
		Particle::TrackCursor& cursor = particle.m_cursors[i];
		while (relTime > cursor.next->time)
		{
			if (!m_emitter.randomRotation && i == ParticleSystem::TRACK_ROTATION_SPEED)
			{
				particle.m_baseRotation += IntegrateTrack(particle, ParticleSystem::TRACK_ROTATION_SPEED, cursor.next->time);
			}

			cursor.prev = cursor.next;
			cursor.next++;
			if (cursor.next == m_emitter.tracks[i]->keys.end())
			{
				cursor.next = cursor.prev;
				break;
			}
		}
	}
}

float EmitterInstance::IntegrateTrack(const Particle& particle, int track, float relTime) const
{
	const Particle::TrackCursor& cursor = particle.m_cursors[track];
	
	float v = 0.0f;
	if (cursor.next->time != cursor.prev->time)
	{
		// Normalize time
		float u = (relTime - cursor.prev->time) / (cursor.next->time - cursor.prev->time);
		switch (m_emitter.tracks[track]->interpolation)
		{
			case ParticleSystem::Emitter::Track::IT_SMOOTH:
				// Integration of cubic interpolation:
				// F(u) = 0.5(a - b)u^4 + (b - a)u^3 + a u + C
				v = (cursor.prev->value - cursor.next->value) * u*u*u*u / 2 + (cursor.next->value - cursor.prev->value) * u*u*u + cursor.prev->value * u;
				break;

			case ParticleSystem::Emitter::Track::IT_LINEAR:
				// Integration of linear interpolation:
				// F(u) = a u + 0.5 (b - a) u^2 + C
				v = u * (cursor.prev->value + u * (cursor.next->value - cursor.prev->value) / 2);
				break;

			case ParticleSystem::Emitter::Track::IT_STEP:
				// Integration of step interpolation:
				// F(u) = a u + C
				v = cursor.prev->value * u;
				break;
		}
		// Denormalize time
		v = v * (cursor.next->time - cursor.prev->time) / 100 * (particle.m_deathTime - particle.m_spawnTime);
	}
	return v;
}

float EmitterInstance::SampleTrack(const Particle& particle, int track, float relTime) const
{
	const Particle::TrackCursor& cursor = particle.m_cursors[track];
	if (cursor.next->time == cursor.prev->time)
	{
		return cursor.next->value;
	}

	// See: http://www.gamedev.net/reference/articles/article1497.asp
	switch (m_emitter.tracks[track]->interpolation)
	{
		case ParticleSystem::Emitter::Track::IT_SMOOTH:
		{
			// Cubic interpolation between keys
			float u = (relTime - cursor.prev->time) / (cursor.next->time - cursor.prev->time);
			return cursor.prev->value * (2*u*u*u - 3*u*u + 1) + cursor.next->value * (3*u*u - 2*u*u*u);
		}

		case ParticleSystem::Emitter::Track::IT_LINEAR:
		{
			// Linear interpolation between keys
			float u = (relTime - cursor.prev->time) / (cursor.next->time - cursor.prev->time);
			return cursor.prev->value + u * (cursor.next->value - cursor.prev->value);
		}

		case ParticleSystem::Emitter::Track::IT_STEP:
			return cursor.prev->value;
	}
	return 0.0f;
}

void EmitterInstance::UpdateParticle(Particle& particle, float t)
{
	static const float PI = 3.1415926535897932384626433832795f;

	// Convert to percentage time
	float relTime = t * 100 / (particle.m_deathTime - particle.m_spawnTime);

	UpdateTrackCursors(particle, relTime);

    if (m_emitter.groundBehavior == ParticleSystem::GROUND_BOUNCE)
    {
        while (t > particle.m_bounceTime)
        {
            // The particle has bounced
            float bt = particle.m_bounceTime - particle.m_positionTime;
            particle.m_initialPosition =  particle.m_initialPosition + (particle.m_initialSpeed + 0.5 * particle.m_acceleration * bt) * bt;
            particle.m_initialSpeed    =  particle.m_initialSpeed + particle.m_acceleration * bt;
            particle.m_initialSpeed.z  = -particle.m_initialSpeed.z * m_emitter.bounciness;
            particle.m_positionTime    =  particle.m_bounceTime;

            // Calculate new bounce time
            if (particle.m_acceleration.z == 0 || particle.m_initialSpeed.z == 0)
            {
                // No more bounces
                particle.m_bounceTime = FLT_MAX;
            }
            else
            {
                // Calculate the new parabola
                // We know x(0) is 0, so the problem becomes a lot simpler
                particle.m_bounceTime += 2 * -particle.m_initialSpeed.z / particle.m_acceleration.z;
            }
        }
    }

    const float scaleSample =
        SampleTrack(particle, ParticleSystem::TRACK_SCALE, relTime);
    if (&particle == m_particleList)
    {
        // Keep the diagnostic tied to the value that actually drives rendered
        // geometry. Reading SampleTrack again from the bridge would bypass the
        // paused-idle invalidation contract this cache exists to observe.
        m_liveSampleValid               = true;
        m_liveSampleRelativeTimePercent = relTime;
        m_liveSampleScale               = scaleSample;
    }
	float offset = particle.m_baseScale * scaleSample / 2;

    // Calculate position with constant acceleration:
	// x(t) = x(0) + v(0) * t + 0.5 * a * t * t
    float pt = t - particle.m_positionTime;
	D3DXVECTOR3 position = particle.m_initialPosition + (particle.m_initialSpeed + 0.5 * particle.m_acceleration * pt) * pt;
    position += (m_system.GetPosition() - particle.m_systemSpawnPosition) * (m_emitter.linkToSystem ? 1.0f : 0.0f);
	position += (GetPosition()          - particle.m_parentSpawnPosition) * m_emitter.parentLinkStrength;

    if (m_emitter.isWeatherParticle)
    {
        // Move to weather cube center, modulo box size.
        const Engine::Camera& camera = m_engine.GetCamera();
        D3DXVECTOR3 looking = camera.Target - camera.Position;
        D3DXVec3Normalize(&looking, &looking);
        D3DXVECTOR3 center = camera.Position + looking * m_emitter.weatherCubeDistance;
        
        float w = m_emitter.weatherCubeSize;
        position.x = fmodf(fmodf(position.x - center.x + w/2, w) + w, w) - w/2 + center.x;
        position.y = fmodf(fmodf(position.y - center.y + w/2, w) + w, w) - w/2 + center.y;
        position.z = fmodf(fmodf(position.z - center.z + w/2, w) + w, w) - w/2 + center.z;
    }
    else switch (m_emitter.groundBehavior)
    {
        case ParticleSystem::GROUND_DISAPPEAR: // Disappear
            if (position.z < 0.0f) offset = 0.0f;
            break;

        case ParticleSystem::GROUND_STICK: // Stick
            if (position.z < 0.0f) position.z = 0.0f;
            break;

        default: break;
    }
	particle.setPosition(position);

	float rotation = particle.m_baseRotation;
	if (!m_emitter.randomRotation)
	{
		rotation += IntegrateTrack(particle, ParticleSystem::TRACK_ROTATION_SPEED, relTime);
	}
	float angle = 2 * PI * rotation * particle.m_rotationDirection;

	Vertex* verts = &m_vertices[particle.m_verticesIndex];
	verts[0].Position = D3DXVECTOR3(-offset,-offset,0);
	verts[1].Position = D3DXVECTOR3( offset,-offset,0);
	verts[2].Position = D3DXVECTOR3( offset, offset,0);
	verts[3].Position = D3DXVECTOR3(-offset, offset,0);
    
	// Calculate velocity with constant acceleration:
	// v(t) = v(0) + a * t
    D3DXVECTOR3 velocity = particle.m_initialSpeed + particle.m_acceleration * t;
    if (m_emitter.parentLinkStrength != 0.0f)
    {
        velocity += GetVelocity() * m_emitter.parentLinkStrength;
    }
    particle.setVelocity(velocity);

	if (m_emitter.hasTail)
	{
		// Match in-game behavior: the EaW tail render path orients the
		// quad along velocity and ignores the rotation track entirely.
		// Discovered via P_hp_imperial_damage.alo "Fire Small": rotation
		// values were set, the editor preview rotated, but the game did
		// not. Override the previously-computed angle here.
		angle = 0;

		float length = D3DXVec3Length(&velocity);

        if (length > 0)
        {
            float mult = (m_emitter.parentLinkStrength != 0.0f) ? length / 1000.0f : 1.0f;

            if (!m_emitter.isWorldOriented)
            {
    		    // Transform world-velocity into screen-velocity
		        D3DXVec3TransformCoord(&velocity, &velocity, &m_engine.GetViewRotationMatrix());
            }
		    angle = atan2f(velocity.y, velocity.x) + PI / 4;
		    velocity.z = 0.0f;
		    length = m_emitter.tailSize * mult * D3DXVec3Length(&velocity) / length ;
        }
        verts[3].Position *= max(1.0f, sqrtf(length * length / 2) );
	}

    // Set Normal vector
    verts[0].Normal = D3DXVECTOR3(0,0,1);
    if (!m_emitter.isWorldOriented)
	{
	    // Rotate towards camera
		D3DXVec3TransformCoord(&verts[0].Normal, &verts[0].Normal, &m_engine.GetBillboardMatrix());
    }
    verts[3].Normal = verts[2].Normal = verts[1].Normal = verts[0].Normal;

	for (int i = 0; i < 4; i++)
	{
		// Rotate particle
		float x = verts[i].Position.x;
		verts[i].Position.x = cosf(angle) * x - sinf(angle) * verts[i].Position.y;
		verts[i].Position.y = sinf(angle) * x + cosf(angle) * verts[i].Position.y;

		if (!m_emitter.isWorldOriented)
		{
			// Rotate towards camera
			D3DXVec3TransformCoord(&verts[i].Position, &verts[i].Position, &m_engine.GetBillboardMatrix());
            D3DXVec3TransformCoord(&verts[i].Normal,   &verts[i].Normal,   &m_engine.GetBillboardMatrix());
		}

    	    // Move into position
        verts[i].Position += position;
    }

	// Texture coordinates
	unsigned int texIndex = (unsigned int)floor(SampleTrack(particle, ParticleSystem::TRACK_INDEX, relTime));
    float d = 1.0f / m_textureSizeSqrt;
	float u = (float)(texIndex % m_textureSizeSqrt) / m_textureSizeSqrt;
	float v = (float)(texIndex / m_textureSizeSqrt) / m_textureSizeSqrt;
	verts[3].TexCoord1 = verts[3].TexCoord0 = D3DXVECTOR2(u    , v    );
	verts[2].TexCoord1 = verts[2].TexCoord0 = D3DXVECTOR2(u + d, v    );
	verts[1].TexCoord1 = verts[1].TexCoord0 = D3DXVECTOR2(u + d, v + d);
	verts[0].TexCoord1 = verts[0].TexCoord0 = D3DXVECTOR2(u,     v + d);

	// Color — per-particle tint from curve-editor tracks. Verified against the
	// game (in-game vertex-color diagnostic, 2026): the engine writes the
	// curve-editor color into COLOR0 for bump blend modes too, so the previous
	// editor-only override that wrote a rotation-tangent encoding here was
	// diverging the editor from in-game appearance. The bump shader derives
	// its tangent from ddx/ddy of UV in the PS, so it doesn't need vertex
	// color for that purpose.
    D3DXVECTOR4 color = particle.m_baseColor;
    color.x += SampleTrack(particle, ParticleSystem::TRACK_RED_CHANNEL,   relTime);
    color.y += SampleTrack(particle, ParticleSystem::TRACK_GREEN_CHANNEL, relTime);
    color.z += SampleTrack(particle, ParticleSystem::TRACK_BLUE_CHANNEL,  relTime);
	color.w += SampleTrack(particle, ParticleSystem::TRACK_ALPHA_CHANNEL, relTime);
	verts[3].Color = verts[2].Color = verts[1].Color = verts[0].Color = D3DCOLOR_COLORVALUE(color.x, color.y, color.z, color.w);
}

// Kill a particle
int EmitterInstance::KillParticle(TimeF currentTime, Particle& particle)
{
    if (particle.m_childEmitter != NULL)
    {
        // Detach and stop child emitter
        particle.m_childEmitter->Detach();
        particle.m_childEmitter->StopSpawning();
        particle.m_childEmitter = NULL;
    }

    int numParticles = 0;
    if (m_emitter.spawnOnDeath != -1)
    {
        // Spawn child emitter. NULL when the instance budget refused it
        // (overload guard) — the death child is simply dropped.
        EmitterInstance* emitter = m_system.SpawnEmitter(currentTime, m_emitter.spawnOnDeath, &particle);
        if (emitter != NULL)
        {
            emitter->Detach();
            emitter->StopSpawning();
        }
    }

	FreeParticle(particle);
    return numParticles;
}

// Device Reset invalidates every D3DPOOL_DEFAULT resource, and under D3D9Ex the
// D3DX texture helpers silently use DEFAULT (see TextureManager::OnLostDevice).
// The cache drops its own references there, but THESE two are separate, owning
// references — so nothing freed them and nothing re-fetched them, and the
// emitter went on binding a handle the device had invalidated
// (2026-07 audit, an-audit-finding).
void EmitterInstance::ReleaseDeviceTextures()
{
	SAFE_RELEASE(m_pColorTexture);
	SAFE_RELEASE(m_pNormalTexture);
}

void EmitterInstance::ReacquireDeviceTextures(const Engine& engine)
{
	ReleaseDeviceTextures();
	m_pColorTexture  = engine.GetTextureForDeviceReset(m_emitter.colorTexture);
	m_pNormalTexture = engine.GetTextureForDeviceReset(m_emitter.normalTexture);
}

void EmitterInstance::onParticleSystemChanged(const Engine& engine, int track)
{
	if (track == -1)
	{
		// Recalculate composite values
        m_nParticlesPerBurst = (!m_emitter.useBursts) ? 1 : m_emitter.nParticlesPerBurst;
		m_spawnDelay         = (!m_emitter.useBursts) ? 1.0f / m_emitter.nParticlesPerSecond : max(0.01f, m_emitter.burstDelay);   // Ensure burst delay isn't 0
		m_acceleration       = D3DXVECTOR3(m_emitter.acceleration) + m_emitter.gravity * engine.GetGravity();
		m_textureSizeSqrt    = (int)floor(sqrtf((float)max(1, m_emitter.textureSize)));

		// The next spawn was scheduled against the OLD delay, and recomputing
		// m_spawnDelay above does not move it — so raising the rate on a slow
		// emitter changed nothing until the old delay elapsed, and the slider
		// looked dead for up to a full second (2026-07 audit, an-audit-finding).
		// Clamp only after the authored initialDelay has elapsed: never collapse
		// that first wait, defer a spawn, or drag an overdue one forward.
		m_nextSpawnTime = ReconcileNextSpawnTime(m_nextSpawnTime, GetTimeF(),
                                                 m_spawnDelay, m_nextSpawnUsesInitialDelay);

		// Reload resources
		SAFE_RELEASE(m_pColorTexture);
		SAFE_RELEASE(m_pNormalTexture);
		m_pColorTexture  = engine.GetTexture(m_emitter.colorTexture);
		m_pNormalTexture = engine.GetTexture(m_emitter.normalTexture);

        // Default texture stage settings:
        // ColorOp[0]   = Modulate;
        // ColorArg1[0] = Texture;
        // ColorArg2[0] = Diffuse;
        //
        // AlphaOp[0]   = SelectArg1;
        // AlphaArg1[0] = Texture;
        //
        // ColorOp[1]   = Disable;
        // AlphaOp[1]   = Disable;

		// Set blend options
		switch (m_emitter.blendMode)
		{
			default:
				// Opaque
                m_colorOp        = D3DTOP_MODULATE;
				m_alphaSrcBlend  = D3DBLEND_ONE;
				m_alphaDestBlend = D3DBLEND_ZERO;
				break;

			case ParticleSystem::BLEND_ADDITIVE:
				// Additive
                m_colorOp        = D3DTOP_MODULATE;
				m_alphaSrcBlend  = D3DBLEND_ONE;
				m_alphaDestBlend = D3DBLEND_ONE;
				break;

			case ParticleSystem::BLEND_TRANSPARENT:
				// Transparent
                m_colorOp        = D3DTOP_MODULATE;
				m_alphaSrcBlend  = D3DBLEND_SRCALPHA;
				m_alphaDestBlend = D3DBLEND_INVSRCALPHA;
				break;

            case 3:
                m_colorOp        = D3DTOP_ADD;
				m_alphaSrcBlend  = D3DBLEND_ZERO;
				m_alphaDestBlend = D3DBLEND_SRCCOLOR;
                break;

            case 4:
                m_colorOp        = D3DTOP_MODULATE;
				m_alphaSrcBlend  = D3DBLEND_ONE;
				m_alphaDestBlend = D3DBLEND_ONE;
                break;

            case 5:
                m_colorOp        = D3DTOP_MODULATE;
				m_alphaSrcBlend  = D3DBLEND_SRCALPHA;
				m_alphaDestBlend = D3DBLEND_INVSRCALPHA;
                break;

            case 6:
                m_colorOp        = D3DTOP_ADD;
				m_alphaSrcBlend  = D3DBLEND_ZERO;
				m_alphaDestBlend = D3DBLEND_SRCCOLOR;
                break;  // was missing — mode 6 fell into 7.

            case 7:
                m_colorOp        = D3DTOP_MODULATE2X;
				m_alphaSrcBlend  = D3DBLEND_SRCALPHA;
				m_alphaDestBlend = D3DBLEND_INVSRCALPHA;
                break;
		}
	}
	// Cursor reseat runs for BOTH the `-1` (reseat-everything) path and a
	// specific `track`. For `-1` we reseat EVERY track: callers like
	// BridgeDispatcher::propagateLinkGroup reassign a sibling's track
	// multisets via copySharedParamsFrom, then call
	// OnParticleSystemChanged(-1) to fix the orphaned cursors. Previously this
	// reseat lived only in the `else` branch, so the `-1` path left them
	// singular → xtree:181 deref on the next Engine::Update.
	{
		TimeF currentTime = GetTimeF();

		// Reload track cursors on all particles. We must reseat not only the
		// edited `track` but EVERY channel whose `tracks[j]` aliases the same
		// `keys` container — i.e. lock-group members (a channel locked to an
		// earlier one shares its multiset, ParticleSystem.h slot aliasing).
		// An erase on the edited track orphans the cursors of all aliasing
		// channels at once; reseating only `track` would leave the aliased
		// channels' cursors dangling and crash on the next UpdateTrackCursors.
		for (Particle* particle = m_particleList; particle != NULL; particle = particle->m_next)
		{
			float relTime = (float)(currentTime - particle->m_spawnTime) * 100 / (float)(particle->m_deathTime - particle->m_spawnTime);

			for (int t = 0; t < ParticleSystem::NUM_TRACKS; t++)
			{
				// Process the edited track + any channel aliasing it. For
				// track == -1 reseat EVERY track; the `track != -1` guard also
				// short-circuits the otherwise out-of-bounds `tracks[-1]` read.
				if (track != -1 && t != track && m_emitter.tracks[t] != m_emitter.tracks[track]) continue;

				Particle::TrackCursor& cursor = particle->m_cursors[t];
				cursor.prev = cursor.next = m_emitter.tracks[t]->keys.begin();
				while (cursor.next->time < relTime)
				{
					cursor.prev = cursor.next;
					if (++cursor.next == m_emitter.tracks[t]->keys.end())
					{
						cursor.next = cursor.prev;
						break;
					}
				}
			}
		}
	}
}

int EmitterInstance::Update(TimeF currentTime)
{
    int numParticles = 0;
    // A real update pass replaces the cached observation. When Engine skips a
    // paused-idle pass this method is not entered, so the prior rendered sample
    // deliberately remains observable.
    m_liveSampleValid = false;

    if (IsFrozen(currentTime))
	{
		currentTime = m_freezeTime;
	}

    if (!m_emitter.isWeatherParticle)
    {
        // Spawn new particles
        while (!DoneSpawning() && currentTime > m_nextSpawnTime)
        {
            // Overload guard: once the engine-wide spawn budget is gone,
            // snap m_nextSpawnTime PAST currentTime and bail. Two reasons:
            // (a) missed spawns must be DROPPED, not deferred — otherwise
            // resume would fire the whole backlog as one burst; (b) at
            // pathological rates (delay ~1e-9 s) this catch-up loop would
            // otherwise iterate millions of times per frame doing zero-work
            // SpawnParticles calls — a CPU spin even though memory is safe.
            // NoteSpawnSuppressed keeps the overload latch held: bailing
            // here skips the TryConsume refusal that normally flags it.
            if (m_engine.SpawnBudgetExhausted())
            {
                m_engine.NoteSpawnSuppressed();
                m_nextSpawnUsesInitialDelay = false;
                m_nextSpawnTime = currentTime + GetSpawnDelay();
                break;
            }
            int spawned = SpawnParticles(m_nextSpawnTime);
            numParticles += spawned;
            // A round that spawned NOTHING (while asked to spawn
            // something) was refused — by the engine budget mid-round or
            // by the per-instance uint16 index cap. Same treatment as
            // the budget bail above: drop the remaining catch-up rounds
            // (don't churn thousands of refused alloc/free rounds per
            // frame — that starves the render loop and the 4 Hz stats
            // timer) and hold the overload latch: spawning IS being
            // suppressed relative to the authored rate. The
            // m_nParticlesPerBurst > 0 guard keeps authored zero-burst
            // emitters on their legacy timing (a 0-particle round is
            // not a refusal for them).
            if (spawned == 0 && m_nParticlesPerBurst > 0)
            {
                m_engine.NoteSpawnSuppressed();
                m_nextSpawnTime = currentTime + GetSpawnDelay();
                break;
            }
        }
    }

	// We make this static so we don't reallocate every single frame
	static set<size_t> kills;

	for (Particle* particle = m_particleList; particle != NULL; particle = particle->m_next)
	{
		if (particle->m_deathTime < currentTime)
		{
			// It's dead
            if (!m_emitter.isWeatherParticle || DoneSpawning())
            {
                // Remove it (m_next remains intact)
			    numParticles += KillParticle(currentTime, *particle);
			    kills.insert(particle->m_indicesIndex);
			    continue;
            }

            // Weather particles get reset on death
            ResetParticle(*particle, currentTime);
		}

		float t = (float)(currentTime - particle->m_spawnTime);
		UpdateParticle(*particle, t);
	}

	if (!kills.empty())
	{
        const size_t removed = CompactKilledParticleSlots(m_primitives, m_particleIndex, kills);
        numParticles -= static_cast<int>(removed);
		kills.clear();
	}
    return numParticles;
}

bool EmitterInstance::GetFirstLiveParticleSample(LiveParticleSample& sample) const
{
    if (!m_liveSampleValid || m_particleList == NULL) return false;
    sample.emitterId           = static_cast<int>(GetSourceRank());
    sample.relativeTimePercent = m_liveSampleRelativeTimePercent;
    sample.scale               = m_liveSampleScale;
    return true;
}

void EmitterInstance::StopSpawning()
{
    m_doneSpawning = true;
}

void EmitterInstance::Render(IDirect3DDevice9* pDevice)
{
    if (!m_primitives.empty() && m_emitter.visible)
	{
		pDevice->SetTexture(0, m_pColorTexture);
		pDevice->SetTexture(1, m_pNormalTexture);
		static int s_normDbg = 0;
		const bool dbg = NormDiagEnabled() && (s_normDbg < 3);
		if (dbg)
		{
			NormDbg("[norm-dbg] colorTex=%p normalTex=%p blendMode=%lu\n",
			        (void*)m_pColorTexture, (void*)m_pNormalTexture, m_emitter.blendMode);
			if (m_pNormalTexture)
			{
				D3DSURFACE_DESC d; m_pNormalTexture->GetLevelDesc(0, &d);
				NormDbg("[norm-dbg]   normalTex %ux%u fmt=%d levels=%lu\n",
				        d.Width, d.Height, (int)d.Format, (unsigned long)m_pNormalTexture->GetLevelCount());
				D3DLOCKED_RECT lr;
				// The texel dump uses 4-byte/pixel (BGRA) indexing, valid only for the
				// 32-bit uncompressed formats; for a DXT/compressed normal map the locked
				// data is block-compressed and *4 indexing reads garbage, so dump only when
				// the format is 32bpp BGRA-style and report otherwise.
				const bool is32bpp = (d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8 ||
				                      d.Format == D3DFMT_A8B8G8R8 || d.Format == D3DFMT_X8B8G8R8);
				if (SUCCEEDED(m_pNormalTexture->LockRect(0, &lr, NULL, D3DLOCK_READONLY)))
				{
					if (is32bpp)
					{
						unsigned char* b = (unsigned char*)lr.pBits;
						int cx = d.Width/2, cy = d.Height/2;
						unsigned char* c = b + cy*lr.Pitch + cx*4;
						unsigned char* l = b + cy*lr.Pitch + 10*4;
						unsigned char* r = b + cy*lr.Pitch + (d.Width-10)*4;
						NormDbg("[norm-dbg]   texels BGRA center=%u,%u,%u left=%u,%u,%u right=%u,%u,%u\n",
						        c[0],c[1],c[2], l[0],l[1],l[2], r[0],r[1],r[2]);
					}
					else
						NormDbg("[norm-dbg]   texel dump skipped (fmt=%d not 32bpp BGRA)\n", (int)d.Format);
					m_pNormalTexture->UnlockRect(0);
				}
				else NormDbg("[norm-dbg]   LockRect FAILED (texture not in a lockable pool)\n");
			}
		}
		pDevice->SetRenderState(D3DRS_ZENABLE,     !m_emitter.noDepthTest);
		if (IsHeatEmitter())
		{
			pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
			pDevice->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
			pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    		pDevice->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)m_vertices.size(), 2 * (UINT)m_primitives.size(), &m_primitives[0], D3DFMT_INDEX16, &m_vertices[0], sizeof(Vertex));
		}
		else
		{
            D3DXVECTOR3 position = GetPosition();
            D3DXVECTOR4 eyeObjPosition(
                m_engine.GetCamera().Position.x - position.x,
                m_engine.GetCamera().Position.y - position.y,
                m_engine.GetCamera().Position.z - position.z, 
                0);
            
            Effect* pShader = m_engine.GetShader(m_emitter.blendMode);
            const Effect::Handles& handles = pShader->getHandles();
            ID3DXEffect* pEffect = pShader->getD3DEffect();
            pEffect->SetVector(handles.hEyeObjPosition, &eyeObjPosition);
            if (handles.hDistanceFadeVals)
            {
                D3DXVECTOR4 distanceFade(0.0f, 1.0f, 0.0f, 0.0f);   // (0,1)=no fade; editor-only, engine sets real vals in-game
                pEffect->SetVector(handles.hDistanceFadeVals, &distanceFade);
            }

            // [shadow-leak hunt] Env-gated (ALO_DUMP_RSTATE) snapshot of the FULL
            // device state the particle is ABOUT to draw with — captured here, before
            // the shader's Begin/passes set their own state, so a fresh-clean frame
            // can be diffed against a shadow-enable-then-disable LEAKED frame. No-op
            // when the env var is unset; the method self-throttles to ~every 30th frame.
            m_engine.DumpParticleDrawStateIfRequested(m_emitter.blendMode, m_pColorTexture, m_pNormalTexture);

            UINT nPasses;
            pEffect->Begin(&nPasses, 0);
            for (UINT i = 0; i < nPasses; i++)
            {
                pEffect->BeginPass(i);
                // [bump-alpha] Once-per-blend-mode dump of the alpha-deciding device
                // state read INSIDE BeginPass — i.e. exactly what the .fxo pass set
                // (or failed to set) for this draw. This is the probe that
                // discriminates "shader pass carries its blend/alpha-test state"
                // from "the draw inherits stale state" for issue #481. Same
                // ALO_SHADER_DIAG gate as norm-dbg, so production runs stay silent.
                if (NormDiagEnabled() && i == 0)
                {
                    static unsigned s_blendDumped = 0;
                    const unsigned bit = 1u << (m_emitter.blendMode & 31);
                    if (!(s_blendDumped & bit))
                    {
                        s_blendDumped |= bit;
                        DWORD ab=0,sb=0,db=0,ate=0,aref=0,afunc=0,zw=0;
                        pDevice->GetRenderState(D3DRS_ALPHABLENDENABLE,&ab);
                        pDevice->GetRenderState(D3DRS_SRCBLEND,        &sb);
                        pDevice->GetRenderState(D3DRS_DESTBLEND,       &db);
                        pDevice->GetRenderState(D3DRS_ALPHATESTENABLE, &ate);
                        pDevice->GetRenderState(D3DRS_ALPHAREF,        &aref);
                        pDevice->GetRenderState(D3DRS_ALPHAFUNC,       &afunc);
                        pDevice->GetRenderState(D3DRS_ZWRITEENABLE,    &zw);
                        // Sampler mip state read INSIDE BeginPass proves the engine's
                        // particle-bracket setting (engine.cpp, #481) survives the
                        // effect pass — the Prim* .fxo passes set no sampler state,
                        // so these must report the bracket's values at the draw.
                        DWORD mip0=0,mip1=0,bias0=0,bias1=0;
                        pDevice->GetSamplerState(0, D3DSAMP_MIPFILTER,     &mip0);
                        pDevice->GetSamplerState(1, D3DSAMP_MIPFILTER,     &mip1);
                        pDevice->GetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, &bias0);
                        pDevice->GetSamplerState(1, D3DSAMP_MIPMAPLODBIAS, &bias1);
                        float fb0, fb1; memcpy(&fb0, &bias0, sizeof(fb0)); memcpy(&fb1, &bias1, sizeof(fb1));
                        NormDbg("[bump-alpha] blend=%lu passes=%u AB=%lu SRC=%lu DST=%lu AT=%lu AREF=%lu AFUNC=%lu ZW=%lu MIP0=%lu MIP1=%lu BIAS0=%.2f BIAS1=%.2f\n",
                                m_emitter.blendMode, nPasses, ab, sb, db, ate, aref, afunc, zw,
                                mip0, mip1, fb0, fb1);
                    }
                }
                if (dbg && i == 0)
                {
                    IDirect3DBaseTexture9* bt = NULL; pDevice->GetTexture(1, &bt);
                    DWORD minf=0,magf=0,mipf=0,maxml=0,lodb=0;
                    pDevice->GetSamplerState(1, D3DSAMP_MINFILTER,    &minf);
                    pDevice->GetSamplerState(1, D3DSAMP_MAGFILTER,    &magf);
                    pDevice->GetSamplerState(1, D3DSAMP_MIPFILTER,    &mipf);
                    pDevice->GetSamplerState(1, D3DSAMP_MAXMIPLEVEL,  &maxml);
                    pDevice->GetSamplerState(1, D3DSAMP_MIPMAPLODBIAS,&lodb);
                    NormDbg("[norm-dbg] @draw stage1 boundTex=%p (want normalTex=%p) MIN=%lu MAG=%lu MIP=%lu MAXMIP=%lu LODbiasRaw=%lu texSqrt=%d (textureSize=%lu)\n",
                            (void*)bt, (void*)m_pNormalTexture, minf, magf, mipf, maxml, lodb, m_textureSizeSqrt, (unsigned long)m_emitter.textureSize);
                    if (bt) bt->Release();
                    s_normDbg++;
                }
    		    pDevice->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)m_vertices.size(), 2 * (UINT)m_primitives.size(), &m_primitives[0], D3DFMT_INDEX16, &m_vertices[0], sizeof(Vertex));
                pEffect->EndPass();
            }
            pEffect->End();
            SAFE_RELEASE(pEffect);
		}
	}
}

// Spawns another round of particles
int EmitterInstance::SpawnParticles(TimeF spawnTime)
{
    // Overload guard asymmetry, deliberate: a round that gets here but
    // is then refused (budget runs out mid-burst) still consumes
    // m_currentBurst — finite-burst emitters can burn out with fewer
    // particles emitted during overload. "Dropped, not deferred": we
    // never replay refused work, so the burst is spent either way.
    // (The pre-round bail in Update never reaches here, preserving it.)
    if (m_emitter.useBursts && m_emitter.nBursts > 0)
	{
		if (++m_currentBurst == m_emitter.nBursts)
		{
			m_doneSpawning = true;
		}
	}

    int numParticles = 0;
	for (unsigned long i = 0; i < m_nParticlesPerBurst; i++)
	{
        // Overload guard: every spawn spends one unit of the engine-wide
        // per-frame budget (see Engine::kDefaultMaxPreviewParticles). When
        // it runs out, drop the REST of this burst — never deferred.
        if (!m_engine.TryConsumeSpawnBudget())
            break;
        // Per-instance uint16 index cap refusal: the rest of the burst
        // would refuse too (same live-count pressure), so stop counting
        // AND stop iterating.
        if (!SpawnParticle(spawnTime))
            break;
        numParticles++;
	}

    m_nextSpawnUsesInitialDelay = false;
    m_nextSpawnTime = spawnTime + GetSpawnDelay();

    // If the spawn delay beyond the FP addition accuracy,
    // we increase the spawn delay and particles spawned.
    while (m_nextSpawnTime == spawnTime)
    {
        m_spawnDelay         *= 2;
        m_nParticlesPerBurst *= 2;
        m_nextSpawnTime = spawnTime + GetSpawnDelay();
    }

    return numParticles;
}

bool EmitterInstance::IsFrozen(TimeF currentTime) const
{
	return m_freezeTime > 0.0f && currentTime >= m_freezeTime;
}

// Returns the NEGATIVE count of live particles destroyed, i.e. a delta
// callers feed straight into the engine's m_numParticles accounting
// (Engine::KillParticleSystem, ParticleSystemInstance::RemoveEmitter).
int EmitterInstance::Kill()
{
	// Stop spawning
	m_doneSpawning = true;

	// And destroy any live particles
	int numParticles = -static_cast<int>(m_primitives.size());
	m_primitives.clear();
	m_particleIndex.clear();

	m_particleList = nullptr;
	return numParticles;
}

EmitterInstance::EmitterInstance(TimeF currentTime, ParticleSystemInstance& system, Engine& engine, ParticleSystem::Emitter& emitter, Object3D* parent, int* numParticles)
	: Object3D(parent), m_engine(engine), m_system(system), m_emitter(emitter)
{
	m_doneSpawning        = false;
	m_particleList        = NULL;
	m_currentBurst        = 0;
	m_pColorTexture       = NULL;
	m_pNormalTexture      = NULL;
	m_parentSpawnPosition = parent->GetPosition();
	m_freezeTime          = (m_emitter.freezeTime > 0.0f && m_emitter.freezeTime >= m_emitter.skipTime) ? currentTime + m_emitter.freezeTime - m_emitter.skipTime : 0.0f;
	
    // Initial array size (32 particles)
    m_blocks.push_back(new ParticleBlock(0,32));
	m_vertices     .resize(32 * NUM_VERTICES_PER_PARTICLE);
	m_primitives   .reserve(32);
	m_particleIndex.reserve(32);

	onParticleSystemChanged(engine, -1);

	// Spawn initial particles
    if (m_emitter.isWeatherParticle)
    {
        // Spawn all particles immediately for weather particles.
        // Overload guard: budget-gated like every other spawn path, and
        // *numParticles must report what was ACTUALLY spawned (it feeds
        // Engine::OnEmitterCreated's live particle accounting).
        // Accepted edge: a weather instance fully starved here never
        // spawns again and never dies (zombie until Clear/restart).
        *numParticles = 0;
        for (unsigned long i = 0; i < m_emitter.nParticlesPerSecond; i++)
	    {
            if (!m_engine.TryConsumeSpawnBudget())
                break;
		    if (!SpawnParticle(currentTime))
                break;  // uint16 index cap — don't count refused spawns
            (*numParticles)++;
        }
    }
    else
    {
    	TimeF skipped = m_emitter.initialDelay;
	    currentTime  -= m_emitter.skipTime;

        *numParticles = 0;
	    while (skipped <= m_emitter.skipTime && !DoneSpawning())
	    {
            // Overload guard: stop replaying skip-time history once the
            // engine-wide budget is gone (dropped, not deferred) — at
            // pathological rates this loop would otherwise spin for
            // skipTime/delay iterations doing zero-work spawn rounds.
            if (m_engine.SpawnBudgetExhausted())
            {
                m_engine.NoteSpawnSuppressed();
                break;
            }
		    *numParticles += SpawnParticles(currentTime + skipped);
		    skipped += GetSpawnDelay();
	    }
    	
        if (!DoneSpawning())
	    {
		    // Plan the next spawn
            m_nextSpawnTime = currentTime + skipped;
	    }
    }

    m_emitter.registerEmitterInstance(this);
}

EmitterInstance::~EmitterInstance()
{
	SAFE_RELEASE(m_pColorTexture);
	SAFE_RELEASE(m_pNormalTexture);

    for (size_t i = 0; i < m_blocks.size(); i++)
    {
        delete m_blocks[i];
    }

    Particle* parent = dynamic_cast<Particle*>(GetParent());
    if (parent != NULL)
    {
        // Our parent is a particle, clear the child emitter link
        parent->m_childEmitter = NULL;
    }

    m_emitter.unregisterEmitterInstance(this);
}
