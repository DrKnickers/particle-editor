#pragma once

// Reconciling an emitter's ALREADY-SCHEDULED next spawn against a changed spawn
// rate (2026-07 audit, an-audit-finding).
//
// onParticleSystemChanged recomputes m_spawnDelay when the user edits the rate,
// but the next spawn was scheduled against the OLD delay and nothing reconciles
// it. Raise the rate on a slow emitter — 1/s to 1000/s — and nothing happens
// until the old one-second delay elapses. The slider looks dead for up to a full
// old-delay, which on a paused preview is indefinite.
//
// The rule is a CLAMP, and the three cases it has to get right are why this is a
// named function rather than an inline assignment:
//
//   - Never DEFER. A spawn already scheduled sooner than the new delay permits
//     stays put; pushing it out would drop a round the emitter had earned.
//   - Never drag an OVERDUE spawn forward. A scheduled time in the past means
//     the emitter is behind and Update's catch-up loop owns it — rewriting it to
//     now + delay silently swallows that round.
//   - Only pull IN a spawn the new, shorter delay says is too far out.
//
// min() expresses all three. `scheduled = now + newDelay`, the obvious-looking
// version, breaks the first two — which is exactly what the tests pin.

typedef float SpawnTimeF;   // mirrors engine.h's TimeF; kept local so this
                            // header stays free of the engine's includes

inline SpawnTimeF ReconcileNextSpawnTime(SpawnTimeF scheduled,
                                         SpawnTimeF now,
                                         SpawnTimeF newDelay)
{
    const SpawnTimeF latest = now + newDelay;
    return (scheduled > latest) ? latest : scheduled;
}
