#!/usr/bin/env node
// Cesium 1.136 瓦片遍历、请求/处理生命周期与驻留归因只读采集 CLI。
// 不修改消费端默认配置，不写入源模型或外部工具目录；浏览器任务使用独立 ego-browser taskSpace 并在结束时关闭。
import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';

const execFileAsync = promisify(execFile);
const __dirname = path.dirname(fileURLToPath(import.meta.url));

// 只从环境变量与当前工作目录附近的常见位置定位，不写死任何本机绝对路径。
const DEFAULT_CESIUM_CANDIDATES = [
  process.env.CESIUM_HOME,
  path.join(process.cwd(), 'node_modules/cesium/Build/Cesium'),
  path.join(process.cwd(), '../node_modules/cesium/Build/Cesium'),
  path.join(process.cwd(), 'cesium/Build/Cesium'),
].filter(Boolean);

const MIME_TYPES = Object.freeze({
  '.html': 'text/html; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.wasm': 'application/wasm',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.webp': 'image/webp',
  '.ktx2': 'image/ktx2',
  '.glb': 'model/gltf-binary',
  '.gltf': 'model/gltf+json',
  '.b3dm': 'application/octet-stream',
  '.bin': 'application/octet-stream',
});

function parseArgs(argv) {
  const opts = {
    config: null,
    cesiumDir: null,
    sourceDir: null,
    preserveDir: null,
    referenceDir: null,
    customTilesets: {},
    variants: ['native'],
    poses: ['near', 'side', 'overview', 'near-return'],
    width: 1920,
    height: 851,
    diagnosticOverrides: null,
    captureQuality: false,
    qualityPoints: [],
    compareQualitySampling: false,
    measureFrames: 0,
    motionSeconds: 0,
    output: null,
    spaceName: `cesium-residency-diag-${process.pid}`,
  };

  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--config') opts.config = argv[++i];
    else if (arg === '--cesium-dir') opts.cesiumDir = argv[++i];
    else if (arg === '--source-dir') opts.sourceDir = argv[++i];
    else if (arg === '--preserve-dir') opts.preserveDir = argv[++i];
    else if (arg === '--reference-dir') opts.referenceDir = argv[++i];
    else if (arg === '--tileset') {
      const pair = argv[++i];
      const eq = pair.indexOf('=');
      if (eq <= 0) throw new Error(`--tileset 格式须为 <key>=<目录或tileset.json>: ${pair}`);
      opts.customTilesets[pair.slice(0, eq)] = pair.slice(eq + 1);
    } else if (arg === '--variants') {
      opts.variants = argv[++i].split(',').map((s) => s.trim()).filter(Boolean);
    } else if (arg === '--poses') {
      opts.poses = argv[++i].split(',').map((s) => s.trim()).filter(Boolean);
    } else if (arg === '--diagnostic-overrides') {
      opts.diagnosticOverrides = JSON.parse(argv[++i]);
    } else if (arg === '--motion-seconds') {
      opts.motionSeconds = Number(argv[++i]);
      assert.equal(opts.motionSeconds, 24, 'acceptance trajectory is fixed at 24 seconds');
    } else if (arg === '--measure-frames') {
      opts.measureFrames = Number(argv[++i]);
      assert.ok(Number.isInteger(opts.measureFrames) && opts.measureFrames >= 30 && opts.measureFrames <= 240, 'measure-frames must be 30..240');
    } else if (arg === '--capture-quality') {
      opts.captureQuality = true;
    } else if (arg === '--quality-points') {
      opts.qualityPoints = JSON.parse(argv[++i]);
      assert.ok(Array.isArray(opts.qualityPoints) && opts.qualityPoints.every(p => typeof p.id === 'string' && p.drawingBuffer?.length === 2 && p.drawingBuffer.every(Number.isFinite)), 'quality-points must be [{id,drawingBuffer:[x,y]}]');
    } else if (arg === '--compare-quality-sampling') {
      opts.compareQualitySampling = true;
    } else if (arg === '--output') {
      opts.output = argv[++i];
    } else if (arg === '--help' || arg === '-h') {
      console.log(`用法: node native/tests/collect-cesium-residency.mjs --config <compare-config.json> [选项]
  --config <file>                 消费端对照配置 JSON（只读）
  --cesium-dir <dir>              Cesium 1.136 Build/Cesium 目录（只读；缺省读 CESIUM_HOME 或当前目录附近）
  --preserve-dir <dir>            映射至 /assets/preserve/ 的只读资产根目录
  --reference-dir <dir>           映射至 /assets/reference/ 的只读资产根目录
  --source-dir <dir>              映射至 /assets/source/ 的只读资产根目录
  --tileset <key>=<path>          额外挂载只读变体目录或 tileset.json
  --variants <v1,v2>              待采集变体列表（默认: native）
  --poses <p1,p2>                 相机姿态序列（默认: near,side,overview,near-return）
  --diagnostic-overrides <json>   仅限显式诊断实验覆盖项（启用后标记 consumerDefaultConfig=false）
  --motion-seconds 24            独立冷内容加载及暖驻留连续24秒相机运动（无截图/全树审计）
  --measure-frames <30..240>      稳态CPU/GPU逐帧采样；与质量截图分开运行
  --capture-quality              单独质量采集：PNG及深度拾取；不作为性能计时结果
  --quality-points <json>        追加具名 drawing-buffer 点 [{id,drawingBuffer:[x,y]}]，不替换原九点
  --compare-quality-sampling     同帧额外记录修复前坐标拾取，仅用于核对采集器
  --output <file>                 输出诊断报告 JSON 路径`);
      process.exit(0);
    } else {
      throw new Error(`未知参数: ${arg}`);
    }
  }

  if (!opts.config) {
    throw new Error('必须提供 --config <compare-config.json>');
  }
  assert.ok(!(opts.captureQuality && (opts.measureFrames || opts.motionSeconds)), 'quality capture and frame timing must run separately');
  return opts;
}

async function resolveCesiumBuildDir(explicitDir) {
  const candidates = explicitDir ? [path.resolve(explicitDir)] : DEFAULT_CESIUM_CANDIDATES;
  for (const candidate of candidates) {
    const jsPath = path.join(candidate, 'Cesium.js');
    const pkgPath = path.resolve(candidate, '../../package.json');
    if (fs.existsSync(jsPath)) {
      if (fs.existsSync(pkgPath)) {
        const pkg = JSON.parse(await fsp.readFile(pkgPath, 'utf8'));
        assert.ok(
          String(pkg.version || '').startsWith('1.136.'),
          `Cesium 版本必须为 1.136.x，当前读取到 ${pkg.version} (${pkgPath})`,
        );
      }
      return candidate;
    }
  }
  throw new Error('未找到 Cesium 1.136 Build/Cesium，请通过 --cesium-dir 显式传入');
}

function buildDiagnosticHtml() {
  return `<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <title>Cesium 1.136 Residency Diagnostics</title>
  <link rel="stylesheet" href="/cesium/Widgets/widgets.css">
  <style>
    html, body {
      margin: 0;
      padding: 0;
      width: 100%;
      height: 100%;
      overflow: hidden;
      background: #09121a;
    }
    #view {
      position: fixed;
      left: 0;
      top: 0;
    }
  </style>
</head>
<body>
  <div id="view"></div>
  <script src="/cesium/Cesium.js"></script>
  <script type="module" src="/diagnostics-page.mjs"></script>
</body>
</html>`;
}

export function assertResidencyReportInvariants(rawReport, { expectedDiagnosticOverrides }) {
  const { environment, variants } = rawReport;
  assert.ok(
    String(environment?.cesiumVersion || '').startsWith('1.136.'),
    `运行期 Cesium.VERSION 非 1.136.x: ${environment?.cesiumVersion}`,
  );
  assert.equal(environment.canvasWidth, 1920, '画布宽非 1920');
  assert.equal(environment.canvasHeight, 851, '画布高非 851');
  assert.equal(environment.devicePixelRatio, 1, 'DPR 非 1');

  for (const v of variants) {
    assert.equal(
      v.consumerDefaultConfig,
      !expectedDiagnosticOverrides,
      `${v.variant}: consumerDefaultConfig 标记异常`,
    );
    for (const p of v.poses) {
      for (const [k, ok] of Object.entries(p.invariants)) {
        assert.equal(ok, true, `${v.variant}/${p.poseName} 不变量 ${k} 失败`);
      }
      if (!expectedDiagnosticOverrides) {
        assert.equal(
          p.lifecycle.sse.settledMemoryAdjustedSSE,
          p.lifecycle.sse.configuredMaximumSSE,
          `${v.variant}/${p.poseName}: 消费端默认配置下 memoryAdjustedSSE 被抬高`,
        );
        assert.equal(
          p.lifecycle.sse.qualityDegradedByCacheOverflow,
          false,
          `${v.variant}/${p.poseName}: 消费端默认配置下发生缓存溢出降质`,
        );
      }
    }
  }
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  const cesiumDir = await resolveCesiumBuildDir(opts.cesiumDir);
  const rawConfig = JSON.parse(await fsp.readFile(path.resolve(opts.config), 'utf8'));
  const configForPage = structuredClone(rawConfig);
  configForPage.variants ??= {};

  const routeRoots = [
    ['/cesium/', cesiumDir],
  ];
  if (opts.sourceDir) routeRoots.push(['/assets/source/', path.resolve(opts.sourceDir)]);
  if (opts.preserveDir) routeRoots.push(['/assets/preserve/', path.resolve(opts.preserveDir)]);
  if (opts.referenceDir) routeRoots.push(['/assets/reference/', path.resolve(opts.referenceDir)]);

  for (const [key, rawTarget] of Object.entries(opts.customTilesets)) {
    const resolved = path.resolve(rawTarget);
    const stat = await fsp.stat(resolved);
    let rootDir = resolved;
    let entryRel = 'tileset.json';
    if (stat.isFile()) {
      rootDir = path.dirname(resolved);
      entryRel = path.basename(resolved);
    }
    const prefix = `/assets/custom-${key}/`;
    routeRoots.push([prefix, rootDir]);
    configForPage.variants[key] = {
      label: `自定义只读挂载 (${key})`,
      localUrl: `${prefix}${entryRel}`,
      remoteUrl: `${prefix}${entryRel}`,
    };
  }

  for (const v of opts.variants) {
    assert.ok(configForPage.variants[v], `未找到待采集变体定义: ${v}`);
  }

  const pageScript = await fsp.readFile(
    path.join(__dirname, 'cesium-residency', 'diagnostics-page.mjs'),
    'utf8',
  );
  const html = buildDiagnosticHtml();
  const jobPayload = {
    variants: opts.variants,
    poses: opts.poses,
    width: opts.width,
    height: opts.height,
    fxaa: true,
    diagnosticOverrides: opts.diagnosticOverrides,
    captureQuality: opts.captureQuality,
    qualityPoints: opts.qualityPoints,
    compareQualitySampling: opts.compareQualitySampling,
    measureFrames: opts.measureFrames,
    motionSeconds: opts.motionSeconds,
  };

  const server = http.createServer((req, res) => {
    res.setHeader('Cache-Control', 'no-store');
    res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    if (req.method !== 'GET') {
      res.writeHead(405).end();
      return;
    }
    const url = new URL(req.url, 'http://127.0.0.1');
    if (url.pathname === '/' || url.pathname === '/diagnostics.html') {
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
      res.end(html);
      return;
    }
    if (url.pathname === '/diagnostics-page.mjs') {
      res.writeHead(200, { 'Content-Type': 'text/javascript; charset=utf-8' });
      res.end(pageScript);
      return;
    }
    if (url.pathname === '/config.json') {
      res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
      res.end(JSON.stringify(configForPage));
      return;
    }
    if (url.pathname === '/job.json') {
      res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
      res.end(JSON.stringify(jobPayload));
      return;
    }

    const matched = routeRoots.find(([prefix]) => url.pathname.startsWith(prefix));
    if (!matched) {
      res.writeHead(404).end('not found');
      return;
    }
    const [prefix, rootDir] = matched;
    let filePath;
    try {
      const sub = decodeURIComponent(url.pathname.slice(prefix.length));
      filePath = path.resolve(rootDir, sub);
    } catch {
      res.writeHead(400).end();
      return;
    }
    if (filePath !== rootDir && !filePath.startsWith(rootDir + path.sep)) {
      res.writeHead(403).end();
      return;
    }
    fs.stat(filePath, (err, stat) => {
      if (err || !stat.isFile()) {
        res.writeHead(404).end();
        return;
      }
      res.setHeader('Content-Type', MIME_TYPES[path.extname(filePath).toLowerCase()] || 'application/octet-stream');
      res.setHeader('Content-Length', stat.size);
      fs.createReadStream(filePath).pipe(res);
    });
  });

  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const port = server.address().port;

  const workParent = opts.output ? path.dirname(path.resolve(opts.output)) : os.tmpdir();
  await fsp.mkdir(workParent, { recursive: true });
  const tmpDir = await fsp.mkdtemp(path.join(workParent, 'cesium-residency-ego-'));
  const resultFile = path.join(tmpDir, 'residency-result.json');
  const egoScript = `
const fs = await import("node:fs/promises");
const task = await taskSpace(${JSON.stringify(opts.spaceName)});
let finished = false;
try {
  const page = task.page("p1");
  await page.cdp("Emulation.setDeviceMetricsOverride", {
    width: ${opts.width},
    height: ${opts.height},
    deviceScaleFactor: 1,
    mobile: false,
  });
  await page.cdp("Network.enable");
  await page.cdp("Network.setCacheDisabled", { cacheDisabled: true });
  await page.goto(${JSON.stringify(`http://127.0.0.1:${port}/diagnostics.html`)}, { timeout: 30000 });
  await page.waitForFunction(() => window.cesiumDiagnosticsReady === true, undefined, { timeout: 30000 });
  // evaluate has a fixed browser safety timeout; its second argument is page data,
  // not timeout options. Start the job and observe completion in bounded calls.
  await page.evaluate(() => {
    window.residencyJob = { done: false };
    window.runCesiumResidencyDiagnostics().then(
      data => { window.residencyJob = { done: true, data }; },
      error => { window.residencyJob = { done: true, error: String(error?.stack || error) }; },
    );
  });
  await page.waitForFunction(() => window.residencyJob?.done === true, undefined, { timeout: 600000 });
  const result = await page.evaluate(() => window.residencyJob);
  if (result.error) throw new Error(result.error);
  const data = result.data;
  await fs.writeFile(${JSON.stringify(resultFile)}, JSON.stringify(data), "utf8");
  await task.finish({ keep: [] });
  finished = true;
} finally {
  if (!finished) {
    try { await task.finish({ keep: [] }); } catch {}
  }
}
`;

  let rawReport;
  try {
    await execFileAsync('ego-browser', ['nodejs', '-e', egoScript], {
      maxBuffer: 64 * 1024 * 1024,
      timeout: 660_000,
    });
    rawReport = JSON.parse(await fsp.readFile(resultFile, 'utf8'));
  } finally {
    server.close();
    await fsp.rm(tmpDir, { recursive: true, force: true });
  }

  // Preserve actual browser evidence even if a report invariant rejects acceptance.
  if (opts.output) {
    const rawPath = path.resolve(opts.output).replace(/\.json$/, '') + '.raw.json';
    await fsp.mkdir(path.dirname(rawPath), { recursive: true });
    await fsp.writeFile(rawPath, JSON.stringify(rawReport) + '\n', 'utf8');
  }
  assertResidencyReportInvariants(rawReport, {
    expectedDiagnosticOverrides: opts.diagnosticOverrides,
  });

  if (opts.captureQuality && opts.output) {
    const imageDir = path.dirname(path.resolve(opts.output));
    await fsp.mkdir(imageDir, { recursive: true });
    for (const variant of rawReport.variants) for (const pose of variant.poses) {
      const quality = pose.quality;
      assert.ok(quality?.pngDataUrl?.startsWith('data:image/png;base64,'), 'missing quality frame');
      const name = `quality-${variant.variant}-${pose.poseName}.png`.replace(/[^a-zA-Z0-9._-]/g, '_');
      await fsp.writeFile(path.join(imageDir, name), Buffer.from(quality.pngDataUrl.split(',')[1], 'base64'));
      delete quality.pngDataUrl; quality.pngFile = name;
    }
  }
  const report = {
    schemaVersion: 1,
    qualityCapture: opts.captureQuality,
    performanceTimingValid: !opts.captureQuality,
    generatedAt: new Date().toISOString(),
    disclaimer:
      '只读遍历与驻留归因诊断证据；严格保持消费端配置（除非显式传入 diagnosticOverrides），不代表生产转换器已获性能提升，亦不以降低画质达标。',
    configPath: path.resolve(opts.config),
    cesiumBuildDir: cesiumDir,
    ...rawReport,
  };

  if (opts.output) {
    const outPath = path.resolve(opts.output);
    await fsp.mkdir(path.dirname(outPath), { recursive: true });
    await fsp.writeFile(outPath, JSON.stringify(report, null, 2) + '\n', 'utf8');
  }

  // 输出精炼摘要供终端核验
  const concise = report.variants.map((v) => ({
    variant: v.variant,
    consumerDefaultConfig: v.consumerDefaultConfig,
    glUploads: v.glUploads,
    poses: v.poses.map((p) => ({
      pose: p.poseName,
      configuredSSE: p.lifecycle.sse.configuredMaximumSSE,
      memoryAdjustedSSE: p.lifecycle.sse.settledMemoryAdjustedSSE,
      requestedRenderable: p.lifecycle.requested.everRequestedRenderableCount,
      requestedJson: p.lifecycle.requested.everRequestedTilesetJsonCount,
      processingCompleted: p.lifecycle.processing.tileLoadCompletedCount,
      residentTotal: p.cesiumStatistics.numberOfTilesWithContentReady,
      uniqueTextures: p.cesiumStatistics.uniqueResidentTextureCount,
      totalTextureMiB: Number((p.cesiumStatistics.texturesByteLength / 2 ** 20).toFixed(3)),
      totalGeometryMiB: Number((p.cesiumStatistics.geometryByteLength / 2 ** 20).toFixed(3)),
      trianglesSelected: p.cesiumStatistics.numberOfTrianglesSelected,
      roles: Object.fromEntries(
        Object.entries(p.roles).map(([role, r]) => [
          role,
          {
            tiles: r.tileCount,
            pinned: r.pinnedThisFrameCount,
            evictable: r.evictableLruCount,
            geomMiB: Number((r.geometryBytes / 2 ** 20).toFixed(3)),
            texMiB: Number((r.deduplicatedTextureBytes / 2 ** 20).toFixed(3)),
            exclusiveTexOutsideSelectedMiB: Number(
              (r.exclusiveTextureBytesOutsideSelected / 2 ** 20).toFixed(3),
            ),
          },
        ]),
      ),
      siblingBreakdown: p.siblingBreakdown,
    })),
  }));

  console.log(JSON.stringify({
    cesiumVersion: report.environment.cesiumVersion,
    renderer: report.environment.renderer,
    viewport: `${report.environment.canvasWidth}x${report.environment.canvasHeight}@DPR${report.environment.devicePixelRatio}`,
    variants: concise,
    output: opts.output ? path.resolve(opts.output) : null,
  }, null, 2));
}

const invokedDirectly = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (invokedDirectly) {
  main().catch((err) => {
    console.error(err?.stack || err);
    process.exit(1);
  });
}
