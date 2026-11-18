import test from 'node:test';
import assert from 'node:assert/strict';
import {assertOrientedTriangles, canonicalTriangleIndices} from './oriented-triangles.mjs';

test('cyclic starts and degenerate faces preserve oriented face sequence', () => {
  assert.equal(assertOrientedTriangles([0,1,2,3,3,3,4,5,4], [1,2,0,3,3,3,4,4,5]), 2);
});
test('reversed winding, changed corner, face reordering and incomplete faces fail', () => {
  const source = [0,1,2,3,4,5];
  for (const bad of [[0,2,1,3,4,5], [0,1,6,3,4,5], [3,4,5,0,1,2]])
    assert.throws(() => assertOrientedTriangles(source, bad), /oriented face/);
  assert.throws(() => assertOrientedTriangles([0,1], [0,1]), /incomplete face/);
});
test('all attribute bits participate; position alone cannot hide UV/normal/color change', () => {
  const before = [[1,10,20,30],[2,11,21,31],[3,12,22,32]];
  const rotated = [before[1],before[2],before[0]].map(x => [...x]);
  const compare = (a,b) => a.every((x,i) => x === b[i]);
  assertOrientedTriangles(before, rotated, compare);
  for (const component of [1,2,3]) {
    const bad = structuredClone(rotated); bad[0][component]++;
    assert.throws(() => assertOrientedTriangles(before, bad, compare), /oriented face/);
  }
});
test('fingerprint canonicalization only allows per-face CYCLIC rotation', () => {
  const canonical = a => canonicalTriangleIndices(Uint32Array.from(a));
  const source = canonical([0,1,2,3,4,5]);
  assert.deepEqual(source, canonical([1,2,0,5,3,4]));
  assert.notDeepEqual(source, canonical([0,2,1,3,4,5]));
  assert.notDeepEqual(source, canonical([3,4,5,0,1,2]));
  assert.deepEqual(canonicalTriangleIndices(Uint16Array.from([2,0,1])), Uint16Array.from([0,1,2]));
});
