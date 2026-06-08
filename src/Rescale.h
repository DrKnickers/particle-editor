#ifndef RESCALE_H
#define RESCALE_H

#include "ParticleSystem.h"

bool RescaleParticleSystem(HWND hOwner, ParticleSystem* system);
bool RescaleEmitter(HWND hOwner, ParticleSystem::Emitter* emitter);

// Pure-IO scaling of a single emitter (no UI, no UndoStack). Exposed so
// the new-UI BridgeDispatcher can iterate over a ParticleSystem and
// rescale each emitter in response to `engine/action/rescale-system`.
// `timeScale` and `sizeScale` are multipliers (e.g. 2.0f = 200%).
// Mirrors the inline definition in src/Rescale.cpp:68.
void DoRescaleEmitter(ParticleSystem::Emitter* emitter, float timeScale, float sizeScale);

#endif