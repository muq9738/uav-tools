# 发布前输出完整性校验规格

状态：本轮实现并验证。适用范围是本工程当前生成格式，不是第三方 3D Tiles 通用校验器，也不能替代 Cesium 渲染验收。

## 背景与失败模式

典型故障是“部分瓦片不显示”：失败内容被静默跳过，任务仍以完成结束，直到加载或事后清点才发现缺 GLB、缺图片或清单内容损坏。本工程已禁止转换阶段静默成功，但暂存目录写完后、整体发布前没有再核对“写出的清单与实际资产是否一致”。文件系统错误、误删、截断或后续集成改动都可能让不完整目录以成功退出。

本规格增加一次只读的发布前校验：校验失败则抛错，由现有 `OutputTransaction` 清理暂存，不创建目标输出目录；校验通过不修改任何资产字节。

## 校验对象

对 `OutputTransaction` 的暂存目录执行，入口为 `<staging>/tileset.json`，并覆盖当前转换器实际写出的全部清单：

1. 主入口 `tileset.json` 的显式内联树。
2. `conversion-report.json` 的 `records[]`：每条记录的 `tileset`（分文件清单）必须存在并作为独立 tileset 校验；记录中的 `content[].uri` 与 `content[].images[].uri` 必须存在。
3. 上述清单引用的 GLB 及其图片 URI。

GLB 与图片按规范化路径去重，避免同一资产被重复解析；图片去重同时核对 MIME 声明，不能让后续引用的错误类型绕过校验。

## 具体检查项

### tileset 文档

| 检查 | 依据 | 失败即拒绝发布 |
| --- | --- | --- |
| `asset.version == "1.1"` | 当前写出格式固定 1.1 | 是 |
| 根节点存在且为对象 | 必须有无内容可加载的入口 | 是 |
| 每个瓦片必须有 `content` 或非空 `children` | 空瓦片意味着内容被跳过 | 是 |
| `boundingVolume.box` 为 12 个有限数值 | 当前只写 axis-aligned box；NaN/Inf 会使剔除失效 | 是 |
| `geometricError` 为有限且非负数值 | 误差参与 SSE 选层；非有限值无意义 | 是 |
| `content.uri` 为本地相对 URI，解析后为存在、非空的常规文件 | 缺失内容正是本规格要阻止的失败模式 | 是 |
| 内容扩展名只接受 `.glb` | 当前写出格式只有 GLB；出现其他类型说明格式或校验器未同步 | 是 |
| 拒绝 `contents` 多内容数组与 `implicitTiling` | 当前转换器不生成；静默忽略会漏掉未校验内容 | 是 |
| 嵌套清单 URI（内容指向 `.json`） | 当前树全部内联，分文件清单单独校验 | 不支持，明确失败 |

URI 只接受本工程写出的形式：`/` 分隔的相对路径，无 scheme、无 `//` 前缀、无查询/片段、无百分号转义。解析后的真实路径必须仍在暂存目录内。

### GLB

| 检查 | 依据 |
| --- | --- |
| 魔数 `glTF`、版本 2、头部声明长度等于实际文件大小 | 截断或尾部追加都会破坏加载 |
| 第一个 chunk 必须是 JSON，长度 4 字节对齐且落在文件内；JSON 可解析 | 头/长度/JSON 合同 |
| 其后必须恰好一个 BIN chunk，长度 4 字节对齐且消耗完文件 | 当前 writer 固定写 JSON+BIN 两个 chunk |
| `asset.version == "2.0"` | 当前写出格式 |
| `buffers` 1~2 个且不得带 `uri` | GLB 内嵌或 meshopt 虚拟回退缓冲；外部 buffer 未生成 |
| `buffers[0].byteLength` ≤ BIN 长度，差额 < 4 | 允许 glTF 的 4 字节尾部填充，不允许越界 |
| `buffers[1]` 必须是 `EXT_meshopt_compression.fallback: true`，且确有压缩视图 | 虚拟缓冲只为本工程压缩视图服务 |
| 每个 `bufferViews[]` 的 `buffer` 索引有效；原始视图范围在 `buffers[0]` 内 | 防止越界 view |
| 压缩视图 `buffer == 1`，扩展指向 `buffer 0`；物理范围在 `buffers[0]` 内、虚拟范围在 `buffers[1]` 内；`count × byteStride == byteLength`，`filter == "NONE"`；mode 仅 ATTRIBUTES/INDICES/TRIANGLES，属性 stride 为 4 的倍数且 ≤256，索引 stride 为 2/4，TRIANGLES count 为 3 的倍数 | 按当前 meshopt 生成参数核对，不解码位流 |
| 每个 accessor 的 `bufferView` 索引、`componentType`、`type`、`count` 有效；按 stride 计算的字节范围不超过视图长度；`min/max` 为有限数值且 `min <= max`；POSITION 必须有有限 `min/max` | buffer 范围与有限包围数据 |
| mesh primitive 有 `POSITION`、`indices`、`material`；属性名限于 POSITION/TEXCOORD_0/NORMAL/COLOR_0；POSITION 为 VEC3/FLOAT；索引为无符号 SCALAR；`mode` 0..6；TRIANGLES 的索引数是 3 的倍数；压缩属性只引用 ATTRIBUTES，压缩索引只引用 INDICES/TRIANGLES 且 stride 与 componentType 一致；TRIANGLES 压缩只能被 mode=4 的完整索引 accessor 引用，禁止点、线、strip/fan 和部分视图 | 当前 writer 的属性与图元集合 |
| node 的 `mesh` 索引有效；`matrix` 为 16 个有限数值；`scenes[0].nodes` 索引有效 | 变换与场景引用完整 |
| 每个 `images[]` 有 `uri` 与 `mimeType`（image/png 或 image/jpeg）；文件存在、非空、文件签名与 mimeType 一致 | 图片缺失或类型错配 |
| 每个 `textures[]` 的 `source`/`sampler` 索引有效，材质 `baseColorTexture.index` 也必须落在 textures 内 | 纹理与材质引用完整 |
| 文档中出现的扩展名都必须在 `extensionsUsed` 中；`extensionsRequired` 是 `extensionsUsed` 子集；有压缩视图时 `EXT_meshopt_compression` 必须 required | 声明扩展与内容一致，避免消费端不支持的静默降级 |

### 显式有损编码的附加校验

`KHR_draco_mesh_compression` 必须 required。只有被实际 Draco primitive 覆盖、且独立 decoder 验证点数/面数/属性宽度匹配的 accessor 才可省略 `bufferView`；校验载荷范围、解码成功、有限属性及 POSITION 的实际 min/max，不允许假扩展跳过数组校验。原有 raw/meshopt 访问器仍走原范围检查。

`image/ktx2` 使用真实 Basis transcoder 解析 DFD、sRGB、单张二维图与完整 mip 链，并逐层转码检查载荷。纹理通过 `KHR_texture_basisu.source` 引用 KTX2，扩展必须 required；PNG/JPEG 仍走原签名校验。损坏 KTX2 或缺少 required 声明都有反例回归。新增 Top 从最终内联树被同一发布门禁遍历，不放宽路径/事务检查。

### 明确不检查

- 不检查父子 `geometricError` 大小关系。规范与现有产物允许父误差继承/放大等合法变化，`audit_tileset.py` 的 `childErrorExceedsParent` 仍是独立审计项，不并入发布门禁。
- 不解码 meshopt 位流，不核对几何数值、UV、法线、颜色、像素内容或渲染等价；这些由现有单元测试、独立解码和 Cesium 验收覆盖。
- 不校验第三方扩展、隐式瓦片、外部 tileset JSON、外部 buffer、稀疏 accessor、region/sphere 包围体；当前格式不生成，遇到即明确失败而不是猜测。
- 不检查 `conversion-report.json` 的耗时、RSS 等报告字段，也不把 `records` 数量当正确性指标；报告只作为“应存在哪些分文件清单和内容”的索引。

## 失败语义与只读保证

- 校验函数只读：`stat`/读取 GLB 头与 JSON chunk/读取图片签名，不写入、不修复、不重排暂存内容。
- 任一项失败立即抛 `std::runtime_error`，消息以 `发布前校验失败 [` 开头并带具体资产路径与原因；调用方已有的输出事务析构会删除本次暂存，目标目录不产生 `tileset.json`。
- 校验通过后更新报告中的总耗时和峰值 RSS，再继续 `transaction.commit()`；报告包含门禁阶段成本，不在校验前提前定格。遍历、线程与 checkpoint 调度不变。

## 集成点

`native/src/convert.cpp` 的 `convert()` 末尾，写完 `tileset.json` 与最后一次取消检查之后、`transaction.commit()` 之前，插入且仅插入一处：

```cpp
validateStagedOutput(transaction.staging());
```

Astra 的检查点分支在合并时只需保留这一处调用；不得在校验失败后继续 `commit()`。

## 夹具与验收

新增独立测试 `native/tests/output_validation_cases.cpp`，自建 OSGB 输入，不使用真实模型：

1. 成功基线：转换成功后记录暂存/发布目录全部文件哈希，调用校验，哈希与文件大小不变。
2. 失败注入（每次复制成功产物后只改一处）：
   - 删除一个 GLB；
   - 删除一张被 GLB 引用的图片；
   - 截断 GLB；
   - 把 `bufferView.byteLength` 改到超出 buffer；
   - 把清单 `content.uri` 改为不存在的路径；
   - 把 GLB `images[].uri` 改为不存在的图片；
   - 同一图片重复声明不一致的 MIME；
   - 材质 `baseColorTexture.index` 越界；
   - 删除 `extensionsUsed/Required` 中的 `EXT_meshopt_compression`；
   - TRIANGLES 错误 stride、非 3 倍 count、用于 mode=0/1/2/3/5/6、索引类型/stride 不匹配、部分 accessor，以及属性/索引压缩 mode 互用；
   - 直接构造含 Inf 的 `geometricError`/`boundingVolume.box` 文档。
   每个夹具都必须抛错并命中对应原因。
3. 发布门禁集成：在最后一次 `checkCancelled` 回调中破坏暂存 GLB，`convert()` 必须抛错，目标目录不存在，且没有残留 `.输出名.building-*` 暂存目录。这证明失败输出不被发布，而不是只测到校验函数本身。
4. 运行现有 native 回归与纹理回归，确认默认成功路径未改变资产内容。

## 已知边界

- 校验的是“写出的资产与清单一致”，不是“内容语义正确”。磁盘在校验后立刻损坏、进程被强杀或断电仍可能留下不完整暂存，但不会被标记为已发布。
- 图片只核对存在、非空与编码签名，不解码整像素；损坏但签名有效的图片仍由既有转换/渲染路径负责。
- 单次校验读取主入口与分文件清单 JSON，以及每个 GLB 的 JSON chunk；未与转换并行。主代理已实际转换完整 1,006 OSGB / 1,005 GLB 模型并通过门禁，5,425 个资产 SHA-256 与历史候选全部一致；该检查没有测浏览器性能，也不证明门禁本身零开销。
