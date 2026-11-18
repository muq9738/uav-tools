// 独立验收工具，不属于转换运行时。用目标应用的 meshopt WASM 验证原生输出。
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {assertOrientedTriangles} from './oriented-triangles.mjs';
const [baseline,candidate,decoderFile]=process.argv.slice(2);
if(!decoderFile) throw Error('用法: node tests/verify-render-equivalence.mjs <raw基线目录> <候选目录> <应用meshopt_decoder.mjs>');
const {MeshoptDecoder}=await import(pathToFileURL(path.resolve(decoderFile)).href);await MeshoptDecoder.ready;
const read=async file=>{
  const bytes=await fs.readFile(file);assert.equal(bytes.readUInt32LE(0),0x46546c67);assert.equal(bytes.readUInt32LE(8),bytes.length);
  const length=bytes.readUInt32LE(12),doc=JSON.parse(bytes.subarray(20,20+length)),bin=bytes.subarray(28+length),cache=new Map();
  const virtual=doc.bufferViews.filter(v=>v.extensions?.EXT_meshopt_compression).map(v=>[v.byteOffset||0,(v.byteOffset||0)+v.byteLength,v.buffer]).sort((a,b)=>a[0]-b[0]);
  let end=0;for(const [start,stop,buffer] of virtual){assert.equal(start%4,0);assert.ok(start>=end,'虚拟视图重叠，可能使 Cesium 索引缓存别名');assert.ok(stop<=doc.buffers[buffer].byteLength);end=stop;}
  const physical=doc.bufferViews.map(v=>{const e=v.extensions?.EXT_meshopt_compression;return e?[e.byteOffset||0,(e.byteOffset||0)+e.byteLength]:[v.byteOffset||0,(v.byteOffset||0)+v.byteLength];}).sort((a,b)=>a[0]-b[0]);
  end=0;for(const [start,stop] of physical){assert.equal(start%4,0);assert.ok(start>=end,'物理视图重叠');assert.ok(stop<=doc.buffers[0].byteLength);end=stop;}
  const view=id=>{
    if(cache.has(id))return cache.get(id);const v=doc.bufferViews[id],e=v.extensions?.EXT_meshopt_compression;let data;
    if(e){assert.equal(e.buffer,0);assert.equal(v.buffer,1);assert.equal(e.count*e.byteStride,v.byteLength);assert.equal(e.filter,'NONE');assert.equal(bin[e.byteOffset||0],{ATTRIBUTES:0xa0,INDICES:0xd1,TRIANGLES:0xe1}[e.mode],'压缩声明与位流不符');if(e.mode==='TRIANGLES'){assert.ok([2,4].includes(e.byteStride));assert.equal(e.count%3,0);}assert.ok(e.byteOffset+e.byteLength<=bin.length);data=Buffer.alloc(v.byteLength);MeshoptDecoder.decodeGltfBuffer(data,e.count,e.byteStride,bin.subarray(e.byteOffset,e.byteOffset+e.byteLength),e.mode,e.filter);}
    else {assert.equal(v.buffer,0);data=bin.subarray(v.byteOffset||0,(v.byteOffset||0)+v.byteLength);assert.equal(data.length,v.byteLength);}
    cache.set(id,data);return data;
  };
  const accessor=(id,index=false)=>{
    const a=doc.accessors[id],v=doc.bufferViews[a.bufferView],data=view(a.bufferView);assert.equal(a.byteOffset||0,0);assert.equal(v.byteStride||0,0);
    if(index){assert.equal(a.type,'SCALAR');assert.ok([5123,5125].includes(a.componentType));const T=a.componentType===5123?Uint16Array:Uint32Array;const indices=new T(data.buffer,data.byteOffset,a.count);assert.ok(!indices.includes(a.componentType===5123?65535:4294967295),'索引含 primitive restart 保留值');return {a,width:1,data:indices};}
    assert.equal(a.componentType,5126);const width=Number(a.type.slice(3));assert.ok(width>=2&&width<=4);assert.equal(data.length,a.count*width*4);return {a,width,data:new Uint32Array(data.buffer,data.byteOffset,a.count*width)};
  };
  return {doc,accessor,view};
};
const files=(await fs.readdir(path.join(baseline,'content'))).sort();assert.deepEqual(files,(await fs.readdir(path.join(candidate,'content'))).sort());
let vertices=0,drawCorners=0,views=0,rotatedFaces=0;
for(const file of files){
  const a=await read(path.join(baseline,'content',file)),b=await read(path.join(candidate,'content',file));
  for(const key of ['asset','scene','scenes','nodes','meshes','materials','images','textures','samplers'])assert.deepEqual(a.doc[key],b.doc[key],`${file}: ${key}`);
  assert.deepEqual(a.doc.accessors,b.doc.accessors,`${file}: accessor metadata`);
  const groups=new Map();for(const mesh of a.doc.meshes)for(const p of mesh.primitives){const key=p.attributes.POSITION;if(!groups.has(key))groups.set(key,[]);groups.get(key).push(p);}
  for(const [position,primitives] of groups){
    const count=a.doc.accessors[position].count,used=[new Uint8Array(count),new Uint8Array(count)];
    const attributes=new Set(primitives.flatMap(p=>Object.values(p.attributes)));
    const pairs=[...attributes].map(id=>{const old=a.accessor(id),next=b.accessor(id);assert.equal(old.a.count,count);assert.equal(next.a.count,count);assert.equal(old.width,next.width);return [old,next];});
    const sameCorner=(x,y)=>pairs.every(([old,next])=>{for(let c=0;c<old.width;c++)if(old.data[x*old.width+c]!==next.data[y*next.width+c])return false;return true;});
    for(const p of primitives){
      const old=a.accessor(p.indices,true).data,next=b.accessor(p.indices,true).data;assert.equal(old.length,next.length);
      for(const [side,indices] of [old,next].entries())for(const id of indices){assert.ok(id<count);used[side][id]=1;}
      const e=b.doc.bufferViews[b.doc.accessors[p.indices].bufferView].extensions?.EXT_meshopt_compression;
      if(e?.mode==='TRIANGLES')assert.equal(p.mode??4,4,'TRIANGLES applied to non-triangle primitive');
      if((p.mode??4)===4)rotatedFaces+=assertOrientedTriangles(old,next,sameCorner,file);
      else {assert.deepEqual(old,next,`${file}: 点线索引顺序改变`);for(let k=0;k<old.length;k++)assert.ok(sameCorner(old[k],next[k]),`${file}: 点线角点顺序/属性改变`);}
      drawCorners+=old.length;
    }
    // 数量和完整属性元组的多重集也核对，不能删除未引用顶点或合并重复槽位。
    const rows=(side,unused)=>{const result=[];for(let i=0;i<count;i++)if(!unused||!used[side][i]){const words=[];for(const pair of pairs){const attr=pair[side];for(let c=0;c<attr.width;c++)words.push(attr.data[i*attr.width+c]);}result.push(words.join(','));}return result.sort();};
    assert.deepEqual(rows(0,false),rows(1,false),`${file}: 全顶点属性多重集改变`);
    assert.deepEqual(rows(0,true),rows(1,true),`${file}: 未引用顶点改变`);vertices+=count;
  }
  for(const model of [a,b])for(let i=0;i<model.doc.bufferViews.length;i++)model.view(i);views+=b.doc.bufferViews.length;
}
const textures=(await fs.readdir(path.join(baseline,'textures'))).sort();assert.deepEqual(textures,(await fs.readdir(path.join(candidate,'textures'))).sort());
for(const file of textures)assert.ok((await fs.readFile(path.join(baseline,'textures',file))).equals(await fs.readFile(path.join(candidate,'textures',file))),`${file}: 图片被改写`);
assert.deepEqual(JSON.parse(await fs.readFile(path.join(baseline,'tileset.json'))),JSON.parse(await fs.readFile(path.join(candidate,'tileset.json'))));
const manifests=(await fs.readdir(path.join(baseline,'tiles'))).sort();assert.deepEqual(manifests,(await fs.readdir(path.join(candidate,'tiles'))).sort());
for(const file of manifests)assert.deepEqual(JSON.parse(await fs.readFile(path.join(baseline,'tiles',file))),JSON.parse(await fs.readFile(path.join(candidate,'tiles',file))),`${file}: 分文件清单改变`);
console.log(JSON.stringify({files:files.length,views,vertices,drawCorners,rotatedFaces,textures:textures.length,manifests:manifests.length,orientedFaceSequenceExact:true,physicalRangesDisjoint:true,indexedAttributeBitsExact:true,unusedVerticesPreserved:true,drawOrderPreserved:true,nodeMatricesUnchanged:true,tilesetUnchanged:true,virtualRangesDisjoint:true,reservedIndexValuesAbsent:true},null,2));
