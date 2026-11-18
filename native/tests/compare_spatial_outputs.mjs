#!/usr/bin/env node
// 用独立 meshopt 解码器复核两份产物的世界坐标与不应变化的属性。
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { pathToFileURL } from 'node:url';

const args = {};
for (let i = 2; i < process.argv.length; i += 2) args[process.argv[i].replace(/^--/, '')] = process.argv[i + 1];
for (const key of ['before', 'after', 'decoder', 'output']) if (!args[key]) throw new Error(`缺少 --${key}`);
const imported = await import(pathToFileURL(path.resolve(args.decoder)).href);
const decoder = imported.MeshoptDecoder ?? imported.default?.MeshoptDecoder ?? imported.default;
await decoder.ready;
const readJson = p => JSON.parse(fs.readFileSync(p, 'utf8'));
const before = path.resolve(args.before), after = path.resolve(args.after);
const oldReport = readJson(path.join(before, 'conversion-report.json'));
const newReport = readJson(path.join(after, 'conversion-report.json'));
const oldRoot = readJson(path.join(before, 'tileset.json')).root.transform;
const newRoot = readJson(path.join(after, 'tileset.json')).root.transform;
const contents = report => new Map(report.records.flatMap(row => row.content).map(content => [content.uri, content]));
const oldContents = contents(oldReport), newContents = contents(newReport);
if (oldContents.size !== newContents.size) throw new Error('内容数量变化');
function glb(file) {
  const bytes = fs.readFileSync(file), jsonSize = bytes.readUInt32LE(12);
  const doc = JSON.parse(bytes.subarray(20, 20 + jsonSize).toString('utf8'));
  const binary = bytes.subarray(28 + jsonSize), cache = new Map();
  function view(index) {
    if (cache.has(index)) return cache.get(index);
    const info = doc.bufferViews[index], ext = info.extensions?.EXT_meshopt_compression;
    let data;
    if (ext) {
      data = new Uint8Array(ext.count * ext.byteStride);
      decoder.decodeGltfBuffer(data, ext.count, ext.byteStride, binary.subarray(ext.byteOffset ?? 0, (ext.byteOffset ?? 0) + ext.byteLength), ext.mode, ext.filter ?? 'NONE');
    } else {
      if (info.buffer !== 0) throw new Error('未知物理buffer');
      data = binary.subarray(info.byteOffset ?? 0, (info.byteOffset ?? 0) + info.byteLength);
    }
    cache.set(index, data);
    return data;
  }
  function accessor(index) {
    const info = doc.accessors[index], meta = doc.bufferViews[info.bufferView];
    const components = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4 }[info.type];
    const unit = { 5121: 1, 5123: 2, 5125: 4, 5126: 4 }[info.componentType];
    if (!components || !unit) throw new Error('未知accessor类型');
    const source = view(info.bufferView), width = components * unit, stride = meta.byteStride ?? width;
    const result = Buffer.alloc(info.count * width), offset = info.byteOffset ?? 0;
    for (let i = 0; i < info.count; ++i) result.set(source.subarray(offset + i * stride, offset + i * stride + width), i * width);
    return { bytes: result, count: info.count, components, type: info.componentType };
  }
  return { doc, accessor };
}
const identity = [1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];
const point = (p, m) => [p[0]*m[0]+p[1]*m[4]+p[2]*m[8]+m[12], p[0]*m[1]+p[1]*m[5]+p[2]*m[9]+m[13], p[0]*m[2]+p[1]*m[6]+p[2]*m[10]+m[14]];
const world = (p, m, root) => { const q = point(p, m); return point([q[0], -q[2], q[1]], root); };
const xyz = (bytes, i) => [bytes.readFloatLE(i*12), bytes.readFloatLE(i*12+4), bytes.readFloatLE(i*12+8)];
let vertices = 0, normalComponents = 0, maxWorldDeltaMeters = 0, maxNormalComponentDelta = 0, comparedContents = 0, comparedImages = 0;
const images = new Set();
for (const [uri, oldContent] of oldContents) {
  const newContent = newContents.get(uri);
  if (!newContent) throw new Error(`内容URI变化 ${uri}`);
  const a = glb(path.join(before, uri)), b = glb(path.join(after, uri));
  if (a.doc.nodes.length !== b.doc.nodes.length || a.doc.meshes.length !== b.doc.meshes.length) throw new Error(`节点/mesh数量变化 ${uri}`);
  for (const key of ['materials', 'samplers', 'textures', 'images']) if (JSON.stringify(a.doc[key]) !== JSON.stringify(b.doc[key])) throw new Error(`非空间材质/图片描述变化 ${uri} ${key}`);
  const tolerance = oldContent.coordinateRoundingErrorMeters + newContent.coordinateRoundingErrorMeters + 1e-7;
  if (!Number.isFinite(tolerance) || tolerance < 0) throw new Error(`缺失或无效的舍入误差 ${uri}`);
  for (let n = 0; n < a.doc.nodes.length; ++n) {
    const oldNode = a.doc.nodes[n], newNode = b.doc.nodes[n];
    if (oldNode.mesh !== newNode.mesh) throw new Error(`mesh关联变化 ${uri}`);
    const oldPrimitives = a.doc.meshes[oldNode.mesh].primitives, newPrimitives = b.doc.meshes[newNode.mesh].primitives;
    if (oldPrimitives.length !== newPrimitives.length) throw new Error(`primitive数量变化 ${uri}`);
    for (let j = 0; j < oldPrimitives.length; ++j) {
      const oldPrimitive = oldPrimitives[j], newPrimitive = newPrimitives[j];
      if ((oldPrimitive.mode ?? 4) !== (newPrimitive.mode ?? 4)) throw new Error(`图元模式变化 ${uri}`);
      const oldIndices = a.accessor(oldPrimitive.indices), newIndices = b.accessor(newPrimitive.indices);
      if (oldIndices.count !== newIndices.count || oldIndices.type !== newIndices.type || !oldIndices.bytes.equals(newIndices.bytes)) throw new Error(`索引变化 ${uri}`);
      const names = Object.keys(oldPrimitive.attributes).sort();
      if (JSON.stringify(names) !== JSON.stringify(Object.keys(newPrimitive.attributes).sort())) throw new Error(`属性集合变化 ${uri}`);
      for (const name of names) {
        const av = a.accessor(oldPrimitive.attributes[name]), bv = b.accessor(newPrimitive.attributes[name]);
        if (av.count !== bv.count || av.type !== bv.type || av.components !== bv.components) throw new Error(`属性布局变化 ${uri} ${name}`);
        if (name === 'POSITION') {
          for (let i = 0; i < av.count; ++i) {
            const p = world(xyz(av.bytes, i), oldNode.matrix ?? identity, oldRoot);
            const q = world(xyz(bv.bytes, i), newNode.matrix ?? identity, newRoot);
            const difference = Math.hypot(p[0]-q[0], p[1]-q[1], p[2]-q[2]);
            maxWorldDeltaMeters = Math.max(maxWorldDeltaMeters, difference);
            if (!Number.isFinite(difference) || difference > tolerance) throw new Error(`世界坐标变化超过两份写出舍入与数值余量 ${uri}: ${difference} > ${tolerance}`);
            ++vertices;
          }
        } else if (name === 'NORMAL') {
          for (let i = 0; i < av.bytes.length; i += 4) {
            const difference = Math.abs(av.bytes.readFloatLE(i)-bv.bytes.readFloatLE(i));
            if (!Number.isFinite(difference)) throw new Error(`非有限法线 ${uri}`);
            maxNormalComponentDelta = Math.max(maxNormalComponentDelta, difference);
            ++normalComponents;
          }
        } else if (!av.bytes.equals(bv.bytes)) throw new Error(`未参与空间转换的属性变化 ${uri} ${name}`);
      }
    }
  }
  for (const image of oldContent.images) {
    if (images.has(image.uri)) continue;
    images.add(image.uri);
    const digest = root => crypto.createHash('sha256').update(fs.readFileSync(path.join(root, image.uri))).digest('hex');
    if (digest(before) !== digest(after)) throw new Error(`图片字节变化 ${image.uri}`);
    ++comparedImages;
  }
  ++comparedContents;
}
const result = { status: 'passed', decoderSha256: crypto.createHash('sha256').update(fs.readFileSync(args.decoder)).digest('hex'), comparedContents, vertices, normalComponents, comparedImages, maxWorldDeltaMeters, maxNormalComponentDelta, invariant: '索引/UV/颜色/图片字节相同；世界坐标差在两份写出舍入误差加1e-7m内', limitation: '该前后比较不能代替绝对坐标基准；法线只记录原分量差，独立解析正确性由spatial_cases验证；未测浏览器画面或GPU性能' };
fs.writeFileSync(args.output, JSON.stringify(result, null, 2) + '\n');
console.log(JSON.stringify(result, null, 2));
