// 浏览器端 WebGL2 真实 GPU S3TC 渲染对照执行器（由独立本地测试页加载）。
function base64ToUint8Array(b64) {
  const binary = atob(b64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) {
    bytes[i] = binary.charCodeAt(i);
  }
  return bytes;
}

function glErrorName(gl, code) {
  const map = {
    [gl.NO_ERROR]: 'NO_ERROR',
    [gl.INVALID_ENUM]: 'INVALID_ENUM',
    [gl.INVALID_VALUE]: 'INVALID_VALUE',
    [gl.INVALID_OPERATION]: 'INVALID_OPERATION',
    [gl.INVALID_FRAMEBUFFER_OPERATION]: 'INVALID_FRAMEBUFFER_OPERATION',
    [gl.OUT_OF_MEMORY]: 'OUT_OF_MEMORY',
  };
  return map[code] || `GL_ERROR_0x${code.toString(16)}`;
}

function clearGlErrors(gl) {
  let guard = 0;
  while (gl.getError() !== gl.NO_ERROR && ++guard < 32) {
    // drain prior errors
  }
}

function compareFrames(gpuBytes, candBytes, width, height) {
  const totalPixels = width * height;
  let diffPixels = 0;
  let rgbDiffPixels = 0;
  let alphaDiffPixels = 0;
  const maxAbsDiffRgba = [0, 0, 0, 0];
  let sumAbs = 0;
  let sumSq = 0;
  const deltaHistogram = { d0: 0, d1: 0, d2: 0, d3_4: 0, d5_plus: 0 };
  let firstMismatch = null;

  for (let p = 0; p < totalPixels; p++) {
    const o = p * 4;
    const dr = Math.abs(gpuBytes[o] - candBytes[o]);
    const dg = Math.abs(gpuBytes[o + 1] - candBytes[o + 1]);
    const db = Math.abs(gpuBytes[o + 2] - candBytes[o + 2]);
    const da = Math.abs(gpuBytes[o + 3] - candBytes[o + 3]);
    if (dr > maxAbsDiffRgba[0]) maxAbsDiffRgba[0] = dr;
    if (dg > maxAbsDiffRgba[1]) maxAbsDiffRgba[1] = dg;
    if (db > maxAbsDiffRgba[2]) maxAbsDiffRgba[2] = db;
    if (da > maxAbsDiffRgba[3]) maxAbsDiffRgba[3] = da;

    const rgbMax = Math.max(dr, dg, db);
    const pixMax = Math.max(rgbMax, da);
    if (rgbMax > 0) rgbDiffPixels++;
    if (da > 0) alphaDiffPixels++;
    if (pixMax > 0) {
      diffPixels++;
      if (!firstMismatch) {
        firstMismatch = {
          x: p % width,
          y: Math.floor(p / width),
          gpuRgba: [gpuBytes[o], gpuBytes[o + 1], gpuBytes[o + 2], gpuBytes[o + 3]],
          candidateRgba: [candBytes[o], candBytes[o + 1], candBytes[o + 2], candBytes[o + 3]],
          absDiff: [dr, dg, db, da],
        };
      }
    }

    if (pixMax === 0) deltaHistogram.d0++;
    else if (pixMax === 1) deltaHistogram.d1++;
    else if (pixMax === 2) deltaHistogram.d2++;
    else if (pixMax <= 4) deltaHistogram.d3_4++;
    else deltaHistogram.d5_plus++;

    sumAbs += dr + dg + db + da;
    sumSq += dr * dr + dg * dg + db * db + da * da;
  }

  const maxChannelDelta = Math.max(...maxAbsDiffRgba);
  const rgbMaxDelta = Math.max(maxAbsDiffRgba[0], maxAbsDiffRgba[1], maxAbsDiffRgba[2]);
  const alphaMaxDelta = maxAbsDiffRgba[3];
  const mse = sumSq / (totalPixels * 4);
  const meanAbsError = sumAbs / (totalPixels * 4);
  const psnrDb = mse === 0 ? null : 10 * Math.log10((255 * 255) / mse);

  return {
    totalPixels,
    diffPixels,
    rgbDiffPixels,
    alphaDiffPixels,
    diffRatio: diffPixels / totalPixels,
    maxAbsDiffRgba,
    maxChannelDelta,
    rgbMaxDelta,
    alphaMaxDelta,
    meanAbsError,
    mse,
    psnrDb,
    deltaHistogram,
    firstMismatch,
  };
}

async function decodePngBitmap(b64, expectedWidth, expectedHeight) {
  const bytes = base64ToUint8Array(b64);
  const blob = new Blob([bytes], { type: 'image/png' });
  const bitmap = await createImageBitmap(blob, {
    premultiplyAlpha: 'none',
    colorSpaceConversion: 'none',
    imageOrientation: 'none',
  });
  if (bitmap.width !== expectedWidth || bitmap.height !== expectedHeight) {
    const actual = `${bitmap.width}x${bitmap.height}`;
    bitmap.close();
    throw new Error(`候选 PNG 解码尺寸 ${actual} 不匹配预期 ${expectedWidth}x${expectedHeight}`);
  }
  return bitmap;
}

window.runBcGpuOracle = async function runBcGpuOracle() {
  const response = await fetch('/payload.json', { cache: 'no-store' });
  if (!response.ok) {
    throw new Error(`拉取测试载荷失败: HTTP ${response.status}`);
  }
  const payload = await response.json();
  const canvas = document.createElement('canvas');
  canvas.width = 64;
  canvas.height = 64;
  const gl = canvas.getContext('webgl2', {
    antialias: false,
    depth: false,
    stencil: false,
    alpha: true,
    premultipliedAlpha: false,
    preserveDrawingBuffer: true,
    powerPreference: 'high-performance',
  });
  if (!gl) {
    throw new Error('当前浏览器环境不支持 WebGL2');
  }

  gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
  gl.pixelStorei(gl.PACK_ALIGNMENT, 1);
  gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
  gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
  gl.pixelStorei(gl.UNPACK_COLORSPACE_CONVERSION_WEBGL, gl.NONE);
  gl.disable(gl.BLEND);
  gl.disable(gl.DITHER);
  gl.disable(gl.DEPTH_TEST);
  gl.disable(gl.CULL_FACE);

  const simulatedBlocked = new Set(payload.simulateMissingExtensions || []);
  const extS3tc = simulatedBlocked.has('WEBGL_compressed_texture_s3tc')
    ? null
    : gl.getExtension('WEBGL_compressed_texture_s3tc');
  const extS3tcSrgb = simulatedBlocked.has('WEBGL_compressed_texture_s3tc_srgb')
    ? null
    : gl.getExtension('WEBGL_compressed_texture_s3tc_srgb');
  const debugExt = gl.getExtension('WEBGL_debug_renderer_info');

  const rawCompressedFormats = Array.from(gl.getParameter(gl.COMPRESSED_TEXTURE_FORMATS) || []);
  const activeCompressedFormats = new Set(
    rawCompressedFormats.filter((code) => {
      if (simulatedBlocked.has('WEBGL_compressed_texture_s3tc') && code >= 0x83f0 && code <= 0x83f3) {
        return false;
      }
      if (simulatedBlocked.has('WEBGL_compressed_texture_s3tc_srgb') && code >= 0x8c4c && code <= 0x8c4f) {
        return false;
      }
      return true;
    }),
  );

  const environment = {
    userAgent: navigator.userAgent,
    webglVersion: gl.getParameter(gl.VERSION),
    shadingLanguageVersion: gl.getParameter(gl.SHADING_LANGUAGE_VERSION),
    vendor: gl.getParameter(gl.VENDOR),
    renderer: gl.getParameter(gl.RENDERER),
    unmaskedVendor: debugExt ? gl.getParameter(debugExt.UNMASKED_VENDOR_WEBGL) : null,
    unmaskedRenderer: debugExt ? gl.getParameter(debugExt.UNMASKED_RENDERER_WEBGL) : null,
    extensions: {
      WEBGL_compressed_texture_s3tc: Boolean(extS3tc),
      WEBGL_compressed_texture_s3tc_srgb: Boolean(extS3tcSrgb),
    },
    simulatedMissingExtensions: [...simulatedBlocked],
    compressedTextureFormatsHex: [...activeCompressedFormats]
      .sort((a, b) => a - b)
      .map((n) => '0x' + n.toString(16)),
  };

  // 编译全屏三角形着色器（gl_FragCoord 中心精确映射至 uvRect）
  const compile = (type, src) => {
    const sh = gl.createShader(type);
    gl.shaderSource(sh, src);
    gl.compileShader(sh);
    if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
      const log = gl.getShaderInfoLog(sh);
      gl.deleteShader(sh);
      throw new Error(`Shader 编译失败: ${log}`);
    }
    return sh;
  };

  const vs = compile(
    gl.VERTEX_SHADER,
    `#version 300 es
precision highp float;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}`,
  );
  const fs = compile(
    gl.FRAGMENT_SHADER,
    `#version 300 es
precision highp float;
uniform sampler2D uTex;
uniform vec2 uViewport;
uniform vec4 uUvRect;
uniform int uUseExplicitLod;
uniform float uLod;
out vec4 outColor;
void main() {
  vec2 t = gl_FragCoord.xy / uViewport;
  vec2 uv = mix(uUvRect.xy, uUvRect.zw, t);
  if (uUseExplicitLod == 1) {
    outColor = textureLod(uTex, uv, uLod);
  } else {
    outColor = texture(uTex, uv);
  }
}`,
  );
  const program = gl.createProgram();
  gl.attachShader(program, vs);
  gl.attachShader(program, fs);
  gl.linkProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    throw new Error(`Program 链接失败: ${gl.getProgramInfoLog(program)}`);
  }
  gl.useProgram(program);
  const vao = gl.createVertexArray();
  gl.bindVertexArray(vao);

  const locTex = gl.getUniformLocation(program, 'uTex');
  const locViewport = gl.getUniformLocation(program, 'uViewport');
  const locUvRect = gl.getUniformLocation(program, 'uUvRect');
  const locUseLod = gl.getUniformLocation(program, 'uUseExplicitLod');
  const locLod = gl.getUniformLocation(program, 'uLod');
  gl.uniform1i(locTex, 0);

  function renderTextureToRgba(texture, item) {
    const vw = item.viewportWidth;
    const vh = item.viewportHeight;
    const fboTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, fboTex);
    const fboInternal = item.fboEncoding === 'srgb8_alpha8' ? gl.SRGB8_ALPHA8 : gl.RGBA8;
    gl.texImage2D(gl.TEXTURE_2D, 0, fboInternal, vw, vh, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);

    const fbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, fboTex, 0);
    const fbStatus = gl.checkFramebufferStatus(gl.FRAMEBUFFER);
    if (fbStatus !== gl.FRAMEBUFFER_COMPLETE) {
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.deleteFramebuffer(fbo);
      gl.deleteTexture(fboTex);
      throw new Error(`FBO 不完整 (${item.fboEncoding}): 0x${fbStatus.toString(16)}`);
    }

    gl.viewport(0, 0, vw, vh);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);

    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.uniform2f(locViewport, vw, vh);
    gl.uniform4f(locUvRect, item.uvRect[0], item.uvRect[1], item.uvRect[2], item.uvRect[3]);
    gl.uniform1i(locUseLod, item.useExplicitLod ? 1 : 0);
    gl.uniform1f(locLod, item.lod);
    gl.drawArrays(gl.TRIANGLES, 0, 3);

    const out = new Uint8Array(vw * vh * 4);
    gl.readPixels(0, 0, vw, vh, gl.RGBA, gl.UNSIGNED_BYTE, out);
    const err = gl.getError();

    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.deleteFramebuffer(fbo);
    gl.deleteTexture(fboTex);
    if (err !== gl.NO_ERROR) {
      throw new Error(`渲染或 readPixels GL 错误: ${glErrorName(gl, err)}`);
    }
    return out;
  }

  function configureSampler(tex, item, maxLevel) {
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_BASE_LEVEL, 0);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAX_LEVEL, maxLevel);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, item.minFilterEnum);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, item.magFilterEnum);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, item.wrapSEnum);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, item.wrapTEnum);
  }

  function uploadCompressedSourceTexture(item) {
    const extSupported = Boolean(environment.extensions[item.requiredExtension]);
    if (!extSupported || !activeCompressedFormats.has(item.internalFormat)) {
      return {
        skipped: true,
        skipReason: `missing_extension_or_enum:${item.requiredExtension}:${item.internalFormatHex}`,
      };
    }
    clearGlErrors(gl);
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    for (const lv of item.levels) {
      const raw = base64ToUint8Array(lv.compressedBase64);
      gl.compressedTexImage2D(
        gl.TEXTURE_2D,
        lv.level,
        item.internalFormat,
        lv.width,
        lv.height,
        0,
        raw,
      );
      const err = gl.getError();
      if (err !== gl.NO_ERROR) {
        gl.deleteTexture(tex);
        return {
          skipped: true,
          skipReason: `webgl_compressedTexImage2D_rejected:level=${lv.level}:${lv.width}x${lv.height}:${glErrorName(gl, err)}`,
        };
      }
    }
    configureSampler(tex, item, item.levels.length - 1);
    const paramErr = gl.getError();
    if (paramErr !== gl.NO_ERROR) {
      gl.deleteTexture(tex);
      return {
        skipped: true,
        skipReason: `webgl_sampler_rejected:${glErrorName(gl, paramErr)}`,
      };
    }
    return { skipped: false, texture: tex };
  }

  async function uploadCandidatePngTexture(item, candidatePngBase64Levels, generateMipmapFromBase = false) {
    clearGlErrors(gl);
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    const internalFormat = item.srgb ? gl.SRGB8_ALPHA8 : gl.RGBA8;

    if (generateMipmapFromBase) {
      const lv0 = item.levels[0];
      const bmp = await decodePngBitmap(candidatePngBase64Levels[0], lv0.width, lv0.height);
      try {
        gl.texImage2D(gl.TEXTURE_2D, 0, internalFormat, lv0.width, lv0.height, 0, gl.RGBA, gl.UNSIGNED_BYTE, bmp);
      } finally {
        bmp.close();
      }
      gl.generateMipmap(gl.TEXTURE_2D);
    } else {
      if (candidatePngBase64Levels.length !== item.levels.length) {
        gl.deleteTexture(tex);
        throw new Error(
          `候选 mip 层数 (${candidatePngBase64Levels.length}) 与源压缩纹理层数 (${item.levels.length}) 不一致`,
        );
      }
      for (let i = 0; i < item.levels.length; i++) {
        const lv = item.levels[i];
        const bmp = await decodePngBitmap(candidatePngBase64Levels[i], lv.width, lv.height);
        try {
          gl.texImage2D(
            gl.TEXTURE_2D,
            lv.level,
            internalFormat,
            lv.width,
            lv.height,
            0,
            gl.RGBA,
            gl.UNSIGNED_BYTE,
            bmp,
          );
        } finally {
          bmp.close();
        }
      }
    }
    configureSampler(tex, item, item.levels.length - 1);
    const err = gl.getError();
    if (err !== gl.NO_ERROR) {
      gl.deleteTexture(tex);
      throw new Error(`候选 PNG 纹理上传 GL 错误: ${glErrorName(gl, err)}`);
    }
    return tex;
  }

  let gpuRepeatZeroDiff = true;
  const caseMap = new Map();
  const results = [];

  for (const item of payload.cases) {
    caseMap.set(item.id, item);
    const src = uploadCompressedSourceTexture(item);
    if (src.skipped) {
      results.push({
        id: item.id,
        category: item.category,
        description: item.description,
        formatKey: item.formatKey,
        formatName: item.formatName,
        internalFormatHex: item.internalFormatHex,
        srgb: item.srgb,
        fboEncoding: item.fboEncoding,
        width: item.width,
        height: item.height,
        mipCount: item.mipCount,
        minFilter: item.minFilter,
        magFilter: item.magFilter,
        wrapS: item.wrapS,
        wrapT: item.wrapT,
        lod: item.lod,
        useExplicitLod: item.useExplicitLod,
        viewport: [item.viewportWidth, item.viewportHeight],
        inputSha256: item.inputSha256,
        candidatePngSha256: item.candidatePngSha256,
        status: 'skipped',
        skipReason: src.skipReason,
      });
      continue;
    }

    if (item.candidateError) {
      gl.deleteTexture(src.texture);
      results.push({
        id: item.id,
        category: item.category,
        description: item.description,
        formatKey: item.formatKey,
        formatName: item.formatName,
        internalFormatHex: item.internalFormatHex,
        srgb: item.srgb,
        fboEncoding: item.fboEncoding,
        width: item.width,
        height: item.height,
        mipCount: item.mipCount,
        minFilter: item.minFilter,
        magFilter: item.magFilter,
        wrapS: item.wrapS,
        wrapT: item.wrapT,
        lod: item.lod,
        useExplicitLod: item.useExplicitLod,
        viewport: [item.viewportWidth, item.viewportHeight],
        inputSha256: item.inputSha256,
        candidatePngSha256: null,
        status: 'failed',
        failureReason: `candidate_missing_or_rejected:${item.candidateError}`,
      });
      continue;
    }

    let candTex = null;
    try {
      const gpuA = renderTextureToRgba(src.texture, item);
      const gpuB = renderTextureToRgba(src.texture, item);
      const repeatDiff = compareFrames(gpuA, gpuB, item.viewportWidth, item.viewportHeight);
      if (repeatDiff.maxChannelDelta !== 0) {
        gpuRepeatZeroDiff = false;
      }

      candTex = await uploadCandidatePngTexture(
        item,
        item.levels.map((lv) => lv.candidatePngBase64),
        false,
      );
      const candPixels = renderTextureToRgba(candTex, item);
      const metrics = compareFrames(gpuA, candPixels, item.viewportWidth, item.viewportHeight);
      const passed = metrics.maxChannelDelta <= payload.maxChannelDelta;

      results.push({
        id: item.id,
        category: item.category,
        description: item.description,
        formatKey: item.formatKey,
        formatName: item.formatName,
        internalFormatHex: item.internalFormatHex,
        srgb: item.srgb,
        fboEncoding: item.fboEncoding,
        width: item.width,
        height: item.height,
        mipCount: item.mipCount,
        minFilter: item.minFilter,
        magFilter: item.magFilter,
        wrapS: item.wrapS,
        wrapT: item.wrapT,
        lod: item.lod,
        useExplicitLod: item.useExplicitLod,
        viewport: [item.viewportWidth, item.viewportHeight],
        inputSha256: item.inputSha256,
        candidatePngSha256: item.candidatePngSha256,
        status: passed ? 'passed' : 'failed',
        failureReason: passed
          ? null
          : `maxChannelDelta=${metrics.maxChannelDelta} exceeds tolerance=${payload.maxChannelDelta}`,
        metrics,
      });
    } catch (err) {
      results.push({
        id: item.id,
        category: item.category,
        description: item.description,
        formatKey: item.formatKey,
        formatName: item.formatName,
        internalFormatHex: item.internalFormatHex,
        srgb: item.srgb,
        fboEncoding: item.fboEncoding,
        width: item.width,
        height: item.height,
        mipCount: item.mipCount,
        minFilter: item.minFilter,
        magFilter: item.magFilter,
        wrapS: item.wrapS,
        wrapT: item.wrapT,
        lod: item.lod,
        useExplicitLod: item.useExplicitLod,
        viewport: [item.viewportWidth, item.viewportHeight],
        inputSha256: item.inputSha256,
        candidatePngSha256: item.candidatePngSha256,
        status: 'failed',
        failureReason: String(err?.message || err),
      });
    } finally {
      if (candTex) gl.deleteTexture(candTex);
      gl.deleteTexture(src.texture);
    }
  }

  // 负向控制组执行
  const negativeControlResults = [];
  for (const neg of payload.negativeControls) {
    const baseItem = caseMap.get(neg.baseCaseId);
    if (!baseItem) {
      throw new Error(`负向控制组引用不存在的用例: ${neg.baseCaseId}`);
    }
    const src = uploadCompressedSourceTexture(baseItem);
    if (src.skipped) {
      negativeControlResults.push({
        id: neg.id,
        description: neg.description,
        baseCaseId: neg.baseCaseId,
        status: 'skipped',
        skipReason: src.skipReason,
        detectedFailure: null,
      });
      continue;
    }
    let negTex = null;
    try {
      const gpuPixels = renderTextureToRgba(src.texture, baseItem);
      negTex = await uploadCandidatePngTexture(
        baseItem,
        neg.candidatePngBase64Levels,
        neg.mode === 'base_level_only_generate_mipmap',
      );
      const negPixels = renderTextureToRgba(negTex, baseItem);
      const metrics = compareFrames(gpuPixels, negPixels, baseItem.viewportWidth, baseItem.viewportHeight);
      const detectedFailure = metrics.maxChannelDelta >= neg.expectedMinDelta;
      negativeControlResults.push({
        id: neg.id,
        description: neg.description,
        baseCaseId: neg.baseCaseId,
        status: detectedFailure ? 'caught' : 'missed',
        detectedFailure,
        expectedMinDelta: neg.expectedMinDelta,
        observedMaxChannelDelta: metrics.maxChannelDelta,
        observedAlphaMaxDelta: metrics.alphaMaxDelta,
        diffPixels: metrics.diffPixels,
      });
    } finally {
      if (negTex) gl.deleteTexture(negTex);
      gl.deleteTexture(src.texture);
    }
  }

  gl.deleteVertexArray(vao);
  gl.deleteProgram(program);
  gl.deleteShader(vs);
  gl.deleteShader(fs);
  gl.getExtension('WEBGL_lose_context')?.loseContext();

  return {
    environment,
    gpuRepeatZeroDiff,
    cases: results,
    negativeControls: negativeControlResults,
  };
};
window.oraclePageReady = true;
