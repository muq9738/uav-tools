#!/usr/bin/env node
// Verify content repartitioning preserves each source instance's primitives,
// including every attribute bit, oriented face order, transform, material and image.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { pathToFileURL } from 'node:url';
import { loadGlb } from './glb-accessors.mjs';
import { canonicalTriangleIndices, indexValues } from './oriented-triangles.mjs';
const [before, after, decoderFile, output] = process.argv.slice(2);
assert.ok(output, 'usage: before after meshopt-decoder output.json');
const { MeshoptDecoder: decoder } = await import(pathToFileURL(path.resolve(decoderFile))); await decoder.ready;
const json = p => JSON.parse(fs.readFileSync(p, 'utf8'));
const stable = value => JSON.stringify(canonical(value));
function canonical(v) {
  if (Array.isArray(v)) return v.map(canonical);
  if (v && typeof v === 'object') return Object.fromEntries(Object.keys(v).sort().map(k => [k, canonical(v[k])]));
  return v;
}
function material(doc, id) {
  const m = structuredClone(doc.materials[id]);
  const info = m.pbrMetallicRoughness?.baseColorTexture;
  if (info) {
    const texture = doc.textures[info.index]; delete info.index;
    info.image = doc.images[texture.source]; info.sampler = doc.samplers[texture.sampler];
  }
  return m;
}
function inspect(root) {
  const report = json(path.join(root, 'conversion-report.json')), sources = new Map(), images = new Map();
  let primitives = 0, vertices = 0, corners = 0;
  for (const row of report.records) {
    const key = stable([row.source, row.sourceTransform]);
    assert.ok(!sources.has(key), 'duplicate source instance');
    const fingerprints = [];
    for (const c of row.content) {
      const g = loadGlb(path.join(root, c.uri), decoder);
      for (const n of g.doc.nodes) {
        assert.ok(!n.children, 'nested nodes unsupported');
        for (const p of g.doc.meshes[n.mesh].primitives) {
          const h = crypto.createHash('sha256');
          const frame = { ...n }; delete frame.mesh;
          h.update(stable({ frame, material: material(g.doc, p.material), mode: p.mode ?? 4 }));
          for (const name of ['indices', ...Object.keys(p.attributes).sort()]) {
            const a = g.accessor(name === 'indices' ? p.indices : p.attributes[name]);
            h.update(stable([name, a.count, a.components, a.type, a.normalized]));
            if (name === 'indices' && (p.mode ?? 4) === 4) {
              const oriented = canonicalTriangleIndices(indexValues(a));
              h.update(Buffer.from(oriented.buffer, oriented.byteOffset, oriented.byteLength));
            } else h.update(a.bytes);
            if (name === 'indices') corners += a.count;
            if (name === 'POSITION') vertices += a.count;
          }
          fingerprints.push(h.digest('hex')); ++primitives;
        }
      }
      for (const img of c.images) if (!images.has(img.uri))
        images.set(img.uri, crypto.createHash('sha256').update(fs.readFileSync(path.join(root, img.uri))).digest('hex'));
    }
    sources.set(key, fingerprints.sort());
  }
  return { sources: [...sources].sort(), images: [...images].sort(), primitives, vertices, corners };
}
const a = inspect(before), b = inspect(after);
assert.deepEqual(a, b, 'source-instance primitive identity changed');
assert.deepEqual(json(path.join(before, 'tileset.json')).root.transform, json(path.join(after, 'tileset.json')).root.transform);
const result = { status: 'partition-data-passed', productAccepted: false, sources: a.sources.length, primitives: a.primitives,
  vertices: a.vertices, corners: a.corners, images: a.images.length,
  invariant: 'Every primitive preserved exactly once per source instance: all attribute-slot bits (including unused), oriented face sequence (cyclic starts allowed), exact point/line indices, node transforms, materials and image bytes.',
  limitation: 'Does not prove traversal/rendered quality: a changed tree can select different LODs.' };
fs.writeFileSync(output, JSON.stringify(result, null, 2)+'\n'); console.log(JSON.stringify(result, null, 2));
