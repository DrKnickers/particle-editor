// [houdini] The two image formats the validators read, decoded in Node so
// every check is exact and runs without Houdini: the uncompressed 32-bit TGA
// the profile step writes, and the 8-bit PNG frames an observation captures.
// Pixels come back as top-down RGBA bytes.
import { inflateSync } from "node:zlib";

const fail = (message, exitCode = 4) => Object.assign(new Error(message), { exitCode });

// Uncompressed true-colour TGA (type 2), the only kind the profiles write.
// `alphaBits` is the descriptor's low nibble: D3DX reads alpha only when it is 8.
export function decodeTga(bytes) {
  if (bytes.length < 18) throw fail("texture is not a TGA");
  const idLength = bytes[0], colourMapType = bytes[1], type = bytes[2];
  const width = bytes.readUInt16LE(12), height = bytes.readUInt16LE(14), bpp = bytes[16], descriptor = bytes[17];
  const header = { type, colourMapType, width, height, bpp, alphaBits: descriptor & 0x0f, topDown: (descriptor & 0x20) !== 0 };
  if (type !== 2 || colourMapType !== 0 || bpp !== 32) return { ...header, rgba: null };
  const start = 18 + idLength, size = width * height * 4;
  if (bytes.length < start + size) throw fail("texture is shorter than its header says");
  const rgba = Buffer.alloc(size);
  for (let y = 0; y < height; y++) {
    const row = header.topDown ? y : height - 1 - y;
    for (let x = 0; x < width; x++) {
      const from = start + (row * width + x) * 4, to = (y * width + x) * 4;
      rgba[to] = bytes[from + 2]; rgba[to + 1] = bytes[from + 1]; rgba[to + 2] = bytes[from]; rgba[to + 3] = bytes[from + 3];
    }
  }
  return { ...header, rgba };
}

// For tests: a top-down RGBA buffer as an uncompressed 32-bit TGA.
export function encodeTga(width, height, rgba, { alphaBits = 8 } = {}) {
  const out = Buffer.alloc(18 + width * height * 4);
  out[2] = 2; out.writeUInt16LE(width, 12); out.writeUInt16LE(height, 14); out[16] = 32; out[17] = 0x20 | alphaBits;
  for (let i = 0; i < width * height; i++) {
    out[18 + i * 4] = rgba[i * 4 + 2]; out[19 + i * 4] = rgba[i * 4 + 1];
    out[20 + i * 4] = rgba[i * 4]; out[21 + i * 4] = rgba[i * 4 + 3];
  }
  return out;
}

const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

// 8-bit greyscale, RGB or RGBA, non-interlaced: what the observation writes.
export function decodePng(bytes) {
  if (!bytes.subarray(0, 8).equals(PNG_SIGNATURE)) throw fail("frame is not a PNG");
  let width = 0, height = 0, channels = 0;
  const data = [];
  for (let at = 8; at + 8 <= bytes.length;) {
    const length = bytes.readUInt32BE(at), type = bytes.toString("latin1", at + 4, at + 8);
    const body = bytes.subarray(at + 8, at + 8 + length);
    if (type === "IHDR") {
      width = body.readUInt32BE(0); height = body.readUInt32BE(4);
      const depth = body[8], colourType = body[9], interlace = body[12];
      channels = { 0: 1, 2: 3, 6: 4 }[colourType] ?? 0;
      if (depth !== 8 || !channels || interlace !== 0) throw fail(`unsupported PNG (depth ${depth}, colour type ${colourType}, interlace ${interlace})`);
    } else if (type === "IDAT") data.push(body);
    else if (type === "IEND") break;
    at += 12 + length;
  }
  if (!channels) throw fail("PNG has no IHDR");
  const raw = inflateSync(Buffer.concat(data)), stride = width * channels;
  if (raw.length < height * (stride + 1)) throw fail("PNG data is truncated");
  const pixels = Buffer.alloc(height * stride);
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)], line = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    for (let x = 0; x < stride; x++) {
      const left = x >= channels ? pixels[y * stride + x - channels] : 0;
      const up = y > 0 ? pixels[(y - 1) * stride + x] : 0;
      const upLeft = y > 0 && x >= channels ? pixels[(y - 1) * stride + x - channels] : 0;
      let predictor;
      switch (filter) {
        case 0: predictor = 0; break;
        case 1: predictor = left; break;
        case 2: predictor = up; break;
        case 3: predictor = (left + up) >> 1; break;
        case 4: {
          const p = left + up - upLeft, pa = Math.abs(p - left), pb = Math.abs(p - up), pc = Math.abs(p - upLeft);
          predictor = pa <= pb && pa <= pc ? left : pb <= pc ? up : upLeft;
          break;
        }
        default: throw fail(`PNG row ${y} has filter ${filter}`);
      }
      pixels[y * stride + x] = (line[x] + predictor) & 0xff;
    }
  }
  const rgba = Buffer.alloc(width * height * 4);
  for (let i = 0; i < width * height; i++) {
    const p = pixels.subarray(i * channels, i * channels + channels);
    rgba[i * 4] = p[0]; rgba[i * 4 + 1] = channels === 1 ? p[0] : p[1];
    rgba[i * 4 + 2] = channels === 1 ? p[0] : p[2]; rgba[i * 4 + 3] = channels === 4 ? p[3] : 255;
  }
  return { width, height, rgba };
}

// Mean RGB of a width x height box at (x, y) in a decoded image.
export function meanRgb(image, x, y, width, height) {
  const sum = [0, 0, 0];
  for (let j = y; j < y + height; j++) {
    for (let i = x; i < x + width; i++) {
      const o = (j * image.width + i) * 4;
      sum[0] += image.rgba[o]; sum[1] += image.rgba[o + 1]; sum[2] += image.rgba[o + 2];
    }
  }
  return sum.map((v) => v / (width * height));
}
