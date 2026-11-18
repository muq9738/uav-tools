# OSGB → 3D Tiles App

Go/MyGo 桌面与 HTTP 任务界面，直接调用本包内的 C++ native。转换算法、依赖和产物只维护一套。默认编码已统一为无量化 meshopt；raw 作为对照模式。

已有提交、排队、状态、取消、日志、历史持久化、目录选择与打开输出目录。转换器声明 `pauseResume:true` 时还支持文件边界暂停与续转；重启保留已暂停任务，需手动继续。App 协议验证与真实 native 整合边界见 [暂停与续转规格](docs/PAUSE-RESUME.md)。支持 OSGB 目录和索引 `.osgb` 文件；高级选项包括编码、线程、转换内存预算、SRS 和 origin。无绘图或测量引擎，产物质量与性能通过 Cesium 验收。

## 使用完整包

macOS 打开 `OSGB 转 3D Tiles.app`；Windows/Linux 使用相应桌面包或便携版。便携版：

```bash
./osgb-tiles-app --headless
./bin/osgb-tiles INPUT NEW_OUTPUT
```

macOS App 的 CLI 位于 `OSGB 转 3D Tiles.app/Contents/Resources/bin/osgb-tiles`。Windows 文件名带 `.exe`。

App 默认只在本机监听。服务部署时显式设置 `--addr`、`--token` 与 `--data-dir`。例如 Docker 映射输入、输出和状态目录，并用自己的令牌替换镜像默认值：

```bash
docker run --rm -p 8990:8990 \
  -v /data/models:/data -v /data/app-state:/state \
  osgb-tiles-app:local --headless --addr 0.0.0.0:8990 --token YOUR_TOKEN --data-dir /state
```

正式包从自己的 `bin/` 或 macOS `Contents/Resources/bin/` 找 native，不回退 PATH 中的其他版本。`--sidecar` / `OSGB_TILES_BIN` 仅用于显式开发覆盖；指定路径错误会直接报错。App 验证 native 协议和默认值，最终报告缺失或与任务参数不符也会失败。

## 一次构建整个产品

在仓库根目录执行 `app/scripts/build.py`。脚本依次构建 native、运行原生回归、收集运行时、审核依赖、运行 Go 测试，再调用 MyGo 或输出便携目录。禁止只编译 Go 后宣称完成平台分发。

构建需要目标平台 C++17、CMake、Ninja、Python3、Go（版本见 `go.mod`）、OSG 3.6.5、libpng、libxml2、SQLite 开发库与 sqlite3 工具。PROJ 9.6.2 固定下载并静态嵌入数据库；json 与 meshoptimizer 已随 native 源码提供。Node 不参与转换。

macOS：

```bash
brew install openscenegraph libpng libxml2 sqlite ninja
python3 app/scripts/build.py --sdk-prefix "$(brew --prefix)"
```

Linux（Debian 12/Ubuntu 24.04 系列）：

```bash
sudo apt-get install cmake ninja-build g++ python3 patchelf sqlite3 \
  libsqlite3-dev libopenscenegraph-dev libpng-dev libxml2-dev
python3 app/scripts/build.py --sdk-prefix /usr --skip-desktop
```

Windows 在 x64 Native Tools / VS Developer PowerShell 中构建；依赖清单和固定 baseline 位于 `app/sdk/vcpkg.json`：

```powershell
git clone https://github.com/microsoft/vcpkg app/sdk/toolchain
git -C app/sdk/toolchain checkout 84bab45d415d22042bd0b9081aea57f362da3f35
app/sdk/toolchain/bootstrap-vcpkg.bat -disableMetrics
app/sdk/toolchain/vcpkg.exe install --triplet x64-windows --x-manifest-root=app/sdk --x-install-root=app/sdk/installed
python app/scripts/build.py --sdk-prefix app/sdk/installed/x64-windows --toolchain app/sdk/toolchain/scripts/buildsystems/vcpkg.cmake
```

`--skip-desktop` 生成 `app/dist/<平台>/portable/`，包含浏览器/HTTP 外壳和完整 native 运行时。桌面模式使用 MyGo 生成平台包；Windows WebView2、Linux WebKit/桌面服务及操作系统基础库属于目标平台要求，需要在真实设备上验收。独立分发第三方转换 SDK，不等于脱离操作系统基础环境。

## 资源与 SDK 合同

SDK 安装前缀仅在编译、收集阶段使用；分发资源统一位于：

```text
app/resources/<goos>-<goarch>/
  bin/osgb-tiles[.exe]
  bin/native-runtime.json
  lib/…                                  # macOS / Linux
  lib/osgPlugins-3.6.5/…                  # macOS / Linux
  bin/*.dll + bin/osgPlugins-3.6.5/…       # Windows
  licenses/…                               # 产品、PROJ、SDK 与随源码依赖许可
```

MyGo 将平台目录内容复制到 App 资源中；便携版直接复制同一目录。清单记录协议、能力、许可以及全部受管资源 SHA-256。native 按自身路径加载随包插件；macOS 改为 `@loader_path`，Linux 写入相对 RPATH，Windows 的依赖与主程序同目录。构建脚本会对传递库做依赖闭包审核。必须一起分发库、插件和许可。

```bash
# 核验便携目录、.app 或包含这些包的 dist 目录
python3 app/scripts/build.py --verify-package app/dist/darwin-arm64
# CI 单独调用 MyGo 后：刷新签名后的 Mach-O 哈希、封存并核验
python3 app/scripts/build.py --finalize-package app/dist/darwin-arm64
```

详细闭包、CRT、RPATH、许可和验证边界见[分发合同](docs/DISTRIBUTION.md)。Windows 的离线解析测试和 CI 配置不代表 Windows 已完成 native、全模型或桌面验收。

开发构建与分发资源目录不是两套算法。`native/build` 可单独用于原生回归；正式入口以 `build.py` 收集后的 native 为准。若重用 PROJ 源码缓存，可传 `--proj-source`；若只重新收集已构建产物，可传 `--stage-only --native-build <目录>`，之后仍会运行 Go 检查并打包。

## 真实一致性验证

```bash
python3 app/tests/verify_parity.py \
  --package app/dist/darwin-arm64/portable \
  --input /data/osgb --evidence .agents-debug/sessions/parity
```

该验证清除 OSG/PROJ/DYLD 等开发环境，使用包内 App 自动找到同包 native；同一输入分别通过 CLI 默认值和 App 默认值转换，逐文件比较主清单、GLB、图片及诊断清单，并比较除耗时/RSS采样外的报告。大样本另验证运行中真实进度与合作取消后的暂存清理。macOS 也可把 `--package` 指向 `.app`。

当前完整模型已通过 5,425 个资产文件哈希一致性检查。该检查不代替画面与资源验收。协议与接口分别见 [NATIVE-CONTRACT](docs/NATIVE-CONTRACT.md) 和 [HTTP-API](docs/HTTP-API.md)。
