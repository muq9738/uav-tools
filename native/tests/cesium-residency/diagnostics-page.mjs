// Cesium 1.136 瓦片遍历、请求/处理生命周期与驻留归因只读采集页脚本。
/* global Cesium */
const C = Cesium;

const NAMED_POSES = Object.freeze({
  near: { yaw: 0, pitch: 0.55, ratio: 0.55 },
  side: { yaw: 1.2, pitch: 0.7, ratio: 0.8 },
  overview: { yaw: 0, pitch: 1.3, ratio: 1.6 },
  'near-return': { yaw: 0, pitch: 0.55, ratio: 0.55 },
});

const CONTENT_STATE_NAME = Object.freeze({
  0: 'UNLOADED',
  1: 'LOADING',
  2: 'PROCESSING',
  3: 'READY',
  4: 'EXPIRED',
  5: 'FAILED',
});

const pause = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function normalizeAssetUri(rawUrl, variantPrefix) {
  if (!rawUrl) return null;
  try {
    const parsed = new URL(rawUrl, location.origin);
    let p = decodeURIComponent(parsed.pathname);
    if (variantPrefix && p.startsWith(variantPrefix)) {
      p = p.slice(variantPrefix.length);
    }
    return p.replace(/^\/+/, '');
  } catch {
    return String(rawUrl);
  }
}

function getTileRawUrl(tile) {
  return (
    tile?.content?.url ||
    tile?._contentResource?.getUrlComponent?.(true) ||
    tile?._contentResource?.url ||
    tile?._contentHeader?.uri ||
    tile?._contentHeader?.url ||
    null
  );
}

const config = await (await fetch('/config.json', { cache: 'no-store' })).json();
const job = await (await fetch('/job.json', { cache: 'no-store' })).json();

const viewEl = document.getElementById('view');
viewEl.style.width = `${job.width}px`;
viewEl.style.height = `${job.height}px`;

const viewer = new C.Viewer('view', {
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
  useDefaultRenderLoop: false,
  msaaSamples: config.msaaSamples || 1,
  contextOptions: {
    webgl: {
      antialias: Boolean(config.msaaSamples),
      alpha: false,
      preserveDrawingBuffer: true,
      powerPreference: 'high-performance',
    },
  },
});
viewer.resolutionScale = 1;

const scene = viewer.scene;
scene.globe.show = false;
scene.skyBox.show = false;
scene.skyAtmosphere.show = false;
scene.sun.show = false;
scene.moon.show = false;
scene.fog.enabled = false;
scene.highDynamicRange = false;
scene.backgroundColor = new C.Color(0.035, 0.07, 0.1, 1);
scene.postProcessStages.fxaa.enabled = job.fxaa !== false;
scene.screenSpaceCameraController.enableInputs = false;

let renderErrorText = '';
scene.renderError.addEventListener((_, err) => {
  renderErrorText = String(err?.stack || err);
});

const { frame, manifest } = config;
const { east, north, up } = frame.localAxesInWorld;
const L = C.Matrix4.fromArray([...east, 0, ...north, 0, ...up, 0, ...frame.originWorld, 1]);
const M = C.Matrix4.fromArray(manifest.transform.matrix);
const modelMatrix = C.Matrix4.multiply(
  C.Matrix4.multiply(L, M, new C.Matrix4()),
  C.Matrix4.inverse(L, new C.Matrix4()),
  new C.Matrix4(),
);
viewer.camera.frustum.near = 0.01;
viewer.camera.frustum.far = manifest.camera.radius * 100;

const gl = scene.context._gl;
const debugExt = gl.getExtension('WEBGL_debug_renderer_info');
const glUploadCounters = {
  texImage2DCalls: 0,
  compressedTexImage2DCalls: 0,
  compressedFormats: {},
};

// Record actual WebGL allocations/sampler state by object; sampler enums alone do not prove mip use.
const textureUploadState = new WeakMap();
let boundTexture2D = null;
const origBindTexture = gl.bindTexture.bind(gl);
gl.bindTexture = function (target, texture) {
  if (target === gl.TEXTURE_2D) boundTexture2D = texture;
  return origBindTexture(target, texture);
};
function textureState() {
  if (!boundTexture2D) return null;
  if (!textureUploadState.has(boundTexture2D)) textureUploadState.set(boundTexture2D, { levels: {}, parameters: {}, generatedMipmap: false });
  return textureUploadState.get(boundTexture2D);
}
const origTexParameteri = gl.texParameteri.bind(gl);
gl.texParameteri = function (target, pname, value) {
  if (target === gl.TEXTURE_2D) { const s = textureState(); if (s) s.parameters[pname] = value; }
  return origTexParameteri(target, pname, value);
};
const origGenerateMipmap = gl.generateMipmap.bind(gl);
gl.generateMipmap = function (target) {
  if (target === gl.TEXTURE_2D) { const s = textureState(); if (s) s.generatedMipmap = true; }
  return origGenerateMipmap(target);
};
const origTexImage2D = gl.texImage2D.bind(gl);
gl.texImage2D = function (...args) {
  glUploadCounters.texImage2DCalls++;
  if (args[0] === gl.TEXTURE_2D) {
    const s = textureState(), image = args.at(-1);
    if (s) s.levels[args[1]] = { format: '0x' + Number(args[2]).toString(16), compressed: false,
      width: args.length >= 9 ? args[3] : image?.width, height: args.length >= 9 ? args[4] : image?.height };
  }
  return origTexImage2D(...args);
};
const origCompressedTexImage2D = gl.compressedTexImage2D.bind(gl);
gl.compressedTexImage2D = function (...args) {
  glUploadCounters.compressedTexImage2DCalls++;
  const hex = '0x' + Number(args[2]).toString(16);
  glUploadCounters.compressedFormats[hex] = (glUploadCounters.compressedFormats[hex] || 0) + 1;
  if (args[0] === gl.TEXTURE_2D) { const s = textureState(); if (s) s.levels[args[1]] = { format: hex, compressed: true, width: args[3], height: args[4] }; }
  return origCompressedTexImage2D(...args);
};

// 只读拦截 Cesium3DTile.prototype.requestContent，精确捕获实际发起网络请求的瓦片，不修改返回值与控制流。
let activeTracker = null;
const origRequestContent = C.Cesium3DTile.prototype.requestContent;
C.Cesium3DTile.prototype.requestContent = function (...args) {
  const stateBefore = this._contentState;
  const res = origRequestContent.apply(this, args);
  // A requestContent call on an already LOADING tile is not another network request.
  if (activeTracker && stateBefore !== this._contentState) {
    activeTracker.recordRequestedTile(this);
  }
  return res;
};

function setCameraPose(poseSpec) {
  const pose = typeof poseSpec === 'string' ? NAMED_POSES[poseSpec] : poseSpec;
  if (!pose) {
    throw new Error(`未知相机姿态: ${JSON.stringify(poseSpec)}`);
  }
  const { yaw, pitch, ratio } = pose;
  const distance = ratio * manifest.camera.radius;
  const target = C.Cartesian3.fromArray(manifest.camera.target);
  const local = new C.Cartesian3(
    target.x + Math.cos(yaw) * Math.cos(pitch) * distance,
    target.y + Math.sin(yaw) * Math.sin(pitch) * distance,
    target.z + Math.sin(pitch) * distance,
  );
  const position = C.Matrix4.multiplyByPoint(L, local, new C.Cartesian3());
  const worldTarget = C.Matrix4.multiplyByPoint(L, target, new C.Cartesian3());
  viewer.camera.setView({
    destination: position,
    orientation: {
      direction: C.Cartesian3.normalize(
        C.Cartesian3.subtract(worldTarget, position, new C.Cartesian3()),
        new C.Cartesian3(),
      ),
      up: C.Cartesian3.fromArray(up),
    },
  });
  return pose;
}

function renderSingleFrame() {
  viewer.resize();
  viewer.camera.frustum.fov =
    2 * Math.atan(Math.tan(C.Math.toRadians(55) / 2) * scene.canvas.width / scene.canvas.height);
  scene.requestRender();
  viewer.render();
}

function selectedTexturesAreReady(tileset) {
  return tileset._selectedTiles.length > 0 && tileset._selectedTiles.every(tile => {
    const model = tile.content?._model;
    return model && model.ready && model._loader &&
      (!model._loader.incrementallyLoadTextures || (model._loader.texturesLoaded && model._texturesLoaded));
  });
}

function createLifecycleTracker(variantPrefix) {
  const tileSeq = new WeakMap();
  let nextId = 1;
  const getTileKey = (tile) => {
    if (!tileSeq.has(tile)) {
      tileSeq.set(tile, nextId++);
    }
    const uri = normalizeAssetUri(getTileRawUrl(tile), variantPrefix);
    return uri ? `${uri}#${tileSeq.get(tile)}` : `empty#${tileSeq.get(tile)}`;
  };

  const requestedRenderableUris = new Set();
  const requestedTilesetJsonUris = new Set();
  const requestedAllKeys = new Set();
  const processingObservedKeys = new Set();
  const processingObservedUris = new Set();
  const tileLoadCompletedUris = new Set();
  const tileUnloadedUris = new Set();
  const tileFailedErrors = [];

  let peakRequestedInSingleFrame = 0;
  let peakPendingRequests = 0;
  let peakProcessingQueue = 0;
  let maxMemoryAdjustedSSE = -Infinity;
  let minMemoryAdjustedSSE = Infinity;
  let memoryAdjustedSSEElevatedFrames = 0;
  const frameTimeline = [];

  const classifyRequestUri = (tile) => {
    const uri = normalizeAssetUri(getTileRawUrl(tile), variantPrefix);
    const key = getTileKey(tile);
    requestedAllKeys.add(key);
    if (uri) {
      if (uri.toLowerCase().endsWith('.json') || tile.hasTilesetContent) {
        requestedTilesetJsonUris.add(uri);
      } else {
        requestedRenderableUris.add(uri);
      }
    }
  };

  return {
    getTileKey,
    recordRequestedTile(tile) {
      classifyRequestUri(tile);
    },
    recordTileLoad(tile) {
      const key = getTileKey(tile);
      processingObservedKeys.add(key);
      const uri = normalizeAssetUri(getTileRawUrl(tile), variantPrefix);
      if (uri) {
        if (tile.hasTilesetContent || uri.toLowerCase().endsWith('.json')) {
          requestedTilesetJsonUris.add(uri);
        } else {
          requestedRenderableUris.add(uri);
          processingObservedUris.add(uri);
          tileLoadCompletedUris.add(uri);
        }
      }
    },
    recordTileUnload(tile) {
      const uri = normalizeAssetUri(getTileRawUrl(tile), variantPrefix);
      if (uri) tileUnloadedUris.add(uri);
    },
    recordTileFailed(event) {
      tileFailedErrors.push(String(event?.message || event?.url || event));
    },
    observeFrame(tileset, frameIndex, startedMs) {
      const stats = tileset.statistics;
      const reqFrameLen = tileset._requestedTiles?.length || 0;
      const procQueueLen = Math.max(
        stats.numberOfTilesProcessing || 0,
        tileset._processingQueue?.length || 0,
      );
      peakRequestedInSingleFrame = Math.max(peakRequestedInSingleFrame, reqFrameLen);
      peakPendingRequests = Math.max(peakPendingRequests, stats.numberOfPendingRequests || 0);
      peakProcessingQueue = Math.max(peakProcessingQueue, procQueueLen);

      for (const reqTile of tileset._requestedTiles || []) {
        classifyRequestUri(reqTile);
      }
      for (const procTile of tileset._processingQueue || []) {
        processingObservedKeys.add(getTileKey(procTile));
        const uri = normalizeAssetUri(getTileRawUrl(procTile), variantPrefix);
        if (uri && !uri.toLowerCase().endsWith('.json')) {
          processingObservedUris.add(uri);
        }
      }

      // 遍历当前树捕捉本帧处于 LOADING / PROCESSING 的节点
      if (tileset.root) {
        const stack = [tileset.root];
        while (stack.length) {
          const t = stack.pop();
          if (t._contentState === 1) {
            classifyRequestUri(t);
          } else if (t._contentState === 2) {
            processingObservedKeys.add(getTileKey(t));
            const uri = normalizeAssetUri(getTileRawUrl(t), variantPrefix);
            if (uri && !uri.toLowerCase().endsWith('.json')) {
              processingObservedUris.add(uri);
            }
          }
          for (let i = 0; i < t.children.length; i++) {
            stack.push(t.children[i]);
          }
        }
      }

      const memSSE = tileset.memoryAdjustedScreenSpaceError;
      const maxSSE = tileset.maximumScreenSpaceError;
      maxMemoryAdjustedSSE = Math.max(maxMemoryAdjustedSSE, memSSE);
      minMemoryAdjustedSSE = Math.min(minMemoryAdjustedSSE, memSSE);
      if (memSSE > maxSSE) {
        memoryAdjustedSSEElevatedFrames++;
      }

      frameTimeline.push({
        frame: frameIndex,
        elapsedMs: Number((performance.now() - startedMs).toFixed(3)),
        selected: stats.selected,
        visited: stats.visited,
        requestedThisFrame: reqFrameLen,
        pendingRequests: stats.numberOfPendingRequests,
        processing: stats.numberOfTilesProcessing,
        contentReady: stats.numberOfTilesWithContentReady,
        trianglesSelected: stats.numberOfTrianglesSelected,
        commands: stats.numberOfCommands,
        texturesByteLength: stats.texturesByteLength,
        geometryByteLength: stats.geometryByteLength,
        totalMemoryUsageInBytes: tileset.totalMemoryUsageInBytes,
        maximumScreenSpaceError: maxSSE,
        memoryAdjustedScreenSpaceError: memSSE,
      });
    },
    snapshotSummary(tileset) {
      return {
        requested: {
          everRequestedRenderableCount: requestedRenderableUris.size,
          everRequestedTilesetJsonCount: requestedTilesetJsonUris.size,
          everRequestedNodeInstances: requestedAllKeys.size,
          peakRequestedInSingleFrame,
          peakPendingRequests,
          settledRequestedThisFrame: tileset._requestedTiles?.length || 0,
          settledPendingRequests: tileset.statistics.numberOfPendingRequests,
          renderableUris: [...requestedRenderableUris].sort(),
          tilesetJsonUris: [...requestedTilesetJsonUris].sort(),
        },
        processing: {
          everProcessingRenderableCount: processingObservedUris.size,
          everProcessingNodeInstances: processingObservedKeys.size,
          tileLoadCompletedCount: tileLoadCompletedUris.size,
          tileUnloadedCount: tileUnloadedUris.size,
          peakProcessingQueue,
          settledProcessingCount: tileset.statistics.numberOfTilesProcessing,
          settledProcessingQueueLength: tileset._processingQueue?.length || 0,
        },
        sse: {
          configuredMaximumSSE: tileset.maximumScreenSpaceError,
          settledMemoryAdjustedSSE: tileset.memoryAdjustedScreenSpaceError,
          minMemoryAdjustedSSE: Number.isFinite(minMemoryAdjustedSSE) ? minMemoryAdjustedSSE : null,
          maxMemoryAdjustedSSE: Number.isFinite(maxMemoryAdjustedSSE) ? maxMemoryAdjustedSSE : null,
          elevatedFrameCount: memoryAdjustedSSEElevatedFrames,
          qualityDegradedByCacheOverflow:
            tileset.memoryAdjustedScreenSpaceError > tileset.maximumScreenSpaceError ||
            memoryAdjustedSSEElevatedFrames > 0,
          cacheBytes: tileset.cacheBytes,
          maximumCacheOverflowBytes: tileset.maximumCacheOverflowBytes,
          cacheByteLimit: tileset.cacheBytes + tileset.maximumCacheOverflowBytes,
          totalMemoryUsageInBytes: tileset.totalMemoryUsageInBytes,
        },
        tileFailedErrors,
        frameCount: frameTimeline.length,
        frameTimeline,
      };
    },
  };
}

function auditSettledTileset(tileset, tracker, variantPrefix, visibleEventTiles) {
  const stats = tileset.statistics;

  // 1. 解析 Cesium3DTilesetCache 哨兵双向链表：[head, sentinel) 为可淘汰缓存，(sentinel, tail] 为本帧 touch 保活
  const evictableSet = new Set();
  const pinnedSet = new Set();
  const cacheList = tileset._cache?._list;
  const sentinel = tileset._cache?._sentinel;
  if (!cacheList || !sentinel) {
    throw new Error('无法读取 Cesium3DTileset._cache 哨兵链表');
  }
  let cursor = cacheList.head;
  let passedSentinel = false;
  let guard = 0;
  while (cursor && guard++ < 100000) {
    if (cursor === sentinel) {
      passedSentinel = true;
    } else if (cursor.item) {
      if (!passedSentinel) evictableSet.add(cursor.item);
      else pinnedSet.add(cursor.item);
    }
    cursor = cursor.next;
  }
  if (!passedSentinel) {
    throw new Error('遍历 Cesium3DTilesetCache 未遇到 sentinel 节点');
  }

  // 2. 遍历整棵瓦片树
  const allTiles = [];
  const stack = tileset.root ? [{ tile: tileset.root, treeDepth: 0 }] : [];
  while (stack.length) {
    const { tile, treeDepth } = stack.pop();
    allTiles.push({ tile, treeDepth });
    for (let i = tile.children.length - 1; i >= 0; i--) {
      stack.push({ tile: tile.children[i], treeDepth: treeDepth + 1 });
    }
  }

  const selectedSet = new Set(tileset._selectedTiles || []);
  const strictAncestorSet = new Set();
  for (const s of selectedSet) {
    let p = s.parent;
    while (p) {
      if (!selectedSet.has(p)) {
        strictAncestorSet.add(p);
      }
      p = p.parent;
    }
  }

  const hasSelectedAncestor = (tile) => {
    let p = tile.parent;
    while (p) {
      if (selectedSet.has(p)) return true;
      p = p.parent;
    }
    return false;
  };

  // 查找从 tile 向上仅跨过非可渲染包装节点（外部 tileset.json 或空节点）所能到达的最近祖先
  const findNearestAncestorThroughEmptyWrappers = (tile) => {
    let p = tile.parent;
    let hoppedEmpty = false;
    while (p && !p.hasRenderableContent) {
      if (strictAncestorSet.has(p)) {
        return { ancestor: p, viaEmptyWrapper: true };
      }
      hoppedEmpty = true;
      p = p.parent;
    }
    if (p && strictAncestorSet.has(p)) {
      return { ancestor: p, viaEmptyWrapper: hoppedEmpty };
    }
    return null;
  };

  const treeNodeCounts = {
    totalNodes: allTiles.length,
    renderableNodes: 0,
    externalTilesetWrapperNodes: 0,
    emptyStructuralNodes: 0,
    byContentState: {},
  };

  const roleBuckets = {
    selected: [],
    ancestor: [],
    sibling: [],
    descendant_of_selected: [],
    cached_unreferenced: [],
  };
  const siblingSubkinds = {
    sibling_replace_invisible_guard: 0,
    sibling_via_empty_wrapper: 0,
    sibling_load_siblings_flag: 0,
    sibling_visible_other: 0,
  };

  const globalTextureBytesById = new Map();
  const selectedTextureIds = new Set();
  let nonModelTextureBytes = 0;

  // 第一遍：收集所有驻留可渲染瓦片及其精确纹理 ID 表
  const residentEntries = [];
  for (const { tile, treeDepth } of allTiles) {
    const stateName = CONTENT_STATE_NAME[tile._contentState] || `STATE_${tile._contentState}`;
    treeNodeCounts.byContentState[stateName] = (treeNodeCounts.byContentState[stateName] || 0) + 1;

    if (tile.hasTilesetContent) treeNodeCounts.externalTilesetWrapperNodes++;
    else if (tile.hasEmptyContent || !tile.hasRenderableContent) treeNodeCounts.emptyStructuralNodes++;
    else treeNodeCounts.renderableNodes++;

    if (!(tile.contentReady && tile.hasRenderableContent)) {
      continue;
    }

    const content = tile.content;
    const uri = normalizeAssetUri(getTileRawUrl(tile), variantPrefix);
    const geomBytes = content?.geometryByteLength || 0;
    const batchBytes = content?.batchTableByteLength || 0;
    const triangles = content?.trianglesLength || 0;
    const hasModelTexApi = typeof content?.getTextureIds === 'function';
    const texEntries = [];
    if (hasModelTexApi) {
      for (const texId of content.getTextureIds()) {
        const bytes = content.getTextureByteLengthById(texId) || 0;
        texEntries.push({ id: texId, bytes });
        if (!globalTextureBytesById.has(texId)) {
          globalTextureBytesById.set(texId, bytes);
        }
      }
    } else {
      nonModelTextureBytes += content?.texturesByteLength || 0;
    }

    let role;
    let siblingReason = null;
    if (selectedSet.has(tile)) {
      role = 'selected';
      for (const t of texEntries) selectedTextureIds.add(t.id);
    } else if (strictAncestorSet.has(tile)) {
      role = 'ancestor';
    } else if (hasSelectedAncestor(tile)) {
      role = 'descendant_of_selected';
    } else {
      const ancLink = findNearestAncestorThroughEmptyWrappers(tile);
      if (ancLink) {
        role = 'sibling';
        if (ancLink.viaEmptyWrapper) {
          siblingReason = 'sibling_via_empty_wrapper';
        } else if (!tile.isVisible && tileset.loadSiblings) {
          siblingReason = 'sibling_load_siblings_flag';
        } else if (!tile.isVisible) {
          siblingReason = 'sibling_replace_invisible_guard';
        } else {
          siblingReason = 'sibling_visible_other';
        }
        siblingSubkinds[siblingReason]++;
      } else {
        role = 'cached_unreferenced';
      }
    }

    const inPinned = pinnedSet.has(tile);
    const inEvictable = evictableSet.has(tile);
    if (inPinned === inEvictable) {
      throw new Error(
        `驻留瓦片缓存链表状态异常 (${uri}): pinned=${inPinned}, evictable=${inEvictable}`,
      );
    }

    const entry = {
      key: tracker.getTileKey(tile),
      uri,
      role,
      siblingReason,
      cacheState: inPinned ? 'pinnedThisFrame' : 'evictableLru',
      treeDepth,
      cesiumDepth: tile._depth,
      geometricError: tile.geometricError,
      screenSpaceError: tile._screenSpaceError,
      distanceToCamera: tile._distanceToCamera,
      foveatedFactor: tile._foveatedFactor,
      parentGeometricError: tile.parent?.geometricError,
      parentContentUri: tile.parent ? normalizeAssetUri(getTileRawUrl(tile.parent), variantPrefix) : null,
      isVisible: Boolean(tile.isVisible),
      inRequestVolume: Boolean(tile._inRequestVolume),
      refines: Boolean(tile._refines),
      triangles,
      geometryBytes: geomBytes,
      batchTableBytes: batchBytes,
      textures: texEntries,
      ...(role === 'selected' ? { modelTextureLoading: { ready: content?._model?.ready, incremental: content?._model?._loader?.incrementallyLoadTextures,
        loaderTexturesLoaded: content?._model?._loader?.texturesLoaded, modelTexturesLoaded: content?._model?._texturesLoaded }, selectedModelPrimitives: (content?._model?._sceneGraph?.components?.nodes || []).flatMap(n => (n.primitives || []).map(p => {
        const tr = p.material?.metallicRoughness?.baseColorTexture || p.material?.specularGlossiness?.diffuseTexture;
        const t = tr?.texture;
        return { attributes: (p.attributes || []).map(a => ({ semantic: a.semantic, count: a.count, componentDatatype: a.componentDatatype, type: a.type,
            gpuComponentDatatype: a.quantization?.componentDatatype ?? a.componentDatatype, gpuType: a.quantization?.type ?? a.type,
            bufferBytes: a.buffer?.sizeInBytes, byteStride: a.byteStride, byteOffset: a.byteOffset,
            quantization: a.quantization ? { componentDatatype: a.quantization.componentDatatype, type: a.quantization.type,
              quantizationBits: a.quantization.quantizationBits, normalizationRange: a.quantization.normalizationRange,
              octEncoded: a.quantization.octEncoded } : null })),
          indices: p.indices ? { count: p.indices.count, indexDatatype: p.indices.indexDatatype } : null,
          baseColorTexture: t ? { id: t.id, width: t.width, height: t.height, pixelFormat: t.pixelFormat, pixelDatatype: t.pixelDatatype,
            sampler: { minificationFilter: t.sampler?.minificationFilter, magnificationFilter: t.sampler?.magnificationFilter, wrapS: t.sampler?.wrapS, wrapT: t.sampler?.wrapT },
            actualWebGL: textureUploadState.get(t._texture) || null } : null };
      })) } : {}),
      nonModelTextureBytes: hasModelTexApi ? 0 : (content?.texturesByteLength || 0),
    };
    residentEntries.push(entry);
    roleBuckets[role].push(entry);
  }

  // 汇总桶指标及相对于 selected 的边际独占纹理字节
  const summarizeEntries = (entries) => {
    const bucketTexMap = new Map();
    const exclusiveOutsideSelectedMap = new Map();
    const sharedWithSelectedMap = new Map();
    let triangles = 0;
    let geometryBytes = 0;
    let batchTableBytes = 0;
    let pinnedCount = 0;
    let evictableCount = 0;
    let nonModelTex = 0;

    for (const e of entries) {
      triangles += e.triangles;
      geometryBytes += e.geometryBytes;
      batchTableBytes += e.batchTableBytes;
      nonModelTex += e.nonModelTextureBytes;
      if (e.cacheState === 'pinnedThisFrame') pinnedCount++;
      else evictableCount++;

      for (const t of e.textures) {
        bucketTexMap.set(t.id, t.bytes);
        if (selectedTextureIds.has(t.id)) {
          sharedWithSelectedMap.set(t.id, t.bytes);
        } else {
          exclusiveOutsideSelectedMap.set(t.id, t.bytes);
        }
      }
    }

    const sumValues = (m) => {
      let s = 0;
      for (const v of m.values()) s += v;
      return s;
    };

    return {
      tileCount: entries.length,
      pinnedThisFrameCount: pinnedCount,
      evictableLruCount: evictableCount,
      triangles,
      geometryBytes,
      batchTableBytes,
      uniqueTextureCount: bucketTexMap.size,
      deduplicatedTextureBytes: sumValues(bucketTexMap) + nonModelTex,
      sharedWithSelectedTextureCount: sharedWithSelectedMap.size,
      sharedWithSelectedTextureBytes: sumValues(sharedWithSelectedMap),
      exclusiveTextureCountOutsideSelected: exclusiveOutsideSelectedMap.size,
      exclusiveTextureBytesOutsideSelected:
        sumValues(exclusiveOutsideSelectedMap) + (entries === roleBuckets.selected ? 0 : nonModelTex),
      uris: entries.map((e) => e.uri).filter(Boolean).sort(),
    };
  };

  const roleSummary = {};
  for (const [role, list] of Object.entries(roleBuckets)) {
    roleSummary[role] = summarizeEntries(list);
  }

  const cacheSummary = {
    pinnedThisFrame: summarizeEntries(residentEntries.filter((e) => e.cacheState === 'pinnedThisFrame')),
    evictableLru: summarizeEntries(residentEntries.filter((e) => e.cacheState === 'evictableLru')),
  };

  let modelUniqueTextureBytes = 0;
  for (const b of globalTextureBytesById.values()) {
    modelUniqueTextureBytes += b;
  }
  const computedTotalTextureBytes = modelUniqueTextureBytes + nonModelTextureBytes;
  const computedTotalGeometryBytes = residentEntries.reduce((acc, e) => acc + e.geometryBytes, 0);
  const computedSelectedTriangles = roleBuckets.selected.reduce((acc, e) => acc + e.triangles, 0);
  const rolePartitionSum = Object.values(roleBuckets).reduce((acc, list) => acc + list.length, 0);

  // 严格不变量核验
  const invariants = {
    residentMatchesCesiumContentReady:
      residentEntries.length === stats.numberOfTilesWithContentReady,
    selectedMatchesCesiumSelected:
      roleBuckets.selected.length === stats.selected &&
      visibleEventTiles.size === stats.selected,
    selectedTrianglesMatchCesium:
      computedSelectedTriangles === stats.numberOfTrianglesSelected,
    rolePartitionDisjointAndComplete:
      rolePartitionSum === residentEntries.length,
    cacheSentinelSplitComplete:
      cacheSummary.pinnedThisFrame.tileCount + cacheSummary.evictableLru.tileCount ===
        residentEntries.length &&
      pinnedSet.size + evictableSet.size === residentEntries.length,
    geometryBytesMatchCesium:
      computedTotalGeometryBytes === stats.geometryByteLength,
    textureBytesMatchCesium:
      computedTotalTextureBytes === stats.texturesByteLength,
    settledZeroPendingAndProcessing:
      stats.numberOfPendingRequests === 0 && stats.numberOfTilesProcessing === 0,
  };

  for (const [name, ok] of Object.entries(invariants)) {
    if (!ok) {
      throw new Error(
        `Cesium 驻留归因不变量断言失败 (${name}): computed={resident:${residentEntries.length},selected:${roleBuckets.selected.length},tri:${computedSelectedTriangles},geom:${computedTotalGeometryBytes},tex:${computedTotalTextureBytes}} vs stats=${JSON.stringify({
          selected: stats.selected,
          contentReady: stats.numberOfTilesWithContentReady,
          triangles: stats.numberOfTrianglesSelected,
          geom: stats.geometryByteLength,
          tex: stats.texturesByteLength,
          pending: stats.numberOfPendingRequests,
          processing: stats.numberOfTilesProcessing,
        })}`,
      );
    }
  }

  // 非选中纹理去重汇总（联合 ancestor + sibling + descendant + cached_unreferenced 后排除与 selected 共享者）
  const nonSelectedEntries = residentEntries.filter((e) => e.role !== 'selected');
  const nonSelectedSummary = summarizeEntries(nonSelectedEntries);

  return {
    cesiumStatistics: {
      selected: stats.selected,
      visited: stats.visited,
      numberOfCommands: stats.numberOfCommands,
      numberOfTrianglesSelected: stats.numberOfTrianglesSelected,
      numberOfTilesWithContentReady: stats.numberOfTilesWithContentReady,
      numberOfTilesTotal: stats.numberOfTilesTotal,
      numberOfLoadedTilesTotal: stats.numberOfLoadedTilesTotal,
      numberOfTilesCulledWithChildrenUnion: stats.numberOfTilesCulledWithChildrenUnion,
      texturesByteLength: stats.texturesByteLength,
      geometryByteLength: stats.geometryByteLength,
      batchTableByteLength: stats.batchTableByteLength,
      totalMemoryUsageInBytes: tileset.totalMemoryUsageInBytes,
      uniqueResidentTextureCount: globalTextureBytesById.size,
    },
    treeNodeCounts,
    roles: roleSummary,
    residentEntries,
    nonSelectedCombined: nonSelectedSummary,
    siblingBreakdown: siblingSubkinds,
    cacheLru: cacheSummary,
    invariants,
  };
}

async function driveUntilSettled(tileset, tracker, maxWaitMs = 60_000) {
  const started = performance.now();
  let stable = 0;
  let frameIndex = 0;
  let visibleEventTiles = new Set();
  const removeVisibleListener = tileset.tileVisible.addEventListener((tile) => {
    if (tile.contentReady && tile.hasRenderableContent) {
      visibleEventTiles.add(tile);
    }
  });

  try {
    while (performance.now() - started < maxWaitMs) {
      if (renderErrorText) {
        throw new Error(`Cesium 渲染错误: ${renderErrorText}`);
      }
      visibleEventTiles = new Set();
      renderSingleFrame();
      tracker.observeFrame(tileset, frameIndex++, started);

      const stats = tileset.statistics;
      if (
        tileset.tilesLoaded &&
        selectedTexturesAreReady(tileset) &&
        stats.numberOfTrianglesSelected > 0 &&
        stats.numberOfPendingRequests === 0 &&
        stats.numberOfTilesProcessing === 0
      ) {
        stable++;
        if (stable >= 5) {
          return {
            loadWallMs: Number((performance.now() - started).toFixed(3)),
            framesRendered: frameIndex,
            visibleEventTiles,
          };
        }
      } else {
        stable = 0;
      }
      await pause(16);
    }
    throw new Error(`等待瓦片稳态超时 (${maxWaitMs}ms)`);
  } finally {
    removeVisibleListener();
  }
}

async function measureSteadyFrames(count) {
  const ext = gl.getExtension('EXT_disjoint_timer_query_webgl2');
  const cpu = [], gpu = [], queries = [];
  const frame = () => new Promise(resolve => requestAnimationFrame(resolve));
  const stats = values => {
    const sorted = [...values].sort((a, b) => a - b);
    return sorted.length ? { count: sorted.length, median: sorted[Math.floor(sorted.length / 2)], p95: sorted[Math.floor((sorted.length - 1) * 0.95)] } : null;
  };
  let disjoint = false;
  try {
    for (let i = 0; i < 10; ++i) { await frame(); renderSingleFrame(); }
    for (let i = 0; i < count; ++i) {
      await frame();
      const query = ext ? gl.createQuery() : null;
      if (query) gl.beginQuery(ext.TIME_ELAPSED_EXT, query);
      const start = performance.now(); renderSingleFrame(); cpu.push(performance.now() - start);
      if (query) { gl.endQuery(ext.TIME_ELAPSED_EXT); queries.push(query); }
      if (ext && gl.getParameter(ext.GPU_DISJOINT_EXT)) disjoint = true;
    }
    const deadline = performance.now() + 5000;
    while (queries.some(q => !gl.getQueryParameter(q, gl.QUERY_RESULT_AVAILABLE)) && performance.now() < deadline) await frame();
    if (ext && gl.getParameter(ext.GPU_DISJOINT_EXT)) disjoint = true;
    if (!disjoint) for (const query of queries) if (gl.getQueryParameter(query, gl.QUERY_RESULT_AVAILABLE)) gpu.push(gl.getQueryParameter(query, gl.QUERY_RESULT) / 1e6);
    return { cpuMs: stats(cpu), gpuMs: stats(gpu), gpuTimerSupported: !!ext, gpuDisjoint: disjoint,
      gpuComplete: !!ext && !disjoint && gpu.length === count, commands: scene._frameState.commandList.length };
  } finally { for (const query of queries) gl.deleteQuery(query); }
}

function percentileSummary(values) {
  const sorted = [...values].sort((a, b) => a - b);
  const p = q => sorted.length ? sorted[Math.min(sorted.length - 1, Math.ceil(sorted.length * q) - 1)] : null;
  return { count: sorted.length, p50: p(.5), p95: p(.95), p99: p(.99), max: sorted.at(-1) ?? null,
    over50: values.filter(v => v > 50).length, over100: values.filter(v => v > 100).length };
}

// Performance windows deliberately avoid lifecycle.observeFrame and auditSettledTileset:
// those walk the entire tree and belong outside frame-time measurement.
async function measureMotion(tileset, variantPrefix, loadStart, seconds) {
  const now = () => performance.now();
  const events = [], frames = [], longtasks = [], gpuQueries = [], gpuMs = [], selectedSamples = [];
  const gpuExt = gl.getExtension('EXT_disjoint_timer_query_webgl2');
  let gpuDisjoint = false, phase = 'initial-load';
  const uri = t => normalizeAssetUri(getTileRawUrl(t), variantPrefix);
  activeTracker = { recordRequestedTile(t) { events.push({ type: 'request', phase, ms: now() - loadStart, uri: uri(t) }); } };
  const removeLoad = tileset.tileLoad.addEventListener(t => events.push({ type: 'load', phase, ms: now() - loadStart, uri: uri(t) }));
  const removeUnload = tileset.tileUnload.addEventListener(t => events.push({ type: 'unload', phase, ms: now() - loadStart, uri: uri(t) }));
  const originalCancel = C.Request.prototype.cancel;
  C.Request.prototype.cancel = function (...args) {
    events.push({ type: 'cancel', phase, ms: now() - loadStart, url: this.url || null });
    return originalCancel.apply(this, args);
  };
  const observer = typeof PerformanceObserver === 'function' ? new PerformanceObserver(list => {
    for (const e of list.getEntries()) longtasks.push({ startMs: e.startTime - loadStart, durationMs: e.duration, name: e.name });
  }) : null;
  if (observer && PerformanceObserver.supportedEntryTypes?.includes('longtask')) observer.observe({ type: 'longtask', buffered: false });
  function drainGpu() {
    if (!gpuExt) return;
    if (gl.getParameter(gpuExt.GPU_DISJOINT_EXT)) gpuDisjoint = true;
    for (let i = gpuQueries.length - 1; i >= 0; i--) if (gl.getQueryParameter(gpuQueries[i], gl.QUERY_RESULT_AVAILABLE)) {
      gpuMs.push(gl.getQueryParameter(gpuQueries[i], gl.QUERY_RESULT) / 1e6);
      gl.deleteQuery(gpuQueries[i]); gpuQueries.splice(i, 1);
    }
  }
  let firstRenderableMs = null, firstTexturedMs = null, readyMs = null, motionStart = null, motionEnd = null;
  const initialFrames = [];
  try {
    await new Promise((resolve, reject) => {
      let usable = 0, textured = 0, settled = 0, prev = null;
      function frame(t) {
        try {
          const start = now(); renderSingleFrame(); const cpu = now() - start;
          const s = tileset.statistics;
          initialFrames.push({ ms: now() - loadStart, interval: prev === null ? null : t - prev, cpuMs: cpu, selected: tileset._selectedTiles.length,
            triangles: s.numberOfTrianglesSelected, pending: s.numberOfPendingRequests, processing: s.numberOfTilesProcessing }); prev = t;
          usable = tileset._selectedTiles.length > 0 ? usable + 1 : 0;
          if (usable >= 3 && firstRenderableMs === null) firstRenderableMs = now() - loadStart;
          textured = selectedTexturesAreReady(tileset) ? textured + 1 : 0;
          if (textured >= 3 && firstTexturedMs === null) firstTexturedMs = now() - loadStart;
          settled = tileset.tilesLoaded && !s.numberOfPendingRequests && !s.numberOfTilesProcessing && textured ? settled + 1 : 0;
          if (settled >= 15) { readyMs = now() - loadStart; selectedSamples.push({ phase, ms: now() - loadStart, selectedUris: tileset._selectedTiles.map(uri), texturesReady: selectedTexturesAreReady(tileset) }); resolve(); }
          else if (now() - loadStart > 120000) reject(new Error('motion initial-load did not reach SSE-ready state in 120s'));
          else requestAnimationFrame(frame);
        } catch (e) { reject(e); }
      }
      requestAnimationFrame(frame);
    });
    phase = 'motion'; motionStart = now();
    const poses = [NAMED_POSES.near, NAMED_POSES.side, NAMED_POSES.overview, NAMED_POSES['near-return']];
    await new Promise((resolve, reject) => {
      let prev = null, index = 0, nextSampleMs = 0;
      function frame(t) {
        try {
          const elapsed = now() - motionStart, progress = Math.min(3, elapsed / (seconds * 1000) * 3);
          const segment = Math.min(2, Math.floor(progress)), f = progress - segment;
          setCameraPose(Object.fromEntries(['yaw', 'pitch', 'ratio'].map(k => [k, poses[segment][k] * (1 - f) + poses[segment + 1][k] * f])));
          const query = gpuExt && index % 10 === 0 ? gl.createQuery() : null;
          if (query) gl.beginQuery(gpuExt.TIME_ELAPSED_EXT, query);
          const start = now(); renderSingleFrame(); const cpuMs = now() - start;
          if (query) { gl.endQuery(gpuExt.TIME_ELAPSED_EXT); gpuQueries.push(query); }
          drainGpu();
          const s = tileset.statistics;
          frames.push({ ms: elapsed, interval: prev === null ? null : t - prev, cpuMs, pathProgress: progress / 3,
            visibilityState: document.visibilityState,
            selected: tileset._selectedTiles.length, triangles: s.numberOfTrianglesSelected, commands: s.numberOfCommands,
            pending: s.numberOfPendingRequests, processing: s.numberOfTilesProcessing, contentReady: s.numberOfTilesWithContentReady,
            texturesBytes: s.texturesByteLength, geometryBytes: s.geometryByteLength, memoryAdjustedSSE: tileset.memoryAdjustedScreenSpaceError });
          if (elapsed >= nextSampleMs) {
            selectedSamples.push({ phase, ms: now() - loadStart, pathProgress: progress / 3,
              selectedUris: tileset._selectedTiles.map(uri), texturesReady: selectedTexturesAreReady(tileset) });
            nextSampleMs += 1000;
          }
          prev = t; index++;
          if (elapsed >= seconds * 1000) { motionEnd = now(); resolve(); } else requestAnimationFrame(frame);
        } catch (e) { reject(e); }
      }
      requestAnimationFrame(frame);
    });
    phase = 'return-settle';
    // Endpoint stable audit and asynchronous query collection happen after the timing window.
    const endpoint = await driveUntilSettled(tileset, { observeFrame() {} });
    drainGpu();
    if (observer) for (const e of observer.takeRecords()) longtasks.push({ startMs: e.startTime - loadStart, durationMs: e.duration, name: e.name });
    const startRelative = motionStart - loadStart, endRelative = motionEnd - loadStart;
    const motionTasks = longtasks.filter(e => e.startMs >= startRelative && e.startMs < endRelative);
    const requests = events.filter(e => e.type === 'request' && e.uri && !e.uri.endsWith('.json'));
    const counts = {}; for (const e of requests) counts[e.uri] = (counts[e.uri] || 0) + 1;
    const reloads = requests.filter(e => e.phase === 'motion' && e.ms >= startRelative + seconds * 1000 * 2 / 3 &&
      requests.some(a => a.uri === e.uri && a.ms < startRelative + seconds * 1000 * 2 / 3));
    return { durationSeconds: seconds, actualWindowMs: motionEnd - motionStart,
      loading: { fromUrlStartedBeforeAwait: true, firstRenderableMs, firstTexturedMs, configuredSSEReadyMs: readyMs, commonQualityReadyMs: null,
        definition: '3 frames with selected content; 3 with all selected model textures loaded; then 15 tilesLoaded+texture-ready frames at configured SSE; common quality requires separate ROI validation', frames: initialFrames },
      cacheIdentity: 'HTTP disabled/no-store; initial tileset GPU content empty; motion warm resident content; OS/JIT/worker caches uncontrolled',
      frameIntervalsMs: percentileSummary(frames.flatMap(f => f.interval === null ? [] : [f.interval])),
      renderCpuMs: percentileSummary(frames.map(f => f.cpuMs)), gpu: { supported: !!gpuExt, disjoint: gpuDisjoint,
        sampleEveryFrames: 10, milliseconds: gpuDisjoint ? null : percentileSummary(gpuMs), rawMs: gpuDisjoint ? [] : gpuMs },
      longtasks: { supported: PerformanceObserver.supportedEntryTypes?.includes('longtask') || false, motion: motionTasks, all: longtasks },
      requests: { renderableRequestEvents: requests.length, repeatedUris: Object.entries(counts).filter(([, n]) => n > 1),
        returnRequestEventsForPreviouslyRequestedUris: reloads, cancellations: events.filter(e => e.type === 'cancel') },
      endpoint: { settleMsAfterMotion: endpoint.loadWallMs, selectedUris: tileset._selectedTiles.map(uri), memoryAdjustedSSE: tileset.memoryAdjustedScreenSpaceError,
        heap: performance.memory ? { usedJSHeapSize: performance.memory.usedJSHeapSize, totalJSHeapSize: performance.memory.totalJSHeapSize } : null },
      frames, events, selectedSamples };
  } finally {
    activeTracker = null; removeLoad(); removeUnload(); C.Request.prototype.cancel = originalCancel;
    observer?.disconnect(); for (const q of gpuQueries) gl.deleteQuery(q);
  }
}

window.runCesiumResidencyDiagnostics = async function runCesiumResidencyDiagnostics() {
  renderSingleFrame();
  if (scene.canvas.width !== job.width || scene.canvas.height !== job.height) {
    throw new Error(
      `画布尺寸不匹配: 实测 ${scene.canvas.width}x${scene.canvas.height}, 预期 ${job.width}x${job.height}`,
    );
  }

  const environment = {
    cesiumVersion: C.VERSION,
    userAgent: navigator.userAgent,
    documentVisibilityState: document.visibilityState,
    canvasWidth: scene.canvas.width,
    canvasHeight: scene.canvas.height,
    devicePixelRatio: window.devicePixelRatio,
    msaaSamples: scene.msaaSamples,
    fxaa: scene.postProcessStages.fxaa.enabled,
    webglVersion: gl.getParameter(gl.VERSION),
    renderer: debugExt ? gl.getParameter(debugExt.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER),
    vendor: debugExt ? gl.getParameter(debugExt.UNMASKED_VENDOR_WEBGL) : gl.getParameter(gl.VENDOR),
  };

  const variantReports = [];

  for (const variantName of job.variants) {
    const configuredVariant = config.variants?.[variantName];
    if (!configuredVariant) {
      throw new Error(`config.variants 中不存在变体: ${variantName}`);
    }
    const url = configuredVariant.localUrl;
    const matchPrefix = url.match(/^(\/assets\/[^/]+\/)/);
    const variantPrefix = matchPrefix ? matchPrefix[1] : '/assets/';

    performance.clearResourceTimings();
    performance.setResourceTimingBufferSize(5000);
    glUploadCounters.texImage2DCalls = 0;
    glUploadCounters.compressedTexImage2DCalls = 0;
    glUploadCounters.compressedFormats = {};

    const consumerTilesetOptions = {
      maximumScreenSpaceError: 0,
      skipLevelOfDetail: false,
      dynamicScreenSpaceError: false,
      foveatedScreenSpaceError: false,
      cullRequestsWhileMoving: false,
      cacheBytes: 1024 ** 3,
      maximumCacheOverflowBytes: 512 * 1024 ** 2,
      modelMatrix,
      backFaceCulling: true,
      ...config.tilesetOptions,
    };

    const effectiveOptions = job.diagnosticOverrides
      ? { ...consumerTilesetOptions, ...job.diagnosticOverrides }
      : consumerTilesetOptions;

    const initialPoseName = job.poses[0] || 'near';
    setCameraPose(initialPoseName);

    const cumulativeTracker = createLifecycleTracker(variantPrefix);
    activeTracker = cumulativeTracker;

    let tileset = null;
    try {
      const loadStart = performance.now();
      tileset = await C.Cesium3DTileset.fromUrl(url, effectiveOptions);
      tileset.tileLoad.addEventListener((t) => cumulativeTracker.recordTileLoad(t));
      tileset.tileUnload.addEventListener((t) => cumulativeTracker.recordTileUnload(t));
      tileset.tileFailed.addEventListener((e) => cumulativeTracker.recordTileFailed(e));
      scene.primitives.add(tileset);

      const motion = job.motionSeconds ? await measureMotion(tileset, variantPrefix, loadStart, job.motionSeconds) : null;
      const poseSnapshots = [];
      for (let pIdx = 0; pIdx < (job.motionSeconds ? 0 : job.poses.length); pIdx++) {
        const poseName = job.poses[pIdx];
        const poseValues = setCameraPose(poseName);
        const poseTracker = createLifecycleTracker(variantPrefix);
        const removeLoad = tileset.tileLoad.addEventListener((t) => poseTracker.recordTileLoad(t));
        const removeUnload = tileset.tileUnload.addEventListener((t) => poseTracker.recordTileUnload(t));
        const removeFail = tileset.tileFailed.addEventListener((e) => poseTracker.recordTileFailed(e));

        const prevOrigObserve = cumulativeTracker.observeFrame.bind(cumulativeTracker);
        activeTracker = {
          recordRequestedTile(tile) {
            cumulativeTracker.recordRequestedTile(tile);
            poseTracker.recordRequestedTile(tile);
          },
        };
        let settledRun;
        try {
          // 同步推进单姿态轨迹与全会话累计轨迹
          const combinedTracker = {
            getTileKey: cumulativeTracker.getTileKey,
            observeFrame(ts, idx, startedMs) {
              poseTracker.observeFrame(ts, idx, startedMs);
              prevOrigObserve(ts, idx, loadStart);
            },
          };
          settledRun = await driveUntilSettled(tileset, combinedTracker);
        } finally {
          activeTracker = cumulativeTracker;
          removeLoad();
          removeUnload();
          removeFail();
        }

        const lifecycle = poseTracker.snapshotSummary(tileset);
        if (lifecycle.tileFailedErrors.length > 0) {
          throw new Error(`瓦片加载失败 (${variantName}/${poseName}): ${lifecycle.tileFailedErrors.join('; ')}`);
        }
        if (!job.diagnosticOverrides && lifecycle.sse.settledMemoryAdjustedSSE !== tileset.maximumScreenSpaceError) {
          throw new Error(
            `消费端默认配置下发生 memoryAdjustedSSE 抬升降质: ${lifecycle.sse.settledMemoryAdjustedSSE} !== ${tileset.maximumScreenSpaceError}`,
          );
        }

        const partition = auditSettledTileset(
          tileset,
          cumulativeTracker,
          variantPrefix,
          settledRun.visibleEventTiles,
        );

        const frameTiming = job.measureFrames ? await measureSteadyFrames(job.measureFrames) : undefined;
        let quality;
        if (job.captureQuality) {
          renderSingleFrame();
          const pngDataUrl = scene.canvas.toDataURL('image/png');
          const picks = [], rays = [], legacyPicks = [], legacyRays = [];
          const selectedTiles = new Set(tileset._selectedTiles);
          const selectedTransforms = [...selectedTiles].map(tile => {
            const graph = tile.content?._model?._sceneGraph;
            return { uri: tile._contentResource?.url, computedTransform: C.Matrix4.toArray(tile.computedTransform),
              modelComputedMatrix: graph?._computedModelMatrix ? C.Matrix4.toArray(graph._computedModelMatrix) : null,
              axisCorrectionMatrix: graph?._axisCorrectionMatrix ? C.Matrix4.toArray(graph._axisCorrectionMatrix) : null };
          });
          const cssSize = [scene.canvas.clientWidth, scene.canvas.clientHeight];
          const drawingBufferSize = [scene.drawingBufferWidth, scene.drawingBufferHeight];
          const scale = drawingBufferSize.map((v, i) => v / cssSize[i]);
          const samples = [];
          for (const y of [0.35, 0.5, 0.65]) for (const x of [0.25, 0.5, 0.75]) {
            samples.push({ id: `grid-${samples.length}`, x, y, requestedCss: [x * cssSize[0], y * cssSize[1]] });
          }
          for (const p of job.qualityPoints || []) {
            samples.push({ id: p.id, x: p.drawingBuffer[0] / drawingBufferSize[0], y: p.drawingBuffer[1] / drawingBufferSize[1], requestedCss: p.drawingBuffer.map((v, i) => v / scale[i]) });
          }
          function sampleAt(sample, css, outPicks, outRays) {
            const pixel = new C.Cartesian2(...css);
            const position = scene.pickPosition(pixel);
            const ray = viewer.camera.getPickRay(pixel);
            const coordinates = { ...sample, requestedNormalized: { x: sample.x, y: sample.y }, x: css[0] / cssSize[0], y: css[1] / cssSize[1], css, drawingBuffer: css.map((v, i) => v * scale[i]), readPixels: [Math.trunc(css[0] * scale[0]), Math.trunc(drawingBufferSize[1] - css[1] * scale[1])] };
            outPicks.push({ ...coordinates, world: position ? [position.x, position.y, position.z] : null });
            outRays.push({ ...coordinates, origin: [ray.origin.x, ray.origin.y, ray.origin.z], direction: [ray.direction.x, ray.direction.y, ray.direction.z] });
          }
          for (const sample of samples) {
            if (job.compareQualitySampling) sampleAt(sample, sample.requestedCss, legacyPicks, legacyRays);
            // Align depth unprojection and the CPU ray to the same physical pixel center.
            const center = sample.requestedCss.map((v, i) => Math.min(drawingBufferSize[i] - 1, Math.max(0, Math.floor(v * scale[i]))) + 0.5);
            sampleAt(sample, center.map((v, i) => v / scale[i]), picks, rays);
          }
          // Content picking renders another pass; collect it after all depth samples.
          for (const pick of picks) {
            const feature = scene.pick(new C.Cartesian2(...pick.css), 1, 1);
            const content = feature?.content || feature?.primitive?.content;
            const tile = content?.tile;
            pick.contentHit = { uri: content?.url || tile?._contentResource?.url || null, selected: tile ? selectedTiles.has(tile) : null };
          }
          quality = { pngDataUrl, picks, rays, ...(job.compareQualitySampling ? { legacyPicks, legacyRays } : {}), cssSize, drawingBufferSize, modelMatrix: C.Matrix4.toArray(modelMatrix), selectedTransforms };
        }
        poseSnapshots.push({
          ...(quality ? { quality } : {}),
          ...(frameTiming ? { frameTiming } : {}),
          poseName,
          pose: poseValues,
          settleWallMs: settledRun.loadWallMs,
          framesRendered: settledRun.framesRendered,
          lifecycle: {
            requested: lifecycle.requested,
            processing: lifecycle.processing,
            sse: lifecycle.sse,
          },
          ...partition,
        });
      }

      const cumulative = cumulativeTracker.snapshotSummary(tileset);
      const resourceEntries = performance.getEntriesByType('resource');

      variantReports.push({
        variant: variantName,
        label: configuredVariant.label,
        url,
        consumerDefaultConfig: !job.diagnosticOverrides,
        diagnosticOverrides: job.diagnosticOverrides || null,
        effectiveTilesetOptions: {
          maximumScreenSpaceError: tileset.maximumScreenSpaceError,
          skipLevelOfDetail: tileset.skipLevelOfDetail,
          immediatelyLoadDesiredLevelOfDetail: tileset.immediatelyLoadDesiredLevelOfDetail,
          loadSiblings: tileset.loadSiblings,
          dynamicScreenSpaceError: tileset.dynamicScreenSpaceError,
          foveatedScreenSpaceError: tileset.foveatedScreenSpaceError,
          cullWithChildrenBounds: tileset.cullWithChildrenBounds,
          preloadWhenHidden: tileset.preloadWhenHidden,
          backFaceCulling: tileset.backFaceCulling,
          cacheBytes: tileset.cacheBytes,
          maximumCacheOverflowBytes: tileset.maximumCacheOverflowBytes,
        },
        glUploads: {
          texImage2DCalls: glUploadCounters.texImage2DCalls,
          compressedTexImage2DCalls: glUploadCounters.compressedTexImage2DCalls,
          compressedFormats: { ...glUploadCounters.compressedFormats },
        },
        httpResourceRequests: {
          count: resourceEntries.length,
          encodedBodyBytes: resourceEntries.reduce((s, r) => s + (r.encodedBodySize || 0), 0),
        },
        cumulativeSessionLifecycle: {
          requested: cumulative.requested,
          processing: cumulative.processing,
          sse: cumulative.sse,
          totalFrames: cumulative.frameCount,
        },
        ...(motion ? { motion } : {}),
        poses: poseSnapshots,
      });
    } finally {
      activeTracker = null;
      if (tileset) {
        scene.primitives.remove(tileset);
      }
    }
  }

  viewer.destroy();
  return {
    environment,
    variants: variantReports,
  };
};

window.cesiumDiagnosticsReady = true;
