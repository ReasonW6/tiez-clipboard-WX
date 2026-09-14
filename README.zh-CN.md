# TieZ Clipboard WX

基于 [jimuzhe/tiez-clipboard](https://github.com/jimuzhe/tiez-clipboard) 的 Windows 自用剪贴板工具。

保留剪贴板历史、文本和富文本、图片、文件路径、搜索、标签、置顶、顺序粘贴、外部编辑、隐私脱敏、表情收藏以及七款内置主题。已移除 AI、云同步、MQTT、局域网传输、主题商店和公告。

自动更新使用 [ReasonW6/tiez-clipboard-WX 的发行版](https://github.com/ReasonW6/tiez-clipboard-WX/releases)，并校验更新签名。

## 开发

需要 Windows 10/11 x64、Node.js 22.12+ 或 24、Rust MSVC 稳定版、Visual Studio C++ 生成工具和 WebView2。

    npm ci
    npm run tauri:dev

dev 脚本仅启动前端，端口为 1420；剪贴板和系统交互需要完整 Tauri 应用。

## 验证与打包

    npm test
    npm run build
    npm run test:rust
    npm run tauri:build
    npm run build:portable

默认生成 NSIS 安装包，不要求更新签名私钥。release 脚本生成带更新签名的安装包，具体步骤见[发行说明](docs/RELEASING.md)。

便携包输出到 artifacts/portable/tiez-portable.zip，解压后运行 TieZ.exe，数据保存在相邻的 data 目录。敏感剪贴板内容仍由当前 Windows 账户保护。自动更新使用安装版；如需继续使用便携目录，请手动替换新版可执行文件并保留 data。

## 开发入口

- src/features：界面及功能状态。
- src/shared/hooks：历史、设置、键盘导航、剪贴板事件和更新。
- src/shared/config/themes.ts：内置主题注册表。
- src-tauri/src/main.rs：桌面启动和 Tauri 命令注册。
- src-tauri/src/services/clipboard：采集、识别、转换、去重和存储流水线。
- src-tauri/src/services/clipboard_ops.rs：写入剪贴板、恢复焦点和粘贴。
- src-tauri/src/infrastructure/repository：SQLite 操作及数据库升级。

数据库升级 11 会清理已删除功能的设置和同步元数据，保留历史、标签和附件。旧商店主题会回退到云母主题；旧主题商店令牌、缓存样式和公告缓存会在启动时清理。

许可证为 [GPL-3.0](LICENSE)，保留原项目署名和历史。

### 材质主题与界面测试

云母、毛玻璃和液态玻璃支持浅色、深色与跟随系统。液态玻璃可在「界面设置」中调整通透度（0–100%）和控件模糊度（0–32px）。窗口背后的桌面材质由 Windows 提供；模糊度滑块调整应用内导航和控件的效果，不能修改 Windows 固定的桌面模糊半径。Windows 10 使用实色降级。

设计参考：[Apple 材质规范](https://developer.apple.com/design/human-interface-guidelines/materials)、[Microsoft 云母](https://learn.microsoft.com/en-us/windows/apps/design/style/mica)与[毛玻璃](https://learn.microsoft.com/en-us/windows/apps/design/style/acrylic)。

首次执行 `npx playwright install chromium`，然后运行 `npm run test:ui`。测试使用虚构条目并拦截全部 Tauri 命令，不访问真实剪贴板和数据库。也可通过 `PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH` 指定已有的 Chromium。检查范围见[验证说明](docs/UI-VERIFICATION.md)。
