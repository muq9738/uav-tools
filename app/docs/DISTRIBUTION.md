# 分发合同

本文约束 `app/scripts/build.py`、平台 CI 和最终 App/便携包。转换算法只在 `native/` 中维护；App 与命令行入口必须执行同一资源目录里的 `osgb-tiles`，不得各带一套转换器。

## 包内资源

```text
<资源根>/
  bin/osgb-tiles[.exe]
  bin/native-runtime.json
  lib/...                         # macOS / Linux
  lib/osgPlugins-*/...            # macOS / Linux
  bin/*.dll                       # Windows，含非系统依赖和 MSVC CRT
  bin/osgPlugins-*/...            # Windows
  licenses/...                    # 产品及第三方许可/版权声明
```

- SDK 前缀、vcpkg、Homebrew、发行版开发包和 Visual Studio 只用于构建与收集，不是用户部署前提。操作系统基础组件和桌面 WebView/显示服务不属于转换 SDK 闭包。
- `native-runtime.json` 使用相对路径列出 OSG 插件，并记录 `bin/`、`lib/`、`licenses/` 中每个受管文件的 SHA-256；清单自身不哈希自身。缺失、额外或内容被修改的受管文件都必须使核验失败，路径不得逃逸资源根。
- 构建后在资源目录、便携目录和桌面包内分别核验哈希。macOS 桌面包还需通过 `codesign --verify --deep --strict`。哈希核验不是转换正确性的替代品，真实模型一致性仍由 `app/tests/verify_parity.py` 验证。

## 平台加载规则

### Windows / MSVC / vcpkg

- 在目标架构的 MSVC 开发环境中原生构建；`dumpbin` 优先取 `PATH`，否则通过 `vswhere` 定位。SQLite 工具只取 SDK 的 vcpkg `tools/sqlite3` 或 SDK `bin`，并显式传给 PROJ 的 `EXE_SQLITE3`，不静默换用开发机另一份 SQLite。
- 从 PE 导入表递归收集所有非系统 DLL。DLL 查找不依赖大小写，支持 Unicode 构建路径；找不到或同名不同内容均失败。
- 从 `VCToolsRedistDir` 或与 `dumpbin` 工具集匹配的 `VC/Redist/MSVC` 收集目标架构 Release CRT。`VCRUNTIME*`、`MSVCP*` 等不是操作系统白名单。OSG 的 `osgdb_osg`、`osgdb_serializers_osg`、JPEG/PNG 插件必须来自同一 SDK 并随包加载。
- Windows 目前只有 Go 外壳编译历史。解析测试和 CI 配置都不构成 Windows native、完整模型或桌面验收；只有 Windows runner/设备上的实际结果才能改变这一状态。

### macOS

- 所有非系统 Mach-O 依赖改写到 `@loader_path` 相对位置；解析 `@rpath` 时同时使用调用二进制的 `LC_RPATH`、`@loader_path` 和 `@executable_path`。不得遗留 Homebrew 等构建机绝对加载路径。
- 所有随包 Mach-O 文件在路径修正后逐个 ad-hoc 签名。MyGo 深度签名后，`--finalize-package` 只允许刷新签名导致的 Mach-O 哈希变化，再以非递归方式重签顶层 `.app` 封存新清单；非代码资源变化会失败。最终校验整体签名。CMake 构建 RPATH 从实际链接目录产生，不写死 `/opt/homebrew` 或 `/usr/local`。

### Linux

- 同时支持发行版多架构目录（例如 `lib/x86_64-linux-gnu`、`lib/aarch64-linux-gnu`）中的 OSG 插件，宿主多架构目录优先；同名冲突不得静默任选。
- native 使用 `$ORIGIN/../lib`，普通库使用 `$ORIGIN`，插件使用 `$ORIGIN/..`。最终 `ldd` 审核中的每个非系统依赖都必须解析到包内。glibc/loader 保留为目标系统 ABI 基线，其他传递库（包括 libstdc++、OpenGL/X11 栈）按实际导入闭包收集。

## 许可与验证边界

产品许可、随源码提供的 nlohmann/json 与 meshoptimizer 许可、PROJ 许可，以及实际 SDK/依赖包提供的 copyright/LICENSE/COPYING 均复制到 `licenses/`。vcpkg 使用安装树的 `share/*/copyright`，Homebrew 按实际二进制的 Cellar 来源收集，Debian/Ubuntu 按 `dpkg-query` 的文件归属收集；找不到实际依赖的许可不得静默宣称收集完整。

`app/tests/test_distribution.py` 只使用 Python 标准库。它验证导入表/RPATH 解析、Unicode 与大小写定位、多架构插件布局、CRT 分类、许可复制和哈希篡改拒绝，不伪造任何平台运行结果。各平台 CI 还必须原生构建、运行 native/Go 测试、核验完整包，并用测试生成的真实 OSGB 输入执行 App/CLI parity。所有构建并行度最多为 2。
