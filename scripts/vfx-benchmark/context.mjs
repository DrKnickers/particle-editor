// The scorer's view of one pack (plan 3.8 clause 3): which frames exist per
// bundle (the selected frames and the filmstrip tiles; telemetry over the
// review window), which emitters and textures, and whether inspect.json is in
// the pack. The case's own inspect always supplies the emitter identities and
// the semantic checks; only citations depend on what the pack carried.
import fs from "node:fs";
import { join } from "node:path";

export const THREE = ["bundle-a", "bundle-b", "bundle-c"];

export function contextFromPack({ packDir, condition, inspect, io = fs }) {
  const read = (path) => JSON.parse(io.readFileSync(path, "utf8"));
  const three = THREE.every((l) => io.existsSync(join(packDir, l, "summary.json")));
  const bundles = {};
  for (const label of three ? THREE : ["bundle"]) {
    const dir = three ? join(packDir, label) : packDir;
    const selected = read(join(dir, "selected.json")).tiles.map((t) => t.frame);
    const film = read(join(dir, "filmstrip.json")).tiles.map((t) => t.frame);
    const summary = read(join(dir, "summary.json"));
    bundles[label] = { imageFrames: [...new Set([...selected, ...film])].sort((a, b) => a - b), firstFrame: summary.reviewWindow[0], lastFrame: summary.reviewWindow[1] };
  }
  return {
    condition, bundles, emitters: inspect.emitters, inspectInPack: io.existsSync(join(packDir, "inspect.json")),
    textures: read(join(packDir, "textures.json")),
  };
}

// The same, for a case of a corpus store.
export function contextFor(s, c, condition, textures, io = fs) {
  const packDir = s.at(condition === "primary" ? "packs" : "packs-bundle-only", c.id);
  return { ...contextFromPack({ packDir, condition, inspect: s.json("cases", c.id, "inspect.json"), io }), textures };
}
