# Windows 发行与自动更新

此分支的自动更新清单固定指向 [ReasonW6/tiez-clipboard-WX Releases](https://github.com/ReasonW6/tiez-clipboard-WX/releases/latest/download/latest.json)。主配置使用 NSIS，发行配置启用更新包签名。

## 本机签名密钥

此工作区已生成独立密钥，私钥位于 `.tauri/updater.key`，公钥位于同目录的 `.pub` 文件，公钥内容已写入 `src-tauri/tauri.conf.json`。

`.tauri` 被 Git 忽略，其访问权限限定为创建密钥的 Windows 用户和 SYSTEM。私钥是正式签名材料，应单独备份，不要提交到仓库。本次生成的私钥未设置口令。其他机器应恢复这份密钥；不要为已安装的客户端随意更换签名公钥。

在当前 Windows 用户的 PowerShell 中运行：

```powershell
npm ci
npm test
npm run test:rust
npm run release
```

脚本优先使用 `TAURI_SIGNING_PRIVATE_KEY` 环境变量；未设置时使用本机 `.tauri/updater.key`。受限沙盒无法读取此私钥时，应以拥有该文件的 Windows 用户执行发行构建。

安装包和对应 `.sig` 文件生成在 `src-tauri/target/release/bundle/nsis`。普通 `npm run tauri:build` 不要求签名私钥，适合仅本机安装；用于自动更新的发行包必须执行带签名的发行流程。

## GitHub Actions

1. 在 fork 的 Actions secrets 中配置 `TAURI_SIGNING_PRIVATE_KEY`，值为本机私钥文件的完整内容。若以后使用带口令的密钥，再配置 `TAURI_SIGNING_PRIVATE_KEY_PASSWORD`。
2. 同时更新 `package.json`、`src-tauri/Cargo.toml`、`src-tauri/tauri.conf.json` 中的版本，更新锁文件。
3. 推送版本标签，例如 `v0.3.5`，或手动运行 `Publish Windows Release` 工作流。
4. 工作流运行测试、构建 NSIS 安装包、生成签名及 `latest.json`，并把安装包和便携包放入发行草稿。
5. 检查草稿附件，确认存在安装包、签名和 `latest.json`，再发布为正式发行版。草稿和预发行版本不会成为当前配置的 `latest` 更新源。

更新清单中的下载地址由 `tauri-action` 根据当前 fork 和版本标签生成。签名私钥需与客户端内置公钥配对。本次代码修改没有上传私钥、创建远端 secret、推送代码或发布发行版。

实现依据见 [Tauri 更新插件文档](https://v2.tauri.app/plugin/updater/) 和 [tauri-action v0 配置](https://github.com/tauri-apps/tauri-action/blob/v0/action.yml)。

## 便携包

```powershell
npm run build:portable
```

输出为 `artifacts/portable/tiez-portable.zip`。打包脚本创建独立暂存目录并在完成后清理，只包含 `TieZ.exe`、空的 `data` 目录、许可证和使用说明，不包含本机历史、附件或签名密钥。

便携包复用普通构建，保留 Windows 账户级的敏感内容保护。更新插件安装的是 NSIS 安装版；继续使用便携目录时，手动替换新版 `TieZ.exe`，保留原有 `data`。
