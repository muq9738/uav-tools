// Test-side oracle: preserve face order and winding, not the starting corner.
import assert from 'node:assert/strict';

export function assertOrientedTriangles(before, after, sameCorner = (a, b) => a === b, label = 'triangles') {
  assert.equal(before.length, after.length, `${label}: index count`);
  assert.equal(before.length % 3, 0, `${label}: incomplete face`);
  let rotatedFaces = 0;
  for (let face = 0; face < before.length; face += 3) {
    let rotation = -1;
    for (let r = 0; r < 3; ++r) {
      if ([0, 1, 2].every(c => sameCorner(before[face+c], after[face+(c+r)%3]))) { rotation = r; break; }
    }
    assert.notEqual(rotation, -1, `${label}: oriented face ${face/3} changed (attributes, winding or face order)`);
    if (rotation) ++rotatedFaces;
  }
  return rotatedFaces;
}

// For fingerprints only: lexicographically smallest CYCLIC rotation of each
// index triple. Never sort corners or faces; the attribute slots are hashed too.
export function canonicalTriangleIndices(indices) {
  assert.equal(indices.length % 3, 0, 'incomplete face');
  const result = new indices.constructor(indices);
  for (let face = 0; face < indices.length; face += 3) {
    const rotations = [0, 1, 2].map(r => [0, 1, 2].map(c => indices[face+(c+r)%3]));
    rotations.sort((a, b) => a[0]-b[0] || a[1]-b[1] || a[2]-b[2]);
    result.set(rotations[0], face);
  }
  return result;
}

export function indexValues(accessor) {
  assert.equal(accessor.components, 1, 'index must be SCALAR');
  assert.ok([5123, 5125].includes(accessor.type), 'index must be USHORT/UINT');
  const T = accessor.type === 5123 ? Uint16Array : Uint32Array;
  return new T(accessor.bytes.buffer, accessor.bytes.byteOffset, accessor.count);
}
