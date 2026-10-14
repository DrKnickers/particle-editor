// [houdini] The TGA and PNG decoders the validators trust: round trips,
// row order, every PNG filter, and what they refuse.
import { test } from "node:test";
import assert from "node:assert/strict";
import { deflateSync } from "node:zlib";
import { decodePng, decodeTga, encodeTga, meanRgb } from "./images.mjs";

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
