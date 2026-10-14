// [houdini] The TGA, PNG and DDS decoders the validators trust: round trips,
// row order, every PNG filter, every DXT block mode, and what they refuse.
import { test } from "node:test";
import assert from "node:assert/strict";
import { deflateSync } from "node:zlib";
import { decodeDds, decodePng, decodeTga, encodeTga, meanRgb } from "./images.mjs";
import { encodeDds } from "./test-atlas.mjs";

const W = 5, H = 4;
const RGBA = Buffer.from(Array.from({ length: W * H * 4 }, (_, i) => (i * 37 + 11) & 0xff));

test("a TGA round-trips, top-down", () => {
  const tga = encodeTga(W, H, RGBA);
  const image = decodeTga(tga);
  assert.deepEqual([image.type, image.colourMapType, image.width, image.height, image.bpp, image.alphaBits, image.topDown],
    [2, 0, W, H, 32, 8, true]);
  assert.deepEqual(image.rgba, RGBA);
});

test("a bottom-up TGA, as most writers leave it, decodes to the same top-down pixels", () => {
  const tga = encodeTga(W, H, RGBA);
  const flipped = Buffer.from(tga);
  flipped[17] = 8; // origin bottom-left
  for (let y = 0; y < H; y++) tga.copy(flipped, 18 + (H - 1 - y) * W * 4, 18 + y * W * 4, 18 + (y + 1) * W * 4);
  assert.deepEqual(decodeTga(flipped).rgba, RGBA);
  // An image ID is skipped.
  const withId = Buffer.concat([flipped.subarray(0, 18), Buffer.from("abc"), flipped.subarray(18)]);
  withId[0] = 3;
  assert.deepEqual(decodeTga(withId).rgba, RGBA);
});

test("a TGA of another kind reports its header and no pixels; a short one is refused", () => {
  const rle = encodeTga(W, H, RGBA); rle[2] = 10;
  assert.deepEqual([decodeTga(rle).type, decodeTga(rle).rgba], [10, null]);
  const rgb = encodeTga(W, H, RGBA); rgb[16] = 24;
  assert.equal(decodeTga(rgb).rgba, null);
  assert.throws(() => decodeTga(Buffer.alloc(10)), (error) => error.exitCode === 4 && /not a TGA/.test(error.message));
  assert.throws(() => decodeTga(encodeTga(W, H, RGBA).subarray(0, 40)), /shorter than its header/);
});

// A PNG with every row under one filter. The decoder skips CRCs, so they are zero here.
function png(width, height, channels, pixels, filter) {
  const stride = width * channels, raw = Buffer.alloc(height * (stride + 1));
  for (let y = 0; y < height; y++) {
    raw[y * (stride + 1)] = filter;
    for (let x = 0; x < stride; x++) {
      const at = (yy, xx) => (yy < 0 || xx < 0 ? 0 : pixels[yy * stride + xx]);
      const left = at(y, x - channels), up = at(y - 1, x), upLeft = at(y - 1, x - channels);
      const p = left + up - upLeft, pa = Math.abs(p - left), pb = Math.abs(p - up), pc = Math.abs(p - upLeft);
      const predictor = [0, left, up, (left + up) >> 1, pa <= pb && pa <= pc ? left : pb <= pc ? up : upLeft][filter] ?? 0;
      raw[y * (stride + 1) + 1 + x] = (pixels[y * stride + x] - predictor) & 0xff;
    }
  }
  const chunk = (type, body) => {
    const head = Buffer.alloc(8); head.writeUInt32BE(body.length, 0); head.write(type, 4, "latin1");
    return Buffer.concat([head, body, Buffer.alloc(4)]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0); ihdr.writeUInt32BE(height, 4); ihdr[8] = 8; ihdr[9] = { 1: 0, 3: 2, 4: 6 }[channels];
  const idat = deflateSync(raw);
  // Split the data over two IDAT chunks, as encoders may.
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk("IHDR", ihdr),
    chunk("IDAT", idat.subarray(0, 7)), chunk("IDAT", idat.subarray(7)), chunk("IEND", Buffer.alloc(0))]);
}

test("PNG: every filter, in grey, RGB and RGBA", () => {
  for (const filter of [0, 1, 2, 3, 4]) {
    assert.deepEqual(decodePng(png(W, H, 4, RGBA, filter)), { width: W, height: H, rgba: RGBA }, `filter ${filter}`);
    const rgb = Buffer.from(Array.from({ length: W * H * 3 }, (_, i) => RGBA[Math.floor(i / 3) * 4 + (i % 3)]));
    const opaque = Buffer.from(RGBA); for (let i = 3; i < opaque.length; i += 4) opaque[i] = 255;
    assert.deepEqual(decodePng(png(W, H, 3, rgb, filter)).rgba, opaque, `RGB filter ${filter}`);
    const grey = Buffer.from(Array.from({ length: W * H }, (_, i) => RGBA[i * 4]));
    const expanded = Buffer.alloc(W * H * 4);
    grey.forEach((v, i) => expanded.set([v, v, v, 255], i * 4));
    assert.deepEqual(decodePng(png(W, H, 1, grey, filter)).rgba, expanded, `grey filter ${filter}`);
  }
});

test("PNG: what the decoder refuses", () => {
  assert.throws(() => decodePng(Buffer.from("not a png at all")), /not a PNG/);
  const sixteen = png(W, H, 4, RGBA, 0); sixteen[8 + 8 + 8] = 16;
  assert.throws(() => decodePng(sixteen), /unsupported PNG \(depth 16/);
  assert.throws(() => decodePng(png(W, H, 4, RGBA, 5)), /row 0 has filter 5/);
  const taller = png(W, H, 4, RGBA, 0); taller.writeUInt32BE(H * 2, 8 + 8 + 4);
  assert.throws(() => decodePng(taller), /truncated/);
  const palette = png(W, H, 4, RGBA, 0); palette[8 + 8 + 9] = 3;
  assert.throws(() => decodePng(palette), /colour type 3/);
});

test("meanRgb averages a box", () => {
  const image = { width: 2, height: 2, rgba: Buffer.from([10, 20, 30, 255, 30, 40, 50, 255, 0, 0, 0, 255, 100, 100, 100, 255]) };
  assert.deepEqual(meanRgb(image, 0, 0, 2, 1), [20, 30, 40]);
  assert.deepEqual(meanRgb(image, 0, 0, 2, 2), [35, 40, 45]);
});

// A DDS from hand-built blocks, so the decoder is checked against the format,
// not against the test encoder.
function dds(width, height, fourCC, blocks, { mips = 1, flags = 4 } = {}) {
  const header = Buffer.alloc(128);
  header.write("DDS ", 0, "latin1"); header.writeUInt32LE(124, 4);
  header.writeUInt32LE(height, 12); header.writeUInt32LE(width, 16); header.writeUInt32LE(mips, 28);
  header.writeUInt32LE(flags, 80); header.write(fourCC, 84, "latin1");
  return Buffer.concat([header, ...blocks]);
}
const colourBlock = (c0, c1, indexOf) => {
  const b = Buffer.alloc(8);
  b.writeUInt16LE(c0, 0); b.writeUInt16LE(c1, 2);
  b.writeUInt32LE(Array.from({ length: 16 }, (_, t) => indexOf(t) << (2 * t)).reduce((a, v) => (a | v) >>> 0, 0), 4);
  return b;
};
const alphaBlock = (a0, a1, indexOf) => {
  const b = Buffer.alloc(8);
  b[0] = a0; b[1] = a1;
  let bits = 0n;
  for (let t = 0; t < 16; t++) bits |= BigInt(indexOf(t)) << BigInt(3 * t);
  for (let i = 0; i < 6; i++) b[2 + i] = Number((bits >> BigInt(8 * i)) & 0xffn);
  return b;
};
const RED = 0xf800, BLUE = 0x001f, GREEN = 0x07e0;
const row = (image, y) => Array.from({ length: image.width }, (_, x) => [...image.rgba.subarray((y * image.width + x) * 4, (y * image.width + x) * 4 + 4)]);

test("DDS: DXT1 in four-colour and three-colour mode, block by block", () => {
  // Block 0: red over blue, four colours, texel t takes index t % 4. Block 1: red under blue,
  // three colours and transparent black. Block 2: one green (equal ends).
  const image = decodeDds(dds(12, 4, "DXT1", [colourBlock(RED, BLUE, (t) => t & 3), colourBlock(BLUE, RED, (t) => t & 3),
    colourBlock(GREEN, GREEN, () => 0)]));
  assert.deepEqual([image.width, image.height, image.mipCount, image.fourCC], [12, 4, 1, "DXT1"]);
  const want = [[255, 0, 0, 255], [0, 0, 255, 255], [170, 0, 85, 255], [85, 0, 170, 255],
    [0, 0, 255, 255], [255, 0, 0, 255], [128, 0, 128, 255], [0, 0, 0, 0], ...Array(4).fill([0, 255, 0, 255])];
  for (let y = 0; y < 4; y++) assert.deepEqual(row(image, y), want, `row ${y}`);
});

test("DDS: DXT5 alpha in eight-level and six-level mode; its colour is always four-colour", () => {
  const image = decodeDds(dds(8, 4, "DXT5", [
    alphaBlock(255, 0, (t) => t & 7), colourBlock(BLUE, RED, (t) => t & 3),
    alphaBlock(0, 255, (t) => t & 7), colourBlock(RED, RED, () => 0)]));
  const eight = [255, 0, 219, 182, 146, 109, 73, 36], six = [0, 255, 51, 102, 153, 204, 0, 255];
  const colours = [[0, 0, 255], [255, 0, 0], [85, 0, 170], [170, 0, 85]];
  for (let y = 0; y < 4; y++) {
    assert.deepEqual(row(image, y).slice(0, 4), [0, 1, 2, 3].map((x) => [...colours[x], eight[(y * 4 + x) & 7]]), `row ${y}`);
    assert.deepEqual(row(image, y).slice(4), [0, 1, 2, 3].map((x) => [255, 0, 0, six[(y * 4 + x) & 7]]), `row ${y}`);
  }
});

test("DDS: a level smaller than a block, and every mip level's blocks, are accounted for", () => {
  // 2x2: one block, of which only the top-left 2x2 is the image. Two mips: 2x2 and 1x1, a block each.
  const image = decodeDds(dds(2, 2, "DXT1", [colourBlock(RED, BLUE, (t) => t & 3), colourBlock(RED, BLUE, () => 0)], { mips: 2 }));
  assert.deepEqual([row(image, 0), row(image, 1)], [[[255, 0, 0, 255], [0, 0, 255, 255]], [[255, 0, 0, 255], [0, 0, 255, 255]]]);
  assert.throws(() => decodeDds(dds(2, 2, "DXT1", [colourBlock(RED, BLUE, () => 0)], { mips: 2 })), /not the 144 its header says/);
  assert.throws(() => decodeDds(Buffer.concat([dds(4, 4, "DXT1", [colourBlock(RED, BLUE, () => 0)]), Buffer.alloc(1)])),
    /137 bytes, not the 136/);
});

test("DDS: another format reports its header and no pixels; a non-DDS is refused", () => {
  assert.deepEqual(decodeDds(dds(4, 4, "DXT3", [Buffer.alloc(16)])), { width: 4, height: 4, mipCount: 1, fourCC: "DXT3", rgba: null });
  assert.equal(decodeDds(dds(4, 4, "DXT1", [Buffer.alloc(8)], { flags: 0x40 })).fourCC, null, "no FourCC flag: uncompressed");
  assert.throws(() => decodeDds(encodeTga(W, H, RGBA)), /not a DDS/);
  assert.throws(() => decodeDds(dds(4, 4, "DXT1", []).subarray(0, 100)), /not a DDS/);
});

test("DDS: the test encoder's output decodes close to its source", () => {
  const rgba = Buffer.from(Array.from({ length: 16 * 8 * 4 }, (_, i) => ((i & 3) === 3 ? (i >> 2) * 2 : (i * 7) & 0xff)));
  for (const fourCC of ["DXT1", "DXT5"]) {
    const image = decodeDds(encodeDds(16, 8, rgba, fourCC));
    assert.deepEqual([image.width, image.height, image.mipCount], [16, 8, 5]);
    if (fourCC === "DXT5") for (let i = 3; i < rgba.length; i += 4) assert.ok(Math.abs(image.rgba[i] - rgba[i]) <= 19, `alpha at ${i}`);
  }
});
