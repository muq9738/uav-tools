/* global Cesium */

import { installModelCamera } from './viewer-camera.js';

const C = Cesium;
const parameters = new URLSearchParams(location.search);
const jobId = parameters.get('job') || '';
const token = parameters.get('token') || '';
const $ = selector => document.querySelector(selector);

let viewer;
let tileset;
let modelCamera;
let ready = false;
let failed = false;
let errorMessage = '';
let warningMessage = '';
let loadedMilliseconds = null;
let activeView = 'orbit';
let noticeTimer;
let anisotropy = 1;
const loadStarted = performance.now();
const authHeaders = token ? { Authorization: `Bearer ${token}` } : {};
const tilesetURL = `/api/jobs/${encodeURIComponent(jobId)}/tiles/tileset.json`;

function formatCount(value) {
  const number = value || 0;
  if (number >= 10000) return `${(number / 10000).toFixed(number >= 1000000 ? 0 : 1)} 万`;
  return new Intl.NumberFormat('zh-CN').format(number);
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
  if (window.parent !== window) {
    window.parent.postMessage({ type: 'close-preview' }, location.origin);
  } else {
    location.href = '/' + (token ? `?token=${encodeURIComponent(token)}` : '');
  }
}

function markActiveView(view) {
  activeView = view;
  document.querySelectorAll('[data-view]').forEach(button => {
    const active = button.dataset.view === view;
    button.classList.toggle('active', active);
    button.setAttribute('aria-pressed', String(active));
  });
}

async function setView(view) {
  if (!modelCamera || failed) return;
  markActiveView(view);
  await modelCamera.flyTo(view);
}

async function toggleFullscreen() {
  try {
    if (document.fullscreenElement) await document.exitFullscreen();
    else await document.documentElement.requestFullscreen();
  } catch {
    showNotice('当前窗口不支持全屏，可放大应用窗口查看');
  }
}

async function loadJobName() {
  if (!jobId) return;
  try {
    const response = await fetch(`/api/jobs/${encodeURIComponent(jobId)}`, { headers: authHeaders });
    if (!response.ok) return;
    const job = await response.json();
    const output = String(job.spec?.output || '').replace(/[\\/]+$/, '');
    $('#job-name').textContent = output.split(/[\\/]/).pop() || job.id;
  } catch {
    $('#job-name').textContent = jobId;
  }
}

function updateStatistics() {
  if (!tileset || failed) return;
  const stats = tileset.statistics;
  const pending = stats.numberOfPendingRequests || 0;
  const processing = stats.numberOfTilesProcessing || 0;
  const triangles = stats.numberOfTrianglesSelected || 0;
  $('#selected').textContent = formatCount(stats.selected);
  $('#triangles').textContent = formatCount(triangles);
  if (!ready) {
    $('#loading-detail').textContent = pending + processing > 0
      ? `正在加载瓦片 · 等待 ${pending} · 解码 ${processing}`
      : '正在生成首帧';
    $('#loading-progress').style.width = `${Math.min(92, 18 + stats.numberOfTilesWithContentReady * 2)}%`;
  }
  if (tileset.tilesLoaded && triangles > 0 && pending === 0 && processing === 0) {
    if (!ready) {
      ready = true;
      loadedMilliseconds = Math.round(performance.now() - loadStarted);
      $('#loading-progress').style.width = '100%';
      $('#loading').hidden = true;
      $('#stats').classList.add('ready');
      $('#fit').disabled = false;
      document.querySelectorAll('[data-view]').forEach(button => { button.disabled = false; });
      modelCamera?.refreshCenter();
    }
    $('#load-state').textContent = '已就绪';
  } else {
    $('#load-state').textContent = pending + processing > 0 ? `加载 ${pending + processing}` : '加载中';
  }
}

function patchTextureSampling() {
  const util = C.GltfLoaderUtil;
  if (!util || typeof util.createSampler !== 'function' || util.__modelPreviewAnisotropy) return;
  util.__modelPreviewAnisotropy = true;
  const original = util.createSampler.bind(util);
  util.createSampler = options => {
    const sampler = original(options);
    const supported = Number(C.ContextLimits.maximumTextureFilterAnisotropy || 1);
    anisotropy = Math.min(4, Math.max(1, supported));
    if (anisotropy <= 1) return sampler;
    return new C.Sampler({
      wrapS: sampler.wrapS,
      wrapT: sampler.wrapT,
      minificationFilter: sampler.minificationFilter,
      magnificationFilter: sampler.magnificationFilter,
      maximumAnisotropy: anisotropy,
    });
  };
}

function tilesetOptions(maximumScreenSpaceError) {
  return {
    maximumScreenSpaceError,
    cacheBytes: 800 * 1024 * 1024,
    maximumCacheOverflowBytes: 200 * 1024 * 1024,
    skipLevelOfDetail: true,
    baseScreenSpaceError: 48,
    skipScreenSpaceErrorFactor: 8,
    skipLevels: 1,
    foveatedScreenSpaceError: true,
    foveatedConeSize: .25,
    foveatedMinimumScreenSpaceErrorRelaxation: 4,
    foveatedTimeDelay: .1,
    dynamicScreenSpaceError: true,
    dynamicScreenSpaceErrorDensity: .04,
    dynamicScreenSpaceErrorFactor: 4,
    dynamicScreenSpaceErrorHeightFalloff: .25,
    cullWithChildrenBounds: true,
    cullRequestsWhileMoving: false,
    preloadFlightDestinations: true,
    preloadWhenHidden: false,
    enablePick: true,
    enableShowOutline: false,
    enableDebugWireframe: false,
    backFaceCulling: false,
  };
}

// 自动化与验收只读这份快照，不依赖模型自评或截图猜测。
window.__OSGB_PREVIEW__ = {
  get state() {
    const stats = tileset?.statistics;
    const camera = viewer?.camera;
    return {
      jobId,
      cesiumVersion: C.VERSION,
      ready,
      failed,
      error: errorMessage || null,
      warning: warningMessage || null,
      loadedMilliseconds,
      tilesLoaded: Boolean(tileset?.tilesLoaded),
      selected: stats?.selected || 0,
      triangles: stats?.numberOfTrianglesSelected || 0,
      pending: stats?.numberOfPendingRequests || 0,
      processing: stats?.numberOfTilesProcessing || 0,
      contentReady: stats?.numberOfTilesWithContentReady || 0,
      maximumScreenSpaceError: tileset?.maximumScreenSpaceError || null,
      activeView,
      anisotropy,
      msaaSamples: viewer?.scene.msaaSamples || 1,
      targetFrameRate: viewer?.targetFrameRate || null,
      camera: camera ? {
        position: [camera.position.x, camera.position.y, camera.position.z],
        heading: camera.heading,
        pitch: camera.pitch,
      } : null,
    };
  },
};

async function start() {
  if (!jobId) {
    setError('预览地址缺少任务 ID，请从转换任务列表打开。');
    return;
  }
  loadJobName();
  try {
    viewer = new C.Viewer('cesium', {
      animation: false,
      timeline: false,
      baseLayer: false,
      baseLayerPicker: false,
      geocoder: false,
      homeButton: false,
      infoBox: false,
      sceneModePicker: false,
      navigationHelpButton: false,
      fullscreenButton: false,
      selectionIndicator: false,
      requestRenderMode: true,
      maximumRenderTimeChange: Infinity,
      scene3DOnly: true,
      useBrowserRecommendedResolution: false,
      msaaSamples: 4,
      targetFrameRate: 30,
      contextOptions: {
        allowTextureFilterAnisotropic: true,
        webgl: { alpha: false, antialias: true, depth: true, powerPreference: 'high-performance' },
      },
    });
    const scene = viewer.scene;
    scene.globe.show = false;
    scene.globe.showGroundAtmosphere = false;
    scene.skyBox.show = false;
    scene.skyAtmosphere.show = false;
    scene.sun.show = false;
    scene.moon.show = false;
    scene.fog.enabled = false;
    scene.highDynamicRange = false;
    scene.logarithmicDepthBuffer = true;
    scene.postProcessStages.fxaa.enabled = true;
    scene.backgroundColor = C.Color.fromCssColorString('#dce3e5');
    scene.screenSpaceCameraController.minimumZoomDistance = .5;
    scene.screenSpaceCameraController.enableCollisionDetection = true;
    // 保留 Cesium 默认日光；固定 ECEF 方向光会让部分地理区域逆光的模型整体偏暗。
    viewer.shadows = false;
    viewer.camera.frustum.near = .05;
    viewer.camera.percentageChanged = .01;
    viewer.cesiumWidget.screenSpaceEventHandler.removeInputAction(C.ScreenSpaceEventType.LEFT_DOUBLE_CLICK);
    Object.assign(scene.canvas.style, {
      userSelect: 'none', webkitUserSelect: 'none', touchAction: 'none',
      webkitTouchCallout: 'none', outline: 'none', webkitTapHighlightColor: 'transparent',
    });
    patchTextureSampling();
    scene.renderError.addEventListener((_, error) => setError(error));
    scene.postRender.addEventListener(updateStatistics);

    const resource = new C.Resource({
      url: new URL(tilesetURL, location.origin).href,
      headers: authHeaders,
      retryAttempts: 3,
    });
    tileset = await C.Cesium3DTileset.fromUrl(resource, tilesetOptions(Number($('#quality').value)));
    scene.primitives.add(tileset);
    modelCamera = installModelCamera(viewer, tileset, {
      onActivity(activity) {
        $('#orbit-reticle').hidden = !activity;
        if (activity) $('#viewport').classList.add('has-interacted');
      },
    });
    tileset.initialTilesLoaded.addEventListener(() => modelCamera?.refreshCenter());
    tileset.tileFailed.addEventListener(() => showNotice('部分瓦片暂时无法加载，已跳过；可缩放后重试'));
    tileset.loadProgress.addEventListener(updateStatistics);
    await modelCamera.flyTo('orbit');
    scene.requestRender();
  } catch (error) {
    setError(error);
  }
}

$('#close').addEventListener('click', closePreview);
$('#fit').addEventListener('click', () => setView('orbit'));
$('#fullscreen').addEventListener('click', toggleFullscreen);
$('#retry').addEventListener('click', () => location.reload());
document.querySelectorAll('[data-view]').forEach(button => {
  button.addEventListener('click', () => setView(button.dataset.view));
});
$('#quality').addEventListener('input', event => {
  const value = Number(event.target.value);
  if (tileset) {
    tileset.maximumScreenSpaceError = value;
    tileset.skipLevels = value <= 8 ? 0 : 1;
    tileset.baseScreenSpaceError = value <= 8 ? 24 : value >= 24 ? 80 : 48;
    tileset.dynamicScreenSpaceErrorFactor = value <= 8 ? 2 : value >= 24 ? 6 : 4;
    viewer?.scene.requestRender();
  }
});
window.addEventListener('keydown', event => {
  if (event.target instanceof HTMLSelectElement || event.target instanceof HTMLInputElement) return;
  if (event.key.toLowerCase() === 'h') setView('orbit');
  if (event.key.toLowerCase() === 'f') toggleFullscreen();
  if (event.key === '2') setView('top');
  if (event.key === 'Escape' && !document.fullscreenElement && window.parent !== window) closePreview();
});
window.addEventListener('beforeunload', () => {
  clearTimeout(noticeTimer);
  modelCamera?.destroy();
  if (tileset && !tileset.isDestroyed()) tileset.trimLoadedTiles();
  if (viewer && !viewer.isDestroyed()) viewer.destroy();
});

start();
