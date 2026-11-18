# App 与 native 的唯一契约

转换逻辑只在 native；App 与 CLI 使用同包转换器及 SDK 运行时。正式包只查找自身资源；显式 `--sidecar` / `OSGB_TILES_BIN` 用于开发覆盖，不回退 PATH 中其他版本。

## 能力与默认值

`osgb-tiles --capabilities` 返回：

```json
{"name":"osgb-tiles","version":"0.1.0","protocolVersion":1,"jsonEvents":true,"cancelFile":true,"pauseResume":true,"defaults":{"threads":2,"geometry":"meshopt","memoryBudgetMiB":0,"texturePixels":false}}
```

App 拒绝不支持协议 1、真实事件或合作取消的转换器，也检查默认编码与线程。`pauseResume` 为可选能力；缺失或为 false 时保留普通转换，但不展示或接受暂停/继续操作。能力查询在分发模式下会加载必需随包插件；运行时动态库缺失由系统加载器报错。它仍不代替真实模型转换。

能力还包括 `rebuildTop=true`、`topSealDistance=true`、`preview=true`、`previewAlgorithm=mesh-preview-v1`、`geometryFormats=[raw,meshopt,draco]`、`textureFormats=[original,uastc,etc1s]`、`dracoSpeeds=[0,5,10]` 和公开编码库版本；defaults 另含 `preview=false, rebuildTop=false, topSealDistance=2, textureFormat=original, dracoSpeed=5`，`previewDefaults` 给出预览三角形预算、图集页上限、编码与画质档。App 对所选功能检查随包 native 能力，并核对最终报告的实际编码、速度、Top 与预览状态；UI 禁用随包 native 未声明的选项。旧默认路径的协议版本保持 1。

## CLI 与任务参数

```bash
osgb-tiles INPUT OUTPUT --threads 2 --geometry meshopt --memory-budget 0 \
  --json-events --cancel-file CANCEL_PATH \
  [--srs EPSG:32651] [--origin x,y,z] [--texture-pixels]
```

输入支持目录及索引文件。App `jobs.Spec` 与 HTTP 创建请求共用结构，映射如下：

| 任务字段 | native 参数 | 默认 |
| --- | --- | --- |
| `input` / `output` | INPUT / OUTPUT | 必填 |
| `threads` | `--threads` | 2，允许 1…64 |
| `geometry` | `--geometry` | meshopt；另支持 raw / draco（有损） |
| `dracoSpeed` | `--draco-speed` | 5；支持 0/5/10，分别高/中/低压缩 |
| `textureFormat` | `--texture-format` | original；支持 uastc / etc1s（有损 KTX2） |
| `rebuildTop` | `--rebuild-top` | false；新增有损远景祖先，保留源子树 |
| `preview` | `--preview` | false；生成独立轻量预览 `preview/`，不改变主产物 |
| `TopSealDistance` | `--top-seal-distance` | 0..2 米，默认 2；重建前焊接相邻开口边界，0 关闭 |
| `memoryBudgetMiB` | `--memory-budget` | 0，不限；限制根块提交，不是峰值硬限 |
| `srs` / `origin` | `--srs` / `--origin` | 未填写时由 native 读取元数据 |
| `texturePixels` | `--texture-pixels` | false，优先原 JPEG/PNG 字节；true 为无损 PNG 像素对照 |

stdout 仅输出 JSON Lines，stderr 为人读日志。CLI 不带事件参数时输出完整最终 JSON。

## 结构化事件

```json
{"type":"progress","stage":"scan","done":0,"total":0,"unit":"files"}
{"type":"progress","stage":"convert","done":412,"total":0,"unit":"files"}
{"type":"progress","stage":"finalize","done":1006,"total":1006,"unit":"files"}
{"type":"progress","stage":"done","done":1006,"total":1006,"unit":"files"}
{"type":"result","report":{"status":"complete","geometryEncoding":"EXT_meshopt_compression","requestedThreads":2,"memoryBudgetBytes":0,"files":1006,"glbs":1005}}
```

转换中的总量未知时用 0，界面显示已处理数；总量指完成的文件转换工作项，可能含同源文件的不同变换实例，不以 GLB 数估算。事件在线程之间串行发出；成功发布前后分别 finalize/done。示例 report 已省略其他字段，实际报告含完整统计。

最终退出 0、报告 complete、编码/请求线程/预算与任务一致，并已发布 `tileset.json`，App 才标记成功。无报告不能仅凭退出码视作成功。请求 `preview=true` 时还要求报告 `preview.status=complete`、入口 `preview/tileset.json`、算法 `mesh-preview-v1`、几何 `EXT_meshopt_compression`、纹理 `uastc`，并检查 `preview/tileset.json` 与 `preview/preview-report.json` 存在且可解析。该状态不代表画质与性能验收通过。

## 取消与事务

App 为每次任务建立独立取消令牌路径；取消时创建令牌，各平台 native 在文件处理和发布边界检查并抛出取消错误，输出事务清理暂存。App 提供 5 秒宽限，超时由系统强制结束；超大单次读取、强制结束或断电仍可能留隐藏暂存，不能保证即时中断。CLI 同时支持 SIGINT/SIGTERM。

输出目录必须不存在或为空，不覆盖已有产物；正常失败或合作取消后可原路径重跑。

## 暂停与续转

共同约定见 [native 文件边界暂停合同](../../native/docs/specs/pause-resume-contract.md)，App 状态和持久化见 [PAUSE-RESUME](PAUSE-RESUME.md)。只有能力为 `pauseResume:true` 的 native 接收新 flags：

```bash
osgb-tiles INPUT OUTPUT --threads 2 --geometry meshopt --memory-budget 0 \
  --json-events --cancel-file CANCEL_PATH \
  --checkpoint-dir JOB_CHECKPOINT_PATH --pause-file JOB_PAUSE_PATH [--resume]
```

管理器从首次实际转换起分配并传入任务私有检查点和暂停令牌。排队暂停不启动进程，也不需要检查点；排队任务恢复后首次执行不传 `--resume`。运行暂停仅创建暂停令牌，不取消 context，直到 native 在文件边界退出才释放并发名额。

暂停必须退出 **75**，最后一条事件为 `type=result`，报告 `status=paused`、`checkpointDir` 指向本任务目录、`filesCompleted` 为非负整数。App 检查检查点目录确实存在且没有发布 `tileset.json`，才返回 `Result.Paused`；不会要求暂停报告具有完整成功统计。报告允许返回同一目录的规范化路径，暂停令牌可以由 native 消费。

```json
{"type":"result","report":{"status":"paused","checkpointDir":"/state/checkpoints/JOB_ID","filesCompleted":412}}
```

继续复用原 Spec 和同一检查点，移除旧暂停令牌、创建新的取消令牌并传 `--resume`。输入身份、有效编码/坐标/预算参数和检查点资产的一致性由 native 验证，拒绝复用的错误按任务失败记录。暂停历史与检查点归任务持有，重启不自动执行；App 不推测中断进程是否留下了有效暂停点。取消仍真正取消 context，优先于迟到的暂停/成功结果。

普通成功的退出码、完整报告、参数一致性和发布入口校验保持不变；无效事件、重复 result、result 后追加事件也不能标记成功或暂停。这里的 App 测试使用遵循协议的真实子进程 stub，不能代替整合后的真实 native 资产一致性验收。

## 运行时与验证

`native-runtime.json` 放在 native 旁，列出插件相对路径、能力、第三方许可和全部受管资源 SHA-256。macOS/Linux 库在相邻 lib，Windows DLL 在 bin；PROJ 库及数据库静态内嵌，分发模式只使用内嵌资源。

`app/scripts/build.py` 负责构建、依赖收集、加载路径修正与审核，并让 MyGo 与便携版消费同一资源目录；`--verify-package` 独立核验缺失、额外和被篡改的受管文件。`app/tests/verify_parity.py` 使用完整包和真实输入验证 App/CLI 的资产哈希、报告、进度与取消清理。详细要求见[分发合同](DISTRIBUTION.md)，平台完成情况见[状态说明](STATUS.md)。
