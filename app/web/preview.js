import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
import { KTX2Loader } from 'three/addons/loaders/KTX2Loader.js';
import { MeshoptDecoder } from 'three/addons/libs/meshopt_decoder.module.js';
import { TilesRenderer } from '3d-tiles-renderer';

const parameters = new URLSearchParams(location.search);
const jobId = parameters.get('job') || '';
const token = parameters.get('token') || '';
const authHeaders = token ? { Authorization: `Bearer ${token}` } : {};
const tilesetURL = `/api/jobs/${encodeURIComponent(jobId)}/tiles/preview/tileset.json`;
const $ = selector => document.querySelector(selector);
const loadStarted = performance.now();

let renderer;
let controls;
let tiles;
let ktx2Loader;
let scene;
let camera;
let framePending = false;
let ready = false;
let failed = false;
let errorMessage = '';
let warningMessage = '';
let loadedMilliseconds = null;
let activeView = 'orbit';
let boundsSphere = null;
let modelLoaded = false;
let loadingPump = null;
let interacting = false;
let interactionTimer;
let noticeTimer;

function requestRender() {
  if (framePending || failed || !renderer) return;
  framePending = true;
  requestAnimationFrame(() => {
    framePending = false;
    if (!renderer || failed) return;
    controls?.update();
    tiles?.update();
    if (tiles?.root && !boundsSphere) {
      const sphere = new THREE.Sphere();
      if (tiles.getBoundingSphere(sphere)) { boundsSphere = sphere; fit('orbit'); }
    }
    renderer.render(scene, camera);
    updateStatistics();
  });
}

function setError(error) {
  failed = true;
  ready = false;
  errorMessage = String(error?.message || error || '未知错误');
  $('#loading').hidden = true;
  $('#error').hidden = false;
  $('#error-message').textContent = errorMessage;
  $('#load-state').textContent = '加载失败';
  $('#fit').disabled = true;
}

function showNotice(message) {
  warningMessage = message;
  clearTimeout(noticeTimer);
  $('#notice').textContent = message;
  $('#notice').hidden = false;
  noticeTimer = setTimeout(() => { $('#notice').hidden = true; }, 4200);
}

function closePreview() {
  if (window.parent !== window) window.parent.postMessage({ type: 'close-preview' }, location.origin);
  else location.href = '/' + (token ? `?token=${encodeURIComponent(token)}` : '');
}

function formatCount(value) {
  const number = value || 0;
  if (number >= 10000) return `${(number / 10000).toFixed(number >= 1000000 ? 0 : 1)} 万`;
  return new Intl.NumberFormat('zh-CN').format(number);
}

function markActiveView(view) {
  activeView = view;
  document.querySelectorAll('[data-view]').forEach(button => {
    const active = button.dataset.view === view;
    button.classList.toggle('active', active);
    button.setAttribute('aria-pressed', String(active));
  });
}

function fit(view = activeView) {
  if (!boundsSphere || !camera || !controls) return;
  markActiveView(view);
  const center = boundsSphere.center.clone();
  const radius = Math.max(boundsSphere.radius, .5);
  const outward = center.length() > 1000000 ? center.clone().normalize() : new THREE.Vector3(0, 1, 0);
  const reference = Math.abs(outward.y) > .95 ? new THREE.Vector3(1, 0, 0) : new THREE.Vector3(0, 1, 0);
  const tangent = new THREE.Vector3().crossVectors(outward, reference).normalize();
  const bitangent = new THREE.Vector3().crossVectors(outward, tangent).normalize();
  let direction = outward.clone().multiplyScalar(1.25).addScaledVector(tangent, .8).addScaledVector(bitangent, .35);
  if (view === 'top') direction = outward.clone();
  if (view === 'front') direction = tangent.clone().addScaledVector(outward, .08);
  camera.up.copy(outward);
  camera.position.copy(center).addScaledVector(direction.normalize(), radius * 2.1);
  camera.near = Math.max(.02, radius / 10000);
  camera.far = Math.max(10000, center.length() + radius * 20);
  camera.updateProjectionMatrix();
  controls.target.copy(center);
  controls.minDistance = Math.max(.05, radius / 1000);
  controls.maxDistance = Math.max(1000, radius * 20);
  controls.update();
  requestRender();
}

function sceneGeometryStatistics() {
  let meshes = 0;
  let triangles = 0;
  const groups = [];
  tiles?.group?.traverse(object => {
    if (!object.isMesh || !object.geometry) return;
    meshes++;
    const index = object.geometry.index;
    const count = index ? index.count : (object.geometry.attributes.position?.count || 0);
    triangles += count / 3;
    groups.push(...(object.geometry.groups || []).map(group => group.count));
  });
  return { meshes, triangles: Math.round(triangles), groups };
}

function updateStatistics() {
  if (!renderer) return;
  const geometry = sceneGeometryStatistics();
  $('#draw-calls').textContent = formatCount(renderer.info.render.calls);
  $('#triangles').textContent = formatCount(geometry.triangles);
  if (tiles?.root) $('#loading-progress').style.width = ready ? '100%' : '72%';
  if (!ready && boundsSphere && modelLoaded) {
    ready = true;
    loadedMilliseconds = Math.round(performance.now() - loadStarted);
    $('#loading').hidden = true;
    $('#stats').classList.add('ready');
    $('#fit').disabled = false;
    document.querySelectorAll('[data-view]').forEach(button => { button.disabled = false; });
    $('#load-state').textContent = '已就绪';
    fit('orbit');
  }
}

function markActivity() {
  interacting = true;
  $('#orbit-reticle').hidden = false;
  $('#viewport').classList.add('has-interacted');
  clearTimeout(interactionTimer);
  interactionTimer = setTimeout(() => {
    interacting = false;
    $('#orbit-reticle').hidden = true;
  }, 180);
  requestRender();
}

async function loadJobName() {
  if (!jobId) return;
  try {
    const response = await fetch(`/api/jobs/${encodeURIComponent(jobId)}`, { headers: authHeaders });
    if (!response.ok) return;
    const job = await response.json();
    const output = String(job.spec?.output || '').replace(/[\\/]+$/, '');
    $('#job-name').textContent = output.split(/[\\/]/).pop() || job.id;
  } catch { $('#job-name').textContent = jobId; }
}

// 只读诊断入口：供验收脚本检查场景对象，不在产品逻辑中写入。
window.__OSGB_LIGHT_DEBUG__ = {
  get renderer() { return renderer; },
  get scene() { return scene; },
  get tiles() { return tiles; },
  get camera() { return camera; },
};

// 自动化只读这份快照：轻量入口不会创建 Cesium，也不会请求原精细 tileset。
window.__OSGB_LIGHT_PREVIEW__ = {
  get state() {
    return {
      jobId,
      engine: 'three',
      threeRevision: THREE.REVISION,
      tileset: 'preview/tileset.json',
      ready,
      failed,
      error: errorMessage || null,
      warning: warningMessage || null,
      loadedMilliseconds,
      enablePick: false,
      requestRenderMode: true,
      resolutionScale: 1,
      meshoptDecoder: true,
      ktx2Loader: Boolean(ktx2Loader),
      activeView,
      interacting,
      drawCalls: renderer?.info.render.calls || 0,
      renderedTriangles: renderer?.info.render.triangles || 0,
      sceneTriangles: sceneGeometryStatistics().triangles,
      meshes: sceneGeometryStatistics().meshes,
      geometryGroups: sceneGeometryStatistics().groups,
      pixelRatio: renderer?.getPixelRatio() || 1,
      camera: camera ? { position: camera.position.toArray(), target: controls?.target.toArray() } : null,
    };
  },
};

async function start() {
  if (!jobId) { setError('预览地址缺少任务 ID，请从转换任务列表打开。'); return; }
  loadJobName();
  try {
    scene = new THREE.Scene();
    scene.background = new THREE.Color('#dce3e5');
    camera = new THREE.PerspectiveCamera(45, 1, .05, 1000000000);
    renderer = new THREE.WebGLRenderer({ antialias: true, alpha: false, powerPreference: 'high-performance', logarithmicDepthBuffer: true });
    renderer.setPixelRatio(window.devicePixelRatio || 1);
    renderer.outputColorSpace = THREE.SRGBColorSpace;
    renderer.toneMapping = THREE.NoToneMapping;
    renderer.domElement.style.touchAction = 'none';
    $('#three').appendChild(renderer.domElement);

    const manager = new THREE.LoadingManager();
    const gltfLoader = new GLTFLoader(manager);
    gltfLoader.setMeshoptDecoder(MeshoptDecoder);
    ktx2Loader = new KTX2Loader(manager)
      .setTranscoderPath('/vendor/three/examples/jsm/libs/basis/')
      .detectSupport(renderer);
    gltfLoader.setKTX2Loader(ktx2Loader);
    manager.addHandler(/\.glb$/, gltfLoader);
    manager.addHandler(/\.gltf$/, gltfLoader);

    const authenticatedURL = new URL(tilesetURL, location.origin);
    if (token) authenticatedURL.searchParams.set('token', token);
    tiles = new TilesRenderer(authenticatedURL.href);
    tiles.manager = manager;
    tiles.fetchOptions.headers = authHeaders;
    tiles.errorTarget = 0;
    tiles.lruCache.maxBytesSize = 1024 * 1024 * 1024;
    scene.add(tiles.group);
    tiles.setCamera(camera);
    tiles.setResolutionFromRenderer(camera, renderer);
    tiles.addEventListener('load-model', event => {
      modelLoaded = true;
      event.scene.traverse(object => {
        if (!object.material) return;
        for(const material of Array.isArray(object.material) ? object.material : [object.material]) {
          for(const value of Object.values(material)) if (value?.isTexture) value.anisotropy = Math.min(4, renderer.capabilities.getMaxAnisotropy());
        }
      });
      requestRender();
    });
    tiles.addEventListener('load-error', event => {
      showNotice(`部分预览内容加载失败：${event.error?.message || event.error || '未知错误'}`);
      requestRender();
    });
    tiles.addEventListener('tiles-load-end', requestRender);

    controls = new OrbitControls(camera, renderer.domElement);
    controls.enableDamping = true;
    controls.dampingFactor = .12;
    controls.enablePan = true;
    controls.enableZoom = true;
    controls.enableRotate = true;
    controls.addEventListener('change', requestRender);
    controls.addEventListener('start', markActivity);
    controls.addEventListener('end', markActivity);

    const resize = () => {
      const width = Math.max(1, $('#viewport').clientWidth);
      const height = Math.max(1, $('#viewport').clientHeight);
      renderer.setSize(width, height, false);
      camera.aspect = width / height;
      camera.updateProjectionMatrix();
      tiles.setResolutionFromRenderer(camera, renderer);
      requestRender();
    };
    new ResizeObserver(resize).observe($('#viewport'));
    resize();
    // 根 JSON 到达后先取得包围体并移动相机，瓦片遍历才会请求 GLB；首帧后回到按需渲染。
    loadingPump = setInterval(() => {
      if (ready || failed) { clearInterval(loadingPump); loadingPump = null; return; }
      requestRender();
    }, 100);
    requestRender();
  } catch (error) { setError(error); }
}

$('#close').addEventListener('click', closePreview);
$('#fit').addEventListener('click', () => fit('orbit'));
$('#retry').addEventListener('click', () => location.reload());
$('#fullscreen').addEventListener('click', async () => {
  try {
    if (document.fullscreenElement) await document.exitFullscreen();
    else await document.documentElement.requestFullscreen();
  } catch { showNotice('当前窗口不支持全屏，可放大应用窗口查看'); }
});
document.querySelectorAll('[data-view]').forEach(button => button.addEventListener('click', () => fit(button.dataset.view)));
window.addEventListener('keydown', event => {
  if (event.target instanceof HTMLInputElement || event.target instanceof HTMLSelectElement) return;
  if (event.key.toLowerCase() === 'h') fit('orbit');
  if (event.key === 'Escape' && !document.fullscreenElement && window.parent !== window) closePreview();
});
window.addEventListener('beforeunload', () => {
  clearTimeout(noticeTimer);
  clearTimeout(interactionTimer);
  if (loadingPump) clearInterval(loadingPump);
  controls?.dispose();
  tiles?.dispose();
  ktx2Loader?.dispose();
  renderer?.dispose();
});

start();
