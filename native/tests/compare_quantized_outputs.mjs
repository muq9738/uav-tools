#!/usr/bin/env node
// Numerical audit only. Passing the encoding grid bound does NOT accept a quality loss.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { pathToFileURL } from 'node:url';
import { loadGlb, valuesAt } from './glb-accessors.mjs';
import { assertOrientedTriangles, indexValues } from './oriented-triangles.mjs';

const args = {};
for (let i = 2; i < process.argv.length; i += 2) args[process.argv[i].replace(/^--/, '')] = process.argv[i + 1];
for (const key of ['before', 'after', 'decoder', 'output']) assert.ok(args[key], `missing --${key}`);
const imported = await import(pathToFileURL(path.resolve(args.decoder)));
const decoder = imported.MeshoptDecoder ?? imported.default;
await decoder.ready;
const json = p => JSON.parse(fs.readFileSync(p, 'utf8'));
const before = path.resolve(args.before), after = path.resolve(args.after);
const oldReport = json(path.join(before, 'conversion-report.json'));
const newReport = json(path.join(after, 'conversion-report.json'));
const oldRoot = json(path.join(before, 'tileset.json')).root.transform;
const newRoot = json(path.join(after, 'tileset.json')).root.transform;
assert.deepEqual(oldRoot, newRoot, 'placement changed');
const map = r => new Map(r.records.flatMap(row => row.content).map(c => [c.uri, c]));
const oldContents = map(oldReport), newContents = map(newReport);
assert.deepEqual([...oldContents.keys()].sort(), [...newContents.keys()].sort(), 'content closure changed');
const identity = [1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];
const point = (p, m) => [0, 1, 2].map(c => p[0]*m[c]+p[1]*m[4+c]+p[2]*m[8+c]+m[12+c]);
const vector = (p, m) => [0, 1, 2].map(c => p[0]*m[c]+p[1]*m[4+c]+p[2]*m[8+c]);
const yup = p => [p[0], -p[2], p[1]];
const world = (p, m) => point(yup(point(p, m)), oldRoot);
// Independent conservative bound of a half quantization cell in world space.
// Uses the actual serialized matrices, never the converter's reported error.
const gridBound = m => [0, 1, 2].reduce((sum, c) =>
  sum + Math.hypot(...vector(yup([m[c*4], m[c*4+1], m[c*4+2]]), oldRoot)) / (2*65535), 0);
let vertices = 0, uvComponents = 0, contents = 0, maxWorldDeltaMeters = 0, maxUvDelta = 0, maxUvDeltaTexels = 0;
let maxIndependentGridBoundMeters = 0, positionBitsChanged = false, uvBitsChanged = false;
const images = new Set();
for (const [uri, oldContent] of oldContents) {
  const a = loadGlb(path.join(before, uri), decoder), b = loadGlb(path.join(after, uri), decoder);
  assert.equal(a.doc.nodes.length, b.doc.nodes.length, `${uri}: node count`);
  assert.equal(a.doc.meshes.length, b.doc.meshes.length, `${uri}: mesh count`);
  for (const key of ['materials', 'samplers', 'textures', 'images', 'scenes', 'scene'])
    assert.deepEqual(a.doc[key], b.doc[key], `${uri}: ${key} changed`);
  for (let n = 0; n < a.doc.nodes.length; ++n) {
    const an = a.doc.nodes[n], bn = b.doc.nodes[n];
    assert.equal(an.mesh, bn.mesh); assert.ok(!an.children && !bn.children, 'hierarchical GLB requires scene traversal');
    assert.ok(!an.translation && !an.rotation && !an.scale && !bn.translation && !bn.rotation && !bn.scale, 'TRS unsupported');
    const am = an.matrix ?? identity, bm = bn.matrix ?? identity;
    const ap = a.doc.meshes[an.mesh].primitives, bp = b.doc.meshes[bn.mesh].primitives;
    assert.equal(ap.length, bp.length);
    for (let j = 0; j < ap.length; ++j) {
      assert.equal(ap[j].mode ?? 4, bp[j].mode ?? 4); assert.equal(ap[j].material, bp[j].material);
      const ai = a.accessor(ap[j].indices), bi = b.accessor(bp[j].indices);
      assert.equal(ai.type, bi.type);
      if ((ap[j].mode ?? 4) === 4) assertOrientedTriangles(indexValues(ai), indexValues(bi), undefined, uri);
      else assert.ok(ai.bytes.equals(bi.bytes), `${uri}: point/line indices changed`);
      // Slot identity remains strict; the per-slot attribute checks below include unused vertices.
      assert.deepEqual(Object.keys(ap[j].attributes).sort(), Object.keys(bp[j].attributes).sort());
      const material = a.doc.materials[ap[j].material];
      const tex = material?.pbrMetallicRoughness?.baseColorTexture;
      const img = tex ? a.doc.images[a.doc.textures[tex.index].source] : null;
      const imageAudit = img?.uri ? oldContent.images.find(x => x.uri === path.posix.normalize(path.posix.join(path.posix.dirname(uri), img.uri))) : null;
      for (const name of Object.keys(ap[j].attributes)) {
        const av = a.accessor(ap[j].attributes[name]), bv = b.accessor(bp[j].attributes[name]);
        assert.equal(av.count, bv.count); assert.equal(av.components, bv.components);
        if (name === 'POSITION') {
          assert.equal(av.type, 5126, 'baseline POSITION must be FLOAT');
          const quantized = bv.type === 5123 && bv.normalized;
          assert.ok(quantized || bv.type === 5126, 'unsupported candidate POSITION');
          if (quantized) assert.ok(b.doc.extensionsRequired?.includes('KHR_mesh_quantization'), 'quantization extension must be required');
          const bound = quantized ? gridBound(bm) : 0;
          maxIndependentGridBoundMeters = Math.max(maxIndependentGridBoundMeters, bound);
          positionBitsChanged ||= av.type !== bv.type || !av.bytes.equals(bv.bytes);
          for (let i = 0; i < av.count; ++i) {
            const p = world(valuesAt(av, i), am), q = world(valuesAt(bv, i), bm);
            const delta = Math.hypot(...p.map((v, c) => v - q[c]));
            assert.ok(delta <= bound + 1e-7, `${uri}: vertex ${i}, ${delta} exceeds independent cell bound ${bound}`);
            maxWorldDeltaMeters = Math.max(maxWorldDeltaMeters, delta); ++vertices;
          }
        } else if (name.startsWith('TEXCOORD_')) {
          const quantized = bv.type === 5123 && bv.normalized;
          assert.ok(quantized || bv.type === 5126);
          uvBitsChanged ||= av.type !== bv.type || !av.bytes.equals(bv.bytes);
          for (let i = 0; i < av.count; ++i) {
            const p = valuesAt(av, i), q = valuesAt(bv, i);
            for (let c = 0; c < p.length; ++c) {
              const delta = Math.abs(p[c] - q[c]);
              assert.ok(delta <= (quantized ? 0.5/65535 : 0) + 1e-12, `${uri}: UV outside independent rounding bound`);
              maxUvDelta = Math.max(maxUvDelta, delta);
              if (imageAudit) maxUvDeltaTexels = Math.max(maxUvDeltaTexels, delta * (c === 0 ? imageAudit.width : imageAudit.height));
              ++uvComponents;
            }
          }
        } else {
          // A changed node linear transform also transforms normals; byte equality alone is insufficient.
          if (name === 'NORMAL') assert.deepEqual(am.slice(0, 12), bm.slice(0, 12), 'normal transform changed; use world-normal oracle');
          assert.equal(av.type, bv.type); assert.ok(av.bytes.equals(bv.bytes), `${uri}: ${name} changed`);
        }
      }
    }
  }
  for (const img of oldContent.images) {
    if (images.has(img.uri)) continue;
    assert.ok(fs.readFileSync(path.join(before, img.uri)).equals(fs.readFileSync(path.join(after, img.uri))), `image changed ${img.uri}`);
    images.add(img.uri);
  }
  ++contents;
}
const result = {
  status: 'numerical-audit-passed', productAccepted: false,
  before, after, decoderSha256: crypto.createHash('sha256').update(fs.readFileSync(args.decoder)).digest('hex'),
  contents, vertices, images: images.size, uvComponents, maxWorldDeltaMeters, maxIndependentGridBoundMeters,
  maxUvDelta, maxUvDeltaTexels, positionBitsChanged, uvBitsChanged,
  strictLossless: maxWorldDeltaMeters === 0 && !uvBitsChanged,
  limitation: 'Encoding-grid correctness only; no rendered-pixel, measurement or performance acceptance. Quantization changes source values.'
};
fs.writeFileSync(args.output, JSON.stringify(result, null, 2) + '\n');
console.log(JSON.stringify(result, null, 2));
