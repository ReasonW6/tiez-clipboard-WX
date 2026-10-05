# 自有 DWM 场景桥探针

本探针独立验证 `私有 DWM shared visual → 自有 helper HWND → 公共 WGC → D3D11 shader` 的像素来源、光学及背景相关效果。当前自有材质样板采用 `Composition BackdropBrush + GaussianBlur`，早期版本曾使用系统 Acrylic 和 Accent blur。它不接入主应用，不读取剪贴板数据，不证明完整桌面材质或苹果视觉一致性。

**第十六轮已通过当前自有场景内的 F6 重叠排除实验。** output／helper 均无 owner、cover 归 helper 时，输出在固定 A 采样区上方持续约 76 秒，镜像和七控件阶段都有通过原始四色标记的新输入，最终 2,253／2,253 帧有效。第十四轮镜像输入、第十五轮洋红 cover 输入与本轮成功构成 owner 关系的受控对照，支持归属关系影响本机结果；这不证明全部 DWM 内部语义，也不代表生产接入或任意桌面捕获已经可用。版本与证据边界见本文末尾。

第十八轮进一步在冻结来源、重叠输出及 blur 12 → 0 → 12 的新内容更新后，对完整 320 × 360 区域取得 115,200 像素 RGBA 差异全 0，覆盖源模糊和清晰文字；第十七轮受 QA 光标光晕污染的对照保留在后文，不能与干净重测混写。

第十九轮工具窗样式下的采集与停止录像已通过。可选 WebView 的第二十轮首测及第二十一／二十二轮复测分别记录前景输入、按键和启动录像；这些结果与普通探针分开记录。圆角版本第二十三轮仍露出系统黄色捕获边框，第二十四轮在既有 Allowed 状态下去除该边框后又暴露镜像黑列，第二十五轮修正 region 范围后取得 64,880 个被比较像素 RGBA 差异全 0。该比较排除了刻意不镜像的 WebView 前景区，不是圆角版本的整图保真或 Apple 外观验收。

## 控制范围

程序先在主显示器显示自有、全屏、不透明的测试底窗和 A 区样板，等待 F1，不立即创建采集组。A 区是固定的 320 × 360 像素图案及一个自有 Composition 背景模糊窗口。F1 启动后，helper、cover、output 共用 C 区的同一个 320 × 360 矩形；不再有单独可见的 B 区。当前诊断版 output 和 helper 均为无 owner 的独立顶层窗，cover 的 owner 仍为 helper；三窗属于本进程，实际层次必须为 `output > cover > helper`。第十二至十四轮的 output 和 helper owner 都为 base，第十五轮仅 output 无 owner，对应结果不能混写为当前版本的运行证据。

初始化先向 output 的 swapchain 提交不透明占位帧，读回确认全部 alpha 为 255，显示并等待 DWM 完成；再通过 `DeferWindowPos` 将 helper 和 cover 一并放到 output 下方。检查实际归属、三窗矩形和层次后，才挂载私有 shared scene。cover 使用纯洋红诊断填充，便于在录像中检测意外露出；正常输出始终应遮住它。

私有场景始终按 A 区固定的 `sourceRect` 裁切，不接受 API 返回的裁切或目标尺寸变化。F6 仅在采集组启动时，将整个三层组在 C 与 A 之间移动；移动后重新检查矩形和层次，并强制刷新 shared-scene 配置。移动不会改变 A 区裁切或排除规则。所有外部 HWND 仅枚举标识、可见性、遮挡和几何以构造排除表，不读取窗口标题或界面文字。helper、cover、output 始终在排除表中。每轮检查底窗覆盖范围、三窗归属／几何／层次及外部窗口对 A 区的遮挡，失败时停止采集并安全拆除辅助组，保留 base 和 A。

WGC 只指定 helper HWND。每帧要求固定 BGRA8 尺寸并验证四角彩色标记，未通过的帧不会替换已验证纹理。默认 output 为完整矩形；可选圆角构建只裁切三个 HWND 的可见 region，swapchain 仍为 320 × 360。shader 先保留原始场景，再按共享光学 coverage 混合控件折射和高光；控件外仍显示原始场景，最终 alpha 固定为 1。每次 Present 前读回实际 swapchain backbuffer，扫描全部 115,200 个像素，任何 alpha 不为 255 都会停止辅助组。该检查不覆盖 HWND 裁切后的屏幕边缘。四角标记用于确认裁切及场景布局，不能独立证明任意桌面的内容隔离；全屏测试底窗、排除表和遮挡检查共同限定本次受控实验。测试期间保持在自有测试界面内，不将外部应用移入 A 区。

F1 停止辅助组时，先关闭 WGC session 和 frame pool，再拆除 shared scene、恢复 helper 的私有属性并销毁 helper／cover，最后才解除和销毁 output。WebView 构建还会先关闭前景 controller，再执行上述采集清理。base 和 A 继续显示，便于先停止测试区域录像，再退出整个程序。F1 再次启动时恢复 C 区位置。启动、采集或移动异常也只拆辅助组；Escape、窗口关闭和自动退出计时器才清理全部窗口，普通构建为 120 秒，当前 WebView 构建为 180 秒。

程序检查所有自有窗的 `GetWindowDisplayAffinity` 为 `WDA_NONE`，不设置全局截图保护或 `WCA_EXCLUDED_FROM_DDA`。只在被 cover 遮住的 helper 上设置原始 shared-scene 示例要求的 `WCA_EXCLUDED_FROM_LIVEPREVIEW`，退出时恢复；最终 output、底窗及材质样板均不设置该属性。非圆角构建保留 WGC 默认捕获边框。当前圆角构建只通过 `AppCapability.CheckAccess` 查询既有 `graphicsCaptureWithoutBorder` 状态，结果为 Allowed 才设置会话 `IsBorderRequired(false)`；不调用权限请求 API、不弹授权提示、不更改系统设置。查询和 setter 均不能替代实际屏幕边缘验证，适用边界见第二十四轮。

验收工具自身的透明光标标记始终加入排除表。仅当窗口类精确为 `CodexComputerUseCursorOverlay`，并同时具有已观测的 layered、transparent、no-activate、tool-window 四个样式位时，不把它误判为外部应用遮挡；其他外部窗口继续触发停止。

## 构建与操作

在仓库根目录运行下列命令仅编译，不启动窗口或捕获。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File src-tauri/examples/build-dwm-scene-probe.ps1
```

输出为 `artifacts/material-redesign/dwm-scene-probe/dwm_scene_probe.exe`，构建证据记录在同目录 `build-evidence.json`。启动和实际画面检查属于单独的已授权测试步骤。

可选 WebView 前景使用独立输出目录编译，不覆盖普通探针：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File src-tauri/examples/build-dwm-scene-probe.ps1 -WithWebView -OutputDirectory artifacts/material-redesign/dwm-scene-webview-probe
```

该命令仍只编译，生成 `artifacts/material-redesign/dwm-scene-webview-probe/dwm_scene_probe.exe`。构建脚本查找本机 NuGet 缓存中的 WebView2 native SDK；也可用 `-WebViewSdkRoot` 指定包含 `include/WebView2.h` 和 `x64/WebView2LoaderStatic.lib` 的 `build/native` 目录。独立目录避免覆盖普通构建的二进制、日志及构建证据。

WebView 构建在 F1 后为 output 创建普通透明子窗，原生 shader 位于子窗后方，页面以 `NavigateToString` 加载内嵌测试文字、输入框和 Count 按钮。测试 profile 位于该 EXE 同目录的 `webview-profile`，不使用主应用 profile。页面不加载外部内容，固定 `sourceRect`、采集 helper 和排除规则不变。`webview_ready` 只表示本地页面导航完成；swapchain alpha 检查针对背景纹理，不包含 WebView 子窗的最终前景合成。

圆角实验默认关闭，使用以下独立输出目录启用：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File src-tauri/examples/build-dwm-scene-probe.ps1 -WithWebView -RoundedRegion -OutputDirectory artifacts/material-redesign/dwm-scene-rounded-probe
```

`-RoundedRegion` 定义 `TIEZ_PROBE_ROUNDED`，为 helper／cover／output 在首次显示前分别设置相同的名义 24px 半径 region。当前实现采用右／下边界 `width + 1`、`height + 1` 创建区域，并用 `GetRgnBox` 确认有效范围恰好为 320 × 360，再以 `GetWindowRgn`／`EqualRgn` 校验三窗实际区域；修正前的版本见第二十三／二十四轮。成功交给 `SetWindowRgn` 的 HRGN 由系统持有，scope 读取的是独立副本。固定 sourceRect、sentinel 坐标、shader 和 alpha 检查保持不变。GDI 边界裁切不等于 Apple 平滑圆角，也不自动保证裁切后像素正确。

| 快捷键 | 行为 |
| --- | --- |
| F1 | 在 C 区启动／停止 helper、cover、output 辅助组；base 和 A 保持显示。 |
| F2 | 切换 A 区 GaussianBlur 12／0，辅助组存在时强制刷新 shared-scene 配置。 |
| F3 | 切换 output 的水平镜像 shader；冻结期间也可用上一份已验证纹理重绘。 |
| F4 | 冻结／恢复自有图案动画，保留采集轮询和 shader 控制。 |
| F5 | 在原始纹理和共享光学模块的七种控件曲面之间切换，整个输出仍保持不透明。 |
| F6 | 仅在辅助组启动时，将三层组在 C 和固定 A 区之间切换，并刷新 shared scene。当前 owner 关系下的第十六轮受控实验通过；第十四／十五轮失败对照保留。 |
| Escape | 关闭会话、图像池和全部自有窗口。 |

程序要求实际线程 DPI 上下文为 Per-Monitor V2，并拒绝 Windows 11 build 22621 之前的系统。周期定时器启动失败立即结束；普通构建在进程启动后 120 秒自动退出，当前 `-WithWebView` 构建为 180 秒，等待 F1 和停止辅助组期间仍计时。每次采集启动后一直没有首帧会超时停止辅助组；初始空帧可以短暂等待，持续 sentinel 失败也停止辅助组。第二十轮首测使用改为 180 秒之前的旧 WebView 构建，实际计时仍为 120 秒。

WebView 焦点下的 F1–F6／Escape 已实现由 accelerator 回调向 base 投递消息，但按键实现与验收状态分开：第二十轮仅验证 F3 和 F6，第二十一／二十二轮补充的控制与交互结果见对应章节。

## 验证标准

1. 三层组始终同矩形并满足 `output > cover > helper`；启动占位和正常输出均不透明，洋红 cover 与未验证的私有场景不得露出。
2. 首次 `valid_frame` 必须来自尺寸和四角标记都通过检查的纹理，不能仅凭 shared visual 创建成功判定桥接有效。
3. 镜像关闭、F5 关闭时，output 保留 A 区的四角位置、Composition 模糊和动态内容；F2 的变化应到达 output。否则只能记录“桥接未验证”或具体失败。
4. F4 冻结后按 F3，output 仍应立即镜像／还原。无新帧时的成功重绘记录为 `cached_texture: 1`，不会增加采集与验证计数。
5. 范围失效、裁切变化、alpha 检查失败或持续 sentinel 失败应停止辅助组并保留 base／A；停止时 helper／cover 必须先于 output 销毁。
6. 普通截图可见性、录像中的启动与停止、实际背景模糊保留效果必须分别观察确认，编译通过不代表这些条件通过。
7. F6 将 output 移到 A 上方后，开启 F3 镜像，继续检查新的 WGC 输入帧是否通过原始四色标记。仅显示先前缓存或只通过移动 API，不能证明重叠输出被排除且没有自反馈。

运行日志 `scene-evidence.jsonl` 每次启动覆盖，记录 API 结果、范围停止原因、DPI 检查、快捷键及帧计数。`output_alpha_readback` 定期记录实际 backbuffer 的全像素检查结果，结束时记录总检查次数。`output_group_moved` 记录移动完成时的帧计数和位置，后续 `valid_frame.output_over_source` 用于核对重叠状态下的新帧。来源标记失败时仅记录四个固定标记位置的 RGBA 值，不保存未验证的整帧。已有日志需要保留时，应在下一次启动前另存。

## API 边界

ordinal 163／164 的声明来自 [ADeltaX 的原始 API 记录](https://gist.github.com/ADeltaX/aea6aac248604d0cb7d423a61b06e247)。记录明确说明 include 列表无效，且旧版系统的 update 签名不同。本探针不将 include 列表当作允许名单，也不将当前版本检查当作私有 ABI 的长期兼容承诺。

helper 到纹理使用 [公开的 `IGraphicsCaptureItemInterop::CreateForWindow`](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createforwindow)。失败时不会移除 cover 或给最终输出启用截图排除属性。

## 历史第七轮：系统 Acrylic 失败记录，2026-09-14

Windows build 26200，验证版本 EXE SHA256 `01B3A7FFC3229EA8B0549EE404E113810F38F63EA31C23ECDC92DF26BC6865C6`。

- 不设置 helper 的 live-preview 属性时，创建 shared scene 成功，更新返回 `0x80070057`。空 include 列表重试仍相同。仅对 helper 设置此属性后更新成功；setter 成功，getter 不支持读回，不能声称读回已验证。
- 每轮重复更新相同 shared-scene 配置会伴随完全透明的无效帧。改为仅在排除集合变化时更新后，120 秒内收到 **2,537 帧，全部通过四角标记检查**，首次有效帧约 796ms。
- F4 冻结后 F3 镜像使 C 区文字、白条和四色标记反向。普通 Computer Use 截图可见完整输出。证明的是可采样纹理和自定义 shader 路径，不是 Apple 外观一致。
- **原生背景模糊保留仍未通过。** 本轮 A 区的系统 Acrylic 实际呈灰色实底；F2 后首次截图的 C 仍为旧材质，后续稳定画面未在自动退出前取得。因此不能从本轮推断真正的背景模糊或材质属性变化已完整复现。
- 自动退出正常完成，helper 属性恢复调用成功，没有剩余探针进程。

日志与构建记录位于 `artifacts/material-redesign/dwm-scene-probe/seventh-*`，视觉对照位于 `artifacts/material-redesign/qa/native-scene-bridge-*.jpg`。JPEG 仅用于视觉证据，不用于宣称像素精确一致。

## 历史第八／九轮：七控件接入与 Accent blur 失败记录

后续版本加入 F5，直接使用已经通过数值与 GPU 集成检查的 `glass_optics.hlsli` 和 `glass_probe_layout.hlsli`。VS／PS 使用 `fxc /Ges /WX /O3` 构建时编译并嵌入，不在运行时读取着色器源码。F2 的属性变化会重新配置 shared scene，F5 使用验证缓存重绘。

第八轮 EXE SHA256 `16651E2D525E57D9E01BB4866609ABCD86408D1D31D8C04299C92649FA7F8E1A`：F4 冻结后，F5 显示圆形、圆角方形、胶囊和竖向滑块；棋盘通过同一场景纹理发生曲面位移，输出仍可被普通截图看到。2019 个输入帧中 2018 个通过标记检查，材质重新配置后的一帧透明输入被丢弃、未呈现。日志与截图分别为 `eighth-controls-and-native-blur-evidence.jsonl`、`qa/native-scene-controls-integrated.jpg`。

同轮 native blur 样板按已有 `window-vibrancy::apply_blur` 路径请求 AccentState 3，但 A 区呈黑底，初始 C 区却是透明文字叠清晰棋盘。原生 blur 的效果未成立，且原始场景与采样结果不一致，因此不能通过“背景依赖效果保留”验收。之后只调整显示／激活生命周期的第九轮仍复现同样黑底；602 帧通过四角检查仅说明来源标记有效，不能覆盖这个视觉失败。记录在 `ninth-native-blur-lifecycle-evidence.jsonl` 和 `qa/native-blur-lifecycle-check.jpg`。

当时的下一步是使用 Composition GaussianBlur 背景样板，先确认源端真实模糊，再检查 shared-scene 是否保留；后续第十／十一轮的结果见下节。上述失败只描述对应旧版本，帧计数、API 成功或七控件轮廓不能替代背景效果的独立验证。当前探针仍不适合替换正式材质后端。

额外的屏幕捕获交叉验证排除了验收工具的歧义：对同一第九轮二进制，使用交互桌面的 `Graphics.CopyFromScreen` 仅截取自有测试区域 `(760,565,1040,360)`，PNG 同样显示源端黑底与 C 端透明棋盘的差异。记录为 `legacy-screen-capture-verification.json`，图像为 `qa/native-legacy-blur-screen-pixels.png`。普通沙盒中的首次屏幕读取返回“句柄无效”；在既有授权范围内通过正常审批读取交互桌面后成功。该测试的临时截图和临时 EXE 副本已经清理，保留正式 PNG 与日志。

## Composition 模糊保真与录屏，2026-09-14

第十／十一轮使用 EXE SHA256 `AB686F9B7691B528AF99FDABA5E1A719A92A6957DFF539ADB74AED75026F7DFD`。源样板替换为与生产 effect 描述等效的普通 `CreateBackdropBrush + GaussianBlur`；源 HWND 不含 GDI 黑底，标签在独立的透明 D2D surface 中绘制，作为背景的前景同级节点。

F4 冻结后分别设置 blur 12 和 0，使用交互桌面的 `Graphics.CopyFromScreen` 保存限定测试区域的 PNG。对照完整 A／C 两个 320×360 矩形：两种状态的 115,200 个像素均 RGBA 差异 0。正文以下区域的水平亮度步长从 2.685 降至 0.540，同时颜色亮度得以保留，图像也明确显示棋盘由锐利变柔和，不能把这一结果归结为黑底遮盖。数值为 `qa/native-composition-blur-screen-analysis.json`，图像为 `native-composition-blur12-screen.png`、`native-composition-blur0-screen.png`。

第十轮共收到 2519 帧、通过 2515 帧；四个无效输入被丢弃而未呈现，正常自动退出。第十一轮在动态背景与 F5 七控件模式下，使用已核对的单显示器固定区域分别执行 FFmpeg GDI 和 Desktop Duplication 录屏，每种约 4 秒、120 个解码帧。已查看两种视频的解码帧，均包含正确的控件光学与背景。输出区域的帧哈希发生变化只证明视频内容在变化，不代表 120 个独立桌面刷新帧或已达到特定原生帧率。

视频和哈希为 `qa/native-controls-gdi-recording.mp4`、`qa/native-controls-dda-recording.mp4`、`qa/native-recording-verification.json`，日志为 `tenth-composition-blur-evidence.jsonl`、`eleventh-recording-evidence.jsonl`。这些是当前设备、受控场景及两种实际捕获机制的证据，尚未覆盖全部录屏工具、跨屏、HDR、生产窗口接入或 Apple 动画一致性。

## 第十二／十三轮：同位置三层组与两段录像，2026-09-15

两轮使用 EXE SHA256 `2D8124A54A31D630568037DB8D920CE7602C7C4D7A81B2DF8EB6FAAC335A51D1`，构建记录为 `dwm-scene-probe/twelfth-build-evidence.json`。该版本具备 F1 和不透明整矩形输出，尚无 F6；三层组在 C，未与固定采样区 A 重叠。以下路径均相对于 `artifacts/material-redesign`。

| 证据 | 已覆盖阶段 | 解码帧与检测结果 |
| --- | --- | --- |
| `qa/native-coincident-lifecycle-dda.mp4` 与 `qa/native-coincident-lifecycle-analysis.json` | 启动前空闲、F1 启动、F5 控件切换；没有录到停止后的空闲画面 | 共 1,774 帧；第 1,029 帧首次检测到活动输出，持续到第 1,773 帧；洋红命中帧数 0。 |
| `qa/native-coincident-stop-dda.mp4` 与 `qa/native-coincident-stop-analysis.json` | 录制开始时输出已活动，覆盖 F1 停止和停止后的空闲画面；没有录到启动前状态 | 共 1,190 帧；第 0–1,069 帧为活动输出，第 1,070 帧起检测为空闲；洋红命中帧数 0。 |

表中的帧编号是从 0 开始的索引。两份分析都检查录像内 C 区 `(720, 0, 320, 360)`，洋红阈值为 `R > 190, G < 70, B > 140`。这是分别覆盖启动／控件和停止的两段录像，不能写成一段覆盖完整生命周期的录像。检测结果只约束实际解码帧，不能证明两个采样帧之间从未闪露，也不能推出所有录屏工具上的行为。

第十二轮日志 `dwm-scene-probe/twelfth-coincident-lifecycle-evidence.jsonl` 共收到 400 帧，400 帧通过原始四色标记；第十三轮 `dwm-scene-probe/thirteenth-coincident-stop-evidence.jsonl` 为 521／521。实际 swapchain 全像素 alpha 检查分别为 401 次和 522 次，均包含启动占位帧；周期输出的 20 条和 25 条检查记录，每条均扫描 115,200 个像素，最小／最大 alpha 都为 255，非不透明像素为 0，日志没有 alpha 失败。

两轮 `coincident_group_ready` 都在挂载私有场景之前确认三矩形相等、`output > cover > helper` 和预验证的不透明占位。停止日志先记录 `capture_stopped`，再记录 `helper_group_destroyed`，其中 helper／cover 已不存在而 `output_still_visible = 1`，随后才记录 output 也已销毁且 `source_visible = 1`。第十二轮停止组后仍在进程启动约 120 秒时自动退出，说明待机阶段计时器继续工作。以上日志支持有序清理；首段录像本身没有覆盖停止画面。

## 第十四轮：F6 重叠排除失败，2026-09-15

版本 EXE SHA256 `8E8499F0F19331460082011DB60588BCDD0B39B4FB18A2F17F9D4D514B0CB0A4` 加入 F6。证据为 `dwm-scene-probe/fourteenth-build-evidence.json` 和 `dwm-scene-probe/fourteenth-overlap-failure-evidence.jsonl`。

F3 镜像开启后，F6 将整个组移至固定 A 矩形 `(760, 565, 1080, 925)`。`output_group_moved` 记录移动前累计 342 帧、342 帧有效，并确认矩形、层次和 shared-scene 刷新完成。随后第 344、345、346 帧的候选标记按左上、右上、左下、右下依次为绿、红、黄、蓝，RGBA 分别为 `(36,210,132,255)`、`(232,44,88,255)`、`(236,192,40,255)`、`(44,104,232,255)`，与镜像 output 一致。三帧均记录 `presented = false`。最终累计为 346 帧、343 帧有效，说明移动附近还有一帧通过；这不足以证明重叠后的持续采集有效，也未排除在途旧帧。

程序以 `runtime_group_stop / 0x80004005` 停止采集，helper／cover 先销毁，output 随后销毁，base 和 A 保留并回到等待 F1。`qa/native-overlap-failure-analysis.json` 确认所见缓存与预期镜像的全部 115,200 像素 RGBA 差异为 0，同时明确状态为 `failed_overlapping_capture`。这只说明最后一份已验证缓存仍在显示，不能作为新帧通过的证据。当前结果证明重叠后输入已出现输出的镜像特征；尚不能仅凭 WGC 最终纹理区分污染发生在私有 shared scene 还是 WGC 合成阶段。

源码的 `ensure_scope` 将 helper、cover、output 全部加入排除 vector，`update_scene` 将该 vector 及其长度传给 ordinal 164；F6 还强制刷新相同的排除集合，没有发现直接漏传 output 的代码。私有 API 成功返回只能证明调用被接受，不能证明本次系统上排除语义满足需求。

另一条待核对的诊断路径是 `GraphicsCaptureSession.IncludeSecondaryWindows`。当前代码没有读写该属性；[微软文档](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.includesecondarywindows?view=winrt-26100) 写明它自 Windows 11 24H2／26100 提供，默认值为 false。开启后会纳入符合 popup／tool-window 样式、相交和连续层次条件的附属窗口，文档没有把同 owner 列为必要条件。因此不能把未显式设置此属性直接当作本次根因；需先取得实际会话读回值，再进行有界对照。当前 C 和 A 两种布局中的 helper／cover／output 本来就保持相同的相对矩形、层次和 owner，失败只在组移入固定采样区后出现，这更值得优先核对私有场景对重叠输出的排除行为，但仍是诊断方向。

## 第十五轮：无 owner output 后仍采到 cover，2026-09-15

EXE SHA256 `E7091B1DE004CD2E91C31C94C243D9AFC54CA13EB4DBC0ACC363E5F3AEAC71E8` 仅取消 output 的 owner，创建时传入 `nullptr`，`ensure_scope` 检查无 owner，日志记录 `output_owner = none`。helper／cover 的归属、三窗矩形与层次、排除列表、shader、alpha 检查和 F1 生命周期保持原状，没有同时设置 `IncludeSecondaryWindows`。

证据为 `dwm-scene-probe/fifteenth-build-evidence.json` 和 `dwm-scene-probe/fifteenth-unowned-output-failure-evidence.jsonl`。F3 开启后，F6 移入 A 时累计 345 帧、345 帧有效。第 347–368 帧共 22 个候选的四角全部为 RGBA `(255,0,255,255)`，与 cover 的诊断颜色一致；这些候选都被拒绝，`presented = false`。最终为 368 帧、346 帧有效，移动附近的一帧通过仍不能替代持续有效采集。错误输入不再是第十四轮的镜像四色，但重叠排除仍然失败。

日志在约 32,984ms 记录 `runtime_group_stop / 0x80004005` 和采集停止，33,000ms 确认 helper／cover 不存在且 output 仍可见，33,015ms 确认 output 销毁、source 保留并等待 F1。输入纹理中的洋红不等于屏幕上已发生 cover 泄漏；被拒绝的候选没有呈现。这一差异支持进一步检验 helper／cover 与 base 的 owner 分组关系，但尚不足以确定私有 API 的内部规则。

## 第十六轮：独立 owner 关系下的受控重叠成功，2026-09-15

EXE SHA256 `B486BD12133B96CBA0E716AC6175959949A0797722766BF63597E43997B26FEE` 在第十五轮基础上仅取消 helper 的 owner，创建时传入 `nullptr`，`ensure_scope` 检查无 owner，日志记录 `helper_owner = none`。output 保持无 owner，cover 仍归 helper；矩形、显式层次、原子遮盖、排除列表、shader、alpha 检查及销毁顺序均不变，也未设置 `IncludeSecondaryWindows`。

证据为 `dwm-scene-probe/sixteenth-build-evidence.json` 和 `dwm-scene-probe/sixteenth-isolated-overlap-evidence.jsonl`。37,609ms 时 F6 将三窗移至固定 A 区，计数为 500／500；113,922ms 移回 C 时为 2,124／2,124，重叠持续 76,313ms，期间计数增加 1,624。重叠时 F3 镜像保持开启，84,984ms 又开启 F5 七控件。原始镜像阶段的 47 条周期 `valid_frame` 记录，以及镜像加控件阶段的 29 条记录，均为 `output_over_source = 1`、`cached_texture = 0`，对应持续到达并通过原始四色标记的新输入。移回 C 后也继续通过验证；最终为 2,253 帧输入、2,253 帧有效，没有 `sentinel_failure`。

实际 swapchain 全像素 alpha 检查共 2,254 次，包含启动占位帧；105 条周期记录每次均检查全部 115,200 个像素，最小／最大 alpha 都为 255，非不透明像素为 0。120 秒自动退出时，日志仍先关闭采集、销毁 helper／cover，并记录 output 仍可见，随后才销毁 output。

约 20 秒的 Desktop Duplication 录像为 `qa/native-isolated-overlap-dda.mp4`，SHA256 `DB4F10499903CE2F89F7DA954CCB8C43624134917C7829E4CA8F7127F6B875E7`。`qa/native-isolated-overlap-video-analysis.json` 检查全部 1,187 个解码帧的 1,040 × 360 自有测试区域，未发现符合洋红阈值的像素帧。该区域包含始终可见的 A，因此分析中的活动状态不能用来证明启动或停止转场；本录像支持已录制阶段的遮盖与可见性，也不证明帧间瞬态、独立刷新帧率或所有录屏工具。

第十四至十六轮按顺序仅移除 output 的 owner，再移除 helper 的 owner：错误候选从镜像 output 变为洋红 cover，最后持续通过。这是本机、当前版本、固定自有采样区中的 owner 相关实验因果证据，支持这组无 owner 布局下的排除效果；尚不能据此定义私有 API 对所有窗口分组的通用规则。第十四／十五轮失败与对应构建证据继续保留。工具窗样式、实际隐藏、圆角、最小化、失焦、关闭跟随、WebView 前景和生产窗口接入仍需分别验证，也不会自动将 helper 改为 output 的 owned window。

## 第十七／十八轮：移出 QA 光标后的完整像素对照，2026-09-15

两轮仍使用第十六轮二进制，EXE SHA256 `B486BD12133B96CBA0E716AC6175959949A0797722766BF63597E43997B26FEE`。第十七轮 `qa/native-isolated-mirror-pixel-analysis.json` 记录 19,506 个不同像素；排查将差异定位到测试区域上方 `y < 120` 的 QA 光标光晕，模糊与文字主体 `y >= 120` 原本已逐像素一致。保留 `dwm-scene-probe/seventeenth-pixel-comparison-evidence.jsonl` 和 `seventeenth-build-evidence.json`，这次受污染的整图对照不记为通过。

第十八轮在两次截图前都把 pointer 移到采样矩形外的自有空白区域。先 F4 冻结并保存参考图，再 F1 启动、F3 镜像、F6 重叠，最后 F2 将 blur 12 切为 0 再恢复 12，以实际场景变化促使采集新内容。`qa/native-isolated-clean-pixel-analysis.json` 对 `native-isolated-clean-reference-screen.png` 的预期镜像与 `native-isolated-clean-mirror-screen.png` 做完整 320 × 360 比较：115,200 像素全部相同，RGBA 各通道最大误差均为 0，包含 native Gaussian blur 与源端清晰文字。

运行证据为 `dwm-scene-probe/eighteenth-clean-pixel-evidence.jsonl`，构建记录为 `eighteenth-build-evidence.json`。F6 后以及 F2 的 0／12 两个状态均有 `cached_texture = 0` 的有效新帧；最终 10 帧输入、10 帧有效，12 次输出 alpha 检查包含占位和缓存重绘，120 秒自动退出正常。这次结果与第十四轮仅保留正确缓存的截图不同，具有内容切换后的新输入证据；结论仍限于本机固定自有来源和已测状态。

## 第十九轮：helper／cover 工具窗样式与停止录像，2026-09-15

EXE SHA256 `518185C65CA285D7B0D32861142178A010198F27A98044A277215C05FB538559` 仅为 helper 和 cover 增加 `WS_EX_TOOLWINDOW`，output 样式保持原状，没有同时增加 `WS_EX_NOACTIVATE`。`ensure_scope` 每次检查两个工具窗样式位，缺失时停止辅助组；`coincident_group_ready` 和样式失败日志记录实际 `helper_extended_style`、`cover_extended_style`。

此构建仍要求 helper／cover 为可见 HWND，并由既有不透明三层组遮盖，没有调用 `SW_HIDE`，也没有改变固定采样范围、owner、层次、排除列表或 shader。`dwm-scene-probe/nineteenth-toolwindow-evidence.jsonl` 实测 `helper_extended_style = 2097288`、`cover_extended_style = 136`，两者均包含工具窗位；最终 1,477 帧输入、1,477 帧有效，1,478 次 alpha 检查包含占位，69 条周期 alpha 记录全部为 115,200 像素、最小／最大 alpha 255、非不透明像素 0，没有 scope 或 sentinel 失败。构建记录为 `nineteenth-build-evidence.json`。

20 秒 GDI 录像 `qa/native-toolwindow-stop-gdi.mp4` 的 SHA256 为 `C79F61BDED3EF5CB2853989C46EFD1A3797952BDC1C9CBEED1906145E37FAACB`。`qa/native-toolwindow-video-analysis.json` 对全部 599 个解码帧的 1,040 × 360 自有区域检测，洋红命中帧数为 0；来源标记在零基帧索引 506 从 `mirrored_output` 转为 `original_source`，确认录像覆盖了停止后的源画面恢复。对应前后截图为 `qa/native-toolwindow-before-stop.png` 和 `qa/native-toolwindow-after-stop.png`，已由本轮操作者检查。整幅区域始终包含 A，所以通用 active 字段不能单独用作停止证据；这里使用的是来源标记转换。

停止日志仍确认 helper／cover 已销毁时 output 可见，随后 output 销毁而 source 保留。工具窗不出现在任务栏和 Alt+Tab 列表是 [Windows 样式契约](https://learn.microsoft.com/en-us/windows/win32/winmsg/extended-window-styles)；本轮没有打开或操作 Shell 来实测列表行为，不能写成 Shell UI 验收。第十六／十八轮的成功证据对应未加工具窗样式的旧构建，保留各自版本界限。

## 第二十轮：可选透明 WebView 前景首测，2026-09-15

首测 EXE SHA256 `594DEB9B461AB983A8FAB394000C44105B8F9A1683493F4447814725017DB91D`，证据为 `dwm-scene-webview-probe/twentieth-build-evidence.json` 和 `twentieth-foreground-evidence.jsonl`。这一旧构建在约 120 秒自动退出，当前源码为后续复测将 WebView 构建延长至 180 秒，普通构建仍为 120 秒。

日志记录本地页面就绪、背景 alpha 为 0、shader 位于 children 下方；本轮观察确认透明 WebView 前景正确显示，背景 F3 镜像时前景文字没有跟着镜像。输入框取得焦点后，`KP_4` 单字符按键改变了内容；F3、F6 经 WebView 焦点转发成功。F6 移到 A 后仍有 `cached_texture = 0` 的有效新帧，最终累计 2,200／2,200 帧，2,201 次输出 alpha 检查，没有 scope 或 sentinel 失败。该 alpha 结果属于背景 swapchain，不能替代前景内容及最终合成画面的验证。

本轮 `type_text` 尝试没有改变输入框，排查发现测试页的 paste 监听调用了 `preventDefault`；该监听随后移除。在第二十轮结束时，完整文本、Count 按钮、WebView 焦点下 F1／Escape 和启动闪白录像均尚未完成验收，不能把修正源码写成当轮输入成功。后续第二十一／二十二轮的复测结果单列如下，第二十轮的 API 就绪、单字符输入和四角标记结果不承担这些证明。

## 第二十一／二十二轮：前景输入、录屏与退出，2026-09-15

两轮使用 EXE SHA256 `359530E3ACE5D83D6DC6DDEF283B6F08D627A9A632A305F9F07351A52E99ACA2`，测试页不再拦截默认粘贴事件，WebView 构建自动退出时间为 180 秒。所有输入和点击均由已授权 Computer Use 完成，未访问真实剪贴板历史；宿主使用独立 QA profile，本地页面没有主动请求网络或剪贴板 API。

第二十一轮实际输入 `sample 42`，Count 按钮从 0 变为 1。WebView 持有焦点时，F3 镜像、F5 七曲面、F6 移至固定源 A 均生效，前景保持正常文字方向和输入／计数状态。F1 从前景焦点关闭了 controller 与辅助组，source 保留；最终为 3,359／3,359 有效输入帧、3,360 次背景 alpha 检查。日志及构建记录为 `dwm-scene-webview-probe/twentyfirst-input-evidence.jsonl` 和 `twentyfirst-build-evidence.json`。屏幕证据为 `qa/native-webview-foreground-screen.png`；这些是虚构的测试控件，不是生产 TieZ 外观。

`qa/native-webview-foreground-dda.mp4` 实际录下前景及动态材质，20 秒、1,186 解码帧，无洋红命中；SHA256 为 `757CA9C573AFFCC2B89FF7297A87A768A81F055EB63BAE5B0145A4817AA8A5F2`。录像在 F1 动作前结束，不能用作停止转场录像，文件名已去掉 stop。F1 行为由实际输入操作与退出日志证明。

第二十二轮先开始 Desktop Duplication 录像，再在同一次工具编排中发出 F1，避免开始动作落在录像之外。`qa/native-webview-startup-dda.mp4` 为 30 秒、1,769 解码帧；C 区第 568 帧首次显示取景画面，随后显示 WebView 前景。`qa/native-webview-startup-analysis.json` 无洋红帧，没有超过 90% 输出面积的近白帧，最大近白覆盖率约 0.7665%；已检查首次输出和就绪帧。视频 SHA256 为 `EFD5B6615F5030C6954AC6F24E270FDC2365C5602C29B76999C34E60572747CB`。这是既有隔离 profile 下的当前设备采样记录，不能证明冷 profile、所有设备或记录帧之间不存在闪白。

录像完成后，重新聚焦 WebView 输入框并按 Escape。整个探针在约 134 秒关闭，早于 180 秒自动退出；日志为 `twentysecond-startup-escape-evidence.jsonl`，共 1,629／1,629 有效帧、1,630 次背景 alpha 检查。已确认探针进程退出。合并结果与未完成项保存在 `qa/native-webview-foreground-verification.json`；圆角、关闭时仍在创建的异步回调、多 DPI、移动缩放和生产接入仍需独立验证。

## 第二十三轮：冷 profile 启动通过，但圆角露出捕获边框，2026-09-15

EXE SHA256 `E5C7E8BE5E368865E18B3F8D245F2B1D893727087D485F6B76110A8AA9A515F7`，构建和运行证据位于 `dwm-scene-rounded-probe/twentythird-build-evidence.json`、`twentythird-cold-startup-evidence.jsonl`。helper／cover／output 使用名义 24px 半径的相同 region，F1 前测试 profile 不存在。最终 2,549／2,549 帧有效，2,550 次背景 alpha 检查，没有 region mismatch；这些通过项仍不足以证明屏幕圆角正确。

冷 profile 的 Desktop Duplication 录像 `qa/native-rounded-cold-startup-dda.mp4` 为 30 秒、1,785 个解码帧，SHA256 `6829A413B6201131F86214DAE5666D5DA060A60940E6545E474DFF580B9B7454`。`qa/native-rounded-cold-startup-analysis.json` 在 C 区未检出洋红帧，也没有超过 90% 面积的近白帧，最大近白覆盖率约 0.8446%。该结果约束实际记录的启动帧，不证明帧间绝无闪白。

实际屏幕却在圆角外露出了矩形的黄色 Windows capture indicator。`qa/native-rounded-region-verification.json` 明确标记 `failed_visible_capture_indicator_at_rounded_corners`，例如 C 左上外角采样 RGB 为 `(191,172,6)`，不是原始背景。保留 `qa/native-rounded-cold-ready-frame.png`；本轮圆角视觉验收失败，不能用全有效帧、alpha 255、region 相等或无整白录像覆盖这一失败，也没有 Apple 圆角保真结论。

## 第二十四轮：既有 Allowed 状态消除边框，但镜像暴露黑列，2026-09-15

EXE SHA256 `2C3772B8D8A7C5B62527FA9B46ABF0A1074AE9875BE1AF8F5E62D696B5922B38`，证据为 `dwm-scene-rounded-probe/twentyfourth-build-evidence.json` 和 `twentyfourth-existing-access-evidence.jsonl`。圆角路径新增对 `graphicsCaptureWithoutBorder` 的 `AppCapability.CheckAccess` 只读查询，记录状态 `4 = Allowed` 和 `permission_prompt_requested = false`，随后才设置当前 session 的 `IsBorderRequired(false)`。没有调用 `RequestAccess`／`RequestAccessAsync`，也没有更改系统权限设置。

[微软的状态说明](https://learn.microsoft.com/en-us/uwp/api/windows.security.authorization.appcapabilityaccess.appcapabilityaccessstatus?view=winrt-26100) 区分查询与请求，查询不弹提示；本轮只证明这个测试进程在本机得到既有 Allowed 状态，不能外推所有 unpackaged 应用、用户、设备或权限状态。[IsBorderRequired 文档](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.isborderrequired?view=winrt-26100) 也说明 setter 成功仍可能不改变实际边框，因此运行日志将 setter 结果记作 `visual_result_unverified = true`。

屏幕证据 `qa/native-rounded-existing-access-screen.png` 随后确认 C 区四角恢复为背景 RGB `(25,30,40)`，本轮累计 3,386／3,386 帧有效、3,387 次背景 alpha 检查。但 F3 镜像揭示 x = 0 的黑列，`qa/native-rounded-mirror-edge-screen.png` 的 `(0,100)` 像素为 RGB `(0,0,0)`。去除黄色捕获边框与修正几何采样是两个问题；本轮不能判定圆角边缘像素已正确。

## 第二十五轮：修正 region 范围后的背景与边缘像素，2026-09-15

EXE SHA256 `3D19BC2D46DD2BE9E61E05BA4E763AE8D1FC99FED632DD731FCE56F55880F924`，证据为 `dwm-scene-rounded-probe/twentyfifth-build-evidence.json` 和 `twentyfifth-rounded-pixels-evidence.jsonl`。`CreateRoundRectRgn` 的右／下参数改为 `width + 1`、`height + 1`，并在 scope 中用 `GetRgnBox` 验证预期 region 的有效范围覆盖完整 320 × 360，再要求三个实际 region 与它相等。此次没有移动取景范围、改动 shader 或放宽四角验证点。

先 F4 冻结背景，再 F3 镜像、F6 重叠和 F2 blur 12 → 0 → 12，以实际内容变化促使新输入。`qa/native-rounded-geometry-pixel-analysis.json` 排除两个刻意保持文字方向的 WebView 前景包围区 `(12,12,308,85)`、`(12,250,308,347)`，共比较 64,880 个像素，RGBA 各通道最大误差和不同像素数均为 0。比较包含圆角外的 564 个像素，这些位置要求显示未镜像的原始源背景；圆角内部背景要求镜像。原黑列位置 `(0,100)` 的实际与预期 RGBA 都为 `(236,144,62,255)`。该结论是已选背景和边缘区域的像素一致，不是 115,200 像素整图一致，也没有把 WebView 前景纳入背景镜像比较。

运行最终收到 619 帧，其中 618 帧有效。第 17 帧候选的四角 RGBA 均为 0，被作为透明无效输入拒绝，日志记录 `presented = false`，没有替换显示缓存。151,735ms 恢复背景运动后，后续 `cached_texture = 0` 新帧继续有效；程序在 180 秒正常自动退出。输出 alpha 共检查 621 次，36 条周期记录每次均扫描 115,200 像素，最小／最大 alpha 为 255，非不透明像素为 0。该计数包含占位和缓存重绘，不能当作独立输入帧数。

当前源码同时为 WebView 错误消息携带 generation token，并忽略与活动辅助组不匹配的旧消息。这部分目前只有静态复核及正常路径运行，没有注入滞后的旧错误消息，也没有运行复现并验证“旧消息误停新组”的故障场景。圆角后的屏幕边框、背景像素和这一异常消息防护分别记录证据；不能据此宣称全部异步关闭路径、Apple 平滑圆角或生产接入已完成。

## 第二十六轮：圆角移动、折射与停止录像，2026-09-15

沿用第二十五轮二进制。既有许可查询返回 Allowed，内部 helper 会话设置边框偏好，未请求新许可。开启七曲面后从 C 移到固定采样区 A，再用 F1 停止辅助组，source 保持可见。`dwm-scene-rounded-probe/twentysixth-rounded-motion-evidence.jsonl` 记录 1,214／1,214 有效输入帧、1,215 次背景 alpha 检查；辅助层销毁时 output 仍可见。录像结束后底窗在 180 秒正常退出，没有留下探针进程。

`qa/native-rounded-motion-stop-dda.mp4` 为 30 秒、1,773 解码帧，SHA256 `D0E39D4D801C7CF2808EF19EFBE5DA60F79DF5E35C24E330286247E7A8BE7CB6`。`qa/native-rounded-motion-video-analysis.json` 以 C 的来源标记、A 的前景标记确认零基帧 0 位于 C、第 555 帧移至 A、第 1201 帧仅余 source。完整测试区域无洋红命中，A／C 左上角采样点均未检测到此前的黄色指示框。已逐一查看 C、A 和停止后的实际解码帧；检查支持本次录像覆盖的行为，不证明记录帧之间、所有角点或所有设备的任意状态。

实际视频同时包含清晰 WebView 前景、原生折射及背景动画，普通 Desktop Duplication 未隐藏最终输出。只有已有 Allowed 时才请求内部会话省略指示框；其他环境会保留默认边框，当前实验没有替它们申请权限。此条件及私有 DWM ABI 的版本限制仍需在生产设计中处理。

本阶段确认原生取景、真实模糊保留、曲面 shader、透明 WebView、圆角范围及已测生命周期可以在当前受控场景组合。下一步是 TieZ 实际控件与主窗口接入，以及尺寸变化、DPI、状态切换、性能和 Apple 视觉／动态校准。两份新建的隔离 WebView profile 已在确认相关子进程为 0 后清理；源代码、二进制、构建记录、截图和视频证据保留。
