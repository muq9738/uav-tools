# OSGB → 3D Tiles

**将分块 OpenSceneGraph 倾斜摄影模型转换为可流式加载的 3D Tiles 1.1，并在同一应用内直接检查转换结果。**

[English](README.md) · [简体中文](README.zh-CN.md)

[![License: MIT](https://img.shields.io/badge/license-MIT-27845f)](LICENSE)
[![3D Tiles: 1.1](https://img.shields.io/badge/3D%20Tiles-1.1-3565d4)](native/SPEC.md)
[![Cesium: 1.136](https://img.shields.io/badge/Cesium-1.136-6c5ce7)](app/web/vendor/cesium/SOURCE.json)

一套包含 **C++17 原生转换器**、**Go 桌面与 HTTP 应用**、**离线 Cesium 查看器**的工具链。转换运行时不依赖 Node.js、云服务、浏览器扩展或独立浏览器进程。

## 核心特性

### 默认优先保真

- 还原因 `PagedLOD` 与外部引用形成的可达层级，不丢弃最细层内容。
- 默认保留顶点、UV、法线、颜色、三角形朝向，以及 JPEG / PNG 原始编码字节。
- 默认使用无量化 Meshopt 压缩，不简化几何，也不量化数值。
- SRS 投影与局部居中先使用双精度计算，最后才写入 glTF `FLOAT`。
- 发布前逐项校验引用资产、buffer 范围、扩展声明与包围体，检查失败不发布完成产物。

### 全域正射底图

每次转换都会在 `tileset.json` 同目录生成 `orthophoto.tif`。转换器将最细层三角面投影到 0.05 米的像素网格，格网北朝上、东朝右，空白区域透明。整图采用分带写入和 Deflate 无损压缩，大图自动使用 BigTIFF。

有地理参考的模型使用 GeoTIFF 1.1。文件内嵌像素外边界定位（PixelIsArea）及模型中心所在的 WGS 84 UTM 投影，极区使用 UPS。`LOCAL` 模型保留模型局部米制坐标。`conversion-report.json` 的 `orthophoto` 字段包含 `uri`、`width`、`height`、`metersPerPixel`、`crs` 和左上像素的外边界坐标 `upperLeftEdge`。将框选点换算到 `crs` 后，设点坐标为 `(x, y)`，左上外边界为 `(west, north)`，则零基像素列、行为 `floor((x-west)/0.05)`、`floor((north-y)/0.05)`。

### 按需启用优化

Draco 顶点量化、KTX2 UASTC / ETC1S 纹理与远景父层重建均为**显式可选项**，可以自由组合。转换报告会分别记录有损参数、实际编码结果与测得误差，不将有损产物标成保真。

### 真正融入工作流的预览

成功任务可直接在 App 现有的系统 WebView 中打开预览。内嵌 Cesium 1.136 加载的是本次真实产物，不是另一份替代渲染；提供模型中心导航、常用视角、画质选项与实际瓦片统计。

- 左键围绕模型表面旋转，右键或中键平移，滚轮朝视口中心缩放。
- 透视、俯视、正视一键切换，也可随时恢复整体视角。
- 直接选择“流畅 / 均衡 / 精细”，无需理解底层图形参数。
- 高 DPI、4×MSAA、FXAA、4×各向异性过滤，以及最高 30fps 的按需渲染。
- Cesium 资源完全离线；关闭预览即释放 WebGL、Worker 与瓦片缓存。

### 面向真实转换任务

- 文件级并发与可选内存预算调度。
- 合作取消、结构化进度事件与持久化任务历史。
- 内容寻址检查点，支持跨进程暂停和真正续转。
- CLI 与 App 共用同一份随包原生二进制，保持默认参数和输出一致。
- HTTP 默认只监听本机；对外监听需显式配置地址和访问令牌。

## 快速开始

### 在 macOS 构建桌面 App

需要 C++17、CMake、Ninja、Python 3、Go 及下列依赖。构建脚本还会按需下载固定版本的 PROJ、Draco 与 Basis Universal 源码。

```bash
brew install openscenegraph libtiff libpng libxml2 sqlite ninja
python3 app/scripts/build.py --sdk-prefix "$(brew --prefix)"
open "app/dist/darwin-arm64/OSGB 转 3D Tiles.app"
```

脚本会编译转换器、运行原生与 Go 测试、收集运行时和第三方许可、审核依赖闭包，并在 `app/dist/darwin-arm64/` 生成 `.app` 与 DMG。

### 使用命令行转换

仅构建转换器开发版本：

```bash
cmake -S native -B native/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build native/build -j 4

./native/build/osgb-tiles /path/to/osgb /path/to/new-tiles \
  --threads 4 \
  --geometry meshopt \
  --texture-format original
```

`INPUT` 可以是 OSGB 目录或索引 `.osgb` 文件。索引引用的同级分块必须一起保留。输出目录必须不存在或为空。

如果 `metadata.xml` 未提供可用的空间参考，需要显式指定：

```bash
./native/build/osgb-tiles /path/to/root.osgb /path/to/new-tiles \
  --srs 'EPSG:32651' \
  --origin '224107.81419846753,3366639.462932359,-66.472'
```

也支持 ENU 写法：`--srs 'ENU:纬度,经度[,高度]'`。

### 以本地 Web 服务运行 App

```bash
./osgb-tiles-app --headless --addr 127.0.0.1:8990
```

打开终端输出的本机地址，即可创建任务、查看进度与日志、暂停或继续支持的转换，并预览成功产物。非回环地址监听时还必须设置 `--token` 与 `--data-dir`。

## 输出结构

```text
new-tiles/
├── tileset.json             # 3D Tiles 1.1 入口
├── content/*.glb            # 几何与材质内容
├── textures/*               # 原图或 KTX2 纹理
├── tiles/*.json             # 逐源文件诊断清单
├── conversion-report.json   # 源映射、编码、误差与统计报告
└── preview/                 # 可选，仅 --preview 生成
    ├── tileset.json         # 浅层轻量入口
    ├── content/*.glb        # QEM 简化、Meshopt、无 NORMAL
    ├── textures/*.ktx2      # 图集化高质量 UASTC，完整 mip 链
    └── preview-report.json  # 三角面、图集页、字节、耗时与警告
```

在兼容客户端中加载 `tileset.json`。Meshopt 产物要求支持 `EXT_meshopt_compression`；Draco 与 KTX2 变体需要相应解码器。内嵌查看器支持这三种产物。

App 为成功任务提供两个入口：**保真预览**（`tileset.json`，Cesium）和**轻量预览**（`preview/tileset.json`，Three.js + `MeshoptDecoder`/`KTX2Loader`，关闭拾取、默认原分辨率纹理）。轻量入口不会请求保真 tileset。

## 预览操作

| 操作 | 效果 |
| --- | --- |
| 左键拖动 | 围绕拾取到的模型表面中心旋转 |
| 右键 / 中键拖动 | 平移 |
| 滚轮 / 双指缩放 | 缩放模型 |
| 透视 / 俯视 / 正视 | 切换相机预设 |
| **H** 或适应按钮 | 恢复透视整体视角 |
| **2** | 俯视图 |
| **F** | 切换全屏 |
| **Esc** | 关闭内嵌预览 |
| 流畅 / 均衡 / 精细 | 调整流式加载细节与屏幕空间误差 |

有损功能不会被描述成无损。UASTC 主要改善 GPU 驻留与解码，但文件通常明显大于 JPEG。远景父层烘焙同样有损；报告中的采样偏移不代表全表面误差上界。轻量预览只在几何上有损：纹理保持清晰（UASTC），位置与 UV 保持 FLOAT，因材质固定 unlit 而删除 `NORMAL`。主报告的 `sourceExact` 只描述保真产物，预览摘要单独记录。

## 常用转换参数

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--threads N` | `2` | 文件级并发，1–64 |
| `--geometry raw\|meshopt\|draco` | `meshopt` | 原始对照、无量化 Meshopt 或量化 Draco |
| `--texture-format original\|uastc\|etc1s` | `original` | 原始编码图片或 KTX2 GPU 纹理 |
| `--rebuild-top` | 关闭 | 新增简化远景祖先层，原始子树完整保留 |
| `--preview` | 关闭 | 额外生成独立轻量预览 `preview/`，主产物不变 |
| `--memory-budget MiB` | `0` | 控制后续根块提交，不是单文件峰值硬上限 |
| `--srs` / `--origin` | 读取元数据 | 空间参考与源坐标原点 |
| `--checkpoint-dir` / `--pause-file` / `--resume` | 关闭 | 文件边界暂停与身份校验续转 |
| `--json-events` / `--cancel-file` | 关闭 | 结构化进度与合作取消 |
| `--capabilities` | — | 输出机器可读的原生能力合同 |

## 项目结构

```text
native/   C++17 转换核心、CLI、行为规格与原生测试
app/      Go/MyGo 桌面与 HTTP 应用、内嵌 Cesium、打包脚本
data/     小型合成 OSGB 测试夹具
```

App 直接调用随包的 `osgb-tiles` 二进制。桌面、便携与 CLI 分发共用一套转换实现和一份运行时。

## 测试与核验

```bash
# 原生行为与回归测试
ctest --test-dir native/build --output-on-failure

# App、API、任务管理与预览资源隔离
cd app && go test ./...

# 分发元数据测试
python3 app/tests/test_distribution.py

# 核验已打包资源及哈希
python3 app/scripts/build.py --verify-package app/dist/darwin-arm64
```

测试覆盖分页引用、几何与坐标、纹理兼容、编码往返、发布完整性、暂停续转、HTTP 任务管理和预览路径隔离。关键几何检查使用独立解码器，避免只用编码器自身验证自身。

## 平台验证状态

- **macOS arm64：** 已构建并验证桌面包、CLI、随包运行时与内嵌 Cesium 预览。
- **构建与发布**。Windows x64 使用 GitHub Actions，macOS ARM64 在本机 Apple Silicon 上构建和验证。Linux 构建暂缓。详见[项目构建 skill](.agents/skills/project-build/SKILL.md)。
- Windows 桌面需要 WebView2；Linux 桌面需要相应 WebKit 与桌面服务。便携 HTTP 模式的平台依赖更少。

## 文档

- [转换器行为与构建细节](native/README.md)
- [原生行为规格](native/SPEC.md)
- [App、打包与分发](app/README.md)
- [HTTP API](app/docs/HTTP-API.md)
- [暂停与续转合同](app/docs/PAUSE-RESUME.md)

## 许可

本项目使用 [MIT](LICENSE) 协议。OpenSceneGraph、PROJ、Cesium、Draco、Basis Universal 等第三方组件遵循各自许可；分发包中包含对应许可文本与来源信息。
