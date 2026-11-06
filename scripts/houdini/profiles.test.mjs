// [houdini] Output profiles: the layout, the hoiiotool argument lists, the
// frame stats and each validator, one damaged property at a time.
// The real hoiiotool runs in the `houdini` lane.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { decodeDds, decodeTga, encodeTga } from "./images.mjs";
import { MAX_TEXTURE_BYTES, PAD, atlasLayout, checkFrames, encodeArgs, packArgs, parseFrameStats, profileName,
  statsArgs, texconvArgs, textureName, validateDds, validateTexture } from "./profiles.mjs";
import { encodeDds, frameStatsText, syntheticAtlas } from "./test-atlas.mjs";

const check = (atlas, blend, { bytes, rgba = atlas.rgba } = {}) => {
  const tga = encodeTga(atlas.layout.size, atlas.layout.size, rgba);
  return validateTexture(decodeTga(tga), bytes ?? tga.length, { blend, layout: atlas.layout });
};
const texel = (atlas, x, y) => (y * atlas.layout.size + x) * 4;

test("the layout insets every frame by the guard band, left to right, top to bottom", () => {
  const layout = atlasLayout(4, 32);
  assert.deepEqual([layout.size, layout.frame, layout.pad, layout.cells.length], [128, 28, PAD, 16]);
  assert.deepEqual(layout.cells[0], { index: 0, x: 0, y: 0, frameX: 2, frameY: 2 });
  assert.deepEqual(layout.cells[6], { index: 6, x: 64, y: 32, frameX: 66, frameY: 34 });
  // A single texture keeps the band too.
  assert.deepEqual(atlasLayout(1, 64).cells, [{ index: 0, x: 0, y: 0, frameX: 2, frameY: 2 }]);
  assert.throws(() => atlasLayout(1, 4), (error) => error.exitCode === 1 && /no room/.test(error.message));
});

test("profile and texture names", () => {
  assert.deepEqual([profileName("additive", 4), profileName("transparent", 2), profileName("additive", 1), profileName("transparent", 1)],
    ["eaw-flipbook", "eaw-flipbook", "eaw-additive", "eaw-transparent"]);
  assert.equal(textureName("puff", "0123abcd".padEnd(64, "e"), "dxt5"), "pe_gen_puff_0123abcd.dds");
  assert.equal(textureName("puff", "0123abcd".padEnd(64, "e"), "tga"), "pe_gen_puff_0123abcd.tga");
  assert.equal(textureName("debug-atlas", "0123abcd".padEnd(64, "f")), "pe_gen_debug-atlas_0123abcd.tga");
  assert.ok(textureName("a".repeat(40), "0".repeat(64)).length <= 64, "the longest id still fits the editor's 64-byte names");
});

test("every hoiiotool call runs with expressions and wildcards off", () => {
  const layout = atlasLayout(2, 16), frames = ["frames/frame_0001.exr", "frames/frame_0002.exr", "frames/frame_0003.exr", "frames/frame_0004.exr"];
  for (const args of [packArgs(frames, layout, "packed.exr"), encodeArgs("additive", "packed.exr", "t.tga"),
    encodeArgs("transparent", "packed.exr", "t.tga"), statsArgs(frames)])
    assert.deepEqual(args.slice(0, 2), ["--evaloff", "--wildcardoff"]);
  // The canvas, then each frame swapped under it and pasted at its inset.
  assert.deepEqual(packArgs(frames, layout, "packed.exr"), ["--evaloff", "--wildcardoff",
    "--pattern", "constant:color=0,0,0,0", "32x32", "4",
    frames[0], "--swap", "--paste", "+2+2", frames[1], "--swap", "--paste", "+18+2",
    frames[2], "--swap", "--paste", "+2+18", frames[3], "--swap", "--paste", "+18+18", "-d", "float", "-o", "packed.exr"]);
  assert.throws(() => packArgs(frames.slice(1), layout, "p.exr"), (error) => error.exitCode === 1);
  assert.throws(() => encodeArgs("multiply", "p.exr", "t.tga"), (error) => error.exitCode === 1);
});

test("the encode writes 8-bit, uncompressed, attribute-free TGAs with the blend's alpha", () => {
  const additive = encodeArgs("additive", "packed.exr", "out.tga");
  assert.deepEqual(additive.slice(2, 5), ["packed.exr", "--ch", "R,G,B,A=1.0"]);
  for (const args of [additive, encodeArgs("transparent", "packed.exr", "out.tga")]) {
    assert.deepEqual(args.slice(-10), ["--clamp:min=0:max=1", "--nosoftwareattrib", "--eraseattrib", ".*",
      "-d", "uint8", "--compression", "none", "-o", "out.tga"]);
  }
  // Transparent: (colour, coverage 0/1) over (hole-filled colour, alpha 0) puts
  // the filled colour only UNDER the holes; then the alpha comes back, zeroed
  // where it isn't covered. Coverage is alpha at or above 0.002, which rounds
  // to at least 1 in the file, so the file's alpha-0 texels are exactly the
  // filled ones. The writer divides by alpha, so stored colour comes out straight.
  const coverage = ["packed.exr", "--ch", "A", "--subc", "0.002", "--mulc", "1e9", "--clamp:min=0:max=1"];
  assert.deepEqual(encodeArgs("transparent", "packed.exr", "out.tga").slice(2, -10), [
    "packed.exr", "--ch", "R,G,B", ...coverage, "--chappend",
    "packed.exr", "--fillholes", "--ch", "R,G,B,A=0.0", "--over",
    "--ch", "R,G,B", "packed.exr", "--ch", "A", ...coverage, "--mul", "--chappend"]);
});

test("frame stats: size, channels and non-finite counts per frame", () => {
  const layout = atlasLayout(4, 32);
  const stats = parseFrameStats(frameStatsText(16, 28));
  assert.equal(stats.length, 16);
  assert.deepEqual(stats[3], { width: 28, height: 28, channels: 4, nan: 0, inf: 0 });
  assert.deepEqual(checkFrames(stats, layout), []);
  assert.match(checkFrames(parseFrameStats(frameStatsText(15, 28)), layout)[0], /15 frames, not 16/);
  assert.match(checkFrames(parseFrameStats(frameStatsText(16, 28, { nan: 2 })), layout).join(), /frame 0 has 2 NaN/);
  assert.match(checkFrames(parseFrameStats(frameStatsText(16, 30)), layout).join(), /30x30, not 28x28/);
  assert.match(checkFrames(parseFrameStats(frameStatsText(16, 28, { channels: 3 })), layout).join(), /3 channels/);
  // Counts that never printed are not taken as zero.
  const silent = frameStatsText(16, 28).replace(/ +Stats (NanCount|InfCount).*\n/g, "");
  assert.match(checkFrames(parseFrameStats(silent), layout).join(), /frame 0 has \? NaN and \? Inf/);
  // CRLF output parses the same.
  assert.deepEqual(parseFrameStats(frameStatsText(16, 28).replace(/\n/g, "\r\n")), stats);
});

test("a texture built to its profile passes, additive and transparent", () => {
  for (const blend of ["additive", "transparent"]) {
    for (const [grid, cell] of [[4, 32], [2, 64], [1, 64]])
      assert.deepEqual(check(syntheticAtlas({ grid, cell, blend }), blend), [], `${blend} ${grid}x${cell}`);
  }
});

test("each validator fails on its own damage", () => {
  const additive = syntheticAtlas({ blend: "additive" }), transparent = syntheticAtlas({ blend: "transparent" });
  const damaged = (atlas, edit) => { const rgba = Buffer.from(atlas.rgba); edit(rgba); return rgba; };
  // Additive: opaque everywhere, a black band, no empty cell.
  assert.match(check(additive, "additive", { rgba: damaged(additive, (p) => { p[texel(additive, 10, 10) + 3] = 254; }) }).join(), /1 texels have alpha below 255/);
  assert.match(check(additive, "additive", { rgba: damaged(additive, (p) => { p[texel(additive, 33, 1) + 1] = 2; }) }).join(), /1 guard-band texels are not the black/);
  assert.deepEqual(check(additive, "additive", { rgba: damaged(additive, (p) => { p[texel(additive, 33, 1) + 1] = 1; }) }), [], "1/255 is black enough");
  assert.match(check(additive, "additive", { rgba: damaged(additive, (p) => {
    for (let y = 32; y < 64; y++) for (let x = 64; x < 96; x++) p.fill(0, texel(additive, x, y), texel(additive, x, y) + 3);
  }) }).join(), /cells 6 are empty/);
  // Transparent: a transparent band, colour bled under the edges, no empty cell.
  assert.match(check(transparent, "transparent", { rgba: damaged(transparent, (p) => { p[texel(transparent, 0, 40) + 3] = 1; }) }).join(), /1 guard-band texels are not the fully transparent/);
  const unfilled = (p) => { for (let i = 0; i < p.length; i += 4) if (p[i + 3] === 0) p.fill(0, i, i + 3); };
  assert.match(check(transparent, "transparent", { rgba: damaged(transparent, unfilled) }).join(), /beside coverage are much darker than it: a dark fringe/);
  // The fringe rule is relative: a dark sprite needs its fill too, a dim fill
  // under a bright edge is caught, and a near-black sprite can't show one.
  const tinted = (grey) => (p) => { for (let i = 0; i < p.length; i += 4) p.fill(grey, i, i + 3); };
  assert.deepEqual(check(transparent, "transparent", { rgba: damaged(transparent, tinted(51)) }), [], "a dark grey sprite, filled");
  assert.match(check(transparent, "transparent", { rgba: damaged(transparent, (p) => { tinted(51)(p); unfilled(p); }) }).join(), /dark fringe/);
  assert.match(check(transparent, "transparent", { rgba: damaged(transparent, (p) => {
    for (let i = 0; i < p.length; i += 4) p.fill(p[i + 3] ? 255 : 9, i, i + 3);
  }) }).join(), /dark fringe/);
  assert.deepEqual(check(transparent, "transparent", { rgba: damaged(transparent, (p) => { tinted(12)(p); unfilled(p); }) }), [], "a near-black sprite");
  // Faint texels store colour to 255 / alpha: a grey-60 sprite's alpha-2 edge
  // reads 127. That noise beside a correct fill is not a fringe.
  assert.deepEqual(check(transparent, "transparent", { rgba: damaged(transparent, (p) => {
    tinted(60)(p);
    for (let i = 0; i < p.length; i += 4) if (p[i + 3] > 0 && p[i + 3] < 16) { p[i + 3] = 2; p.fill(127, i, i + 3); }
  }) }), [], "quantisation noise at the faint edge");
  assert.match(check(transparent, "transparent", { rgba: damaged(transparent, (p) => {
    for (let y = 96; y < 128; y++) for (let x = 96; x < 128; x++) p[texel(transparent, x, y) + 3] = 0;
  }) }).join(), /cells 15 are empty/);
  // Whole-texture checks.
  assert.match(check(additive, "additive", { bytes: MAX_TEXTURE_BYTES + 1 }).join(), /over the editor's 64 MiB limit/);
  const wrongSize = validateTexture(decodeTga(encodeTga(64, 64, Buffer.alloc(64 * 64 * 4))), 0, { blend: "additive", layout: additive.layout });
  assert.match(wrongSize.join(), /64x64, not 128x128/);
  const odd = atlasLayout(3, 32);
  assert.match(validateTexture(decodeTga(encodeTga(96, 96, Buffer.alloc(96 * 96 * 4))), 0, { blend: "additive", layout: odd }).join(),
    /side 96 is not a power of two/);
  const noAlpha = encodeTga(128, 128, additive.rgba, { alphaBits: 0 });
  assert.match(validateTexture(decodeTga(noAlpha), noAlpha.length, { blend: "additive", layout: additive.layout })[0], /0 alpha bits/);
  const rle = Buffer.from(additive.tga()); rle[2] = 10;
  assert.match(validateTexture(decodeTga(rle), rle.length, { blend: "additive", layout: additive.layout })[0], /type 10/);
});

test("texconv writes a full mip chain, unfiltered and undithered, beside the source", () => {
  assert.deepEqual(texconvArgs("dxt5", "texture.tga"),
    ["-nologo", "-f", "DXT5", "-m", "0", "-if", "NONE", "-mf", "BOX", "-ft", "DDS", "-o", ".", "texture.tga"]);
  assert.equal(texconvArgs("dxt1", "texture.tga")[2], "DXT1");
  assert.throws(() => texconvArgs("tga", "texture.tga"), (error) => error.exitCode === 1 && /does not write tga/.test(error.message));
});

test("a DDS is held to the TGA it was made from", () => {
  const checkDds = (atlas, { blend, format, dds, bytes } = {}) => {
    const file = dds ?? encodeDds(atlas.layout.size, atlas.layout.size, atlas.rgba, format.toUpperCase());
    return validateDds(decodeDds(file), decodeTga(atlas.tga()), bytes ?? file.length, { format, blend, layout: atlas.layout });
  };
  const additive = syntheticAtlas({ grid: 4, cell: 32 }), transparent = syntheticAtlas({ grid: 4, cell: 32, blend: "transparent" });
  const size = additive.layout.size;
  assert.deepEqual(checkDds(additive, { blend: "additive", format: "dxt1" }), []);
  assert.deepEqual(checkDds(additive, { blend: "additive", format: "dxt5" }), []);
  assert.deepEqual(checkDds(transparent, { blend: "transparent", format: "dxt5" }), []);
  // The header: format, size, mips, the byte limit.
  assert.match(checkDds(additive, { blend: "additive", format: "dxt5", dds: encodeDds(size, size, additive.rgba, "DXT1") })[0],
    /not a DXT5 DDS \(it is DXT1\)/);
  assert.match(checkDds(additive, { blend: "additive", format: "dxt1", dds: encodeDds(64, 64, additive.rgba, "DXT1") }).join(),
    /64x64, not 128x128/);
  assert.match(checkDds(additive, { blend: "additive", format: "dxt1", dds: encodeDds(size, size, additive.rgba, "DXT1", { mips: 1 }) }).join(),
    /1 mip levels, not the full chain of 8/);
  assert.match(checkDds(additive, { blend: "additive", format: "dxt1", bytes: MAX_TEXTURE_BYTES + 1 }).join(), /over the editor's 64 MiB limit/);
  assert.match(checkDds(transparent, { blend: "transparent", format: "dxt1" }).join(), /DXT1 drops the colour under alpha/);
  // The pixels: a flipped sheet, colour far off, alpha far off, alpha lost in DXT1.
  const flipped = Buffer.alloc(size * size * 4);
  for (let y = 0; y < size; y++) additive.rgba.copy(flipped, (size - 1 - y) * size * 4, y * size * 4, (y + 1) * size * 4);
  assert.match(checkDds(additive, { blend: "additive", format: "dxt5", dds: encodeDds(size, size, flipped, "DXT5") }).join(),
    /colour differs from the TGA by an RMSE/);
  // A transparent sheet's colour counts as far as its alpha shows it: wrong under faint
  // texels it passes, wrong under coverage it fails, and so does a flipped sheet.
  const recoloured = (keep) => {
    const out = Buffer.from(transparent.rgba);
    for (let i = 0; i < out.length; i += 4) if (!keep(out[i + 3])) for (let c = 0; c < 3; c++) out[i + c] = 255 - out[i + c];
    return encodeDds(size, size, out, "DXT5");
  };
  assert.deepEqual(checkDds(transparent, { blend: "transparent", format: "dxt5", dds: recoloured((a) => a >= 8) }), []);
  assert.match(checkDds(transparent, { blend: "transparent", format: "dxt5", dds: recoloured((a) => a < 128) }).join(),
    /colour differs from the TGA by an RMSE of [\d., ]+ where its alpha shows it, over 4/);
  const flippedSmoke = Buffer.alloc(size * size * 4);
  for (let y = 0; y < size; y++) transparent.rgba.copy(flippedSmoke, (size - 1 - y) * size * 4, y * size * 4, (y + 1) * size * 4);
  assert.match(checkDds(transparent, { blend: "transparent", format: "dxt5", dds: encodeDds(size, size, flippedSmoke, "DXT5") }).join(),
    /colour differs from the TGA by an RMSE/);
  // Alpha: a broad shift fails the RMSE with no texel far off; one texel far off fails alone.
  const faded = Buffer.from(transparent.rgba);
  for (let i = 3; i < faded.length; i += 4) faded[i] = Math.max(0, faded[i] - 10);
  const fadedErrors = checkDds(transparent, { blend: "transparent", format: "dxt5", dds: encodeDds(size, size, faded, "DXT5") });
  assert.match(fadedErrors.join(), /alpha differs from the TGA by an RMSE of [\d.]+, over 4/);
  assert.doesNotMatch(fadedErrors.join(), /alpha texel/);
  const speck = Buffer.from(transparent.rgba);
  speck[(40 * size + 40) * 4 + 3] = speck[(40 * size + 40) * 4 + 3] > 127 ? 0 : 255;
  assert.deepEqual(checkDds(transparent, { blend: "transparent", format: "dxt5", dds: encodeDds(size, size, speck, "DXT5") }),
    [`a DDS alpha texel differs from the TGA by ${Math.abs(speck[(40 * size + 40) * 4 + 3] - transparent.rgba[(40 * size + 40) * 4 + 3])}, over 32`]);
  const holed = Buffer.from(additive.rgba);
  holed[(10 * size + 10) * 4 + 3] = 0;
  assert.match(checkDds(additive, { blend: "additive", format: "dxt1", dds: encodeDds(size, size, holed, "DXT1") }).join(),
    /texels lost their alpha in DXT1/);
});

test("run_generator.py tells builders the same guard band the packer leaves", () => {
  const source = readFileSync(new URL("./hython/run_generator.py", import.meta.url), "utf8");
  assert.equal(Number(/^PAD = (\d+)$/m.exec(source)?.[1]), PAD);
});
