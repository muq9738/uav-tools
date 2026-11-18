# OSGB → 3D Tiles

把 OpenSceneGraph 的分块倾斜摄影模型（`.osgb`）转换成可在 Cesium 等 Web 引擎直接加载的
**3D Tiles 1.1**：一份 `tileset.json` 层级、若干 GLB 内容与原始编码贴图，并附带逐文件可核对的转换报告。

转换核心是 C++17 原生程序，不依赖 Node / JavaScript 运行时；配套一个 Go 编写的本地 Web App，
用同一份随包二进制完成桌面与便携分发。

## 特性

**默认保真路径**

- 按 PagedLOD / Proxy 外部引用还原完整层级，保留最细层与全部可达内容。
- 顶点、UV、法线、颜色不做量化或简化；DrawArrays / STRIP / FAN 正确展开为三角形。
- 原始 JPEG / PNG 编码字节直接透传，不重编码、不缩放、不用占位图。
- 几何压缩为无量化 meshopt，三角形索引使用有向面压缩（仅允许每面循环起点变化）。
- 双精度完成 SRS 投影与局部居中后才写出 FLOAT，避免 ECEF 大坐标精度损失。

**可选有损功能（默认关闭）**

| 选项 | 作用 | 影响 |
| --- | --- | --- |
| `--rebuild-top` | 按源根空间分组新增远景父层：几何合并、QEM 减面、UV 展开与 1024 图集烘焙 | 新增祖先层有损；原始子树与贴图完整保留 |
| `--geometry draco` | Draco 顶点压缩，POSITION14 / UV12 / NORMAL10 / COLOR8，速度档 `0\|5\|10` | 量化改变几何数值 |
| `--texture-format uastc` | KTX2 UASTC 压缩纹理，sRGB、完整 mip 链、保留 alpha | 像素有损，文件通常明显变大 |
| `--texture-format etc1s` | KTX2 ETC1S 压缩纹理 | 像素有损，体积显著小于 UASTC |

四项可自由组合。报告会分别给出量化位数、实测位置/UV 误差、图集采样偏移与实际编码结果，
不会把有损产物标成保真。

**转换工程**

- 文件级并发与可选内存预算；每处理完一个源文件即释放节点与解码图像。
- 基于内容寻址的检查点：`--checkpoint-dir` 支持跨进程暂停与真正续转，输入、参数、转换器或资产
  任一变化都会拒绝复用。
- 发布前完整性校验：入口/分文件清单引用的 GLB、图片、buffer 范围、扩展声明与包围体逐项检查，
  缺项或损坏不发布。
- `--json-events` 输出真实进度；`--capabilities` 提供机器可读契约，供 App 校验随包二进制。

## 目录结构

```
native/          C++17 转换核心，CLI 为 osgb-tiles
app/             Go 本地 Web App（桌面/便携/容器），调用随包 native
data/            合成测试用 OSGB 样例
```

## 快速开始

### 构建转换器

依赖 OpenSceneGraph 3.6、PROJ 9.6.2、libpng、libxml2、Threads。
Draco 1.5.7 与 Basis Universal 1.16.4 由 `native/cmake/Codecs.cmake` 按固定 tag 拉取并核对提交。

```bash
cd native
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/osgb-tiles --help
```

### 命令行转换

```bash
DYLD_LIBRARY_PATH=/opt/homebrew/lib \
./build/osgb-tiles /path/to/osgb/input /path/to/output \
  --threads 4 \
  --srs 'EPSG:32651' --origin '224107.81419846753,3366639.462932359,-66.472'
```

输出目录内为 `tileset.json`、`content/*.glb`、`textures/*` 与 `conversion-report.json`。
目标目录必须不存在或为空，转换在独立暂存目录完成后整体发布。

### 运行 App

```bash
python3 app/scripts/build.py            # 构建 App 与随包 native
./app/dist/*/portable/osgb-tiles-app --headless
```

终端会打印本机地址，用浏览器打开即可选目录、设置参数并查看任务进度。

## 常用参数

| 参数 | 说明 |
| --- | --- |
| `--threads N` | 文件级并发数，1–64 |
| `--memory-budget MiB` | 仅当已驻留内存低于预算时提交下一个根块；不等于单文件峰值上限 |
| `--srs` / `--origin` | 坐标参考与原点；缺省读取 `metadata.xml` |
| `--geometry raw\|meshopt\|draco` | 几何编码，默认 `meshopt` |
| `--texture-format original\|uastc\|etc1s` | 纹理编码，默认 `original` |
| `--rebuild-top` | 新增有损远景层 |
| `--checkpoint-dir` / `--pause-file` / `--resume` | 暂停与续转 |
| `--json-events` / `--cancel-file` | 结构化进度与合作取消 |
| `--capabilities` | 输出能力契约 JSON |

完整行为合同见 [native/SPEC.md](native/SPEC.md)，构建与分发细节见 [native/README.md](native/README.md)
与 [app/README.md](app/README.md)。

## 测试

```bash
cd native/build && ctest --output-on-failure
cd app && go test ./...
```

原生回归覆盖 OSGB 读写、引用与分页边界、坐标与法线、纹理兼容、暂停续转、发布校验、
顶层重建与编码参数；Node 侧提供独立访问器与三角形朝向校验，避免用自己的解码器验证自己的编码器。

## 已知限制

- 已验证平台为 macOS arm64；`.github/workflows/native-app.yml` 提供 macOS / Linux / Windows 的构建矩阵，但除 macOS arm64 外尚未实测。
- `--texture-format uastc` 体积通常远大于等效 JPEG，适合优先考虑显存与解码、而非传输体积的场景。
- 远景层烘焙为有损过程，采样偏移在报告中单列，不等于全表面误差上界。
- 小视图几何不做压缩；收益不足以覆盖扩展元数据时保留原始表示。

## 许可

本项目使用 [MIT](LICENSE) 协议。第三方依赖（OpenSceneGraph、PROJ、libpng、libxml2、
Draco、Basis Universal 等）遵循各自许可，随分发包含许可文本。
