# OSGB 图片读取与材质兼容：行为规格

## 范围与证据

覆盖 OpenSceneGraph 图片读取路径与转换器自身的图片分支：内嵌/外置编码、通道映射、alpha 与 sampler 状态。

- 期望值来自本仓库夹具与公开 OpenSceneGraph API 的行为，不依赖任何第三方实现或私有模块。
- 编码透传、原始像素通道映射、GPU 压缩像素与 sampler 状态四类输入由独立夹具检验；未覆盖的组合仍标未知。
- 证据可复现：`native/tests/texture_oracle_cases.cpp` 生成夹具并输出 `cases.json` 与 `native-observed.json`。

## 输入、输出与检查

自行用公开 OSG API 生成最小三角形 OSGB，并独立记录预期像素和状态，再验证本仓库 native 的输出；不以“能显示”作为数据正确的判据。

| 输入边界 | native 的预期行为 |
| --- | --- |
| 原 JPEG/PNG 编码 | 原字节透传，不重编码；缺失或解码失败不当成功 |
| 原始8位 RGBA、含0/半透明alpha | 所有RGBA像素无损PNG，保留源混合状态；不转JPEG、不丢alpha |
| 奇数宽度RGB及非默认packing/row length | 跳过行填充，按实际行/像素步长读取，不把padding当颜色 |
| BGR/BGRA、灰度、灰度alpha | 按明确通道语义映射，逐像素断言；未支持的格式显式失败 |
| 纹理只有父节点StateSet | 支持的继承语义下保留纹理；不可只读取几何局部状态 |
| 显式关闭纹理 | 输出不应重新启用纹理或因未使用的坏图片失败 |
| 开启纹理但图片缺失或损坏 | 整个转换失败，不发布白图或缺贴图成功产物；清理暂存并允许修复重跑 |
| BC1/DXT1与其他GPU压缩格式 | 先用独立oracle验证读取、透明索引和尺寸。未建立等价输出前明确拒绝，不采用“透明变黑”或缩图等有损替代；拒绝不计为兼容完成 |
| 自定义mip链 | 必须确认是否保留；不能只验证base level便声称采样完全等价。无法等价输出时须明确报告，不静默丢弃 |

每项夹具至少记录：节点/几何是否读取成功、texture/image是否存在、尺寸、pixel format、data type、packing、row length、像素位数、缓冲长度和原始缓冲哈希。任何后处理结果单独记录，不与读取层行为混同。

## 实现与验收边界

夹具先于实现：先固定期望，再修改 native。测试代码只依赖公开 OpenSceneGraph API 与自建夹具。原模型的既有JPEG/PNG透传和几何/采样基线必须回归；App继续使用同一native和SDK，不另写解码逻辑。

每个触发条件都固化为回归用例，不依赖历史故障样本。

## BC1 / BC2 / BC3 独立解码候选

候选的格式、整数插值、sRGB、边缘块、源缓冲与 mip 合同由 `src/compressed_image.cpp` 与 `tests/compressed_image_cases.cpp` 固定。候选提供独立 API 和确定性向量供 GPU oracle 对照，不代表默认转换已兼容 GPU 压缩纹理。默认路径在取得独立等价证据前继续拒绝；不得以 CPU 自测通过或保留拒绝冒充兼容完成。

## 独立 oracle 夹具（native/tests/texture_oracle_cases.cpp）

上表的每条可测边界现在都有固定回归。判据不依赖 OSG 图片插件的输出，否则“插件先改错、再用同一错误当期望”会让缺陷永远通过。

### 反循环做法

1. **原始像素层（`writeGlb` 直调）**：作者按 GL 内存约定构造 `osg::Image`（format、packing、rowLength、origin 都是显式参数），期望画面由测试自己的通道映射逐像素推导。该层完全不经过 OSG 图片插件，因此插件对 alpha/方向的任何改动都不在链路上。行填充写 0x7B 哨兵，把 padding 当颜色会立刻失败。
2. **原编码层（`convert()` 文件级）**：源真相是容器字节本身，由测试用 zlib（PNG）与 turbojpeg（JPEG）自行生成并补齐到精确长度，再逐字节写进 `.osgb` 的内嵌图片槽位（模板为仓库内 `data/test/test.osgb` 的 ContextCapture 瓦片）或作为外部引用文件。断言输出图片与源容器**逐字节相同**，并用独立解码核对画面。
3. **方向**：期望按 GL 约定从作者图片采样（`t=0 → 内存行 0 → 图片底部`），实际按 GLB 自己声明的采样器与 `KHR_texture_transform` 采样，逐像素比较。翻转标志是否自洽由像素数据判定，不依据 `osg::Image::getOrigin()` 的名字。
4. **oracle 反向自检**：测试先故意造四种错误（行序未翻转/多余翻转、全透明像素被清零、alpha 预乘、红蓝互换），要求比较逻辑必须发现；否则 harness 自身失效会立刻暴露。每个夹具还要求画面两轴都不对称，避免对称内容让方向断言空转。

### 覆盖的夹具

| 组 | 夹具 | 断言 |
| --- | --- | --- |
| 原始像素 | `rgba-alpha-extremes`、`rgba-3x5`、`rgba-origin-top-left`、`bgr`、`bgra`、`rgb-odd-width`、`rgb-packing4-odd`、`rgb-row-length`、`luminance`、`luminance-alpha`、`alpha-only`（第一层直调 `writeGlb`），以及 `rgba-alpha-extremes`、`bgra`、`luminance-alpha`、`rgb-odd-width` 经 `convert()` | 逐像素渲染等价、alpha 精确保留、输出必须是 PNG |
| 原编码 | `embedded-png`、`embedded-jpeg`（`readImage(stream)` 路径）、`external-png`、`external-jpeg`、`external-relative-name`（`readImage(name)` 路径） | 输出字节与源容器逐字节相同、mime 一致、审计为 `originalEncoded` |
| 状态 | `parent-state-texture`、`disabled-texture`、`disabled-broken-external` | 父节点 StateSet 继承有效；关闭纹理不重新启用、不因未使用的坏图片失败 |
| 失败 | `external-missing`、`external-corrupt`、`embedded-corrupt` | 整个转换失败、不发布输出、错误标出源 OSGB |
| 明确拒绝 | `custom-mip-chain`、`dxt1-compressed`、`rgb-float-pixels` | 必须显式报错并说明原因；**拒绝计入“未支持”，不计为兼容完成** |
| 未支持容器 | `external-bmp-container` | 非 PNG/JPEG 编码容器明确拒绝；不能伪装成原始像素后静默换成 PNG，不计为格式兼容 |

### 本轮实测结论（macOS，OSG 3.6.5 + ImageIO 插件）

- OSG 的 png/jpg 插件在本机是 **ImageIO**：读取时对预乘解码再做反预乘，**alpha=0 像素的 RGB 会变成 0 且无法还原**；写入时反预乘会放大低 alpha 像素（如 `(40,50,60,128)` 写成 `(80,100,120,128)`），并有 ±1 的色彩空间往返。所以凡涉及像素值的断言都必须走第 1 层，凡涉及原字节的断言只比较字节。这也说明“用插件解码结果当期望”会掩盖真实损失。
- `.osgb`（`WriteImageHint=IncludeData`）在本机构建下保存的是**未编码原始像素**（可逐字节在文件里检出，夹具已自检）；ContextCapture 生成的瓦片则内嵌 JPEG。两条路径分别对应捕获包装器的 `readImage(name)` 与 `readImage(stream)` 重载。
- `.osgb` 序列化会按紧凑行写出像素但仍记录 `packing`，往返后行步长与文档长度不一致（`packing>1` 的图像会被 OSG 侧读错）。因此 packing/rowLength 只在第 1 层直调 `writeGlb` 验证，文件层用 `packing=1` 的奇数宽度。
- `osg::Image::origin` 只是存储顺序元数据：OSG 采样始终是 `t=0 → 内存行 0`，因此同样内存字节带 `TOP_LEFT` 标记必须渲染成同一画面。测试用 `rgba-origin-top-left` 固定这一点。
- 生产实现当前对原始像素固定 `flipV=false`（PNG 行序等于内存行序）、对原编码按 `origin==BOTTOM_LEFT` 决定翻转，两者在像素级 oracle 下都成立。
- 修复前 BMP 外部贴图会静默重编码成 PNG。主线已通过该反例修复：捕获插件成功读取的所有编码流，未知容器保留“已编码但不支持”的标记，在纹理写出前明确失败；`--texture-pixels` 也不能绕过。真正未编码的原始像素仍走原有无损 PNG 路径。严格回归现为32项符合预期（含BMP拒绝）、3项明确未支持、0项已知失败；不将拒绝输入计作兼容完成。

### 运行方式

```bash
cmake --build build -j2
ctest --test-dir build -R native-texture-oracle --output-on-failure
./build/osgb-texture-oracle-tests --out /tmp/oracle   # 保留夹具与 observed.json 取证
./build/osgb-texture-oracle-tests --strict            # 存在已知失败时退出码 2，用于跟踪修复
```

默认把夹具写到系统临时目录，并输出 `cases.json`（期望）、`native-observed.json`（观测）。拒绝项逐条打印原因；CTest固定以 `--strict` 执行，任何重新出现的已知失败都会使CI失败。turbojpeg是构建期测试依赖，缺少时会明确告警并跳过该组，不得将跳过写成通过。
