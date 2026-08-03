#include "Rescale.h"
#include <cmath>
using namespace std;

static void DoRescaleGroup(ParticleSystem::Emitter::Group& group, float scale)
{
    group.cylinderHeight *= scale;
    group.cylinderRadius *= scale;
    group.valX *= scale; group.valY *= scale; group.valZ *= scale;
    group.minX *= scale; group.minY *= scale; group.minZ *= scale;
    group.maxX *= scale; group.maxY *= scale; group.maxZ *= scale;
    group.sideLength   *= scale;
    group.sphereRadius *= scale;
}

void DoRescaleEmitter(ParticleSystem::Emitter* emitter, float timeScale, float sizeScale)
{
    if (timeScale != 1.0f)
    {
        emitter->skipTime     *= timeScale;
        emitter->freezeTime   *= timeScale;
        emitter->initialDelay *= timeScale;
        emitter->lifetime     *= timeScale;
        if (!emitter->isWeatherParticle)
        {
            if (emitter->useBursts)
            {
                emitter->burstDelay *= timeScale;
                float n = (int)(1.0f / emitter->burstDelay * 10000.0f + 0.5f) / 10000.0f;
                if ((int)n == n && emitter->nParticlesPerBurst == 1 && emitter->nBursts == 0)
                {
                    // We can switch to Particles/Second type
                    emitter->useBursts = false;
                    emitter->nParticlesPerSecond = (int)n;
                }
            }
            else
            {
                float n = emitter->nParticlesPerSecond / timeScale;
                if ((int)n != n)
                {
                    // We can't express the scale by staying in the Particle/Second type,
                    // switch to infinite burst mode
                    emitter->useBursts          = true;
                    emitter->nBursts            = 0;
                    emitter->burstDelay         = 1.0f / n;
                    emitter->nParticlesPerBurst = 1;
                }
                else
                {
                    emitter->nParticlesPerSecond = (int)n;
                }
            }

            // Rescale speed and acceleration
            DoRescaleGroup(emitter->groups[ParticleSystem::GROUP_SPEED], 1 / timeScale);
            emitter->inwardSpeed        /= timeScale;
            emitter->inwardAcceleration /= timeScale;
            emitter->acceleration[0]    /= timeScale;
            emitter->acceleration[1]    /= timeScale;
            emitter->acceleration[2]    /= timeScale;
            emitter->gravity            /= timeScale;
        }
    }

    if (sizeScale != 1.0f)
    {
        // Rescale the "Scale" track
        ParticleSystem::Emitter::Track::KeyMap& track = emitter->tracks[ParticleSystem::TRACK_SCALE]->keys;;
        ParticleSystem::Emitter::Track::KeyMap  keys  = track;
        track.clear();
        for (ParticleSystem::Emitter::Track::KeyMap::const_iterator p = keys.begin(); p != keys.end(); p++)
        {
            track.insert(ParticleSystem::Emitter::Track::Key(p->time, p->value * sizeScale));
        }

        // Rescale position and speed groups
        DoRescaleGroup(emitter->groups[ParticleSystem::GROUP_POSITION], sizeScale);
        DoRescaleGroup(emitter->groups[ParticleSystem::GROUP_SPEED],    sizeScale);

        // Rescale speed, acceleration and size
        emitter->inwardSpeed        *= sizeScale;
        emitter->inwardAcceleration *= sizeScale;
        emitter->acceleration[0]    *= sizeScale;
        emitter->acceleration[1]    *= sizeScale;
        emitter->acceleration[2]    *= sizeScale;
        emitter->gravity            *= sizeScale;
        emitter->tailSize           *= sizeScale;
    }
}