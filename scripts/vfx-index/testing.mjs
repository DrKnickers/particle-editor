// [vfx-index] Test helpers: a `--tool index` records document built in code, in
// the exact shape the native tool writes (src/host/ToolIndex.cpp).
import { createHash } from "node:crypto";

export const sha = (text) => createHash("sha256").update(text).digest("hex");
export const STACK = sha("[]");
const MODELS = "DATA\\ART\\MODELS\\";

// One record. `texture` is [request, bytes] for a resolved texture, [request] for
// an unresolved one; bytes stand for the file's content (only its hash is kept).
export function record(effect, index, emitter, slot, [request, bytes], {
  blendMode = 1, textureSize = 1, indexValues = [0], world = false, loaderDefault = false } = {}) {
  const canonicalName = request.toUpperCase();
  return { effect: MODELS + effect, emitter: { index, name: emitter }, slot,
    texture: { request, canonicalName, outcome: bytes == null ? "unresolved" : "resolved",
      source: bytes == null ? null : { kind: "archive", path: "D:/game/Data/Textures.meg",
        member: `DATA\\ART\\TEXTURES\\${canonicalName}`, start: 0, size: bytes.length },
      sha256: bytes == null ? null : sha(bytes), loaderDefault },
    blendMode, isWorldOriented: world, textureSize, indexValues };
}

export function recordsDoc(records, { stack = STACK, unverifiable = null, failures = [] } = {}) {
  const files = [...new Set(records.map((r) => r.effect))].sort();
  return { schema: 1, verb: "index", tool: "0.4.2",
    content: { gameRoots: ["D:/game/corruption", "D:/game/GameData"], layers: [], sha256: stack, workingDirectory: "D:/work" },
    summary: { models: files.length, systems: files.length, meshes: 0, failed: failures.length, records: records.length,
      textures: { lookups: records.length, resolved: records.filter((r) => r.texture.outcome === "resolved").length,
        unresolved: records.filter((r) => r.texture.outcome !== "resolved").length, loads: 0 }, unverifiable },
    systems: files.map((file) => ({ file, source: { kind: "archive", path: "D:/game/Data/Models.meg", member: file, start: 0, size: 1 },
      sha256: sha(file), internalName: file.split("\\").pop().replace(/\.ALO$/, "").toLowerCase(),
      emitters: new Set(records.filter((r) => r.effect === file).map((r) => r.emitter.index)).size })),
    failures, records };
}

// A small stock-like stack: a shared master atlas, a smoke atlas, a spark
// flipbook, a fire flipbook used under two atlas layouts, filename competitors,
// a one-cell atlas, the default normal map, a world-oriented shockwave, and one
// texture found nowhere. Index values are distinct, as the tool writes them.
export function stockRecords() {
  const master = ["p_particle_master.tga", "master-bytes"];
  const depth = ["p_particle_depth_master.tga", "depth-bytes"];
  const r = [];
  const add = (effect, i, name, tex, opts, normal = true) => {
    r.push(record(effect, i, name, "color", tex, opts));
    if (normal) r.push(record(effect, i, name, "normal", depth, { ...opts, loaderDefault: true }));
  };
  // The master atlas: everywhere, mostly additive, 8x8.
  for (let n = 0; n < 6; n++) add(`P_EXPLOSION_SMALL0${n}.ALO`, 0, "debris glows", master, { textureSize: 64, indexValues: [3, 39] });
  add("P_SHIP_SMOKE.ALO", 0, "Smoke Lit", master, { blendMode: 2, textureSize: 64, indexValues: [12] });
  add("P_SHIP_SMOKE.ALO", 1, "Smoke Dark", master, { blendMode: 2, textureSize: 64, indexValues: [13] });
  add("P_SHIP_SMOKE.ALO", 3, "spark glints", master, { textureSize: 64, indexValues: [7] });
  // The smoke atlas: transparent smoke, 4x4 flipbook.
  for (let n = 0; n < 3; n++) add(`P_SMOKE_COLUMN0${n}.ALO`, 0, "smoke puff", ["W_Smoke.tga", "smoke-bytes"],
    { blendMode: 2, textureSize: 16, indexValues: [0, 15] });
  // Sparks: additive, animated.
  add("P_EXPLOSION_MED01.ALO", 1, "sparks", ["p_spark_atlas.dds", "spark-bytes"], { textureSize: 16, indexValues: [0, 7] });
  add("P_EXPLOSION_MED01.ALO", 2, "spark trails", ["P_SPARK_ATLAS.DDS", "spark-bytes"], { textureSize: 16, indexValues: [8, 15] });
  // Fire: additive flipbook under two layouts (4x4 and a single frame).
  add("P_EXPLOSION_MED01.ALO", 0, "fire", ["p_fire.tga", "fire-bytes"], { textureSize: 16, indexValues: [0, 15] });
  add("P_TORCH.ALO", 0, "fire", ["p_fire.tga", "fire-bytes"], { textureSize: 16, indexValues: [0, 3] });
  add("P_CANDLE.ALO", 0, "flame", ["p_fire.tga", "fire-bytes"], { textureSize: 1, indexValues: [0, 17] });
  // Filename competitors for "fire": an exact stem and a prefix.
  add("P_FIRE_BRAZIER.ALO", 0, "coals", ["fire.tga", "coal-bytes"], { textureSize: 16, indexValues: [16] });
  add("P_RING.ALO", 0, "halo", ["fire_ring.tga", "halo-bytes"], { blendMode: 2 });
  // A 4x4 atlas each emitter takes one cell of.
  add("P_SHIP_SMOKE.ALO", 2, "debris chunks", ["p_debris_atlas.tga", "debris-bytes"], { blendMode: 2, textureSize: 16, indexValues: [5] });
  // A world-oriented shockwave ring.
  add("P_EXPLOSION_LARGE.ALO", 3, "shockwave ring", ["p_shockwave.dds", "ring-bytes"], { world: true, indexValues: [0] });
  // Found nowhere.
  r.push(record("P_TORCH.ALO", 1, "embers", "color", ["p_missing_embers.tga"]));
  return r;
}
