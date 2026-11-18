// Independent test-side glTF reader. Decoded accessors are tightly packed;
// their returned stride is NEVER the source bufferView's padded/interleaved stride.
import assert from 'node:assert/strict';
import fs from 'node:fs';

const widths = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4 };
const units = { 5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4 };
function integer(n, name) { assert.ok(Number.isSafeInteger(n) && n >= 0, `invalid ${name}`); return n; }
function slice(bytes, start, length) {
  integer(start, 'offset'); integer(length, 'length');
  assert.ok(start + length <= bytes.length, 'buffer range exceeds payload');
  return bytes.subarray(start, start + length);
}
export function parseGlb(bytes) {
  assert.ok(bytes.length >= 28, 'truncated GLB');
  assert.equal(bytes.readUInt32LE(0), 0x46546c67, 'GLB magic');
  assert.equal(bytes.readUInt32LE(4), 2, 'GLB version');
  assert.equal(bytes.readUInt32LE(8), bytes.length, 'GLB length');
  const size = bytes.readUInt32LE(12);
  assert.equal(size % 4, 0, 'JSON alignment');
  assert.equal(bytes.readUInt32LE(16), 0x4e4f534a, 'JSON chunk');
  const doc = JSON.parse(slice(bytes, 20, size).toString('utf8'));
  const next = 20 + size;
  assert.ok(next + 8 <= bytes.length, 'missing BIN chunk');
  assert.equal(bytes.readUInt32LE(next + 4), 0x004e4942, 'BIN chunk');
  const binary = slice(bytes, next + 8, bytes.readUInt32LE(next));
  assert.equal(next + 8 + binary.length, bytes.length, 'unexpected trailing chunk');
  return { doc, binary };
}
export function accessorReader(doc, binary, decoder) {
  const cache = new Map();
  function view(index) {
    if (cache.has(index)) return cache.get(index);
    const meta = doc.bufferViews[index]; assert.ok(meta, 'unknown bufferView');
    const ext = meta.extensions?.EXT_meshopt_compression;
    let data;
    if (ext) {
      assert.equal(ext.buffer, 0, 'compressed external buffer unsupported');
      integer(ext.count, 'compressed count'); integer(ext.byteStride, 'compressed stride');
      assert.equal(ext.count * ext.byteStride, meta.byteLength, 'decoded view length');
      assert.ok(decoder, 'meshopt decoder required');
      data = Buffer.alloc(meta.byteLength);
      decoder.decodeGltfBuffer(data, ext.count, ext.byteStride,
        slice(binary, ext.byteOffset ?? 0, ext.byteLength), ext.mode, ext.filter ?? 'NONE');
    } else {
      assert.equal(meta.buffer, 0, 'external buffer unsupported');
      data = slice(binary, meta.byteOffset ?? 0, meta.byteLength);
    }
    cache.set(index, data); return data;
  }
  return function accessor(index) {
    const info = doc.accessors[index]; assert.ok(info, 'unknown accessor');
    assert.ok(!info.sparse, 'sparse accessor unsupported in this oracle');
    const components = widths[info.type], unit = units[info.componentType];
    assert.ok(components && unit, 'unsupported accessor type');
    const width = components * unit, sourceStride = doc.bufferViews[info.bufferView].byteStride ?? width;
    integer(info.count, 'accessor count'); integer(sourceStride, 'source stride');
    const offset = integer(info.byteOffset ?? 0, 'accessor offset');
    assert.ok(sourceStride >= width && sourceStride % unit === 0 && offset % unit === 0, 'invalid accessor alignment');
    const source = view(info.bufferView), bytes = Buffer.alloc(info.count * width);
    for (let i = 0; i < info.count; ++i)
      bytes.set(slice(source, offset + i * sourceStride, width), i * width);
    return { bytes, count: info.count, components, type: info.componentType,
      normalized: info.normalized === true, stride: width, sourceStride };
  };
}
export function valuesAt(accessor, index) {
  assert.ok(Number.isInteger(index) && index >= 0 && index < accessor.count, 'vertex out of bounds');
  const { bytes, type, components, normalized, stride } = accessor, unit = units[type];
  const read = { 5120: 'readInt8', 5121: 'readUInt8', 5122: 'readInt16LE', 5123: 'readUInt16LE',
    5125: 'readUInt32LE', 5126: 'readFloatLE' }[type];
  return Array.from({ length: components }, (_, c) => {
    let value = bytes[read](index * stride + c * unit);
    if (normalized) {
      assert.ok(type !== 5126 && type !== 5125, 'invalid normalized type');
      const max = { 5120: 127, 5121: 255, 5122: 32767, 5123: 65535 }[type];
      value = Math.max(value / max, -1);
    }
    assert.ok(Number.isFinite(value), 'non-finite attribute'); return value;
  });
}
export function loadGlb(file, decoder) {
  const { doc, binary } = parseGlb(fs.readFileSync(file));
  return { doc, accessor: accessorReader(doc, binary, decoder) };
}
