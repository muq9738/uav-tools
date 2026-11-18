# OSGB → 3D Tiles 原生转换器

C++17 命令行工程，读取 OSGB 目录/索引，输出 3D Tiles 1.1 和 GLB。转换运行时不依赖 Node.js。输入读取使用 OpenSceneGraph，坐标转换使用 PROJ，图片和 XML 使用 libpng、libxml2。

配套 App 位于 [`app/`](../app/README.md)。App 与 CLI 使用同一份随包二进制、SDK 运行时与默认 meshopt 配置，已通过真实模型逐文件哈希对照。行为合同见 [SPEC.md](SPEC.md)。

## 构建与运行

需要 C++17 编译器、CMake、Ninja、OpenSceneGraph 3.6、libpng、libxml2、SQLite3 开发库与 `sqlite3` 命令。测试另需 libjpeg-turbo 的 turbojpeg（仅在提供 `turbojpeg.h` 时构建图片兼容独立 oracle 回归，缺失则跳过并给出配置告警）。CMake 下载固定版本 PROJ 9.6.2；nlohmann/json 3.12.0 与 meshoptimizer 0.25 编解码源码已附在 `third_party` 并保留许可证、来源记录。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure

./build/osgb-tiles /data/osgb /data/new-tiles --threads 2 --geometry meshopt
```

以上是开发构建。正式分发使用仓库根目录的 `python3 app/scripts/build.py --sdk-prefix SDK_PREFIX`，统一构建、收集依赖和打包；命令行也使用包内的 `bin/osgb-tiles`。macOS 开发构建可加 `-DCMAKE_PREFIX_PATH=/opt/homebrew`；分发收集器会将依赖改为包内路径，不修改系统库。

输入支持直接指定索引文件、包含索引的目录、`Data/分块名/分块名.osgb`。同目录中被其他文件引用的瓦片不会再作为独立根加载。缺少 `metadata.xml` 时必须明确提供坐标系：

```bash
./build/osgb-tiles /data/root.osgb /data/new-tiles \
  --srs 'EPSG:32651' --origin '224107.81419846753,3366639.462932359,-66.472'
```

ENU 写法为 `ENU:纬度,经度[,高度]`，`--origin` 表示源坐标系中的原点。投影、跨文件矩阵及局部居中均先使用 double，最后写入 glTF FLOAT，并记录该步骤的数值舍入误差。

输出目录必须不存在或为空。转换在独立暂存目录中执行，成功后整体发布；正常捕获到的失败会清理本次暂存，修复输入后可以对原输出路径直接重跑。进程被强制终止或断电可能留下隐藏的 `.输出名.building-*` 目录，但不会将其作为完成产物。

`--geometry raw|meshopt|draco` 默认 `meshopt`；raw 用作独立对照。`meshopt` 使用 `EXT_meshopt_compression`，要求消费端支持该扩展，已验证目标 Cesium 1.136。所有属性按首次访问顺序同步置换存储槽位，未引用顶点保留。属性使用 ATTRIBUTES/NONE（顶点版本 0），三角形索引使用 TRIANGLES（索引版本 1），只允许逐面的循环起点变化，面序、朝向及角点完整属性不变；点和线使用 INDICES，索引顺序逐字节不变。不量化数值，也不删除未引用数据。压缩视图的虚拟地址互不重叠，避免 Cesium 索引缓存误复用等长数据。

当前并发粒度是入口根块，工作线程数取请求值、硬件线程数、入口数量的最小值。单个索引入口仍按引用逐文件处理；报告区分 `requestedThreads` 和实际 `threads`。`--memory-budget MiB` 仅门控后续独立根块的提交，不是单文件峰值的硬上限。

显式有损功能：`--rebuild-top` 按源根空间分组新增父层，平均面数预算、边界/材质约束 QEM、UV 展开和 1024 纹理烘焙，原源子树完整保留；`--geometry draco --draco-speed 5` 使用 POSITION14/UV12（另 NORMAL10/COLOR8），速度档支持 0/5/10；`--texture-format uastc|etc1s` 生成 sRGB KTX2 与完整 mip 链，宽高对齐并限制到 4…4096，保留 alpha。默认 `--texture-format original`。四项可组合；有损内容与误差在报告中单独记录，不能据此声明同画质性能通过。行为及限制见 [SPEC.md](SPEC.md) 的「可选有损功能」。

构建额外静态编译官方 Draco 1.5.7 和 Basis Universal 1.16.4（Apache-2.0），由 `cmake/Codecs.cmake` 获取固定 tag 并核对提交；允许用 `FETCHCONTENT_SOURCE_DIR_DRACO` / `FETCHCONTENT_SOURCE_DIR_BASIS_UNIVERSAL` 指向对应提交的 Git 缓存。运行时无需额外编码 CLI 或 Node 运行时。

`--capabilities` 返回协议版本、能力和默认选项；`--json-events` 在扫描、转换、发布阶段输出 JSON Lines。扫描总量尚未确定时 `total=0`，转换阶段按完成工作项持续上报，发布前给出最终总量。App 不再扫描未发布的输出目录估算进度。`--cancel-file PATH` 是各平台相同的合作取消协议；SIGINT/SIGTERM 也在处理边界触发清理，超时强制结束仍可能残留暂存。

需要暂停时，首次运行显式指定检查点与暂停令牌路径：

```bash
./build/osgb-tiles /data/osgb /data/new-tiles --json-events \
  --checkpoint-dir /work/job-123/checkpoint --pause-file /work/job-123/pause
# 另一个进程创建 /work/job-123/pause 后，native 在文件边界暂停，退出 75。
# 删除暂停令牌，再用相同输入、输出和参数恢复：
./build/osgb-tiles /data/osgb /data/new-tiles --json-events \
  --checkpoint-dir /work/job-123/checkpoint --pause-file /work/job-123/pause --resume
```

暂停时最后一条事件仍是 `type=result`，报告含 `status=paused`、`checkpointDir`、`filesCompleted`。恢复会读取源文件验证内容身份，复用已持久化的几何、纹理与瓦片清单；成功报告的 `filesReused` 给出本次复用的文件实例数。源内容、参数、转换器/运行库或资产改变都拒绝恢复，不会静默重跑。恢复验证失败保留原检查点；接管后的真正取消或失败清理检查点，成功发布后也会清理。检查点与输出互不包含，暂停令牌放在两者之外。扫描和逐文件操作的延迟、损坏检查点及强杀边界见[完整合同](docs/specs/pause-resume-contract.md)。

PROJ 数据库静态内嵌，分发模式仅使用内嵌资源；不再绑定开发机 `proj.db` 路径。Windows 使用 UTF-8 输入参数与文件路径，并提供 WorkingSet 采样；各平台实际运行状态以验证记录为准。

## 产物

- `tileset.json`：对外入口，包含完整层级，内容 URI 相对该文件解析。
- `content/*.glb`：每个具有常驻几何的源文件对应一个 REPLACE 单元。
- `textures/*`：原 JPEG/PNG 编码透传；原始 8 位像素转无损 PNG。
- `tiles/*.json`：逐源文件清单，供定位与核查；主入口已内联层级，不需要逐文件请求这些 JSON。
- `conversion-report.json`：完成状态、源文件与内容映射、图片、坐标误差及处理统计。

原始几何不减面，不进行 Draco 重新量化。分页引用缺失、循环引用、无法等价表达的材质/数据类型会报错，不以白图、丢瓦片或“进度完成”代替成功。

V 翻转依据原图与 OSG 内存采样方向处理，并声明所需的 `KHR_texture_transform`；独立 libpng/libvips 解码已用于核查该方向，不能仅凭 `Image::origin` 的名称或重复使用同一解码器判定。

## 实验路径与接手验收

默认 App/CLI 仍使用无量化 meshopt。`--quantize-u16`（16 位位置与 UV）和 `--split-paged-lod`（独立分页分支）是显式实验选项，尚未通过完整验收，不应作为发布配置。量化会改变坐标与 UV；按编码网格正确解码不等于画面或测量无损。

分页实验为每个分支保留一份粗层，消除主入口中会按 SSE 提前停止的内部空分组。它保留全部图元，但可能改变选层，必须另外检查画面与拾取。暂停续转会恢复已物化分支的依赖记录；不能只复用父节点清单。

独立测试工具：

```bash
node --test tests/glb-accessors.test.mjs tests/oriented-triangles.test.mjs
node tests/verify-render-equivalence.mjs BASELINE CANDIDATE /path/to/meshopt_decoder.mjs
node tests/compare_quantized_outputs.mjs --before BASELINE --after CANDIDATE \
  --decoder /path/to/meshopt_decoder.mjs --output numerical-audit.json
node tests/compare_partitioned_outputs.mjs BASELINE CANDIDATE \
  /path/to/meshopt_decoder.mjs partition-audit.json
```

Cesium 采集器的 `--capture-quality` 单独保存画面、拾取点与相机射线；`--measure-frames 60` 采样稳态 CPU/GPU，不能和截图同时启用。GPU 查询不可用或 disjoint 时明确记录，不以 0 当作耗时。采集器在页面启动异步任务后等待完成状态，不向 `page.evaluate` 传无效的超时选项。

源码 LOD 调试工具需要额外的 OSG Viewer/OpenGL，仅在 `-DOSGB_BUILD_DIAGNOSTIC_TOOLS=ON` 时构建；正式转换器不因此引入窗口系统依赖。

源码 LOD 调试工具需要额外的 OSG Viewer/OpenGL，仅在 `-DOSGB_BUILD_DIAGNOSTIC_TOOLS=ON` 时构建；正式转换器不因此引入窗口系统依赖。
## 当前验证状态

- **三角形索引编码**：全模型独立解码与实际四机位对照，GLB 总量 286,979,776 → 255,933,592 字节（−10.82%）。属性位模式、未引用顶点、面序/朝向、矩阵、贴图与瓦片树保持；四机位 PNG 与资源统计相同。仅降低文件/传输体积，不降低 GPU 驻留或绘制命令。
- **暂停续转**：44 项检查点回归与独立 CLI 进程验证。实际转换中暂停退出 75，重启后复用已完成文件，资产逐文件一致且不重写。
- **52 项原生回归**：引用闭包、大小写、失败重跑、元数据、zlib、图元分段、材质/纹理、Vec3d 与坐标变换，属性/点线 codec 逐字节往返、16/32 位 TRIANGLES 有向面往返、未引用顶点保留、等长索引缓存隔离。
- **38 项独立坐标回归**：以椭球公式、UTM 级数与解析法线核对 ENU/负高度/xyz 原点、投影轴单位、跨文件矩阵、一次 up 轴与非均匀缩放；垂直/复合 CRS、动态基准与缺失格网不会静默降级。
- **图片兼容** 32 项符合预期、3 项明确未支持、0 项已知失败，期望画面由测试独立生成与解码，并先注入四类错误确认比较逻辑有效。详见[图片兼容规格](docs/specs/osgb-texture-compatibility.md)。
- **全模型**：1,006 个可达 OSGB → 1,005 个 GLB + 3,413 张原编码图片；最大坐标舍入误差 8.37 微米。
- **产物校验**：全部 GLB 经 glTF Validator 报 0 错误 0 警告；另用独立 meshopt 解码器核对 10,239 个视图、49,951,908 个绘制角点的属性位模式、节点矩阵与全部图片。
- **性能（本机参考）**：M2 Pro 有头 Chrome 三轮，近景加载中位数 1.36 秒，纹理驻留 997.7 MiB，稳态绘制 CPU 3.19 ms/帧。低配设备与真实远程网络尚未验收，不能由本机结果外推。

当前明确不支持的输入会失败，例如非 MODULATE 的纹理混合、多启用纹理单元、非单位纹理矩阵、无编码可透传的 GPU 压缩图片，以及多个无外部文件引用的常驻 LOD 层。macOS 已构建随包运行时与 App，并验证不依赖开发环境变量；其他平台边界见 [App 文档](../app/README.md)。

实现合同见 [SPEC.md](SPEC.md)。
