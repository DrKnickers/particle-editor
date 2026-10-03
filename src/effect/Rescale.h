#ifndef RESCALE_H
#define RESCALE_H

#include "ParticleSystem.h"

// Pure-IO scaling of a single emitter (no UI, no UndoStack). Exposed so
// the bridge dispatcher can iterate over a ParticleSystem and rescale
// each emitter in response to `engine/action/rescale-system` (and one
// emitter for `engine/action/rescale-emitter`).
// `timeScale` and `sizeScale` are multipliers (e.g. 2.0f = 200%).
// Defined in Rescale.cpp, alongside its file-static DoRescaleGroup helper.
void DoRescaleEmitter(ParticleSystem::Emitter* emitter, float timeScale, float sizeScale);

#endif