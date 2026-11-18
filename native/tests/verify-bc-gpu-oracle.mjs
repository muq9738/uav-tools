#!/usr/bin/env node
// BC1/BC2/BC3 独立 WebGL2 GPU Oracle 与解码候选对照入口脚本。
import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import fs from 'node:fs/promises';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import {
  decodePngToRgba,
  encodeRgbaPng,
  sha256Hex,
} from './bc-gpu-oracle/bc-reference.mjs';
import { buildOracleFixtureSuite } from './bc-gpu-oracle/fixtures.mjs';

const execFileAsync = promisify(execFile);
const __dirname = path.dirname(fileURLToPath(import.meta.url));

function parseArgs(argv) {
  const opts = {
    output: null,
    exportFixtures: null,
    candidateDir: null,
    candidateManifest: null,
    candidateCmd: null,
    maxChannelDelta: 0,
    simulateMissingExt: [],
    strictExit: false,
    spaceName: `bc-gpu-oracle-${process.pid}`,
  };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--output') opts.output = argv[++i];
    else if (arg === '--export-fixtures') opts.exportFixtures = argv[++i];
    else if (arg === '--candidate-dir') opts.candidateDir = argv[++i];
    else if (arg === '--candidate-manifest') opts.candidateManifest = argv[++i];
    else if (arg === '--candidate-cmd') opts.candidateCmd = argv[++i];
    else if (arg === '--max-channel-delta') opts.maxChannelDelta = Number(argv[++i]);
    else if (arg === '--simulate-missing-ext') {
      const val = argv[++i];
      for (const token of val.split(',').map((s) => s.trim()).filter(Boolean)) {
        if (token === 's3tc') opts.simulateMissingExt.push('WEBGL_compressed_texture_s3tc');
        else if (token === 's3tc_srgb') opts.simulateMissingExt.push('WEBGL_compressed_texture_s3tc_srgb');
        else if (token === 'all') {
          opts.simulateMissingExt.push(
            'WEBGL_compressed_texture_s3tc',
            'WEBGL_compressed_texture_s3tc_srgb',
          );
        } else {
          opts.simulateMissingExt.push(token);
        }
      }
    } else if (arg === '--strict-exit') opts.strictExit = true;
    else if (arg === '--help' || arg === '-h') {
      console.log(`用法: node native/tests/verify-bc-gpu-oracle.mjs [选项]
  --output <path>                 输出完整 GPU Oracle 对照 JSON 报告
  --export-fixtures <dir>         导出确定性压缩块夹具与清单供外部解码器消费
  --candidate-dir <dir>           读取外部解码候选 PNG 目录（<id>.png 或 <id>.mip<L>.png）
  --candidate-manifest <file>     读取外部候选清单 JSON
  --candidate-cmd <command>       先调用外部命令 "<cmd> <fixtures-manifest.json> <out-dir>" 生成候选
  --max-channel-delta <0..255>    判定 passed 的最大通道误差阈值（默认严格为 0）
  --simulate-missing-ext <s3tc|s3tc_srgb|all>  模拟缺失扩展，验证必记为 skipped 而非 passed
  --strict-exit                   若存在候选像素超阈值项则返回退出码 2`);
      process.exit(0);
    } else {
      throw new Error(`未知参数: ${arg}`);
    }
  }
  assert.ok(
    Number.isInteger(opts.maxChannelDelta) && opts.maxChannelDelta >= 0 && opts.maxChannelDelta <= 255,
    `非法 --max-channel-delta: ${opts.maxChannelDelta}`,
  );
  return opts;
}

export async function exportFixtureSuiteToDir(suite, targetDir) {
  const root = path.resolve(targetDir);
  const blocksDir = path.join(root, 'blocks');
  await fs.mkdir(blocksDir, { recursive: true });
  const manifestCases = [];

  for (const item of suite.cases) {
    const levelEntries = [];
    for (const lv of item.levels) {
      const rel = `blocks/${item.id}.mip${lv.level}.bc`;
      await fs.writeFile(path.join(root, rel), lv.compressedBytes);
      levelEntries.push({
        level: lv.level,
        width: lv.width,
        height: lv.height,
        byteLength: lv.compressedBytes.length,
        sha256: lv.compressedSha256,
        file: rel,
      });
    }
    manifestCases.push({
      id: item.id,
      category: item.category,
      description: item.description,
      formatKey: item.formatKey,
      formatName: item.formatName,
      internalFormat: item.internalFormat,
      internalFormatHex: item.internalFormatHex,
      requiredExtension: item.requiredExtension,
      srgb: item.srgb,
      fboEncoding: item.fboEncoding,
      width: item.width,
      height: item.height,
      mipCount: item.mipCount,
      minFilter: item.minFilter,
      magFilter: item.magFilter,
      wrapS: item.wrapS,
      wrapT: item.wrapT,
      uvRect: item.uvRect,
      viewport: [item.viewportWidth, item.viewportHeight],
      useExplicitLod: item.useExplicitLod,
      lod: item.lod,
      inputSha256: item.inputSha256,
      levels: levelEntries,
    });
  }

  const manifestPath = path.join(root, 'fixtures-manifest.json');
  const manifest = {
    schemaVersion: 1,
    caseCount: manifestCases.length,
    cases: manifestCases,
  };
  await fs.writeFile(manifestPath, JSON.stringify(manifest, null, 2) + '\n', 'utf8');
  return { root, manifestPath, manifest };
}

function verifyPngCodecRoundtrip(suite) {
  for (const item of suite.cases) {
    for (const lv of item.levels) {
      const decoded = decodePngToRgba(lv.referencePng);
      assert.equal(decoded.width, lv.width);
      assert.equal(decoded.height, lv.height);
      assert.ok(
        decoded.rgba.equals(lv.referenceRgba),
        `${item.id} mip${lv.level}: 内置无损 PNG 编解码往返不一致`,
      );
      const reencoded = encodeRgbaPng(lv.width, lv.height, decoded.rgba);
      assert.equal(sha256Hex(reencoded), lv.referencePngSha256);
    }
  }
  return true;
}

async function fileExists(p) {
  try {
    await fs.access(p);
    return true;
  } catch {
    return false;
  }
}

export async function resolveCandidateLevelsForSuite(suite, opts) {
  let candidateSource = 'builtin-khronos-dfs15-cpu-png';
  let manifestDoc = null;
  let baseDir = opts.candidateDir ? path.resolve(opts.candidateDir) : null;

  if (opts.candidateCmd) {
    const tmpRoot = await fs.mkdtemp(path.join(os.tmpdir(), 'bc-oracle-cand-'));
    const exported = await exportFixtureSuiteToDir(suite, path.join(tmpRoot, 'fixtures'));
    const outDir = baseDir || path.join(tmpRoot, 'candidate-out');
    await fs.mkdir(outDir, { recursive: true });
    await execFileAsync('/bin/sh', ['-c', `${opts.candidateCmd} "${exported.manifestPath}" "${outDir}"`], {
      maxBuffer: 32 * 1024 * 1024,
    });
    baseDir = outDir;
    candidateSource = `external-cmd:${opts.candidateCmd}`;
  }

  const manifestFile = opts.candidateManifest
    ? path.resolve(opts.candidateManifest)
    : (baseDir && (await fileExists(path.join(baseDir, 'candidate-manifest.json')))
      ? path.join(baseDir, 'candidate-manifest.json')
      : null);

  if (manifestFile) {
    manifestDoc = JSON.parse(await fs.readFile(manifestFile, 'utf8'));
    if (!baseDir) baseDir = path.dirname(manifestFile);
    candidateSource = `external-manifest:${manifestFile}`;
  } else if (baseDir) {
    candidateSource = `external-dir:${baseDir}`;
  }

  const resolvedCases = [];
  for (const item of suite.cases) {
    if (!baseDir && !manifestDoc) {
      resolvedCases.push({
        ...item,
        candidatePngSha256: item.referenceCandidatePngSha256,
        candidateError: null,
        levels: item.levels.map((lv) => ({
          level: lv.level,
          width: lv.width,
          height: lv.height,
          compressedBase64: lv.compressedBytes.toString('base64'),
          candidatePngBase64: lv.referencePng.toString('base64'),
        })),
      });
      continue;
    }

    const entry = manifestDoc?.cases?.[item.id] || null;
    if (entry && (entry.status === 'unsupported' || entry.status === 'rejected' || entry.error)) {
      resolvedCases.push({
        ...item,
        candidatePngSha256: null,
        candidateError: entry.reason || entry.error || entry.status,
        levels: item.levels.map((lv) => ({
          level: lv.level,
          width: lv.width,
          height: lv.height,
          compressedBase64: lv.compressedBytes.toString('base64'),
          candidatePngBase64: null,
        })),
      });
      continue;
    }

    try {
      const pngBuffers = [];
      for (const lv of item.levels) {
        let pngPath = null;
        if (entry?.pngLevels?.[lv.level]) {
          pngPath = path.resolve(baseDir, entry.pngLevels[lv.level]);
        } else if (lv.level === 0 && entry?.png) {
          if (item.levels.length > 1) {
            throw new Error('多级自定义 mip 用例仅提供了单张 base level PNG');
          }
          pngPath = path.resolve(baseDir, entry.png);
        } else {
          const mipCandidate = path.join(baseDir, `${item.id}.mip${lv.level}.png`);
          const singleCandidate = path.join(baseDir, `${item.id}.png`);
          if (await fileExists(mipCandidate)) {
            pngPath = mipCandidate;
          } else if (lv.level === 0 && item.levels.length === 1 && (await fileExists(singleCandidate))) {
            pngPath = singleCandidate;
          } else {
            throw new Error(`缺少候选 PNG 文件 (${item.id} mip${lv.level})`);
          }
        }
        const buf = await fs.readFile(pngPath);
        const parsed = decodePngToRgba(buf);
        if (parsed.width !== lv.width || parsed.height !== lv.height) {
          throw new Error(
            `候选 PNG 尺寸 ${parsed.width}x${parsed.height} 不匹配预期 ${lv.width}x${lv.height}`,
          );
        }
        pngBuffers.push(buf);
      }
      resolvedCases.push({
        ...item,
        candidatePngSha256: sha256Hex(Buffer.concat(pngBuffers)),
        candidateError: null,
        levels: item.levels.map((lv, idx) => ({
          level: lv.level,
          width: lv.width,
          height: lv.height,
          compressedBase64: lv.compressedBytes.toString('base64'),
          candidatePngBase64: pngBuffers[idx].toString('base64'),
        })),
      });
    } catch (err) {
      resolvedCases.push({
        ...item,
        candidatePngSha256: null,
        candidateError: String(err?.message || err),
        levels: item.levels.map((lv) => ({
          level: lv.level,
          width: lv.width,
          height: lv.height,
          compressedBase64: lv.compressedBytes.toString('base64'),
          candidatePngBase64: null,
        })),
      });
    }
  }

  const resolvedNegatives = suite.negativeControls.map((neg) => ({
    ...neg,
    candidatePngBase64Levels: neg.candidatePngLevels.map((b) => b.toString('base64')),
  }));

  return { candidateSource, cases: resolvedCases, negativeControls: resolvedNegatives };
}

async function runInEgoBrowser(payload, spaceName) {
  const oracleScriptSource = await fs.readFile(
    path.join(__dirname, 'bc-gpu-oracle', 'oracle-page.mjs'),
    'utf8',
  );
  const payloadJson = JSON.stringify(payload);
  const html = `<!doctype html>
<html lang="zh-CN">
<head><meta charset="utf-8"><title>BC1-BC3 WebGL2 GPU Oracle</title></head>
<body>
<pre id="status">Initializing WebGL2 GPU Oracle...</pre>
<script type="module" src="/oracle-page.mjs"></script>
</body>
</html>`;

  const server = http.createServer((req, res) => {
    res.setHeader('Cache-Control', 'no-store');
    if (req.url === '/' || req.url.startsWith('/index.html')) {
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
      res.end(html);
    } else if (req.url === '/oracle-page.mjs') {
      res.writeHead(200, { 'Content-Type': 'text/javascript; charset=utf-8' });
      res.end(oracleScriptSource);
    } else if (req.url === '/payload.json') {
      res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
      res.end(payloadJson);
    } else {
      res.writeHead(404).end('not found');
    }
  });

  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });

  const address = server.address();
  const port = address.port;
  const tmpDir = await fs.mkdtemp(path.join(os.tmpdir(), 'bc-gpu-oracle-ego-'));
  const resultFile = path.join(tmpDir, 'browser-result.json');
  const egoDriverScript = `
const fs = await import("node:fs/promises");
const task = await taskSpace(${JSON.stringify(spaceName)});
let finished = false;
try {
  const page = task.page("p1");
  await page.cdp("Emulation.setDeviceMetricsOverride", {
    width: 1280,
    height: 800,
    deviceScaleFactor: 1,
    mobile: false,
  });
  await page.goto(${JSON.stringify(`http://127.0.0.1:${port}/index.html`)}, { timeout: 20000 });
  await page.waitForFunction(() => window.oraclePageReady === true, undefined, { timeout: 15000 });
  const result = await page.evaluate(() => window.runBcGpuOracle());
  await fs.writeFile(${JSON.stringify(resultFile)}, JSON.stringify(result), "utf8");
  await task.finish({ keep: [] });
  finished = true;
} finally {
  if (!finished) {
    try { await task.finish({ keep: [] }); } catch {}
  }
}
`;

  try {
    await execFileAsync('ego-browser', ['nodejs', '-e', egoDriverScript], {
      maxBuffer: 16 * 1024 * 1024,
      timeout: 90_000,
    });
    const raw = await fs.readFile(resultFile, 'utf8');
    return JSON.parse(raw);
  } finally {
    server.close();
    await fs.rm(tmpDir, { recursive: true, force: true });
  }
}

export function buildSummaryAndAssert(browserResult, { candidateSource, maxChannelDelta, pngCodecLosslessRoundtrip }) {
  const { environment, gpuRepeatZeroDiff, cases, negativeControls } = browserResult;
  assert.ok(
    typeof environment?.webglVersion === 'string' && environment.webglVersion.includes('WebGL 2.0'),
    `未能建立真实 WebGL2 上下文: ${JSON.stringify(environment)}`,
  );
  assert.equal(pngCodecLosslessRoundtrip, true, 'PNG 无损编解码自检失败');
  assert.equal(gpuRepeatZeroDiff, true, 'GPU 同源重复绘制控制组出现非零像素波动');

  const activeNegatives = negativeControls.filter((n) => n.status !== 'skipped');
  const negativeControlsCaught = activeNegatives.length === 0
    ? null
    : activeNegatives.every((n) => n.detectedFailure === true);
  if (activeNegatives.length > 0) {
    assert.equal(
      negativeControlsCaught,
      true,
      `负向控制组未能拦截注入缺陷: ${JSON.stringify(negativeControls)}`,
    );
  }

  let passed = 0;
  let failed = 0;
  let skipped = 0;
  let exactZeroDiffCount = 0;
  let within1UlpCount = 0;
  let within2UlpCount = 0;
  let alphaExactCount = 0;
  const byCategory = {};
  const byFormat = {};

  for (const c of cases) {
    assert.ok(['passed', 'failed', 'skipped'].includes(c.status), `非法用例状态: ${c.status}`);
    if (c.status === 'skipped') {
      assert.ok(c.skipReason, `skipped 用例必须提供 skipReason: ${c.id}`);
      skipped++;
    } else if (c.status === 'passed') {
      assert.ok(c.metrics, `passed 用例必须包含像素误差统计: ${c.id}`);
      assert.ok(
        c.metrics.maxChannelDelta <= maxChannelDelta,
        `passed 用例超过误差阈值: ${c.id}`,
      );
      passed++;
    } else {
      failed++;
    }

    if (c.metrics) {
      if (c.metrics.maxChannelDelta === 0) exactZeroDiffCount++;
      if (c.metrics.maxChannelDelta <= 1) within1UlpCount++;
      if (c.metrics.maxChannelDelta <= 2) within2UlpCount++;
      if (c.metrics.alphaMaxDelta === 0) alphaExactCount++;
    }

    byCategory[c.category] ??= { total: 0, passed: 0, failed: 0, skipped: 0, maxObservedDelta: 0 };
    byCategory[c.category].total++;
    byCategory[c.category][c.status]++;
    if (c.metrics) {
      byCategory[c.category].maxObservedDelta = Math.max(
        byCategory[c.category].maxObservedDelta,
        c.metrics.maxChannelDelta,
      );
    }

    byFormat[c.formatKey] ??= {
      internalFormatHex: c.internalFormatHex,
      total: 0,
      passed: 0,
      failed: 0,
      skipped: 0,
      maxObservedDelta: 0,
    };
    byFormat[c.formatKey].total++;
    byFormat[c.formatKey][c.status]++;
    if (c.metrics) {
      byFormat[c.formatKey].maxObservedDelta = Math.max(
        byFormat[c.formatKey].maxObservedDelta,
        c.metrics.maxChannelDelta,
      );
    }
  }

  assert.equal(passed + failed + skipped, cases.length, '统计总数不闭合');

  const summary = {
    total: cases.length,
    passed,
    failed,
    skipped,
    executedOnGpu: passed + failed,
    toleranceMaxChannelDelta: maxChannelDelta,
    exactZeroDiffCount,
    within1UlpCount,
    within2UlpCount,
    alphaExactCount,
    byCategory,
    byFormat,
    failedCases: cases
      .filter((c) => c.status === 'failed')
      .map((c) => ({
        id: c.id,
        category: c.category,
        formatKey: c.formatKey,
        maxChannelDelta: c.metrics?.maxChannelDelta ?? null,
        maxAbsDiffRgba: c.metrics?.maxAbsDiffRgba ?? null,
        diffPixels: c.metrics?.diffPixels ?? null,
        totalPixels: c.metrics?.totalPixels ?? null,
        failureReason: c.failureReason,
      })),
    skippedCases: cases
      .filter((c) => c.status === 'skipped')
      .map((c) => ({
        id: c.id,
        category: c.category,
        formatKey: c.formatKey,
        width: c.width,
        height: c.height,
        skipReason: c.skipReason,
      })),
  };

  return {
    schemaVersion: 1,
    generatedAt: new Date().toISOString(),
    candidateSource,
    environment,
    controls: {
      pngCodecLosslessRoundtrip,
      gpuRepeatZeroDiff,
      negativeControlsCaught,
      negativeControls,
    },
    summary,
    cases,
  };
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  const suite = buildOracleFixtureSuite();
  const pngCodecLosslessRoundtrip = verifyPngCodecRoundtrip(suite);

  if (opts.exportFixtures) {
    const exported = await exportFixtureSuiteToDir(suite, opts.exportFixtures);
    console.log(JSON.stringify({
      exportedFixturesDir: exported.root,
      manifestPath: exported.manifestPath,
      caseCount: exported.manifest.caseCount,
    }, null, 2));
    if (!opts.output && !opts.candidateDir && !opts.candidateManifest && !opts.candidateCmd) {
      return;
    }
  }

  const resolved = await resolveCandidateLevelsForSuite(suite, opts);
  const browserResult = await runInEgoBrowser(
    {
      maxChannelDelta: opts.maxChannelDelta,
      simulateMissingExtensions: opts.simulateMissingExt,
      cases: resolved.cases,
      negativeControls: resolved.negativeControls,
    },
    opts.spaceName,
  );

  const report = buildSummaryAndAssert(browserResult, {
    candidateSource: resolved.candidateSource,
    maxChannelDelta: opts.maxChannelDelta,
    pngCodecLosslessRoundtrip,
  });

  if (opts.output) {
    const outPath = path.resolve(opts.output);
    await fs.mkdir(path.dirname(outPath), { recursive: true });
    await fs.writeFile(outPath, JSON.stringify(report, null, 2) + '\n', 'utf8');
  }

  console.log(JSON.stringify({
    candidateSource: report.candidateSource,
    renderer: report.environment.unmaskedRenderer || report.environment.renderer,
    extensions: report.environment.extensions,
    controls: {
      pngCodecLosslessRoundtrip: report.controls.pngCodecLosslessRoundtrip,
      gpuRepeatZeroDiff: report.controls.gpuRepeatZeroDiff,
      negativeControlsCaught: report.controls.negativeControlsCaught,
    },
    summary: report.summary,
    output: opts.output ? path.resolve(opts.output) : null,
  }, null, 2));

  if (opts.strictExit && report.summary.failed > 0) {
    process.exitCode = 2;
  }
}

const invokedDirectly = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (invokedDirectly) {
  main().catch((err) => {
    console.error(err?.stack || err);
    process.exit(1);
  });
}
