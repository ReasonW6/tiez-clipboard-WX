# 主题、窗口状态与标签修复验证

## 修复范围

- 原生窗口通过 Tauri/Tao 的 preferred theme 锁定手动明暗模式，并同步 DWM 材质。前端命令使用正确的 camelCase 参数，按顺序提交，避免快速切换时旧请求覆盖新选择。启动时先读取已缓存的颜色模式，SQLite 设置加载后再校准。
- 热键、关闭按钮、失焦隐藏、粘贴后隐藏、边缘停靠隐藏统一通知前端重置临时页面状态。仅失去焦点而未隐藏的固定窗口继续保留当前页面。历史数据、标签及用户偏好不在重置范围内。
- 剪贴板采集去重时保留原记录的标签、敏感标记、置顶信息和使用次数。更新记录与标签关联在同一事务内完成，随后才执行容量清理。数据库查询失败时停止本次分发，避免展示未保存的记录。
- 标签页小于等于 620px 时使用顶部标签栏，更宽时使用侧栏。标签切换和刷新使用请求序号防止过期响应覆盖当前内容；读取失败保留已有条目并显示错误。编辑及批量粘贴按 ID 读取完整内容，避免使用列表截断预览。
- 标签页仍保留原有的删除语义：删除标签会删除该标签下的所有条目，操作前有明确确认；单条删除和粘贴是独立按钮。
- 重做云母和毛玻璃材质，加入液态玻璃及单一透明度滑块。适配明暗模式、Windows 10 实色降级、减少透明度与减少动态效果。

## 自动检查

本次执行结果：27 项前端测试、72 项 Rust 测试、43 项浏览器回归通过，TypeScript 与 Vite 生产构建通过。Rust 最终全量测试使用正常 Windows 用户凭据验证 DPAPI。

- `npm test`：前端单元测试及设置页渲染。
- `npm run test:ui`：Playwright 浏览器回归。覆盖全部七款主题与相反系统明暗、快速切换、关闭重置、固定窗口失焦、250/320/352/425/780px 标签布局、响应乱序、失败时保留内容、粘贴参数、编辑/删除命中区域、长文本与设置重载。
- `cargo test --offline --manifest-path src-tauri/Cargo.toml --bin tiez-app`：Rust 回归，包含真实内存 SQLite 中的去重、标签索引、容量清理及 DPAPI 密文检查。Windows DPAPI 检查需要有效用户凭据，受限沙盒账户可能无法执行；请在正常 Windows 用户会话运行。
- `npm run build`：TypeScript 与 Vite 生产构建。

首次运行浏览器测试需要 `npx playwright install chromium`。也可通过 `PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH` 指定已有的 Chromium。测试页面仅存在于 tests/ui/harness.html，不会进入 Vite 正式构建入口。所有 Tauri 调用都被拦截，只使用虚构数据，不访问真实剪贴板、文件或数据库。

本地发行构建和签名校验的结果记录在 artifacts/build-verification.json，界面对比预览位于 artifacts/ui-preview/index.html。源码提交或合并不会自动更新已有安装包；需要重新运行 npm run release 和 npm run build:portable 后，再验证签名及便携包内程序的一致性。

## 合并前审查补充

- 通过 Tauri MockRuntime 和正式配置复现了 `window.theme` 与 `event.emit_to` 的权限拒绝；补齐主题查询和预览通信权限后通过，同时验证未列出的窗口仍被拒绝。测试不会启动原生 WebView2。
- 启动时为实际数据目录中的 attachments 和 emoji_favorites 恢复资源访问权限，并恢复用户选定背景文件的单文件权限。使用 Tauri 实际路径匹配器验证便携路径、目录名中的方括号、背景路径恢复及数据库访问隔离。
- 标签输入框在鼠标按下时重新激活原生窗口，避免 DOM 焦点未变化时无法重新获取键盘输入。浏览器回归确认不会连带触发粘贴；新增最小尺寸与默认尺寸的布局检查。

## 材质边界与人工检查

液态玻璃参考 [Apple Materials](https://developer.apple.com/design/human-interface-guidelines/materials)，把玻璃用于导航和控件；云母与毛玻璃参考 [Microsoft Mica](https://learn.microsoft.com/en-us/windows/apps/design/style/mica) 和 [Acrylic](https://learn.microsoft.com/en-us/windows/apps/design/style/acrylic)。Windows 原生背景与 CSS 控件材质共同实现外观，不包含 Apple 平台的原生折射渲染器。液态玻璃的透明度滑块联动底色、应用内模糊、折射与高光强度，内部模糊范围为 0–64px。Windows 原生桌面磨砂半径固定；完全通透时关闭原生背景磨砂。

浏览器预览可以核对布局和网页颜色，不能证明 DWM 桌面材质、实际粘贴目标或开机自启动行为。发布后可在正常用户会话选择毛玻璃浅色、保持系统深色，再重启检查自启动；同时复核固定与非固定模式下的标签粘贴及重新呼出主页。本次未重启用户电脑，也未操作其真实剪贴板或数据库。

## 材质与快捷键二次调整

- 液态玻璃仅保留一个 0–100% 透明度滑块，从磨砂到清透连续调整；旧独立模糊配置不再参与渲染，现有透明度值保留。不透明度映射扩大到完全清透至实色，窗口底板、内容面板与控件同步变化。云母、毛玻璃、液态玻璃与樱花的浅色、深色端点及重载均有覆盖；清除深色模式遗留的固定 84% 不透明顶栏与内容底色。
- 液态玻璃以 SVG 位移滤镜折射边缘背景，指针高光平滑跟随，按钮有按压回弹与光波，筛选选中背景和页面切换使用弹簧过渡。文字与图标不经过位移滤镜。指针停止后动画帧停止，隐藏、切换主题与减少动态效果会清理光波和监听。
- Windows 11 的云母、毛玻璃、液态玻璃与樱花主题由 DWM 统一裁切窗口，网页填满原生客户区，不再叠加 14/18px 的第二套窗口圆角；其他系统保留网页圆角。
- 修复 Windows 快捷键强制使用 macOS 符号的问题，改为带分隔符的键名，并将 Command/Meta 等已存储别名显示为 Win。250px 与 352px 下验证显示、录制与不溢出。
- 专项测试确认滤镜开关确实改变渲染像素，且按钮标签不变；另覆盖原生材质清除、启动时恢复清透设置、无障碍模式与隐藏后的动效清理。
- 设计参考 [Apple Meet Liquid Glass](https://developer.apple.com/videos/play/wwdc2025/219/) 中的折射与交互响应原则；实现使用现有 React/Framer Motion、CSS 与 SVG，无新增运行时依赖。

预览位于 artifacts/material-preview/index.html，包含通透/磨砂对比、明暗模式、快捷键及交互视频。预览使用虚构数据与测试背景，不包含原生桌面合成；本轮未执行真实 Windows 窗口的人工交互或重启验证。
