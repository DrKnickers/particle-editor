#pragma once
// Distance-unit parity anchor.
//
// The editor's world units are the GAME's world units, 1:1. This is proven at the
// VERTEX level: an exported `.alo` stores raw object-space coordinates with no scale
// or unit conversion, and those coordinates are vertex-identical to the MEG-extracted
// shipped assets (n=3 meshes / 2 object classes; a 1% scale would stand ~4.5 orders
// above the float-precision floor and be detected).
// So there is NO length conversion factor -- this constant is 1.0 by measurement,
// and exists as the single documented anchor rather than a magic literal.
//
// EaW art is authored in ABSTRACT engine units (there is no real-world metre mapping
// baked into an asset), so "units" is the only honest distance label.
//
// IMPORTANT: <Scale_Factor> is a SEPARATE concept -- a per-object RENDER multiplier
// (the game draws each object at native_geometry x Scale_Factor; trooper 1.5, AT-AT
// 1.8). It is NOT a unit conversion and must never touch stored `.alo` / particle
// coordinates; it is applied only at reference-object render time (see
// src/rendering/ReferenceObjectWorld.h / Engine::ReferenceObjectWorldFrom).
//
constexpr float EDITOR_UNITS_PER_GAME_UNIT = 1.0f;

// Time base. The game simulates in fixed logic frames, 30 per game second, in
// every game mode (decompiled FrameSynchronizerClass::Init: LogicalFPS = 30).
// Per-frame XML quantities convert with it: a projectile moves <Max_Speed>
// world units each logic frame, so 30 x Max_Speed units/s. Particle fields in
// the .alo are already per second (units/s, units/s^2, seconds) and need no
// conversion; the game steps them once per rendered frame instead.
constexpr float GAME_LOGIC_FPS = 30.0f;
