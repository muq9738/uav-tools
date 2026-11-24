# 更新日志

## 0.6.9

- Selecting a project folder now follows its OSGB paging index, preserving coarse geometry and tile placement. Select `Data/` directly to convert all independent blocks.
- Fixed blank areas in distant Cesium views by linking drawable tiles across empty index levels. Zooming in still selects the original detail tiles.
- Rebuilt overview tiles now preserve material tint, opacity, and texture orientation. They also support the same vertex color and UV formats as the source tile exporter, including 16-bit colors.
- Conversion can now pause and resume when a disabled texture references a missing file. If a drawable uses the missing texture, the error identifies the affected OSGB file.

## 0.6.8

- Skip empty `osg::Geometry` nodes (zero vertices or no drawable indices) and empty leaf OSGB tiles during conversion and lightweight preview generation instead of aborting the whole task, while still failing explicitly when the entire input contains no valid geometry.
- Support non-georeferenced local coordinate systems (`SRS=LOCAL` and `LOCAL_CS[...]`) by preserving original metric local coordinates without PROJ distortion and anchoring the tileset root on the `(0, 0, 0)` ENU tangent frame for Cesium compatibility.

## 0.6.7

- New tilesets and GLBs, including lightweight previews, now identify their generator as `uav tools v0.6.7`.
- The converter reads its release version from `app/mygo.json` at build time. The 3D Tiles and glTF format versions, geometry, and texture encoding are unchanged.

## 0.6.6

- **国内自动更新加速**：桌面端内置更新源新增中国大陆网络自动检测（探测 `ipip` / Cloudflare trace / 国内连通性并缓存结果），处于中国大陆时自动并发竞速实测可用的 GitHub 加速代理（如 `gh-proxy.com`、`ghfast.top`、`gh-proxy.org` 等），过滤 HTML 限流页并在资产下载失败时自动切换节点或回退 GitHub 直连；第三方代理全程不携带任何 GitHub 令牌，且支持通过 `OSGB_TILES_UPDATE_PROXY` 自定义代理或设为 `off` 禁用。
- **智能识别任务项目名**：修复大疆智图 / RPA 等复用 `.../项目名/models/pc/0/terra_osgbs/out` 路径时任务列表与详情弹窗统一显示 `out` 的问题；后端与前端自动跳过 `out`、`terra_osgbs`、`models`、`pc`、`0` 等通用目录层级提取真实项目名（如 `11月23号项目2_9688`），并同步回写 `/task/create` 响应 `name`。

## 0.6.5

- `/task/create` 切片兼容接口默认允许清空并覆盖已有非空输出目录（`overwrite: true`，显式传 `settings.overwrite: false` 可关闭），兼容大疆智图 / RPA 上游重跑同一任务并复用原 `out` 输出目录的场景，避免因残留旧成果触发 `400` 创建失败。
- 覆盖清理在任务创建返回 `201` 前同步完成，防止上游轮询 `tileset.json` 时误读旧成果；同时内置路径安全防护，严格拒绝覆盖输入目录、父目录、根目录、符号链接或正被进行中任务占用的输出目录。

## 0.6.4

- `/task/create` 切片兼容接口（大疆智图 / RPA 接力）默认开启轻量预览（`preview: true`，显式传 `settings.preview: false` 可关闭），转换完成后与主 `tileset.json` 一同原子发布 `preview/`（含 `preview/tileset.json`），确保打包上传与桌面端均可直接打开轻量预览。
- 优化首帧加载保障：主入口 `tileset.json` 与轻量预览 `preview/tileset.json` 的空容器根节点统一保证 `geometricError >= 2000`，确保任意初始视距下首帧立即细化到首个有效内容层。
- 优化多块模型 `--rebuild-top` + `--embed-textures` 首帧体积：不透明顶层烘焙图集自动以 JPEG 编码内嵌（含透明通道时自动回退无损 PNG），大幅缩减首帧顶层瓦片大小。

## 0.6.3

- 新增 `--embed-textures`（`embedTextures`）选项，将原始 JPEG/PNG 或 KTX2 贴图直接内嵌到 GLB `bufferView` 中（同时兼容 Draco 几何压缩），消除首帧加载时的二次串行外置贴图 HTTP 请求，请求数减少约 75% 且零画质损失。
- `/task/create` 切片兼容接口默认启用 `Draco` 几何压缩 + `GLB` 内嵌原始贴图，显著降低单瓦片体积与网络往返，大幅提升 Cesium 首帧加载速度与交互流畅度。

## 0.6.2

- 内置 `42225` 切片兼容服务支持同端口自动识别明文 `HTTP` 与 `HTTPS`（首字节 TLS 嗅探），兼容原有发送 `http://<IP>:42225/task/create` 的 RPA / GISBox 调用脚本，无需修改调用方协议。

## 0.6.1

- 修复 Windows 桌面端启动及执行转换任务时弹出 `bin\osgb-tiles.exe` 黑色控制台窗口的问题（为子进程启用 `CREATE_NO_WINDOW` 隐藏控制台，独立命令行调用不受影响）。
- 修复 Windows 下检查点工作目录（`C:\Users\...\AppData\Roaming\...`）与输出目录（如 `E:\...`）跨盘符发布时因目录跨卷移动报 `Access is denied` 失败的问题，并增加目录占用/尾斜杠路径容错发布逻辑。
- 强化应用内签名热更新：在侧边栏与“运行环境”页展示更新状态与快捷检查入口，并在重启动作触发前释放本机数据目录锁与 `42225` HTTPS 监听端口。

## 0.6.0

- 桌面应用（Windows `.exe` / macOS `.app`）双击启动后自动开启内置 HTTPS 兼容服务（`https://0.0.0.0:42225/task/create`），无需额外命令行脚本。
- 新增基于 Ed25519 签名验证的应用内自动更新功能，并支持通过嵌入式只读 GitHub 令牌读取 Release 清单与更新包。
- “运行环境”页面新增 HTTPS 兼容服务监听状态显示及“检查更新”按钮。
- 修复简体中文 Windows 路径下转换器因代码页导致的启动失败问题，并嵌入 UTF-8 `activeCodePage` 清单。
