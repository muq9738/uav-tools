#!/usr/bin/env node
// Exercise the embedded App form and inspect the native result, using ego-browser.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
const [packageArg,inputArg,evidenceArg]=process.argv.slice(2);
assert(evidenceArg,'usage: verify_codecs_ui.mjs PORTABLE_PACKAGE OSGB_FIXTURE EVIDENCE_DIR');
const packageDir=path.resolve(packageArg),input=path.resolve(inputArg),evidence=path.resolve(evidenceArg);fs.mkdirSync(evidence,{recursive:true});
const app=spawn(path.join(packageDir,'osgb-tiles-app'),['--headless','--addr','127.0.0.1:0','--data-dir',path.join(evidence,'state')],{stdio:['ignore','pipe','pipe']});
let log='';app.stdout.on('data',b=>log+=b);app.stderr.on('data',b=>log+=b);
const started=Date.now();while(!log.match(/http:\/\/127\.0\.0\.1:\d+\//)){assert(Date.now()-started<15000,'App startup timeout');await new Promise(r=>setTimeout(r,50))}
const url=log.match(/http:\/\/127\.0\.0\.1:\d+\//)[0];
function ego(script){return new Promise((resolve,reject)=>{const p=spawn('ego-browser',['nodejs'],{stdio:['pipe','pipe','pipe']});let text='';p.stdout.on('data',b=>text+=b);p.stderr.on('data',b=>text+=b);p.on('error',reject);p.on('close',code=>code?reject(new Error(text)):resolve(text));p.stdin.end(script)})}
try{
 const previous=fs.existsSync(path.join(evidence,'initial.log'))?Number(fs.readFileSync(path.join(evidence,'initial.log'),'utf8').match(/"spaceId":(\d+)/)?.[1]):null;
 const first=await ego(`const task=await taskSpace(${previous||JSON.stringify('native codec App form')});const page=task.page("p1");await page.cdp("Emulation.setDeviceMetricsOverride",{width:1280,height:900,deviceScaleFactor:1,mobile:false});await page.goto(${JSON.stringify(url)});await page.waitForFunction(()=>document.querySelector('#textureFormat option[value="etc1s"]').disabled===false);console.log(JSON.stringify({spaceId:task.spaceId}));console.log(await page.snapshot());`);
 fs.writeFileSync(path.join(evidence,'initial.log'),first);const space=Number(first.match(/"spaceId":(\d+)/)?.[1]);assert(space);
 const chosen=await ego(`const task=await taskSpace(${space});const page=task.page("p1");await page.click('#add');await page.fill('#input',${JSON.stringify(input)});await page.fill('#output',${JSON.stringify(path.join(evidence,'output'))});await page.click('.advanced summary');await page.fill('#srs','ENU:0,0');await page.selectOption('#geometry','draco');await page.selectOption('#dracoSpeed','10');await page.selectOption('#textureFormat','etc1s');await page.selectOption('#rebuildTop','true');await page.evaluate(async()=>{const response=await fetch('/api/jobs');window.codecOldJobIds=new Set((await response.json()).jobs.map(job=>job.id))});await page.click('#submit');await page.waitForFunction(async()=>{const response=await fetch('/api/jobs');const body=await response.json();return body.jobs.some(job=>!window.codecOldJobIds.has(job.id)&&['succeeded','failed'].includes(job.state))},undefined,{timeout:60000});console.log(JSON.stringify(await page.evaluate(async()=>{const response=await fetch('/api/jobs');const body=await response.json();return {jobs:body.jobs.filter(job=>!window.codecOldJobIds.has(job.id))}})));await page.screenshot({path:${JSON.stringify(path.join(evidence,'form-result.png'))}});`);
 fs.writeFileSync(path.join(evidence,'result.log'),chosen);const line=chosen.split('\n').find(x=>x.startsWith('{"jobs":'));assert(line);const result=JSON.parse(line),job=result.jobs[0];
 assert.equal(job.state,'succeeded',JSON.stringify(job));assert.equal(job.spec.geometry,'draco');assert.equal(job.spec.textureFormat,'etc1s');assert.equal(job.spec.rebuildTop,true);assert.equal(job.spec.dracoSpeed,10);
 assert.equal(job.report.geometryEncoding,'KHR_draco_mesh_compression');assert.equal(job.report.textureFormat,'etc1s');assert.equal(job.report.topGlbs,1);assert.equal(job.report.dracoSpeed,10);
 fs.writeFileSync(path.join(evidence,'result.json'),JSON.stringify({passed:true,job},null,2)+'\n');
 await ego(`const task=await taskSpace(${space});await task.finish({keep:[]});`);
 console.log(`App form/native codec checks passed: ${evidence}`);
}finally{app.kill('SIGTERM');fs.writeFileSync(path.join(evidence,'app.log'),log)}
