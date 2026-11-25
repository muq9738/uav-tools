# HTTP API

默认 `127.0.0.1:8990`（`osgb-tiles-app` 未传参数启动桌面窗口或 `--browser` 模式时，本地界面使用随机 `127.0.0.1` 端口，并同时自动启动 `--https-addr 0.0.0.0:42225` 内置兼容服务，同端口自动识别明文 HTTP 与自签 HTTPS 请求；传 `--https-addr=off` 可关闭）。可选设置 `--token` 时，接口支持 `Authorization: Bearer <token>` 或 `?token=…`；留空则不启用鉴权。加 `--tls` 可将主监听地址也切换为内置 HTTPS（基于 Go 标准库 `crypto/tls`，自动生成并持久化自签证书到 `<data-dir>/tls/`，也可用 `--tls-cert` / `--tls-key` 指定外部证书）。

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| GET | `/api/health` | 外壳版本与 native 可用性 |
| GET | `/api/config` | 数据目录、native 路径/能力/默认值、桌面能力、HTTPS 兼容地址与更新状态 |
| GET | `/api/doctor` | 数据目录写入、HTTPS 兼容地址与 native 能力检查 |
| POST | `/api/check-updates` | 触发桌面端签名自动更新检查（非桌面模式返回 501） |
| POST | `/api/jobs` | 创建任务 |
| POST | `/task/create` | 兼容外部切片任务接口（`options.inputPath`/`outputPath`/`settings` → 201 Created） |
| GET | `/api/jobs` | 任务列表 |
| GET | `/api/jobs/{id}` | 任务详情与最终报告 |
| POST | `/api/jobs/{id}/cancel` | 请求取消（含正在暂停和已暂停的任务） |
| POST | `/api/jobs/{id}/pause` | 暂停排队任务或请求文件边界暂停 |
| POST | `/api/jobs/{id}/resume` | 将已暂停任务重新排队，必要时续用检查点 |
| GET | `/api/jobs/{id}/logs?after=0&limit=200` | 增量日志与下一游标 |
| GET | `/api/jobs/{id}/events?after=0` | SSE 状态、进度和日志 |
| GET | `/api/jobs/{id}/tiles/{path}` | 成功任务的 Cesium 预览资源；支持 Range，限制在输出目录内 |
| POST | `/api/pick-directory` | 桌面目录选择；无桌面支持 501 |
| POST | `/api/jobs/{id}/reveal` | 打开输出目录；无桌面支持 501，目录未创建 409 |

创建示例：

```json
{"input":"/data/osgb","output":"/data/new-tiles","threads":2,"geometry":"meshopt","memoryBudgetMiB":512,"srs":"EPSG:32651","origin":"500000,0,10","texturePixels":false}
```

可选字段还包括 `name`（任务显示名称；缺省或为 `out`/`terra_osgbs` 等通用目录名时自动从输入/输出路径提取项目名）、`preview`（`/api/jobs` 默认 false，`/task/create` 兼容接口默认 true；同一任务额外生成独立 `preview/` 轻量预览，主产物不变，传 `settings.preview: false` 可关闭）、`overwrite`（`/api/jobs` 默认 false，`/task/create` 兼容接口默认 true；开启时会在提交前同步清空已有非空输出目录以兼容上游重跑覆盖行为，传 `settings.overwrite: false` 可关闭；仍拒绝覆盖输入目录、根目录、符号链接及进行中任务占用的目录）、`embedTextures`（默认 false；将贴图直接内嵌在 GLB `bufferView` 中减少 HTTP 请求，`/task/create` 兼容接口默认开启并默认使用 `draco` 压缩以优化首帧加载）、`rebuildTop`（默认 false）、`topSealDistance`（重建顶层前的边界焊缝距离，0..2 米，默认 2，0 表示不焊）、`textureFormat`（默认 `original`，另支持 `uastc` / `etc1s`）、`dracoSpeed`（默认 5，支持 0/5/10）；`geometry` 另支持 `draco`。这些选项及组合明确有损，保留保真默认；Top 只新增祖先，不删除原子树。参数会持久化并进入 native 检查点身份；旧任务 Spec 缺省字段按保真默认解释，转换器版本或二进制变化的检查点仍按身份合同拒绝复用。

App 根据可用逻辑 CPU 数选择同时运行的任务数：达到 4 个时默认 2 个任务，少于 4 个时默认 1 个，其余任务排队。`--max-parallel N` 可覆盖此设置，`/api/config` 的 `maxParallel` 返回实际值。每个任务默认使用 2 个转换线程，内存预算按任务分别计算。并行任务可读取同一输入，各自需要独立的输出目录。

input 可以是目录或索引 `.osgb`。除了 input/output 均可省略：默认 threads=2、geometry=meshopt、memoryBudgetMiB=0、texturePixels=false；坐标由 native 读取 metadata。0 预算代表不限，不保证单文件峰值。未知字段与无效参数返回 400。

任务状态：queued、running、pausing、paused、succeeded、failed、canceled。暂停不是终态，只有 succeeded、failed、canceled 是终态。`succeeded` 是完整转换执行成功，不代表画质与性能验收通过。`progress.source` 为 converter，done 是 native 已处理工作项，total=0 表示未知，此时应显示处理数而不是虚构百分比。

```json
{"id":"…","state":"running","progress":{"stage":"convert","done":147,"total":0,"unit":"files","source":"converter"}}
```

暂停和继续接口无需请求体，成功返回 202 和当前任务快照；未知任务返回 404，不合法状态返回 409。只有 `/api/config` 的 `sidecar.available` 与 `sidecar.pauseResume` 均为 true 时才支持操作，否则返回 501。暂停对 pausing/paused 幂等；继续仅接受 paused。

排队暂停直接变为 paused，首次执行不传 `--resume`。运行暂停先变为 pausing，继续接收进度与日志，待 native 退出 75 且暂停报告校验通过才变为 paused；如果转换已经正常完成，则仍按成功合同判断。暂停不会取消进程 context。取消对 queued/running/pausing/paused 生效，取消后不能继续。

任务可包含管理器分配的 `checkpointDir`、`resumeRequired` 和最近一次 `pausedAt`；这些字段不能放入创建请求。已实际转换且确认暂停的任务 `resumeRequired=true`，继续时复用原任务参数与检查点。paused 的 `finishedAt` 为空；运行后暂停的 `exitCode=75` 和 `report.status=paused` 代表上一轮暂停结果，尚未启动的排队暂停没有退出码或报告。继续会清除上一轮报告与退出码。

SSE 含 state/progress/log 等通知及任务快照，连接在 paused 时保持，可以观察后续继续。日志按 seq 增量查询，继续及重启不重置序号。重启保留 paused 历史，不自动排队；queued/running/pausing 标为中断失败，不能将未确认的暂停当成有效检查点。详见 [App 暂停与续转规格](PAUSE-RESUME.md)。

预览入口为 `/viewer.html?job={id}`；带令牌部署时附加 `&token=…`。查看器对 tileset 及其派生 GLB、纹理请求使用 Bearer 头。静态界面、Cesium 脚本与 Worker 不含用户数据，不需要令牌；所有 `/api/` 接口仍要求令牌。任务非 `succeeded` 时资源接口返回 409，缺失文件或逃逸输出目录的符号链接返回 404。

App/native 的事件、取消和分发合同见 [NATIVE-CONTRACT](NATIVE-CONTRACT.md)。
