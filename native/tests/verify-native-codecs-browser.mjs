#!/usr/bin/env node
// Functional Cesium 1.136 GPU-consumption oracle. No viewer policy changes.
import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';
import { spawn } from 'node:child_process';
import assert from 'node:assert/strict';
const [fixtureArgument,outputArgument,cesiumArgument,variantArgument,modeArgument]=process.argv.slice(2);
const variants=(variantArgument||'draco-5,uastc,etc1s,resumed').split(',');
const tilesetMode=modeArgument==='tileset';
assert(fixtureArgument && outputArgument && cesiumArgument,'usage: verify-native-codecs-browser.mjs FIXTURE_ROOT OUTPUT_DIR CESIUM_BUILD_1_136');
const root=path.resolve(fixtureArgument),output=path.resolve(outputArgument);
const cesium=path.resolve(cesiumArgument);
fs.mkdirSync(output,{recursive:true});
const html=`<!doctype html><html><head><link rel="stylesheet" href="/cesium/Widgets/widgets.css"><style>html,body,#view{margin:0;width:100%;height:100%;overflow:hidden}</style></head><body><div id="view"></div><script>
window.uploads=[];window.errors=[];
for(const Type of [WebGLRenderingContext,WebGL2RenderingContext]) for(const name of ['compressedTexImage2D','texImage2D']) {
 if(!Object.hasOwn(Type.prototype,name))continue;const original=Type.prototype[name];
 Type.prototype[name]=function(...args){ if(name==='compressedTexImage2D')window.uploads.push({level:args[1],format:args[2],width:args[3],height:args[4],bytes:args[6]?.byteLength||0});return original.apply(this,args) };
}
window.addEventListener('error',e=>window.errors.push(e.message));
window.CESIUM_BASE_URL='/cesium/';
</script><script src="/cesium/Cesium.js"></script><script>
window.results=[];window.done=false;
window.run=async()=>{
 try {
 const C=Cesium;if(C.VERSION!=='1.136.0')throw new Error('Wrong Cesium version '+C.VERSION);
 const viewer=new C.Viewer('view',{baseLayer:false,globe:false,skyBox:false,skyAtmosphere:false,baseLayerPicker:false,animation:false,timeline:false,geocoder:false,homeButton:false,sceneModePicker:false,navigationHelpButton:false,fullscreenButton:false,contextOptions:{webgl:{alpha:true,preserveDrawingBuffer:true}}});
 window.viewer=viewer;viewer.scene.backgroundColor=C.Color.TRANSPARENT;
 const tick=()=>new Promise(resolve=>requestAnimationFrame(resolve));
 const gl=viewer.scene.context._gl;const extensions=gl.getSupportedExtensions();
 let fixedSphere;
 for(const variant of ${JSON.stringify(variants)}) {
  window.uploads=[];const tileset=await(await fetch('/assets/'+variant+'/tileset.json')).json();
  let tile=tileset.root;while(!tile.content)tile=tile.children[0];
  const uri=tile.content.uri;
  const model=${tilesetMode?"await C.Cesium3DTileset.fromUrl('/assets/'+variant+'/tileset.json',{maximumScreenSpaceError:8})":"await C.Model.fromGltfAsync({url:'/assets/'+variant+'/'+uri,modelMatrix:C.Matrix4.fromArray(tileset.root.transform),incrementallyLoadTextures:false})"};
  viewer.scene.primitives.add(model);const start=performance.now();
  ${tilesetMode?'':'while(!model.ready){if(performance.now()-start>20000)throw new Error("Model readiness timeout "+variant);await tick()}'}
  if(!fixedSphere)fixedSphere=C.BoundingSphere.clone(model.boundingSphere);
  viewer.camera.viewBoundingSphere(fixedSphere,new C.HeadingPitchRange(0,-Math.PI/2,fixedSphere.radius*3));viewer.camera.lookAtTransform(C.Matrix4.IDENTITY);
  ${tilesetMode?'let stable=0;while(stable<6){if(performance.now()-start>20000)throw new Error("Tileset readiness timeout "+variant);await tick();stable=model.tilesLoaded&&model._selectedTiles.length?stable+1:0}':''}
  for(let i=0;i<12;i++)await tick();
  const pixels=new Uint8Array(gl.drawingBufferWidth*gl.drawingBufferHeight*4);gl.readPixels(0,0,gl.drawingBufferWidth,gl.drawingBufferHeight,gl.RGBA,gl.UNSIGNED_BYTE,pixels);
  let visible=0,partialAlpha=0;for(let i=0;i<pixels.length;i+=4)if(pixels[i]||pixels[i+1]||pixels[i+2]){visible++;if(pixels[i+3]>0&&pixels[i+3]<255)partialAlpha++}
  const error=gl.getError();window.results.push({variant,ready:${tilesetMode?'model.tilesLoaded':'model.ready'},mode:${JSON.stringify(tilesetMode?'tileset':'model')},selectedTiles:${tilesetMode?'model._selectedTiles.length':'null'},visiblePixels:visible,partialAlphaPixels:partialAlpha,glError:error,uploads:[...window.uploads],extensions,cesium:C.VERSION});
  if(!visible||${tilesetMode?'false':'!partialAlpha'}||error)throw new Error('Rendered pixel/alpha/GL assertion failed '+variant);
  if((variant.includes('uastc')||variant.includes('etc1s')||variant==='resumed')&&(!window.uploads.length||!window.uploads.some(u=>u.level===3)))throw new Error('KTX2 compressed mip upload missing '+variant);
  if(variant!==${JSON.stringify(variants.at(-1))})viewer.scene.primitives.remove(model);
 }
 window.done=true;
 }catch(e){window.failure=e.stack||String(e);window.done=true}
};
</script></body></html>`;
fs.writeFileSync(path.join(output,'oracle.html'),html);
const mime={'.js':'application/javascript','.css':'text/css','.json':'application/json','.wasm':'application/wasm','.ktx2':'image/ktx2','.glb':'model/gltf-binary','.png':'image/png','.jpg':'image/jpeg'};
const server=http.createServer((req,res)=>{
 const url=new URL(req.url,'http://localhost');let file;
 if(url.pathname==='/'){res.setHeader('content-type','text/html');res.end(html);return}
 if(url.pathname.startsWith('/cesium/'))file=path.resolve(cesium,'.'+url.pathname.slice(7));
 else if(url.pathname.startsWith('/assets/'))file=path.resolve(root,'.'+url.pathname.slice(7));
 if(!file||(!file.startsWith(cesium+path.sep)&&!file.startsWith(root+path.sep))||!fs.existsSync(file)||!fs.statSync(file).isFile()){res.writeHead(404);res.end();return}
 res.setHeader('content-type',mime[path.extname(file)]||'application/octet-stream');fs.createReadStream(file).pipe(res);
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
function ego(script){return new Promise((resolve,reject)=>{const p=spawn('ego-browser',['nodejs'],{stdio:['pipe','pipe','pipe']});let stdout='',stderr='';p.stdout.on('data',x=>stdout+=x);p.stderr.on('data',x=>stderr+=x);p.on('error',reject);p.on('close',code=>code?reject(new Error(stdout+'\n'+stderr)):resolve({stdout,stderr}));p.stdin.end(script)})}
let space;
try{
 const url=`http://127.0.0.1:${server.address().port}/`;
 const previous=fs.existsSync(path.join(output,'start.log'))?Number(fs.readFileSync(path.join(output,'start.log'),'utf8').match(/"spaceId":(\d+)/)?.[1]):null;
 const opened=await ego(`const task=await taskSpace(${previous||JSON.stringify('native codec consumption')});console.log(JSON.stringify({spaceId:task.spaceId}));const page=task.page("p1");await page.cdp("Emulation.setDeviceMetricsOverride",{width:1920,height:851,deviceScaleFactor:1,mobile:false});await page.goto(${JSON.stringify(url)});await page.evaluate(()=>{window.run()});console.log(JSON.stringify({started:true}));`);
 fs.writeFileSync(path.join(output,'start.log'),opened.stdout+'\n'+opened.stderr);
 space=Number((opened.stdout+'\n'+opened.stderr).match(/"spaceId":(\d+)/)?.[1]);assert(space,'no task space id');
 const measured=await ego(`const task=await taskSpace(${space});const page=task.page("p1");await page.waitForFunction(()=>window.done,undefined,{timeout:60000});const result=await page.evaluate(()=>({results:window.results,errors:window.errors,failure:window.failure||null}));console.log(JSON.stringify(result));await page.screenshot({path:${JSON.stringify(path.join(output,'render.png'))}});`);
 fs.writeFileSync(path.join(output,'result.log'),measured.stdout+'\n'+measured.stderr);
 const line=(measured.stdout+'\n'+measured.stderr).split('\n').find(x=>x.startsWith('{"results":'));assert(line,'no browser result');const result=JSON.parse(line);fs.writeFileSync(path.join(output,'result.json'),JSON.stringify(result,null,2)+'\n');
 assert.equal(result.failure,null);assert.equal(result.errors.length,0);assert.equal(result.results.length,variants.length);
 // Observe success first; finish in its own last browser invocation.
 await ego(`const task=await taskSpace(${space});await task.finish({keep:[]});`);space=undefined;
 console.log(`Cesium codec GPU checks passed: ${output}`);
}finally{server.close();}
