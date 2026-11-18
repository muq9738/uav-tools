import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import zlib from 'node:zlib';

import {
  buildBc2Block,
  buildBc3Block,
  buildColorBlock,
  decodePngToRgba,
  decodeS3tcLevel,
  encodeRgbaPng,
  packRgb565,
  sha256Hex,
} from './bc-gpu-oracle/bc-reference.mjs';
import { buildOracleFixtureSuite } from './bc-gpu-oracle/fixtures.mjs';
import {
  buildSummaryAndAssert,
  exportFixtureSuiteToDir,
  resolveCandidateLevelsForSuite,
} from './verify-bc-gpu-oracle.mjs';
import { assertResidencyReportInvariants } from './collect-cesium-residency.mjs';

test('BC1 RGB vs RGBA transparent index and BC2/BC3 c0<=c1 4-color semantics', () => {
  const c0 = packRgb565(4, 10, 20);
  const c1 = packRgb565(28, 50, 8);
  assert.ok(c0 < c1);
  const indices = [0, 1, 2, 3, 3, 2, 1, 0, 0, 3, 1, 2, 2, 1, 3, 0];
  const bc1Block = buildColorBlock(c0, c1, indices);

  const rgbaOut = decodeS3tcLevel('BC1_RGBA', bc1Block, 4, 4);
  const rgbOut = decodeS3tcLevel('BC1_RGB', bc1Block, 4, 4);

  // texel (3,0) has index 3: BC1_RGBA -> (0,0,0,0), BC1_RGB -> (0,0,0,255)
  assert.deepEqual([...rgbaOut.subarray(12, 16)], [0, 0, 0, 0]);
  assert.deepEqual([...rgbOut.subarray(12, 16)], [0, 0, 0, 255]);

  // BC2 and BC3 with the exact same c0 < c1 color block MUST still use 4-color interpolation (index 3 != black)
  const bc2Block = buildBc2Block(new Array(16).fill(9), c0, c1, indices);
  const bc2Out = decodeS3tcLevel('BC2_RGBA', bc2Block, 4, 4);
  assert.ok(bc2Out[12] > 0 && bc2Out[13] > 0 && bc2Out[14] > 0, 'BC2 c0<c1 索引3不得退化为黑');
  assert.equal(bc2Out[15], 9 * 17);

  // BC3 a0 <= a1 special indices 6 -> 0, 7 -> 255 across 48-bit bitfield
  const alphaIdx = [0, 1, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 3, 2, 1, 0];
  const bc3Block = buildBc3Block(40, 200, alphaIdx, c0, c1, indices);
  const bc3Out = decodeS3tcLevel('BC3_RGBA', bc3Block, 4, 4);
  assert.equal(bc3Out[6 * 4 + 3], 0, 'BC3 a0<=a1 索引6 alpha 必须为 0');
  assert.equal(bc3Out[7 * 4 + 3], 255, 'BC3 a0<=a1 索引7 alpha 必须为 255');
  assert.equal(bc3Out[8 * 4 + 3], 255, '跨字节索引位 slot 8 (index 7) alpha 必须为 255');
  assert.equal(bc3Out[9 * 4 + 3], 0, '跨字节索引位 slot 9 (index 6) alpha 必须为 0');
  assert.ok(bc3Out[12] > 0 && bc3Out[13] > 0, 'BC3 c0<c1 索引3颜色仍为 (c0+2c1)/3');
});

test('PNG encoder/decoder supports lossless roundtrip and Paeth/Sub/Up/Average filters', () => {
  const w = 5;
  const h = 3;
  const src = Buffer.alloc(w * h * 4);
  for (let i = 0; i < src.length; i++) {
    src[i] = (i * 73 + 19) & 0xff;
  }
  const encoded = encodeRgbaPng(w, h, src);
  const decoded = decodePngToRgba(encoded);
  assert.equal(decoded.width, w);
  assert.equal(decoded.height, h);
  assert.ok(decoded.rgba.equals(src));

  // Corrupt 1 byte in IDAT and ensure CRC check fails
  const corrupted = Buffer.from(encoded);
  corrupted[corrupted.length - 18] ^= 0x01;
  assert.throws(() => decodePngToRgba(corrupted), /CRC/);
});

test('Fixture export is deterministic and external candidate loader enforces mip & unsupported contracts', async () => {
  const tmpDir = await fs.mkdtemp(path.join(os.tmpdir(), 'bc-oracle-unit-'));
  try {
    const suiteA = buildOracleFixtureSuite();
    const suiteB = buildOracleFixtureSuite();
    assert.equal(suiteA.cases.length, suiteB.cases.length);
    for (let i = 0; i < suiteA.cases.length; i++) {
      assert.equal(suiteA.cases[i].inputSha256, suiteB.cases[i].inputSha256);
      assert.equal(
        suiteA.cases[i].referenceCandidatePngSha256,
        suiteB.cases[i].referenceCandidatePngSha256,
      );
    }

    const expDir = path.join(tmpDir, 'exported');
    const exported = await exportFixtureSuiteToDir(suiteA, expDir);
    assert.equal(exported.manifest.caseCount, suiteA.cases.length);

    // Construct a mock external candidate directory:
    // - write valid single PNG for 'bc1-rgba-transparent-index-nearest'
    // - write ONLY base PNG for 4-level mip case 'bc1-mip-custom-nearest-lod1' (must fail with mip error!)
    // - mark 'bc2-interp-4bitalpha-and-c0lec1-nearest' as unsupported in candidate-manifest.json
    const candDir = path.join(tmpDir, 'cand');
    await fs.mkdir(candDir, { recursive: true });
    const singleCase = suiteA.cases.find((c) => c.id === 'bc1-rgba-transparent-index-nearest');
    await fs.writeFile(
      path.join(candDir, `${singleCase.id}.png`),
      singleCase.levels[0].referencePng,
    );

    const mipCase = suiteA.cases.find((c) => c.id === 'bc1-mip-custom-nearest-lod1');
    await fs.writeFile(
      path.join(candDir, `${mipCase.id}.png`),
      mipCase.levels[0].referencePng,
    );

    await fs.writeFile(
      path.join(candDir, 'candidate-manifest.json'),
      JSON.stringify({
        cases: {
          'bc2-interp-4bitalpha-and-c0lec1-nearest': {
            status: 'unsupported',
            reason: 'BC2 decoder not implemented yet',
          },
          [mipCase.id]: {
            png: `${mipCase.id}.png`,
          },
        },
      }),
      'utf8',
    );

    const resolved = await resolveCandidateLevelsForSuite(suiteA, {
      candidateDir: candDir,
      candidateManifest: null,
      candidateCmd: null,
    });

    const resolvedSingle = resolved.cases.find((c) => c.id === singleCase.id);
    assert.equal(resolvedSingle.candidateError, null);
    assert.equal(resolvedSingle.candidatePngSha256, singleCase.referenceCandidatePngSha256);

    const resolvedMip = resolved.cases.find((c) => c.id === mipCase.id);
    assert.match(resolvedMip.candidateError, /多级自定义 mip/);

    const resolvedUnsupported = resolved.cases.find(
      (c) => c.id === 'bc2-interp-4bitalpha-and-c0lec1-nearest',
    );
    assert.equal(resolvedUnsupported.candidateError, 'BC2 decoder not implemented yet');
  } finally {
    await fs.rm(tmpDir, { recursive: true, force: true });
  }
});

test('Summary assertions strictly separate skipped from passed and enforce negative controls', () => {
  const mockBrowserResult = {
    environment: { webglVersion: 'WebGL 2.0 (OpenGL ES 3.0 Chromium)' },
    gpuRepeatZeroDiff: true,
    cases: [
      {
        id: 'c-pass',
        category: 'transparent_index',
        formatKey: 'BC1_RGBA',
        internalFormatHex: '0x83f1',
        status: 'passed',
        metrics: {
          maxChannelDelta: 0,
          alphaMaxDelta: 0,
          maxAbsDiffRgba: [0, 0, 0, 0],
          diffPixels: 0,
          totalPixels: 16,
        },
      },
      {
        id: 'c-skip',
        category: 'srgb',
        formatKey: 'BC1_SRGB',
        internalFormatHex: '0x8c4c',
        width: 8,
        height: 4,
        status: 'skipped',
        skipReason: 'missing_extension',
      },
    ],
    negativeControls: [
      { id: 'neg-1', status: 'caught', detectedFailure: true },
    ],
  };

  const report = buildSummaryAndAssert(mockBrowserResult, {
    candidateSource: 'unit-test',
    maxChannelDelta: 0,
    pngCodecLosslessRoundtrip: true,
  });
  assert.equal(report.summary.passed, 1);
  assert.equal(report.summary.skipped, 1);
  assert.equal(report.summary.failed, 0);

  // If a negative control fails to catch an injected bug, buildSummaryAndAssert MUST throw
  assert.throws(() => {
    buildSummaryAndAssert(
      {
        ...mockBrowserResult,
        negativeControls: [{ id: 'neg-1', status: 'missed', detectedFailure: false }],
      },
      {
        candidateSource: 'unit-test',
        maxChannelDelta: 0,
        pngCodecLosslessRoundtrip: true,
      },
    );
  }, /负向控制组/);
});

test('Cesium residency report invariant checker rejects cache-overflow SSE elevation under consumer config', () => {
  const baseValidReport = {
    environment: {
      cesiumVersion: '1.136.0',
      canvasWidth: 1920,
      canvasHeight: 851,
      devicePixelRatio: 1,
    },
    variants: [
      {
        variant: 'native',
        consumerDefaultConfig: true,
        poses: [
          {
            poseName: 'near',
            invariants: {
              residentMatchesCesiumContentReady: true,
              selectedMatchesCesiumSelected: true,
              selectedTrianglesMatchCesium: true,
              rolePartitionDisjointAndComplete: true,
              cacheSentinelSplitComplete: true,
              geometryBytesMatchCesium: true,
              textureBytesMatchCesium: true,
              settledZeroPendingAndProcessing: true,
            },
            lifecycle: {
              sse: {
                configuredMaximumSSE: 8,
                settledMemoryAdjustedSSE: 8,
                qualityDegradedByCacheOverflow: false,
              },
            },
          },
        ],
      },
    ],
  };

  assert.doesNotThrow(() =>
    assertResidencyReportInvariants(baseValidReport, { expectedDiagnosticOverrides: null }),
  );

  const degradedReport = structuredClone(baseValidReport);
  degradedReport.variants[0].poses[0].lifecycle.sse.settledMemoryAdjustedSSE = 9.6;
  degradedReport.variants[0].poses[0].lifecycle.sse.qualityDegradedByCacheOverflow = true;

  assert.throws(
    () => assertResidencyReportInvariants(degradedReport, { expectedDiagnosticOverrides: null }),
    /memoryAdjustedSSE/,
  );
});
