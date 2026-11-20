# 更新日志

## 0.6.0

- 桌面应用（Windows `.exe` / macOS `.app`）双击启动后自动开启内置 HTTPS 兼容服务（`https://0.0.0.0:42225/task/create`），无需额外命令行脚本。
- 新增基于 Ed25519 签名验证的应用内自动更新功能，并支持通过嵌入式只读 GitHub 令牌读取 Release 清单与更新包。
- “运行环境”页面新增 HTTPS 兼容服务监听状态显示及“检查更新”按钮。
- 修复简体中文 Windows 路径下转换器因代码页导致的启动失败问题，并嵌入 UTF-8 `activeCodePage` 清单。
