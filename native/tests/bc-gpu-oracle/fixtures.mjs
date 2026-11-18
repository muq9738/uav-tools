// 确定性 BC1/BC2/BC3 压缩纹理测试夹具集（含多级自定义 mip 与负向控制组）。
import {
  S3TC_FORMATS,
  buildColorBlock,
  buildBc2Block,
  buildBc3Block,
  decodeS3tcLevel,
  encodeRgbaPng,
  packRgb565,
  sha256Hex,
} from './bc-reference.mjs';

export const GL_ENUMS = Object.freeze({
  NEAREST: 0x2600,
  LINEAR: 0x2601,
  NEAREST_MIPMAP_NEAREST: 0x2700,
  LINEAR_MIPMAP_NEAREST: 0x2701,
  NEAREST_MIPMAP_LINEAR: 0x2702,
  LINEAR_MIPMAP_LINEAR: 0x2703,
  REPEAT: 0x2901,
  CLAMP_TO_EDGE: 0x812f,
  MIRRORED_REPEAT: 0x8370,
});

const ASYM_INDICES_A = [
  0, 2, 3, 1,
  2, 0, 1, 3,
  3, 1, 2, 0,
  1, 3, 0, 2,
];

const ASYM_INDICES_B = [
  1, 0, 2, 3,
  3, 2, 0, 1,
  0, 3, 1, 2,
  2, 1, 3, 0,
];

function concatBlocks(blocks) {
  return Buffer.concat(blocks);
}

function makeCase({
  id,
  category,
  description,
  formatKey,
  levels, // [{ width, height, compressedBytes }]
  minFilter = 'NEAREST',
  magFilter = 'NEAREST',
  wrapS = 'CLAMP_TO_EDGE',
  wrapT = 'CLAMP_TO_EDGE',
  uvRect = [0, 0, 1, 1], // [u0, v0, u1, v1]
  viewport = null, // [vw, vh]
  useExplicitLod = false,
  lod = 0.0,
  fboEncoding = null, // 'rgba8' | 'srgb8_alpha8'
}) {
  const fmt = S3TC_FORMATS[formatKey];
  const base = levels[0];
  const vw = viewport ? viewport[0] : base.width;
  const vh = viewport ? viewport[1] : base.height;
  const resolvedFbo = fboEncoding || (fmt.srgb ? 'srgb8_alpha8' : 'rgba8');

  const preparedLevels = levels.map((lv, idx) => {
    const rgba = decodeS3tcLevel(formatKey, lv.compressedBytes, lv.width, lv.height);
    const png = encodeRgbaPng(lv.width, lv.height, rgba);
    return {
      level: idx,
      width: lv.width,
      height: lv.height,
      compressedBytes: lv.compressedBytes,
      compressedSha256: sha256Hex(lv.compressedBytes),
      referenceRgba: rgba,
      referencePng: png,
      referencePngSha256: sha256Hex(png),
    };
  });

  const combinedInputHash = sha256Hex(
    Buffer.concat(preparedLevels.map((l) => l.compressedBytes)),
  );
  const combinedPngHash = sha256Hex(
    Buffer.concat(preparedLevels.map((l) => l.referencePng)),
  );

  return {
    id,
    category,
    description,
    formatKey,
    formatName: fmt.glName,
    internalFormat: fmt.internalFormat,
    internalFormatHex: '0x' + fmt.internalFormat.toString(16),
    requiredExtension: fmt.extension,
    srgb: fmt.srgb,
    width: base.width,
    height: base.height,
    mipCount: preparedLevels.length,
    minFilter,
    minFilterEnum: GL_ENUMS[minFilter],
    magFilter,
    magFilterEnum: GL_ENUMS[magFilter],
    wrapS,
    wrapSEnum: GL_ENUMS[wrapS],
    wrapT,
    wrapTEnum: GL_ENUMS[wrapT],
    uvRect,
    viewportWidth: vw,
    viewportHeight: vh,
    useExplicitLod,
    lod,
    fboEncoding: resolvedFbo,
    inputSha256: combinedInputHash,
    referenceCandidatePngSha256: combinedPngHash,
    levels: preparedLevels,
  };
}

export function buildOracleFixtureSuite() {
  const cases = [];

  // 1. 端点插值：先测纯端点（仅索引 0/1，隔离 RGB565 位复制展开 vs 1/3 内插权重）
  const c0Exact = packRgb565(30, 60, 24);
  const c1Exact = packRgb565(6, 12, 3);
  const c0Frac = packRgb565(29, 53, 19);
  const c1Frac = packRgb565(4, 11, 2);
  assertGreater(c0Exact, c1Exact);
  assertGreater(c0Frac, c1Frac);

  cases.push(
    makeCase({
      id: 'bc1-endpoints-idx01-only-nearest',
      category: 'endpoint_interpolation',
      description: 'BC1 仅使用索引 0/1（纯 RGB565 端点位复制展开，无 1/3 内插）NEAREST',
      formatKey: 'BC1_RGBA',
      levels: [{
        width: 8,
        height: 4,
        compressedBytes: concatBlocks([
          buildColorBlock(c0Exact, c1Exact, [0, 1, 0, 1, 1, 0, 1, 0, 0, 0, 1, 1, 1, 1, 0, 0]),
          buildColorBlock(c0Frac, c1Frac, [1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1]),
        ]),
      }],
    }),
  );

  cases.push(
    makeCase({
      id: 'bc1-interp-4color-nearest',
      category: 'endpoint_interpolation',
      description: 'BC1 四色端点插值（c0 > c1，覆盖整除3与余1/余2分数端点差）NEAREST',
      formatKey: 'BC1_RGBA',
      levels: [{
        width: 8,
        height: 4,
        compressedBytes: concatBlocks([
          buildColorBlock(c0Exact, c1Exact, ASYM_INDICES_A),
          buildColorBlock(c0Frac, c1Frac, ASYM_INDICES_B),
        ]),
      }],
    }),
  );

  // BC1 三色+特殊索引模式（c0 < c1 及 c0 == c1）在 BC1_RGB 下索引3为不透明黑
  const cLow = packRgb565(5, 19, 27);
  const cHigh = packRgb565(26, 50, 8);
  const cEqual = packRgb565(17, 35, 22);
  cases.push(
    makeCase({
      id: 'bc1-interp-3color-midpoint-nearest',
      category: 'endpoint_interpolation',
      description: 'BC1_RGB 三色模式（c0 < c1 中点插值与 c0 == c1 退化块，索引3不透明黑）',
      formatKey: 'BC1_RGB',
      levels: [{
        width: 8,
        height: 4,
        compressedBytes: concatBlocks([
          buildColorBlock(cLow, cHigh, ASYM_INDICES_A),
          buildColorBlock(cEqual, cEqual, [0, 1, 2, 3, 3, 2, 1, 0, 2, 3, 0, 1, 1, 0, 3, 2]),
        ]),
      }],
    }),
  );

  // BC2 显式 4-bit alpha (0..15) + 验证 c0 <= c1 时颜色子块仍保持四色模式
  const bc2AlphaRamp = Array.from({ length: 16 }, (_, i) => i);
  const bc2AlphaChecker = [0, 15, 3, 12, 9, 6, 1, 14, 5, 10, 8, 7, 2, 13, 4, 11];
  cases.push(
    makeCase({
      id: 'bc2-interp-4bitalpha-and-c0lec1-nearest',
      category: 'endpoint_interpolation',
      description: 'BC2 0..15 全16档 4-bit alpha 展开，且验证 c0 < c1 时颜色仍为四色模式',
      formatKey: 'BC2_RGBA',
      levels: [{
        width: 8,
        height: 4,
        compressedBytes: concatBlocks([
          buildBc2Block(bc2AlphaRamp, c0Frac, c1Frac, ASYM_INDICES_A),
          buildBc2Block(bc2AlphaChecker, cLow, cHigh, ASYM_INDICES_B),
        ]),
      }],
    }),
  );

  // BC3 8级 alpha 插值 (a0 > a1) 与 6级+0/255 模式 (a0 <= a1)，覆盖跨字节 3-bit 索引位
  const bc3IdxAll8 = [0, 1, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 3, 2, 1, 0];
  const bc3IdxSpecial = [0, 2, 4, 6, 7, 5, 3, 1, 6, 7, 0, 1, 2, 3, 4, 5];
  cases.push(
    makeCase({
      id: 'bc3-interp-alpha8-alpha6-c0lec1-nearest',
      category: 'endpoint_interpolation',
      description: 'BC3 8级(a0>a1)与6级+0/255(a0<=a1) alpha 插值，跨字节3-bit位带及 c0<c1 四色保持',
      formatKey: 'BC3_RGBA',
      levels: [{
        width: 8,
        height: 4,
        compressedBytes: concatBlocks([
          buildBc3Block(241, 29, bc3IdxAll8, c0Frac, c1Frac, ASYM_INDICES_A),
          buildBc3Block(37, 213, bc3IdxSpecial, cLow, cHigh, ASYM_INDICES_B),
        ]),
      }],
    }),
  );

  // 2. 透明索引：同一 8 字节块在 BC1_RGBA 与 BC1_RGB 下对照，及 LINEAR 过滤透明边界
  const transparentTestBlock = buildColorBlock(
    packRgb565(6, 44, 25),
    packRgb565(28, 12, 9), // c0 < c1
    [
      0, 0, 3, 3,
      0, 2, 3, 1,
      3, 3, 1, 1,
      3, 2, 1, 0,
    ],
  );
  cases.push(
    makeCase({
      id: 'bc1-rgba-transparent-index-nearest',
      category: 'transparent_index',
      description: 'BC1_RGBA c0<=c1 索引3必须输出透明 (0,0,0,0) 而非不透明黑',
      formatKey: 'BC1_RGBA',
      levels: [{ width: 4, height: 4, compressedBytes: transparentTestBlock }],
    }),
  );
  cases.push(
    makeCase({
      id: 'bc1-rgb-opaque-index3-nearest',
      category: 'transparent_index',
      description: '同一压缩块以 BC1_RGB 上传时索引3必须输出不透明黑 (0,0,0,255)',
      formatKey: 'BC1_RGB',
      levels: [{ width: 4, height: 4, compressedBytes: transparentTestBlock }],
    }),
  );
  cases.push(
    makeCase({
      id: 'bc1-rgba-transparent-index-linear',
      category: 'transparent_index',
      description: 'BC1_RGBA 透明索引与不透明纹素交界处的 LINEAR 双线性过滤（检验透明纹素 RGB=0）',
      formatKey: 'BC1_RGBA',
      levels: [{ width: 4, height: 4, compressedBytes: transparentTestBlock }],
      minFilter: 'LINEAR',
      magFilter: 'LINEAR',
      viewport: [29, 29],
    }),
  );

  // 3. 块边界（4x4 块拼接缝、跨块双线性插值及 REPEAT / MIRRORED_REPEAT 外缘）
  const boundary8x8 = concatBlocks([
    buildColorBlock(packRgb565(31, 0, 0), packRgb565(20, 8, 2), ASYM_INDICES_A),
    buildColorBlock(packRgb565(0, 63, 0), packRgb565(2, 30, 10), ASYM_INDICES_B),
    buildColorBlock(packRgb565(0, 0, 31), packRgb565(5, 10, 18), ASYM_INDICES_B),
    buildColorBlock(packRgb565(31, 63, 0), packRgb565(18, 22, 28), ASYM_INDICES_A),
  ]);
  cases.push(
    makeCase({
      id: 'bc1-block-boundary-8x8-nearest',
      category: 'block_boundary',
      description: '8x8 四块拼接跨 x=3|4, y=3|4 块边界 NEAREST 纹素对齐',
      formatKey: 'BC1_RGBA',
      levels: [{ width: 8, height: 8, compressedBytes: boundary8x8 }],
    }),
  );
  cases.push(
    makeCase({
      id: 'bc1-block-boundary-8x8-linear',
      category: 'block_boundary',
      description: '8x8 四块拼接跨 x=3|4, y=3|4 块边界非整倍数视口 (37x29) LINEAR 采样',
      formatKey: 'BC1_RGBA',
      levels: [{ width: 8, height: 8, compressedBytes: boundary8x8 }],
      minFilter: 'LINEAR',
      magFilter: 'LINEAR',
      viewport: [37, 29],
    }),
  );

  const bc3Blocks12x8 = [];
  for (let by = 0; by < 2; by++) {
    for (let bx = 0; bx < 3; bx++) {
      const cA = packRgb565(28 - bx * 7, 15 + by * 35, 6 + bx * 9);
      const cB = packRgb565(5 + bx * 4, 55 - by * 25, 27 - bx * 8);
      const a0 = by === 0 ? 250 - bx * 30 : 45 + bx * 20;
      const a1 = by === 0 ? 60 + bx * 25 : 220 - bx * 15;
      bc3Blocks12x8.push(
        buildBc3Block(a0, a1, (bx + by) % 2 === 0 ? bc3IdxAll8 : bc3IdxSpecial, cA, cB, ASYM_INDICES_A),
      );
    }
  }
  const bytes12x8Bc3 = concatBlocks(bc3Blocks12x8);
  cases.push(
    makeCase({
      id: 'bc3-block-boundary-12x8-repeat-linear',
      category: 'block_boundary',
      description: 'BC3 12x8 跨块与外缘 REPEAT 寻址 [-0.25, 1.25]^2 LINEAR 采样',
      formatKey: 'BC3_RGBA',
      levels: [{ width: 12, height: 8, compressedBytes: bytes12x8Bc3 }],
      minFilter: 'LINEAR',
      magFilter: 'LINEAR',
      wrapS: 'REPEAT',
      wrapT: 'REPEAT',
      uvRect: [-0.25, -0.25, 1.25, 1.25],
      viewport: [43, 31],
    }),
  );
  cases.push(
    makeCase({
      id: 'bc3-block-boundary-12x8-mirror-linear',
      category: 'block_boundary',
      description: 'BC3 12x8 跨块与外缘 MIRRORED_REPEAT 寻址 [-0.25, 1.25]^2 LINEAR 采样',
      formatKey: 'BC3_RGBA',
      levels: [{ width: 12, height: 8, compressedBytes: bytes12x8Bc3 }],
      minFilter: 'LINEAR',
      magFilter: 'LINEAR',
      wrapS: 'MIRRORED_REPEAT',
      wrapT: 'MIRRORED_REPEAT',
      uvRect: [-0.25, -0.25, 1.25, 1.25],
      viewport: [43, 31],
    }),
  );

  // 4. 小尺寸与非 4 倍数边界（区分 1x1/2x2/4x2/2x4 与 WebGL level-0 非4倍数探针 3x3/5x3）
  const smallBlockBc1 = buildColorBlock(
    packRgb565(29, 14, 22),
    packRgb565(7, 52, 5),
    [0, 1, 2, 3, 2, 3, 0, 1, 1, 0, 3, 2, 3, 2, 1, 0],
  );
  cases.push(
    makeCase({
      id: 'bc1-small-1x1-nearest',
      category: 'small_dimensions',
      description: '1x1 最小尺寸 BC1 单纹素有效裁剪（忽略 4x4 块其余 15 个纹素）',
      formatKey: 'BC1_RGBA',
      levels: [{ width: 1, height: 1, compressedBytes: smallBlockBc1 }],
      viewport: [8, 8],
    }),
  );
  cases.push(
    makeCase({
      id: 'bc1-small-2x2-nearest',
      category: 'small_dimensions',
      description: '2x2 小尺寸 BC1 左上 2x2 有效区 NEAREST 采样',
      formatKey: 'BC1_RGBA',
      levels: [{ width: 2, height: 2, compressedBytes: smallBlockBc1 }],
      viewport: [8, 8],
    }),
  );
  cases.push(
    makeCase({
      id: 'bc1-small-2x2-linear',
      category: 'small_dimensions',
      description: '2x2 小尺寸 BC1 LINEAR 采样（验证第3/4列未激活纹素不混入边缘插值）',
      formatKey: 'BC1_RGBA',
      levels: [{ width: 2, height: 2, compressedBytes: smallBlockBc1 }],
      minFilter: 'LINEAR',
      magFilter: 'LINEAR',
      viewport: [17, 17],
    }),
  );
  cases.push(
    makeCase({
      id: 'bc2-small-4x2-linear',
      category: 'small_dimensions',
      description: '4x2 半块高度 BC2 LINEAR 采样',
      formatKey: 'BC2_RGBA',
      levels: [{
        width: 4,
        height: 2,
        compressedBytes: buildBc2Block(bc2AlphaChecker, c0Frac, c1Frac, ASYM_INDICES_A),
      }],
      minFilter: 'LINEAR',
      magFilter: 'LINEAR',
      viewport: [20, 13],
    }),
  );
  cases.push(
    makeCase({
      id: 'bc3-small-2x4-linear',
      category: 'small_dimensions',
      description: '2x4 半块宽度 BC3 LINEAR 采样',
      formatKey: 'BC3_RGBA',
      levels: [{
        width: 2,
        height: 4,
        compressedBytes: buildBc3Block(230, 35, bc3IdxAll8, c0Frac, c1Frac, ASYM_INDICES_B),
      }],
      minFilter: 'LINEAR',
      magFilter: 'LINEAR',
      viewport: [13, 20],
    }),
  );
  // WebGL S3TC 规范对 level-0 非 1/2/4k 尺寸通常返回 INVALID_OPERATION；探针如实检测，驱动拒绝则记 skipped
  cases.push(
    makeCase({
      id: 'bc1-small-3x3-level0-probe',
      category: 'small_dimensions',
      description: '3x3 非4倍数 level-0 S3TC 上传探针（WebGL 若按规范拒绝则记 skipped）',
      formatKey: 'BC1_RGBA',
      levels: [{ width: 3, height: 3, compressedBytes: smallBlockBc1 }],
      viewport: [12, 12],
    }),
  );
  cases.push(
    makeCase({
      id: 'bc3-small-5x3-level0-probe',
      category: 'small_dimensions',
      description: '5x3 跨两块非4倍数 level-0 S3TC 上传探针（WebGL 若按规范拒绝则记 skipped）',
      formatKey: 'BC3_RGBA',
      levels: [{
        width: 5,
        height: 3,
        compressedBytes: concatBlocks([
          buildBc3Block(245, 20, bc3IdxAll8, c0Frac, c1Frac, ASYM_INDICES_A),
          buildBc3Block(50, 210, bc3IdxSpecial, cLow, cHigh, ASYM_INDICES_B),
        ]),
      }],
      minFilter: 'LINEAR',
      magFilter: 'LINEAR',
      viewport: [19, 13],
    }),
  );

  // WebGL2 允许在合法 4x4/8x4 level-0 下的子 mip 层出现 2x2、1x1、4x2、2x1 等小于 4x4 的物理块裁剪尺寸；
  // 将 4x4 块内越界纹素 (x>=w || y>=h) 故意填为对比色索引 1，有效区内用索引 0，严格检验裁剪与 LINEAR 边缘不串色。
  const crop2x2SentinelBlock = buildColorBlock(
    packRgb565(31, 0, 0),   // index 0: 纯红 (有效 2x2 区左上/右下)
    packRgb565(0, 63, 31),  // index 1: 高亮青色 (填在 x>=2 || y>=2 的非激活纹素作哨兵)
    [
      0, 0, 1, 1,
      0, 0, 1, 1,
      1, 1, 1, 1,
      1, 1, 1, 1,
    ],
  );
  const crop1x1SentinelBlock = buildColorBlock(
    packRgb565(0, 63, 0),   // index 0: 纯绿 (仅 (0,0) 有效)
    packRgb565(31, 0, 31),  // index 1: 洋红哨兵 (其余 15 个纹素)
    [
      0, 1, 1, 1,
      1, 1, 1, 1,
      1, 1, 1, 1,
      1, 1, 1, 1,
    ],
  );
  const subBlockMipChain = [
    { width: 4, height: 4, compressedBytes: smallBlockBc1 },
    { width: 2, height: 2, compressedBytes: crop2x2SentinelBlock },
    { width: 1, height: 1, compressedBytes: crop1x1SentinelBlock },
  ];
  cases.push(
    makeCase({
      id: 'bc1-small-subblock-mip1-2x2-nearest',
      category: 'small_dimensions',
      description: '4x4->2x2->1x1 链中 L1(2x2) 有效区裁剪 NEAREST（越界纹素填青色哨兵）',
      formatKey: 'BC1_RGBA',
      levels: subBlockMipChain,
      minFilter: 'NEAREST_MIPMAP_NEAREST',
      magFilter: 'NEAREST',
      viewport: [12, 12],
      useExplicitLod: true,
      lod: 1.0,
    }),
  );
  cases.push(
    makeCase({
      id: 'bc1-small-subblock-mip1-2x2-linear',
      category: 'small_dimensions',
      description: '4x4->2x2->1x1 链中 L1(2x2) LINEAR_MIPMAP_NEAREST（验证哨兵纹素不渗入 2x2 双线性边缘）',
      formatKey: 'BC1_RGBA',
      levels: subBlockMipChain,
      minFilter: 'LINEAR_MIPMAP_NEAREST',
      magFilter: 'LINEAR',
      viewport: [17, 17],
      useExplicitLod: true,
      lod: 1.0,
    }),
  );
  cases.push(
    makeCase({
      id: 'bc1-small-subblock-mip2-1x1-linear',
      category: 'small_dimensions',
      description: '4x4->2x2->1x1 链中 L2(1x1) LINEAR_MIPMAP_NEAREST（验证其余15个洋红哨兵纹素完全隔离）',
      formatKey: 'BC1_RGBA',
      levels: subBlockMipChain,
      minFilter: 'LINEAR_MIPMAP_NEAREST',
      magFilter: 'LINEAR',
      viewport: [11, 11],
      useExplicitLod: true,
      lod: 2.0,
    }),
  );
  const rectSubBlockMipChain = [
    {
      width: 8,
      height: 4,
      compressedBytes: concatBlocks([
        buildBc2Block(bc2AlphaRamp, c0Exact, c1Exact, [0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0]),
        buildBc2Block(bc2AlphaChecker, c0Exact, c1Exact, [1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1]),
      ]),
    },
    {
      width: 4,
      height: 2,
      compressedBytes: buildBc2Block(
        [15, 10, 5, 0, 0, 5, 10, 15, 15, 15, 15, 15, 15, 15, 15, 15],
        packRgb565(30, 58, 4),
        packRgb565(2, 6, 30),
        [0, 1, 0, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1],
      ),
    },
    {
      width: 2,
      height: 1,
      compressedBytes: buildBc2Block(
        [12, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15],
        packRgb565(4, 60, 28),
        packRgb565(31, 2, 2),
        [0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1],
      ),
    },
    {
      width: 1,
      height: 1,
      compressedBytes: buildBc2Block(
        [9, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15],
        packRgb565(25, 45, 15),
        packRgb565(0, 0, 0),
        new Array(16).fill(0),
      ),
    },
  ];
  cases.push(
    makeCase({
      id: 'bc2-small-subblock-mip1-4x2-linear',
      category: 'small_dimensions',
      description: 'BC2 8x4->4x2->2x1->1x1 链中 L1(4x2) 与 L2(2x1) 扁平子块裁剪 LINEAR 采样',
      formatKey: 'BC2_RGBA',
      levels: rectSubBlockMipChain,
      minFilter: 'LINEAR_MIPMAP_NEAREST',
      magFilter: 'LINEAR',
      viewport: [19, 11],
      useExplicitLod: true,
      lod: 2.0,
    }),
  );

  // 5. sRGB 压缩格式（WEBGL_compressed_texture_s3tc_srgb vs SRGB8_ALPHA8 PNG）
  const srgbBc1Blocks = concatBlocks([
    buildColorBlock(packRgb565(27, 49, 13), packRgb565(6, 15, 25), ASYM_INDICES_A),
    buildColorBlock(packRgb565(19, 28, 29), packRgb565(3, 9, 4), ASYM_INDICES_B),
  ]);
  cases.push(
    makeCase({
      id: 'bc1-srgb-4color-nearest',
      category: 'srgb',
      description: 'BC1_SRGB 端点与1/3插值色在 SRGB8_ALPHA8 FBO 下的 NEAREST 对照',
      formatKey: 'BC1_SRGB',
      levels: [{ width: 8, height: 4, compressedBytes: srgbBc1Blocks }],
    }),
  );
  cases.push(
    makeCase({
      id: 'bc1-srgb-4color-linear',
      category: 'srgb',
      description: 'BC1_SRGB 在线性空间双线性过滤后回写 SRGB8_ALPHA8 FBO 的 LINEAR 对照',
      formatKey: 'BC1_SRGB',
      levels: [{ width: 8, height: 4, compressedBytes: srgbBc1Blocks }],
      minFilter: 'LINEAR',
      magFilter: 'LINEAR',
      viewport: [33, 19],
    }),
  );
  cases.push(
    makeCase({
      id: 'bc1-srgb-alpha-transparent-nearest',
      category: 'srgb',
      description: 'BC1_SRGB_ALPHA 含 c0<=c1 透明索引 NEAREST 对照',
      formatKey: 'BC1_SRGB_ALPHA',
      levels: [{ width: 4, height: 4, compressedBytes: transparentTestBlock }],
    }),
  );
  cases.push(
    makeCase({
      id: 'bc1-srgb-alpha-transparent-linear',
      category: 'srgb',
      description: 'BC1_SRGB_ALPHA 含透明索引边界的线性光双线性过滤对照',
      formatKey: 'BC1_SRGB_ALPHA',
      levels: [{ width: 4, height: 4, compressedBytes: transparentTestBlock }],
      minFilter: 'LINEAR',
      magFilter: 'LINEAR',
      viewport: [25, 25],
    }),
  );
  cases.push(
    makeCase({
      id: 'bc2-srgb-alpha-nearest',
      category: 'srgb',
      description: 'BC2_SRGB_ALPHA 显式 4-bit alpha + sRGB 颜色块 NEAREST 对照',
      formatKey: 'BC2_SRGB_ALPHA',
      levels: [{
        width: 8,
        height: 4,
        compressedBytes: concatBlocks([
          buildBc2Block(bc2AlphaRamp, c0Frac, c1Frac, ASYM_INDICES_A),
          buildBc2Block(bc2AlphaChecker, cLow, cHigh, ASYM_INDICES_B),
        ]),
      }],
    }),
  );
  cases.push(
    makeCase({
      id: 'bc3-srgb-alpha-linear',
      category: 'srgb',
      description: 'BC3_SRGB_ALPHA 插值 alpha + sRGB 颜色块跨块 LINEAR 对照',
      formatKey: 'BC3_SRGB_ALPHA',
      levels: [{
        width: 8,
        height: 4,
        compressedBytes: concatBlocks([
          buildBc3Block(241, 29, bc3IdxAll8, c0Frac, c1Frac, ASYM_INDICES_A),
          buildBc3Block(37, 213, bc3IdxSpecial, cLow, cHigh, ASYM_INDICES_B),
        ]),
      }],
      minFilter: 'LINEAR',
      magFilter: 'LINEAR',
      viewport: [33, 19],
    }),
  );

  // 6. 自定义 mip 链与 mip 过渡边界（8x8 -> 4x4 -> 2x2 -> 1x1 故意异色）
  const customMipBc1Levels = [
    {
      width: 8,
      height: 8,
      compressedBytes: concatBlocks([
        // L0: 高饱和红/青棋盘
        buildColorBlock(packRgb565(31, 4, 4), packRgb565(2, 58, 29), ASYM_INDICES_A),
        buildColorBlock(packRgb565(28, 8, 2), packRgb565(4, 52, 27), ASYM_INDICES_B),
        buildColorBlock(packRgb565(2, 58, 29), packRgb565(31, 4, 4), ASYM_INDICES_B),
        buildColorBlock(packRgb565(4, 52, 27), packRgb565(28, 8, 2), ASYM_INDICES_A),
      ]),
    },
    {
      width: 4,
      height: 4,
      // L1: 纯绿/洋红（与 L0 下采样截然不同）
      compressedBytes: buildColorBlock(packRgb565(2, 61, 3), packRgb565(29, 3, 28), ASYM_INDICES_A),
    },
    {
      width: 2,
      height: 2,
      // L2: 深蓝/亮黄（2x2 子块）
      compressedBytes: buildColorBlock(packRgb565(3, 7, 30), packRgb565(30, 60, 2), [0, 1, 1, 0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3]),
    },
    {
      width: 1,
      height: 1,
      // L3: 1x1 含透明或特征橙色
      compressedBytes: buildColorBlock(packRgb565(30, 32, 3), packRgb565(10, 10, 10), new Array(16).fill(0)),
    },
  ];

  for (const lod of [0, 1, 2, 3]) {
    cases.push(
      makeCase({
        id: `bc1-mip-custom-nearest-lod${lod}`,
        category: 'mip_boundary',
        description: `BC1 4级异色自定义 mip 链 NEAREST_MIPMAP_NEAREST 精确采样 L${lod}`,
        formatKey: 'BC1_RGBA',
        levels: customMipBc1Levels,
        minFilter: 'NEAREST_MIPMAP_NEAREST',
        magFilter: 'NEAREST',
        viewport: [16, 16],
        useExplicitLod: true,
        lod,
      }),
    );
  }

  for (const [tag, lod] of [['0p5', 0.5], ['1p5', 1.5], ['2p5', 2.5]]) {
    cases.push(
      makeCase({
        id: `bc1-mip-custom-trilinear-lod${tag}`,
        category: 'mip_boundary',
        description: `BC1 4级异色自定义 mip 链 LINEAR_MIPMAP_LINEAR 跨级三线性边界 LOD=${lod}`,
        formatKey: 'BC1_RGBA',
        levels: customMipBc1Levels,
        minFilter: 'LINEAR_MIPMAP_LINEAR',
        magFilter: 'LINEAR',
        viewport: [23, 23],
        useExplicitLod: true,
        lod,
      }),
    );
  }

  // BC3 自定义 mip 链 + 隐式屏幕导数 dFdx/dFdy 缩小触发 mip 选择（不走 textureLod）
  const customMipBc3Levels = [
    {
      width: 8,
      height: 8,
      compressedBytes: concatBlocks([
        buildBc3Block(250, 40, bc3IdxAll8, packRgb565(30, 10, 6), packRgb565(6, 50, 26), ASYM_INDICES_A),
        buildBc3Block(220, 60, bc3IdxSpecial, packRgb565(26, 12, 8), packRgb565(8, 46, 24), ASYM_INDICES_B),
        buildBc3Block(60, 230, bc3IdxSpecial, packRgb565(6, 50, 26), packRgb565(30, 10, 6), ASYM_INDICES_B),
        buildBc3Block(40, 250, bc3IdxAll8, packRgb565(8, 46, 24), packRgb565(26, 12, 8), ASYM_INDICES_A),
      ]),
    },
    {
      width: 4,
      height: 4,
      compressedBytes: buildBc3Block(190, 25, bc3IdxAll8, packRgb565(4, 60, 8), packRgb565(28, 6, 28), ASYM_INDICES_A),
    },
    {
      width: 2,
      height: 2,
      compressedBytes: buildBc3Block(120, 240, bc3IdxSpecial, packRgb565(4, 8, 30), packRgb565(30, 58, 4), ASYM_INDICES_B),
    },
    {
      width: 1,
      height: 1,
      compressedBytes: buildBc3Block(180, 180, new Array(16).fill(0), packRgb565(29, 34, 5), packRgb565(12, 12, 12), new Array(16).fill(0)),
    },
  ];

  cases.push(
    makeCase({
      id: 'bc3-mip-custom-trilinear-lod0p5',
      category: 'mip_boundary',
      description: 'BC3 4级异色自定义 mip 链 LINEAR_MIPMAP_LINEAR LOD=0.5 颜色+alpha 三线性混合',
      formatKey: 'BC3_RGBA',
      levels: customMipBc3Levels,
      minFilter: 'LINEAR_MIPMAP_LINEAR',
      magFilter: 'LINEAR',
      viewport: [23, 23],
      useExplicitLod: true,
      lod: 0.5,
    }),
  );

  cases.push(
    makeCase({
      id: 'bc3-mip-custom-derivative-minification',
      category: 'mip_boundary',
      description: 'BC3 8x8 自定义 mip 链在 3x3 视口 + REPEAT [0,2]^2 下通过硬件导数隐式选层三线性采样',
      formatKey: 'BC3_RGBA',
      levels: customMipBc3Levels,
      minFilter: 'LINEAR_MIPMAP_LINEAR',
      magFilter: 'LINEAR',
      wrapS: 'REPEAT',
      wrapT: 'REPEAT',
      uvRect: [0, 0, 2, 2],
      viewport: [3, 3],
      useExplicitLod: false,
      lod: 0,
    }),
  );

  // 非2幂 mip 链（12x8 -> 6x4 -> 3x2 -> 1x1），测试 mip1(6x4) 与 mip2(3x2) 非4倍数子层在 WebGL2 下的实际支持与精度
  const npotMipLevels = [
    { width: 12, height: 8, compressedBytes: bytes12x8Bc3 },
    {
      width: 6,
      height: 4,
      compressedBytes: concatBlocks([
        buildBc3Block(235, 45, bc3IdxAll8, packRgb565(3, 59, 10), packRgb565(27, 8, 25), ASYM_INDICES_A),
        buildBc3Block(55, 225, bc3IdxSpecial, packRgb565(29, 55, 3), packRgb565(5, 12, 28), ASYM_INDICES_B),
      ]),
    },
    {
      width: 3,
      height: 2,
      compressedBytes: buildBc3Block(205, 30, bc3IdxAll8, packRgb565(28, 25, 4), packRgb565(6, 42, 29), ASYM_INDICES_A),
    },
    {
      width: 1,
      height: 1,
      compressedBytes: buildBc3Block(155, 155, new Array(16).fill(0), packRgb565(15, 50, 20), packRgb565(8, 8, 8), new Array(16).fill(0)),
    },
  ];
  cases.push(
    makeCase({
      id: 'bc3-mip-npot-12x8-lod1-6x4',
      category: 'mip_boundary',
      description: 'BC3 非2幂 mip 链 (12x8->6x4->3x2->1x1) 采样 L1 (6x4 非4倍数宽度)',
      formatKey: 'BC3_RGBA',
      levels: npotMipLevels,
      minFilter: 'LINEAR_MIPMAP_LINEAR',
      magFilter: 'LINEAR',
      viewport: [19, 13],
      useExplicitLod: true,
      lod: 1.0,
    }),
  );
  cases.push(
    makeCase({
      id: 'bc3-mip-npot-12x8-lod2-3x2',
      category: 'mip_boundary',
      description: 'BC3 非2幂 mip 链 (12x8->6x4->3x2->1x1) 采样 L2 (3x2 小尺寸子层)',
      formatKey: 'BC3_RGBA',
      levels: npotMipLevels,
      minFilter: 'LINEAR_MIPMAP_LINEAR',
      magFilter: 'LINEAR',
      viewport: [15, 11],
      useExplicitLod: true,
      lod: 2.0,
    }),
  );

  // 负向控制组（必须被检出显著像素差异，证明 Oracle 不会漏放典型错误解码）
  const negOpaqueBlackRgba = decodeS3tcLevel('BC1_RGBA', transparentTestBlock, 4, 4);
  for (let i = 0; i < negOpaqueBlackRgba.length; i += 4) {
    if (negOpaqueBlackRgba[i + 3] === 0) {
      negOpaqueBlackRgba[i + 3] = 255; // 反例：BC1 丢弃 alpha 后变不透明黑
    }
  }
  const negRgbBleedRgba = decodeS3tcLevel('BC1_RGBA', transparentTestBlock, 4, 4);
  for (let i = 0; i < negRgbBleedRgba.length; i += 4) {
    if (negRgbBleedRgba[i + 3] === 0) {
      negRgbBleedRgba[i] = 255;
      negRgbBleedRgba[i + 2] = 255; // alpha=0 但残留脏 RGB，在非预乘 LINEAR 下会向邻近像素渗色
    }
  }

  const negativeControls = [
    {
      id: 'neg-bc1-transparent-to-opaque-black',
      description: '负向断言：模拟将 BC1_RGBA 透明索引3解码为不透明黑 (0,0,0,255)，必须被检出 alpha 误差 255',
      baseCaseId: 'bc1-rgba-transparent-index-nearest',
      mode: 'custom_png_levels',
      candidatePngLevels: [encodeRgbaPng(4, 4, negOpaqueBlackRgba)],
      expectedMinDelta: 255,
    },
    {
      id: 'neg-bc1-transparent-dirty-rgb-linear-bleed',
      description: '负向断言：模拟 BC1_RGBA 透明纹素 A=0 但残留非零 RGB，在 LINEAR 过滤下必须被检出渗色误差',
      baseCaseId: 'bc1-rgba-transparent-index-linear',
      mode: 'custom_png_levels',
      candidatePngLevels: [encodeRgbaPng(4, 4, negRgbBleedRgba)],
      expectedMinDelta: 32,
    },
    {
      id: 'neg-dropped-custom-mip-auto-generate',
      description: '负向断言：模拟仅上传 base level L0 并调用 generateMipmap 替代源自定义 mip 链，在 LOD=1.0 必须检出大误差',
      baseCaseId: 'bc1-mip-custom-nearest-lod1',
      mode: 'base_level_only_generate_mipmap',
      candidatePngLevels: [
        encodeRgbaPng(
          8,
          8,
          decodeS3tcLevel('BC1_RGBA', customMipBc1Levels[0].compressedBytes, 8, 8),
        ),
      ],
      expectedMinDelta: 100,
    },
  ];

  return { cases, negativeControls };
}

function assertGreater(a, b) {
  if (!(a > b)) {
    throw new Error(`预期 uint16 c0(${a}) > c1(${b})`);
  }
}
