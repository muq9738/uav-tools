// 公开规范（Khronos Data Format Specification v1.3 §15 / EXT_texture_compression_s3tc）
// 独立参考实现与无损 PNG 编解码器。仅用于独立 GPU Oracle 测试与夹具生成，不进入生产转换器。
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import zlib from 'node:zlib';

export const S3TC_FORMATS = Object.freeze({
  BC1_RGB: {
    name: 'BC1_RGB',
    glName: 'COMPRESSED_RGB_S3TC_DXT1_EXT',
    internalFormat: 0x83f0,
    extension: 'WEBGL_compressed_texture_s3tc',
    blockBytes: 8,
    has1BitAlpha: false,
    alphaMode: 'opaque',
    srgb: false,
  },
  BC1_RGBA: {
    name: 'BC1_RGBA',
    glName: 'COMPRESSED_RGBA_S3TC_DXT1_EXT',
    internalFormat: 0x83f1,
    extension: 'WEBGL_compressed_texture_s3tc',
    blockBytes: 8,
    has1BitAlpha: true,
    alphaMode: 'bc1_1bit',
    srgb: false,
  },
  BC2_RGBA: {
    name: 'BC2_RGBA',
    glName: 'COMPRESSED_RGBA_S3TC_DXT3_EXT',
    internalFormat: 0x83f2,
    extension: 'WEBGL_compressed_texture_s3tc',
    blockBytes: 16,
    has1BitAlpha: false,
    alphaMode: 'bc2_4bit',
    srgb: false,
  },
  BC3_RGBA: {
    name: 'BC3_RGBA',
    glName: 'COMPRESSED_RGBA_S3TC_DXT5_EXT',
    internalFormat: 0x83f3,
    extension: 'WEBGL_compressed_texture_s3tc',
    blockBytes: 16,
    has1BitAlpha: false,
    alphaMode: 'bc3_interpolated',
    srgb: false,
  },
  BC1_SRGB: {
    name: 'BC1_SRGB',
    glName: 'COMPRESSED_SRGB_S3TC_DXT1_EXT',
    internalFormat: 0x8c4c,
    extension: 'WEBGL_compressed_texture_s3tc_srgb',
    blockBytes: 8,
    has1BitAlpha: false,
    alphaMode: 'opaque',
    srgb: true,
  },
  BC1_SRGB_ALPHA: {
    name: 'BC1_SRGB_ALPHA',
    glName: 'COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT',
    internalFormat: 0x8c4d,
    extension: 'WEBGL_compressed_texture_s3tc_srgb',
    blockBytes: 8,
    has1BitAlpha: true,
    alphaMode: 'bc1_1bit',
    srgb: true,
  },
  BC2_SRGB_ALPHA: {
    name: 'BC2_SRGB_ALPHA',
    glName: 'COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT',
    internalFormat: 0x8c4e,
    extension: 'WEBGL_compressed_texture_s3tc_srgb',
    blockBytes: 16,
    has1BitAlpha: false,
    alphaMode: 'bc2_4bit',
    srgb: true,
  },
  BC3_SRGB_ALPHA: {
    name: 'BC3_SRGB_ALPHA',
    glName: 'COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT',
    internalFormat: 0x8c4f,
    extension: 'WEBGL_compressed_texture_s3tc_srgb',
    blockBytes: 16,
    has1BitAlpha: false,
    alphaMode: 'bc3_interpolated',
    srgb: true,
  },
});

export function sha256Hex(buffer) {
  return crypto.createHash('sha256').update(buffer).digest('hex');
}

export function packRgb565(r5, g6, b5) {
  assert.ok(r5 >= 0 && r5 <= 31 && g6 >= 0 && g6 <= 63 && b5 >= 0 && b5 <= 31, 'RGB565 通道越界');
  return ((r5 & 0x1f) << 11) | ((g6 & 0x3f) << 5) | (b5 & 0x1f);
}

export function expandRgb565(c) {
  const r5 = (c >>> 11) & 0x1f;
  const g6 = (c >>> 5) & 0x3f;
  const b5 = c & 0x1f;
  return [
    (r5 << 3) | (r5 >>> 2),
    (g6 << 2) | (g6 >>> 4),
    (b5 << 3) | (b5 >>> 2),
  ];
}

/**
 * 组装单个 8 字节 BC1 颜色块（或 BC2/BC3 后 8 字节颜色子块）。
 * @param {number} c0 uint16 RGB565
 * @param {number} c1 uint16 RGB565
 * @param {number[]} indices16 长度 16 的数组（y*4+x），每项 0..3
 */
export function buildColorBlock(c0, c1, indices16) {
  assert.equal(indices16.length, 16);
  const out = Buffer.alloc(8);
  out.writeUInt16LE(c0 & 0xffff, 0);
  out.writeUInt16LE(c1 & 0xffff, 2);
  for (let y = 0; y < 4; y++) {
    let rowByte = 0;
    for (let x = 0; x < 4; x++) {
      const idx = indices16[y * 4 + x];
      assert.ok(idx >= 0 && idx <= 3, `颜色索引越界: ${idx}`);
      rowByte |= (idx & 0x3) << (2 * x);
    }
    out[4 + y] = rowByte;
  }
  return out;
}

/**
 * 组装单个 16 字节 BC2 (DXT3) 块。
 * @param {number[]} alpha4x16 长度 16 的 4-bit alpha 数组 (0..15)
 */
export function buildBc2Block(alpha4x16, c0, c1, colorIndices16) {
  assert.equal(alpha4x16.length, 16);
  const out = Buffer.alloc(16);
  for (let y = 0; y < 4; y++) {
    let word = 0;
    for (let x = 0; x < 4; x++) {
      const a4 = alpha4x16[y * 4 + x];
      assert.ok(a4 >= 0 && a4 <= 15, `BC2 4-bit alpha 越界: ${a4}`);
      word |= (a4 & 0x0f) << (4 * x);
    }
    out.writeUInt16LE(word, y * 2);
  }
  buildColorBlock(c0, c1, colorIndices16).copy(out, 8);
  return out;
}

/**
 * 组装单个 16 字节 BC3 (DXT5) 块。
 * @param {number} a0 0..255
 * @param {number} a1 0..255
 * @param {number[]} alphaIndices16 长度 16 的 3-bit alpha 索引 (0..7)
 */
export function buildBc3Block(a0, a1, alphaIndices16, c0, c1, colorIndices16) {
  assert.equal(alphaIndices16.length, 16);
  assert.ok(a0 >= 0 && a0 <= 255 && a1 >= 0 && a1 <= 255);
  const out = Buffer.alloc(16);
  out[0] = a0;
  out[1] = a1;
  let bits48 = 0n;
  for (let i = 0; i < 16; i++) {
    const idx = alphaIndices16[i];
    assert.ok(idx >= 0 && idx <= 7, `BC3 alpha 索引越界: ${idx}`);
    bits48 |= BigInt(idx & 0x7) << BigInt(3 * i);
  }
  for (let k = 0; k < 6; k++) {
    out[2 + k] = Number((bits48 >> BigInt(8 * k)) & 0xffn);
  }
  buildColorBlock(c0, c1, colorIndices16).copy(out, 8);
  return out;
}

export function expectedCompressedByteLength(formatKey, width, height) {
  const fmt = S3TC_FORMATS[formatKey];
  assert.ok(fmt, `未知格式: ${formatKey}`);
  assert.ok(width >= 1 && height >= 1);
  const blocksX = Math.ceil(width / 4);
  const blocksY = Math.ceil(height / 4);
  return blocksX * blocksY * fmt.blockBytes;
}

function decodeColorSubBlock(bytes, offset, allowThreeColorMode, transparentIndexZeroAlpha) {
  const c0 = bytes.readUInt16LE(offset);
  const c1 = bytes.readUInt16LE(offset + 2);
  const [r0, g0, b0] = expandRgb565(c0);
  const [r1, g1, b1] = expandRgb565(c1);
  const palette = new Array(4);
  palette[0] = [r0, g0, b0, 255];
  palette[1] = [r1, g1, b1, 255];
  if (!allowThreeColorMode || c0 > c1) {
    palette[2] = [
      Math.floor((2 * r0 + r1 + 1) / 3),
      Math.floor((2 * g0 + g1 + 1) / 3),
      Math.floor((2 * b0 + b1 + 1) / 3),
      255,
    ];
    palette[3] = [
      Math.floor((r0 + 2 * r1 + 1) / 3),
      Math.floor((g0 + 2 * g1 + 1) / 3),
      Math.floor((b0 + 2 * b1 + 1) / 3),
      255,
    ];
  } else {
    palette[2] = [
      Math.floor((r0 + r1) / 2),
      Math.floor((g0 + g1) / 2),
      Math.floor((b0 + b1) / 2),
      255,
    ];
    palette[3] = [0, 0, 0, transparentIndexZeroAlpha ? 0 : 255];
  }
  const texels = new Array(16);
  for (let y = 0; y < 4; y++) {
    const row = bytes[offset + 4 + y];
    for (let x = 0; x < 4; x++) {
      const idx = (row >>> (2 * x)) & 0x3;
      const p = palette[idx];
      texels[y * 4 + x] = [p[0], p[1], p[2], p[3]];
    }
  }
  return texels;
}

function decodeBc3AlphaSubBlock(bytes, offset) {
  const a0 = bytes[offset];
  const a1 = bytes[offset + 1];
  const palette = new Uint8Array(8);
  palette[0] = a0;
  palette[1] = a1;
  if (a0 > a1) {
    for (let k = 1; k <= 6; k++) {
      palette[k + 1] = Math.floor(((7 - k) * a0 + k * a1 + 3) / 7);
    }
  } else {
    for (let k = 1; k <= 4; k++) {
      palette[k + 1] = Math.floor(((5 - k) * a0 + k * a1 + 2) / 5);
    }
    palette[6] = 0;
    palette[7] = 255;
  }
  let bits48 = 0n;
  for (let k = 0; k < 6; k++) {
    bits48 |= BigInt(bytes[offset + 2 + k]) << BigInt(8 * k);
  }
  const alphas = new Uint8Array(16);
  for (let i = 0; i < 16; i++) {
    const idx = Number((bits48 >> BigInt(3 * i)) & 0x7n);
    alphas[i] = palette[idx];
  }
  return alphas;
}

/**
 * 按 Khronos DFS §15 独立参考规则将 BC1/BC2/BC3 压缩块解码为紧密排列的 8-bit RGBA。
 */
export function decodeS3tcLevel(formatKey, compressedBytes, width, height) {
  const fmt = S3TC_FORMATS[formatKey];
  assert.ok(fmt, `未知 S3TC 格式: ${formatKey}`);
  const expectedLen = expectedCompressedByteLength(formatKey, width, height);
  assert.equal(
    compressedBytes.length,
    expectedLen,
    `${formatKey} ${width}x${height} 压缩块字节数不符: ${compressedBytes.length} !== ${expectedLen}`,
  );
  const blocksX = Math.ceil(width / 4);
  const blocksY = Math.ceil(height / 4);
  const out = Buffer.alloc(width * height * 4);

  for (let by = 0; by < blocksY; by++) {
    for (let bx = 0; bx < blocksX; bx++) {
      const blockOffset = (by * blocksX + bx) * fmt.blockBytes;
      let texels;
      if (fmt.alphaMode === 'opaque' || fmt.alphaMode === 'bc1_1bit') {
        texels = decodeColorSubBlock(
          compressedBytes,
          blockOffset,
          true,
          fmt.has1BitAlpha,
        );
      } else if (fmt.alphaMode === 'bc2_4bit') {
        // BC2 颜色子块即使 c0 <= c1 也始终使用四色模式。
        texels = decodeColorSubBlock(compressedBytes, blockOffset + 8, false, false);
        for (let y = 0; y < 4; y++) {
          const word = compressedBytes.readUInt16LE(blockOffset + y * 2);
          for (let x = 0; x < 4; x++) {
            const a4 = (word >>> (4 * x)) & 0x0f;
            texels[y * 4 + x][3] = (a4 << 4) | a4;
          }
        }
      } else if (fmt.alphaMode === 'bc3_interpolated') {
        // BC3 颜色子块同样始终使用四色模式。
        texels = decodeColorSubBlock(compressedBytes, blockOffset + 8, false, false);
        const alphas = decodeBc3AlphaSubBlock(compressedBytes, blockOffset);
        for (let i = 0; i < 16; i++) {
          texels[i][3] = alphas[i];
        }
      } else {
        throw new Error(`未实现的 alphaMode: ${fmt.alphaMode}`);
      }

      for (let ly = 0; ly < 4; ly++) {
        const py = by * 4 + ly;
        if (py >= height) continue;
        for (let lx = 0; lx < 4; lx++) {
          const px = bx * 4 + lx;
          if (px >= width) continue;
          const dst = (py * width + px) * 4;
          const rgba = texels[ly * 4 + lx];
          out[dst] = rgba[0];
          out[dst + 1] = rgba[1];
          out[dst + 2] = rgba[2];
          out[dst + 3] = rgba[3];
        }
      }
    }
  }
  return out;
}

// --- 纯 Node 标准库无损 8-bit RGBA PNG 编解码器（RFC 2083） ---
const PNG_SIGNATURE = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) {
      c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    }
    table[n] = c >>> 0;
  }
  return table;
})();

function crc32(buffers) {
  let c = 0xffffffff;
  for (const buf of buffers) {
    for (let i = 0; i < buf.length; i++) {
      c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
    }
  }
  return (c ^ 0xffffffff) >>> 0;
}

function makePngChunk(typeAscii, dataBuffer) {
  const typeBuf = Buffer.from(typeAscii, 'ascii');
  assert.equal(typeBuf.length, 4);
  const out = Buffer.alloc(12 + dataBuffer.length);
  out.writeUInt32BE(dataBuffer.length, 0);
  typeBuf.copy(out, 4);
  dataBuffer.copy(out, 8);
  out.writeUInt32BE(crc32([typeBuf, dataBuffer]), 8 + dataBuffer.length);
  return out;
}

export function encodeRgbaPng(width, height, rgbaBytes) {
  assert.ok(Number.isInteger(width) && width >= 1);
  assert.ok(Number.isInteger(height) && height >= 1);
  assert.equal(rgbaBytes.length, width * height * 4);

  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; // bit depth 8
  ihdr[9] = 6; // color type RGBA
  ihdr[10] = 0; // compression
  ihdr[11] = 0; // filter
  ihdr[12] = 0; // interlace none

  const stride = width * 4;
  const rawScanlines = Buffer.alloc((stride + 1) * height);
  for (let y = 0; y < height; y++) {
    const rowStart = y * (stride + 1);
    rawScanlines[rowStart] = 0; // filter type 0 (None)
    Buffer.from(rgbaBytes.buffer, rgbaBytes.byteOffset + y * stride, stride).copy(
      rawScanlines,
      rowStart + 1,
    );
  }
  const idat = zlib.deflateSync(rawScanlines, { level: 9 });
  return Buffer.concat([
    PNG_SIGNATURE,
    makePngChunk('IHDR', ihdr),
    makePngChunk('IDAT', idat),
    makePngChunk('IEND', Buffer.alloc(0)),
  ]);
}

function paethPredictor(a, b, c) {
  const p = a + b - c;
  const pa = Math.abs(p - a);
  const pb = Math.abs(p - b);
  const pc = Math.abs(p - c);
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

/**
 * 独立解析无损 8-bit RGB/RGBA PNG（支持外部候选生成的任意标准滤镜 0..4 PNG 校验）。
 */
export function decodePngToRgba(pngBytes) {
  assert.ok(pngBytes.length >= 33, 'PNG 过短');
  assert.ok(pngBytes.subarray(0, 8).equals(PNG_SIGNATURE), '非法 PNG 签名');
  let offset = 8;
  let width = 0;
  let height = 0;
  let bitDepth = 0;
  let colorType = 0;
  let interlace = 0;
  const idatChunks = [];

  while (offset < pngBytes.length) {
    assert.ok(offset + 12 <= pngBytes.length, 'PNG chunk 截断');
    const len = pngBytes.readUInt32BE(offset);
    const type = pngBytes.subarray(offset + 4, offset + 8).toString('ascii');
    const data = pngBytes.subarray(offset + 8, offset + 8 + len);
    const expectedCrc = pngBytes.readUInt32BE(offset + 8 + len);
    const actualCrc = crc32([pngBytes.subarray(offset + 4, offset + 8), data]);
    assert.equal(actualCrc, expectedCrc, `PNG chunk ${type} CRC 校验失败`);
    offset += 12 + len;

    if (type === 'IHDR') {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      bitDepth = data[8];
      colorType = data[9];
      interlace = data[12];
      assert.equal(bitDepth, 8, `仅支持 8-bit PNG，收到 bitDepth=${bitDepth}`);
      assert.ok(colorType === 6 || colorType === 2, `仅支持 RGB/RGBA PNG，收到 colorType=${colorType}`);
      assert.equal(interlace, 0, '不支持隔行扫描 Adam7 PNG');
    } else if (type === 'IDAT') {
      idatChunks.push(data);
    } else if (type === 'IEND') {
      break;
    }
  }
  assert.ok(width >= 1 && height >= 1, '缺少有效 IHDR');
  const bpp = colorType === 6 ? 4 : 3;
  const stride = width * bpp;
  const inflated = zlib.inflateSync(Buffer.concat(idatChunks));
  assert.equal(inflated.length, (stride + 1) * height, 'PNG 解压扫描线字节数不匹配');

  const recon = Buffer.alloc(height * stride);
  for (let y = 0; y < height; y++) {
    const filter = inflated[y * (stride + 1)];
    const srcBase = y * (stride + 1) + 1;
    const dstBase = y * stride;
    for (let i = 0; i < stride; i++) {
      const x = inflated[srcBase + i];
      const a = i >= bpp ? recon[dstBase + i - bpp] : 0;
      const b = y > 0 ? recon[dstBase - stride + i] : 0;
      const c = y > 0 && i >= bpp ? recon[dstBase - stride + i - bpp] : 0;
      let val;
      if (filter === 0) val = x;
      else if (filter === 1) val = (x + a) & 0xff;
      else if (filter === 2) val = (x + b) & 0xff;
      else if (filter === 3) val = (x + ((a + b) >>> 1)) & 0xff;
      else if (filter === 4) val = (x + paethPredictor(a, b, c)) & 0xff;
      else throw new Error(`未知的 PNG 行滤镜: ${filter}`);
      recon[dstBase + i] = val;
    }
  }

  if (bpp === 4) {
    return { width, height, rgba: recon };
  }
  const rgba = Buffer.alloc(width * height * 4);
  for (let p = 0; p < width * height; p++) {
    rgba[p * 4] = recon[p * 3];
    rgba[p * 4 + 1] = recon[p * 3 + 1];
    rgba[p * 4 + 2] = recon[p * 3 + 2];
    rgba[p * 4 + 3] = 255;
  }
  return { width, height, rgba };
}
