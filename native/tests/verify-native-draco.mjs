#!/usr/bin/env node
// Independent public WASM decoder oracle against the raw/meshopt source GLBs.
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import crypto from 'node:crypto';
import {createRequire} from 'node:module';
import assert from 'node:assert/strict';
import {parseGlb,accessorReader,valuesAt} from './glb-accessors.mjs';
const [baseArgument,encodedArgument,outputArgument,decoderArgument,meshoptArgument]=process.argv.slice(2);
assert(outputArgument && decoderArgument && meshoptArgument,'usage: verify-native-draco.mjs BASELINE_DIR ENCODED_DIR REPORT_JSON PUBLIC_DRACO_WRAPPER PUBLIC_MESHOPT_MODULE');
const baseline=path.resolve(baseArgument),encoded=path.resolve(encodedArgument);
const decoder=path.resolve(decoderArgument);
const meshoptPath=path.resolve(meshoptArgument);
const code=fs.readFileSync(decoder,'utf8'),moduleBox={exports:{}};
vm.runInNewContext(code,{module:moduleBox,exports:moduleBox.exports,require:createRequire(decoder),__dirname:path.dirname(decoder),__filename:decoder,process,console,Buffer,WebAssembly,TextDecoder,TextEncoder,setTimeout,clearTimeout,performance},{filename:decoder});
const draco=await moduleBox.exports({wasmBinary:fs.readFileSync(path.join(path.dirname(decoder),'draco_decoder.wasm'))});
const {MeshoptDecoder}=await import(meshoptPath);await MeshoptDecoder.ready;
const sha=b=>crypto.createHash('sha256').update(b).digest('hex');
const rawReport=JSON.parse(fs.readFileSync(path.join(baseline,'conversion-report.json'))),encodedReport=JSON.parse(fs.readFileSync(path.join(encoded,'conversion-report.json')));
const sources=new Map(rawReport.records.map(row=>[row.source,row]));let rows=[];
function multiset(values){const m=new Map();for(const v of values)m.set(v,(m.get(v)||0)+1);return [...m].sort(([a],[b])=>a.localeCompare(b))}
function faceKey(c){return [0,1,2].map(i=>[c[i],c[(i+1)%3],c[(i+2)%3]].join('|')).sort()[0]}
function compare(baseFile,encodedFile){
 const raw=parseGlb(fs.readFileSync(baseFile)),packed=parseGlb(fs.readFileSync(encodedFile)),read=accessorReader(raw.doc,raw.binary,MeshoptDecoder);let primitives=[];
 const originalPrimitives=raw.doc.meshes.flatMap(m=>m.primitives),compressedPrimitives=packed.doc.meshes.flatMap(m=>m.primitives);assert.equal(originalPrimitives.length,compressedPrimitives.length);
 for(let pi=0;pi<originalPrimitives.length;pi++){
  const p=originalPrimitives[pi],q=compressedPrimitives[pi],extension=q.extensions.KHR_draco_mesh_compression,view=packed.doc.bufferViews[extension.bufferView];
  const bytes=packed.binary.subarray(view.byteOffset||0,(view.byteOffset||0)+view.byteLength),decoderObject=new draco.Decoder(),buffer=new draco.DecoderBuffer();
  for(const type of [draco.POSITION,draco.TEX_COORD,draco.COLOR])decoderObject.SkipAttributeTransform(type);
  buffer.Init(bytes,bytes.length);const mesh=new draco.Mesh(),status=decoderObject.DecodeBufferToMesh(buffer,mesh);assert(status.ok(),status.error_msg());
  const sourceAttributes=Object.fromEntries(Object.entries(p.attributes).map(([name,index])=>[name,read(index)]));
  const count=sourceAttributes.POSITION.count;assert.equal(mesh.num_points(),count);const indices=read(p.indices);assert.equal(mesh.num_faces()*3,indices.count);
  const sourceKeys=Array(count).fill(''),decodedKeys=Array(count).fill('');let maximumLocalPositionError=0,maximumUvError=0;const quantization={};
  for(const [name,id]of Object.entries(extension.attributes)){
   const attribute=decoderObject.GetAttributeByUniqueId(mesh,id),source=sourceAttributes[name];assert.equal(attribute.num_components(),source.components);
   if(name==='NORMAL')continue;
   const transform=new draco.AttributeQuantizationTransform();assert(transform.InitFromAttribute(attribute));
   const bits=transform.quantization_bits(),range=transform.range(),minimum=Array.from({length:source.components},(_,i)=>transform.min_value(i));
   assert.equal(bits,name==='POSITION'?14:name==='COLOR_0'?8:12);quantization[name]={bits,range,minimum};
   const array=new draco.DracoUInt16Array();assert(decoderObject.GetAttributeUInt16ForAllPoints(mesh,attribute,array));
   for(let i=0;i<count;i++){
    const value=valuesAt(source,i),expected=value.map((v,k)=>Math.floor(Math.fround(Math.fround(v-minimum[k])*Math.fround(((1<<bits)-1)/range))+.5));
    const actual=value.map((_,k)=>array.GetValue(i*source.components+k));sourceKeys[i]+=name+':'+expected.join(',')+';';decodedKeys[i]+=name+':'+actual.join(',')+';';
    if(name==='POSITION')maximumLocalPositionError=Math.max(maximumLocalPositionError,Math.hypot(...expected.map((v,k)=>minimum[k]+v*range/((1<<bits)-1)-value[k])));
    if(name==='TEXCOORD_0')maximumUvError=Math.max(maximumUvError,...expected.map((v,k)=>Math.abs(minimum[k]+v*range/((1<<bits)-1)-value[k])));
   }
   draco.destroy(array);draco.destroy(transform);
  }
  assert.deepEqual(multiset(sourceKeys),multiset(decodedKeys),'quantized point/unused-point multiplicity');
  const before=[],after=[],face=new draco.DracoInt32Array();
  for(let i=0;i<indices.count;i+=3)before.push(faceKey([0,1,2].map(k=>sourceKeys[valuesAt(indices,i+k)[0]])));
  for(let i=0;i<mesh.num_faces();i++){decoderObject.GetFaceFromMesh(mesh,i,face);after.push(faceKey([0,1,2].map(k=>decodedKeys[face.GetValue(k)])))}
  assert.deepEqual(multiset(before),multiset(after),'all directed quantized faces including degenerate faces');
  primitives.push({points:count,triangles:mesh.num_faces(),quantization,maximumLocalPositionError,maximumUvError,coveragePassed:true});
  for(const item of [face,mesh,buffer,decoderObject])draco.destroy(item);
 }
 return {baseline:baseFile,encoded:encodedFile,primitives};
}
for(const row of encodedReport.records){
 const source=sources.get(row.source);assert(source,'source ledger mismatch');assert.equal(source.content.length,row.content.length);
 for(let i=0;i<row.content.length;i++)rows.push(compare(path.join(baseline,source.content[i].uri),path.join(encoded,row.content[i].uri)));
}
for(let i=0;i<(encodedReport.topAncestors||[]).length;i++){
 const base=rawReport.topAncestors?.[i];if(base)rows.push(compare(path.join(baseline,base.content),path.join(encoded,encodedReport.topAncestors[i].content)));
}
const result={passed:true,decoder,decoderSha256:sha(Buffer.from(code)),decoderWasmSha256:sha(fs.readFileSync(path.join(path.dirname(decoder),'draco_decoder.wasm'))),meshoptPath,scope:'POSITION/UV/COLOR quantized point and directed face multisets; normals verified by native tests, not this oracle',contents:rows};
fs.writeFileSync(outputArgument,JSON.stringify(result,null,2)+'\n');console.log(JSON.stringify({passed:true,contents:rows.length,primitives:rows.reduce((n,r)=>n+r.primitives.length,0),report:outputArgument}));
