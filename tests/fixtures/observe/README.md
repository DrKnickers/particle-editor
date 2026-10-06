# Observation fixture texture

`zz_observe_soft.tga` is the colour texture of the `late-flash` fixture that
`--gen-replay-proof-fixture late-flash` writes (`src/main.cpp`), used by the
`observe` gate lane (`scripts/observe-lane.mjs`). Its smoke cloud is
alpha-blended and its flash additive, so the timing problem the fixture plants
reads by eye in the bundle's frames, not only in its telemetry.

It is procedurally generated (clean-room, not a game asset): 64×64 RGBA, every
channel (R, G, B and A) `round(255 × max(0, 1 − r)²)` where `r` is the pixel
centre's distance from the image centre divided by 32. Uncompressed TGA written
by Pillow. The falloff must be in the colour channels as well as alpha: the
additive blend adds texture colour and ignores alpha, so a white-RGB disc draws
as a hard square.

The fixture references it by a repo-relative path, so it resolves against the
working directory (the repo root in the lane) and needs no game install.
