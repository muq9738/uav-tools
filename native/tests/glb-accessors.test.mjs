import test from 'node:test';
import assert from 'node:assert/strict';
import { accessorReader, valuesAt, parseGlb } from './glb-accessors.mjs';

function fixture(compressed = false) {
  // Offset 4, padded VEC3/USHORT (8B), three distinct vertices. Padding is poison.
  const binary = Buffer.alloc(28, 0xab), expected = [[1, 2, 3], [23665, 65535, 33195], [17, 999, 77]];
  expected.forEach((v, i) => v.forEach((x, c) => binary.writeUInt16LE(x, 4 + i * 8 + c * 2)));
  const view = { buffer: 0, byteOffset: 0, byteLength: binary.length, byteStride: 8 };
  if (compressed) view.extensions = { EXT_meshopt_compression: { buffer: 0, byteLength: 28, byteStride: 4, count: 7, mode: 'ATTRIBUTES' } };
  const doc = { bufferViews: [view], accessors: [{ bufferView: 0, byteOffset: 4, count: 3, componentType: 5123, type: 'VEC3', normalized: true }] };
  const decoder = { decodeGltfBuffer(out, count, stride, data) { out.set(data); } };
  return { doc, binary, expected, decoder };
}
for (const compressed of [false, true]) test(`padded ushort, offset and normalization; compressed=${compressed}`, () => {
  const f = fixture(compressed), a = accessorReader(f.doc, f.binary, f.decoder)(0);
  assert.equal(a.stride, 6); assert.equal(a.sourceStride, 8); assert.equal(a.bytes.length, 18);
  f.expected.forEach((v, i) => assert.deepEqual(valuesAt(a, i), v.map(x => x / 65535)));
  // Reproduce the previous reader's defect: vertex zero passes, vertex one fails.
  const broken = { ...a, stride: a.sourceStride };
  assert.deepEqual(valuesAt(broken, 0), valuesAt(a, 0));
  assert.notDeepEqual(valuesAt(broken, 1), valuesAt(a, 1));
  assert.throws(() => valuesAt(broken, 2));
});
test('reject truncated accessor instead of padding silently with zero', () => {
  const f = fixture(); f.doc.accessors[0].count = 4;
  assert.throws(() => accessorReader(f.doc, f.binary)(0), /range exceeds/);
});
test('FLOAT layout preserves subnormal and signed zero bits', () => {
  const binary = Buffer.alloc(24); [-0, 1e-40, 0.5, -4, 2, 9].forEach((x, i) => binary.writeFloatLE(x, i * 4));
  const doc = { bufferViews: [{ buffer: 0, byteLength: 24 }], accessors: [{ bufferView: 0, componentType: 5126, count: 2, type: 'VEC3' }] };
  const a = accessorReader(doc, binary)(0);
  assert.deepEqual(a.bytes, binary); assert.ok(Object.is(valuesAt(a, 0)[0], -0));
  assert.deepEqual(valuesAt(a, 1), [-4, 2, 9]);
});
test('malformed header is not accepted as GLB', () => assert.throws(() => parseGlb(Buffer.alloc(28)), /magic/));
