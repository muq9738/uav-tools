# 转换器行为合同

本文件是 `osgb-tiles` 的实现合同：输入、输出、保真边界、失败语义与验收方式。改动实现前先改这里。

## 目标与范围

单一功能：把 OSGB 目录（`metadata.xml`、索引 `.osgb` 或 `Data/` 分块目录）转换成
3D Tiles 1.1 + GLB，可直接在 Cesium 1.136 等标准消费者中加载。C++17 实现，不依赖
Node / JavaScript 转换运行时。

默认路径为保真路径：不量化、不简化、不改贴图编码。四项有损功能（远景层重建、Draco、
KTX2-UASTC、KTX2-ETC1S）均为显式选项且默认关闭，详见下文「可选有损功能」。

## 默认路径行为

- 读取 OpenSceneGraph 正式 OSGB 序列化格式及其 zlib 压缩，不扫描二进制猜测几何。
- 按 PagedLOD / Proxy 外部引用构建层级；相对路径以引用文件及 `databasePath` 解析，保留大小写，
  检测缺失引用与循环。目录根入口按引用闭包去重，扩展名大小写不影响识别。一个源文件的常驻几何
  构成同一 REPLACE 单元。
- 顶点、UV、法线、颜色不减精度不简化；DrawArrays / TRIANGLES / STRIP / FAN 正确展开；不自动生成
  或覆盖法线。按首次访问顺序同步置换所有属性槽位与索引，面序与朝向不变；未引用顶点保留。
- 几何编码默认 `meshopt`：meshoptimizer 0.25 的 ATTRIBUTES/NONE 顶点版本 0、TRIANGLES 索引版本 1
  与点线 INDICES 序列版本 1，无量化。属性与点线每次编码后原生解码逐字节核对；三角形逐面核对，
  仅允许每面循环起点不同，严禁反面、换面序或改角点。位流头必须与声明 mode/version 一致。
  小视图不压缩（收益须覆盖扩展元数据）。压缩扩展挂在 accessor 实际引用的 bufferView、声明 required，
  虚拟解码范围 4 字节对齐、互不重叠且被 buffer 长度覆盖。
- 通过 ReaderWriter 包装保留内嵌原 JPEG/PNG 编码字节；原始像素输入使用无损 PNG，不缩放，
  不用白图替代失败图片。含内嵌 mip 链的源纹理显式失败，不静默丢弃。
- 保留状态继承、纹理采样、alpha、顶点颜色与变换；无法等价表达的状态报出具体错误。
- glTF 使用 Y-up，瓦片与源 OSGB 局部坐标保持 Z-up；轴变换只执行一次。局部矩阵保持双精度，
  禁止把 ECEF 大坐标直接存 float32。
- 位置与矩阵保持 double，Vec3d 在应用源矩阵、投影与局部居中后才写 glTF FLOAT。
- 主产物和轻量预览的入口、逐源清单及 GLB 均使用 `uav tools v<version>` 作为 `asset.generator`，
  其中发布版本在构建时从 `app/mygo.json` 读取；预览算法标识仍在报告中。`asset.version`
  分别保持 3D Tiles `1.1` 和 glTF `2.0`，不把软件版本写入数据集修订字段 `tilesetVersion`。
- 在独立暂存目录写内容与清单，成功后整体发布；可捕获失败清理本次暂存，修复输入后原路径可重跑；
  已有非空输出不覆盖。发布前只读校验入口/分文件清单引用的 GLB、图片、buffer 范围、扩展声明与
  有限包围体/误差，缺项或损坏即失败、不发布，见[发布前完整性校验](docs/specs/output-validation.md)。
- 线程数显式可控；文件级处理后释放源节点与解码图片，不在内存中保留全部 LOD。
- `--memory-budget MiB` 只在已驻留内存低于预算时提交下一个独立根块，超预算先等最早一块结束。
  该预算不限制单个源文件内部峰值，报告须同时给出实测峰值与是否真正触发门控。

## 坐标与法线合同

- `ENU:纬度,经度[,椭球高]` 使用 WGS84，角度为度，高度与局部坐标为米；`SRSOrigin=(east,north,up)`
  在该 ENU 基底内叠加，高度允许为负。数值须完整、有限，拒绝尾随字符与空分量。源顶点先应用文件内
  及跨文件全部矩阵再加原点；只在 glTF 节点执行一次 Z-up→Y-up。
- 其他 SRS 必须是 PROJ 能识别的静态 geographic / projected / geocentric CRS 或其 BoundCRS。
  输入采用 `proj_normalize_for_visualization` 轴顺序：地理坐标为经度、纬度，投影为 easting、northing；
  保留 CRS 源轴单位。二维 CRS 显式提升到三维，新增 z 为其基准椭球的米制椭球高，不能把海拔自动
  当成椭球高。既有三维 CRS 按高度单位转换。
- 固定 PROJ 9.6.2、关闭网络，显式设置 `ALLOW_BALLPARK=NO`、`ONLY_BEST=YES`。转换与原点定位使用
  同一 ECEF 操作；ENU 原点必须等于该操作转换后的 `SRSOrigin`。不实现或猜测垂直/复合 CRS 高程模型，
  不接受缺少坐标历元的动态基准；工程、纯垂直、时间 CRS 及非 CRS 的 PROJ pipeline 均失败。
  缺少最优操作所需格网时失败，不退化为未校正高度。
- 投影路径法线按源矩阵与投影 Jacobian 的逆转置转换并归一化；差分步长按源轴单位选取，不能对度
  与米一律用 `0.01`。地理坐标精确极点没有可逆经纬 Jacobian，携带法线时显式失败；其他投影奇点或
  数值退化同样失败。不凭法线转换生成或覆盖缺失法线。
- 回归以 WGS84 椭球公式、已知 ECEF 平移、UTM 中央经线/赤道、Web Mercator 公式及解析切向量为预期，
  生成正式 OSGB 后解码 GLB 与 tileset 的完整世界坐标链。PROJ 正反闭环只作辅助；同时核对未参与
  空间变换的 UV/颜色/法线位模式、变换后法线方向，以及由居中坐标 FLOAT 半 ULP 与源线性矩阵推导的
  舍入上界。`coordinateRoundingErrorMeters` 只衡量 FLOAT 写出舍入，不含 CRS 绝对精度、PROJ 误差、
  原始数据误差或高程基准误差。

## 引用、路径与分页边界

- PagedLOD / ProxyNode 的槽位 0、1 非空文件名都表示外部引用；完整保留预载孩子时，该槽位不能再
  当作常驻几何重复导出。OSG 序列化可能省略外部孩子并把常驻孩子压缩到数组前部，此时这些孩子仍属
  常驻内容，不能按压缩后的下标丢弃。普通 Group 的常驻几何与分页孩子必须同时保留。多个无文件引用
  的 PagedLOD 分支在映射规则明确前显式失败。
- `NodeMask==0` 按 OSG 默认遍历语义排除整个子树；非零掩码不排除。目录根发现仍按结构引用确定归属，
  不能把被掩码孩子提升为独立根。输入没有任何有效内容时失败，不发布空产物。
- 引用文件名与 `databasePath` 中的路径分隔符统一解释；相对 `databasePath` 以当前引用文件所在目录为
  基准。大小写敏感文件系统要求引用大小写精确匹配；文件系统允许大小写别名时，指向同一实际文件的
  别名不得制造重复根或重复实例。UTF-8 路径不转码为本地窄字符编码。
- 同一实际文件与同一源矩阵的共享引用复用转换结果，不同源矩阵是不同绘制实例。目录根发现沿全部
  直接、间接有效引用闭包去重，但不把不可达文件加入根层；真实循环与缺失引用必须报错。
- 项目索引与 `Data/` 或 `data/` 分块布局并列时，优先按直属 OSGB 索引的分页引用确定范围，保留索引内的常驻几何和跨文件变换。只有独立模型与分块并列时，沿用分块入口。单独选中索引文件或 `Data/` 可明确指定转换范围。
- 对外入口将只有分页引用、且没有局部变换的中间分组展开到最近的有内容祖先或总根。逐源清单仍记录各索引文件，渲染器按屏幕误差停止细化时可以直接选中有内容的瓦片。
- 父内容或任一有效孩子失败时整个转换失败且不发布。回归必须从最终 `tileset.json` 遍历所有内容引用，
  并独立解码 GLB 的索引、顶点与节点矩阵核对实际绘制三角形；文件数、GLB 数或报告 `complete`
  都不能单独作为完整性证据。

## 层级与误差

- 叶 `geometricError` 为 0；非叶取最大非零孩子未舍入误差 ×2，孩子均 0 时取合并包围盒最大边 /50；
  写盘时误差保留两位、包围盒六位小数。块根与外层包装另有固定值。这些是启发式值，
  **不代表实测几何误差上界**。
- 子树使用 REPLACE 语义并显式写出 `refine`，不依赖消费者默认推断。

## 可选有损功能

四项参数均进入检查点身份与 App 能力协商，默认关闭；开启后报告中 `sourceExact` 为 false，
原始内容是否逐字节保留由单独的 `sourceContentsExact` 表达。

**轻量预览（`--preview`）**：与主产物相互独立，主产物的保真结论、编码和误差不变。沿原 OSGB 的
`REPLACE` 金字塔保留顶层概览、偶数层级与全部叶层（跳过冗余奇数中间层以减少文件数与层级跳转），
生成流式多级 LOD 的 `preview/tileset.json`，供 Three.js `3d-tiles-renderer` 按视锥与屏幕误差动态
流式加载并用 LRU 回收离屏瓦片：远景秒开、拉近至任意位置自动加载与保真版同等清晰度的原生叶层纹理。
几何省略 `NORMAL`，使用 `KHR_mesh_quantization`（位置与 `[0,1]` UV 归一化 U16 量化，越界 UV 自动
回退 FLOAT）叠加 `EXT_meshopt_compression` 与 `KHR_materials_unlit` 以压缩体积与显存；纹理原样
透传内嵌 JPEG/PNG（零重编码失真，按内容去重存放在 `preview/textures/`），采样器统一启用三线性
mipmap（`LINEAR_MIPMAP_LINEAR`），瓦片按 `--threads` 并行转换。产物位于 `preview/`，入口
`preview/tileset.json`，详细统计写入 `preview/preview-report.json`，主报告 `conversion-report.json`
增加独立 `preview` 摘要。预览参数与 `mesh-preview-v2` 进入检查点身份；暂停、取消与失败清理预览暂存；
两套产物同一暂存目录发布，任一套校验失败都不能标记完整成功。预览根 `geometricError` 必须为正：
Cesium 在根节点误差为 0 时会跳过整棵子树。CLI 默认关闭，App 新建任务默认开启。

**远景层重建（`--rebuild-top`）**：源根内容前沿 → 间距统计空间分组 → 目标面数取成功输入面数平均值
向上取整 → 相邻开口边界焊接（合并距离 ≤2 m、垂距 ≤6 m、链长 ≥5，报告 `mergePairs`/`welds`/
`maxMergeDistanceMeters`）→ 受材质与边界约束的平面 QEM（拒绝翻面、保持材质/颜色分区）→
LSCM chart 展开与矩形打包 → 1024 图集烘焙（法线投射、最近表面回退、空像素膨胀）→ 父 GLB
接回原子树。渲染状态一致（cull/blend/alpha-test/TexEnv）且共享图集的材质合并为**单个 primitive**，
降低绘制命令。合批须保留各材质的漫反射 RGBA 因子与 alpha 阈值。重建读取与源 GLB 相同的 RGB/RGBA 和 UV 属性类型及绑定，按 OSG 纹理坐标采样内存行，保留逐顶点颜色（COLOR_0）及纹理方向。只新增祖先，源内容、原图与全部后代不变。烘焙为有损过程，报告单列采样偏移与回退像素数。

**顶点压缩（`--geometry draco`、`--draco-speed 0|5|10`）**：POSITION14 / UV12 / NORMAL10 / COLOR8，
按 primitive 量化，encode/decode 速度相同。含重复属性点、未引用点或退化面的 primitive 使用保留
索引与点身份的 sequential 分支，其余使用 edgebreaker。编码后解码核对格点点重数与有向面覆盖，
不符即失败。量化改变几何数值，报告给出逐属性步长与实测世界误差。

**KTX2（`--texture-format uastc|etc1s`）**：固定公开版本 Basis Universal 1.16.4 原生编码器，
生成真实 UASTC / ETC1S 块、sRGB transfer、完整 mip 链并保留 alpha；宽高按 4 像素对齐并限制 4…4096。
UASTC 关闭 RDO、Zstd 超压缩；ETC1S compression level 2 / quality 128。不把 JPEG 放进 KTX 容器
冒充 GPU 压缩，源 sampler 状态保留。

## 暂停与续转

- `--checkpoint-dir` 启用文件边界暂停与真正续转，绑定全部可达输入、外部资源、有效参数、转换器与
  运行库的 SHA-256。材质已关闭且缺失的外部贴图允许暂停、续转。启用贴图的材质缺图时，错误须标出源 OSGB。
- `--pause-file` 在当前文件本地内容持久化后停下，退出码 75、`result.status=paused`。
- `--resume` 校验总账、记录与全部资产后复用已生成内容；损坏或身份变化拒绝复用。取消清理，
  暂停保留且不发布。详见[暂停续转合同](docs/specs/pause-resume-contract.md)。

## App 与分发

- 转换只在 native 实现；App 调用同包 native，默认编码、线程、预算与坐标参数一致。
  `--capabilities` 协议为 1，默认 meshopt、threads=2、预算不限、透传原编码；外壳必须拒绝不匹配的 native。
- 分发模式从自身目录加载 SDK 库与 OSG 插件，PROJ 数据库内嵌；不从开发机 PATH 偷换转换器。
- `--json-events` 提供真实进度与最终报告；总量未知用 0，不以 GLB 数冒充源文件进度。
  最终退出 0、报告 complete 且编码/线程/预算匹配、入口已发布，App 才能标记成功。
- 合作取消文件在 Windows、macOS、Linux 使用相同语义。

## 验收

1. 原生回归覆盖上述输入、失败与坐标/纹理边界；`ctest` 全部通过。
2. 从指定入口转换全部可达文件，逐引用/索引/数组/纹理检查，无文件静默跳过；不可达文件不擅自加入根层。
3. 在真实 Cesium 页面加载输出，检查不同相机距离、方向、全部叶层级、拾取与渲染错误。
4. 固定画质与机位测量加载、纹理/几何驻留与交互帧时间；转换器运行更快不等于浏览器运行更快，
   不能用降精度宣称性能达标。
5. 记录尚未满足的指标，不把未实测的低配体验或严格质量保证标记为完成。
