# Windows 桌面液态玻璃实现研究

研究日期：2026-09-14。范围：Windows 11、Tauri 2 / WebView2、真实桌面折射、正常截图与录屏。初始研究仅涉及代码与网页。随后主代理在用户授权下验证了单个自有背景窗口的 WGC / D3D11 隔离探针；结果记录于 `src-tauri/examples/glass_capture_probe.md` 和 `artifacts/material-redesign/probe`。没有因该探针接入生产采集或宣称完成桌面材质。

## 决策

**可以确定采用独立的 GPU 光学渲染层，但尚不能确定一个同时满足“真实桌面、无自反馈、正常截图录屏、稳定支持”的生产背景来源。** 已公开的 D3D11 光学实现能够对纹理做折射；问题集中在获得不含自身、又不影响外部录屏的实时桌面纹理。不能把这一缺口重新包装成 CSS 高光，也不能把编译通过作为完整液态玻璃的证据。

推荐先完成独立光学样板和背景来源探针，分别验收，再合并。未经背景探针验证，不将桌面捕获管线接入主窗口。移除截图限制应当是独立改动，不应依赖高级材质是否成功。Microsoft 对 `WDA_NONE` 的定义是不限制显示位置；`WDA_EXCLUDEFROMCAPTURE` 会让窗口不出现在捕获输出，后者不能成为任何默认或静默回退路径。[SetWindowDisplayAffinity](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity)

“与 iOS 完全一样”仍是待验证的目标，不能由采用某个开源 shader 推出。FloatNote 自身明确说明其实现是独立的艺术近似，并非 Apple 官方实现或私有渲染代码移植。[FloatNote 实现说明](https://github.com/Fryingpan-Jason/FloatNote-Liquid-Glass/blob/main/docs/WINDOWS_LIQUID_GLASS.md)

## 已有项目证据

- `src-tauri/src/infrastructure/window_material.rs` 目前是 `CreateBackdropBrush` 输入加 Gaussian blur，前景 WebView 独立绘制。它没有桌面纹理或折射着色器。
- `artifacts/materials-revision/native-verification.md` 记录了三项实际失败：非树形 RGB 分支、效果图过复杂、`Backdrop brushes cannot be transformed. (0x80070057)`。这些是本项目上轮的结果，不是本轮重测。
- 同一记录中，`CompositionVisualSurface` 包装虽然初始化成功，但没有证明可见桌面折射，随后被删除。不能据此称它已绕过系统限制。

官方也明确说明 `CompositionEffectBrush` 只使用系统内置 shader，不支持自定义 shader；Win2D 的 `DisplacementMapEffect` 标记为 `NoComposition`。因此继续向现有 Composition effect graph 添加位移、RGB 分支或白边不是可靠的折射实现路线。[CompositionEffectBrush](https://learn.microsoft.com/en-us/uwp/api/windows.ui.composition.compositioneffectbrush?view=winrt-26100)、[DisplacementMapEffect](https://microsoft.github.io/Win2D/WinUI3/html/T_Microsoft_Graphics_Canvas_Effects_DisplacementMapEffect.htm)

## 候选路线比较

| 路线 | 已有证据 | 本项目决策 |
| --- | --- | --- |
| Composition BackdropBrush + 内置 blur | 官方支持背景采样和内置效果；当前项目已经实现。不能指定自定义 shader，位移不受支持。[API](https://learn.microsoft.com/en-us/uwp/api/windows.ui.composition.compositioneffectbrush?view=winrt-26100) | 保留为毛玻璃 / 可读性底层，不作为真实桌面折射。 |
| 整屏 WGC / Desktop Duplication + D3D11 shader + 全局 WDA 排除 | FloatNote 的采集源码明确检查 `WDA_EXCLUDEFROMCAPTURE`，否则返回 `E_ACCESSDENIED`；electron-liquid-glass 也通过该标记断开自反馈。[FloatNote 源码](https://raw.githubusercontent.com/Fryingpan-Jason/FloatNote-Liquid-Glass/main/src/monitor_backdrop_capture.h)、[Electron 源码](https://raw.githubusercontent.com/hicccc77/electron-liquid-glass/main/src/panel.cc) | 淘汰这种背景来源。移除标记后直接抓整屏会把自己的玻璃作为下一帧背景，不能只删保护开关就沿用该采集器。 |
| Desktop Duplication + `WCA_EXCLUDED_FROM_DDA` | electron-liquid-glass 的 npm 说明提供 `dda-only`，同时明确所有采用 Desktop Duplication 的外部录屏仍然看不到面板；独立面板也可能不进单窗口 WGC。[作者发布说明](https://www.npmjs.com/package/@hicccc77/electron-liquid-glass) | 淘汰。仅使部分截图工具恢复可见，仍不符合用户要求；该私有属性也没有稳定 SDK 契约。 |
| 单窗口 WGC + 自行按 Z 顺序合成背景 | Microsoft 提供公开的 `CreateForWindow`，从 Windows 10 1903 起支持。[API](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createforwindow) | 可作公开 API 探针。只采后方受控测试窗口时无须全局排除自身。推广至整个桌面意味着自行重建透明、阴影、弹窗、壁纸、跨屏与时序；未找到已验证的完整 DWM 等效实现，不能承诺像素一致。 |
| Magnification 的每控件排除名单 + 像素回调 | `MagSetWindowFilterList` 的过滤范围确实是指定放大镜；但取像素的 `MagSetImageScalingCallback` 已弃用，Microsoft 要求新应用不要使用，并只承诺 DWM 关闭时有效。[过滤 API](https://learn.microsoft.com/en-us/windows/win32/api/magnification/nf-magnification-magsetwindowfilterlist)、[回调 API](https://learn.microsoft.com/en-us/windows/win32/api/magnification/nf-magnification-magsetimagescalingcallback) | 仅作为受控实验候选，不作为正式后端。Windows 8 起 DWM 始终启用，不能用关闭 DWM 解决。[DWM 说明](https://learn.microsoft.com/en-us/windows/win32/w8cookbook/desktop-window-manager-is-always-on) |
| 私有 `DwmpCreateSharedMultiWindowVisual` / `DwmpUpdateSharedMultiWindowVisual` | ADeltaX 源码通过 ordinal 163 / 164 获取接口，支持排除 HWND；同一 ordinal 的旧新签名有差异。输出是 `IDCompositionVisual2`。[原始代码](https://gist.github.com/ADeltaX/aea6aac248604d0cb7d423a61b06e247) | 暂不接入。排除列表不等于可读取 shader 纹理；还需要证明视觉到 GPU 纹理的桥、采集可见性及版本兼容。现成 Acrylic 演示仅证明 blur / saturation。[演示项目](https://github.com/selastingeorge/Win32-Acrylic-Effect) |
| 公共 DWM thumbnail | Microsoft 明确它直接在目标窗口中二维显示；返回 thumbnail 关系，不是任意 GPU 纹理。[官方说明](https://learn.microsoft.com/en-us/windows/win32/dwm/thumbnail-ovw) | 不能直接作为任意 shader 输入。不要混淆公共 thumbnail API 与私有 shared visual。 |

公开的 `GraphicsCaptureSession` 包含光标、边框、次级窗口与更新间隔控制，没有任意 HWND 排除名单。`IncludeSecondaryWindows` 只控制与主窗口相交的特定弹窗 / 工具窗口，不是从整屏采集中排除当前应用。[会话属性](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession?view=winrt-26100)、[次级窗口定义](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.includesecondarywindows?view=winrt-26100)

`CompositionVisualSurface` 的来源是 `Windows.UI.Composition.Visual`；`GraphicsCaptureItem.CreateFromVisual` 也接受这个类型。上面的私有 DWM 示例返回传统 `IDCompositionVisual2`。本轮没有找到文档支持将后者直接当作前者或导出为 D3D11 shader resource，不能把名称相近当作可互通证据。[VisualSurface](https://learn.microsoft.com/en-us/uwp/api/windows.ui.composition.compositionvisualsurface?view=winrt-26100)、[CreateFromVisual](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscaptureitem.createfromvisual?view=winrt-26100)

## 收敛后的背景探针

**首选公开 WGC 单来源，再验证多来源。** Microsoft 的 winapp 官方说明明确 WGC 读取目标窗口的 DWM 合成表面，保留圆角与透明，并可在窗口被其他窗口遮挡时工作。这比“根据 API 名称推测能看到背后窗口”更有力，但仍须在本项目的 WebView2 / DComp 测试窗口上复现。[官方捕获行为说明](https://learn.microsoft.com/en-us/windows/apps/dev-tools/winapp-cli/ui-automation)

Microsoft 的 `ScreenCaptureforHWND` 示例给出完整公开链条：`CreateForWindow` → `Direct3D11CaptureFramePool` → `frame.Surface()` → `ID3D11Texture2D` → swap chain → `CreateCompositionSurfaceForSwapChain`。shader 可以放在取得输入纹理与呈现之间，无须把屏幕整体复制回来。[窗口选择源码](https://github.com/microsoft/Windows.UI.Composition-Win32-Samples/blob/master/cpp/ScreenCaptureforHWND/ScreenCaptureforHWND/capture.interop.h)、[采集与呈现源码](https://github.com/microsoft/Windows.UI.Composition-Win32-Samples/blob/master/cpp/ScreenCaptureforHWND/ScreenCaptureforHWND/SimpleCapture.cpp)

hecomi 的 uWindowCapture 已实现多个 HWND 的 WGC 纹理呈现，作者展示了模仿桌面布局的多窗口场景。因此多来源 GPU 采样有实际工程参考，但它没有证明完整重现 DWM 桌面合成。先用两个相互重叠的受控背景窗复原窗口顺序、逐像素 alpha、圆角和动态内容，再测试系统毛玻璃背景、弹窗和显示器边界。[实现仓库](https://github.com/hecomi/uWindowCapture)、[作者实现与性能说明](https://tips.hecomi.com/entry/2021/04/30/230322)

**次选私有 shared visual 的两段式桥，仅为待测假设。** 将 `DwmpCreateSharedMultiWindowVisual` 的结果附着到本项目的 helper HWND，使用私有排除名单去掉 helper 与最终输出 HWND；再通过公开 `CreateForWindow(helper)` 获得纹理。这个设计避免错误转换 `IDCompositionVisual2` 类型，但本轮没有找到完整成功实现。首先验证 WGC 是否包含这种共享视觉、helper 被遮挡后是否持续更新，以及 ADeltaX 示例中的 `WCA_EXCLUDED_FROM_LIVEPREVIEW` 是否使 helper 无法捕获。需要修改全局截图可见性、隐藏窗口导致不再更新或返回空帧时立即淘汰；不把成功创建对象当作像素有效。[共享视觉原始代码](https://gist.github.com/ADeltaX/aea6aac248604d0cb7d423a61b06e247)、[公开 WGC 表面链](https://github.com/microsoft/Windows.UI.Composition-Win32-Samples/blob/master/cpp/ScreenCaptureforHWND/ScreenCaptureforHWND/SimpleCapture.cpp)

## Magnification 开源实现的具体核查

PyGlass 当前核查版本为 `c12d91f1526badbe18f619ff893d2cdbbd86dac5`。作者声称在现代 Windows 上验证了 DirectComposition；这是有对应实现和提交的作者报告，但与已弃用回调的官方支持边界并不一致，仍需本机探针，不能写成已验证可交付。[引入提交](https://github.com/neomosh8/pyglass/commit/dcd5e879911c6e7fa8f3878e1eae3121ed66e89a)

- `_magnifier.py` 用 `MagSetImageScalingCallback` 的 `srcdata` 复制 CPU 像素，并以 `UpdateWindow` 触发更新，没有 GPU 共享纹理输出。[采集源码](https://github.com/neomosh8/pyglass/blob/c12d91f1526badbe18f619ff893d2cdbbd86dac5/pyglass/_magnifier.py)
- 一个紧随其后的修复提交承认静止时会发生自采集，改为每帧重设过滤名单。这是需要复现的风险，不能忽略。[修复提交](https://github.com/neomosh8/pyglass/commit/af32d8889ccb4ee0a9cae69013f286a2e3dba73e)
- 初始化失败会回退到全局 `WDA_EXCLUDEFROMCAPTURE`，而不只是在旧版 Windows 上回退。该行为不可移植到本项目。[背景与回退源码](https://github.com/neomosh8/pyglass/blob/c12d91f1526badbe18f619ff893d2cdbbd86dac5/pyglass/backdrop.py)
- 采样间隔默认约 120 ms、静态约 300 ms，拖动时使用旧屏幕缓存；坐标按主屏 DPR 换算，CPU 再进行折射 / 模糊。这不能证明连续拖动、混合 DPI、多屏或 iOS 级动态效果。Magnification 本身也不支持 WOW64。[背景源码](https://github.com/neomosh8/pyglass/blob/c12d91f1526badbe18f619ff893d2cdbbd86dac5/pyglass/backdrop.py)、[渲染源码](https://github.com/neomosh8/pyglass/blob/c12d91f1526badbe18f619ff893d2cdbbd86dac5/pyglass/refract.py)、[平台限制](https://learn.microsoft.com/en-us/windows/win32/winauto/magapi/magapi-intro)

## 推荐的可逆 POC

以下方案已经分项执行，具体进展和边界以末尾运行记录及各探针说明为准。用户已明确授权本项目测试界面使用 Computer Use；沿用该范围与虚构数据，不访问剪贴板历史，不给最终输出设置捕获排除属性。

1. **先隔离光学和采集。** 以应用自有的动态棋盘格、细直线、正文排版、黑 / 白 / 中灰纹理作为 D3D11 输入。用圆角矩形的连续高度 / 法线生成折射采样；中心保持稳定，边缘弯曲连续，色散应受同一法线驱动。前景文字与交互仍由 WebView2 绘制。先验收四角、长宽比、缩放与端点，没有星形、硬拼接或重复高光再继续。原生 D3D 内容通过 composition swap chain 呈现是公开支持路径。[DXGI 接入](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforcomposition)
2. **公开背景探针优先。** 创建一个受控背景测试窗口，用 `CreateForWindow` 采集它，输出到前面的透明测试窗口。保持 `WDA_NONE`。验证输入纹理确实随背景变化且不含前景，外部整屏和单窗口捕获都包含最终玻璃。这个探针只证明单来源，不等于整个桌面已完成。
3. **单独验证全桌面来源。** 如继续研究 Magnification，仅用独立进程尝试每控件排除，逐项检查 API 返回值、首次帧超时、静止自反馈和硬件加速背景；不得失败后转入全局排除。私有 DWM 路径先证明 shared visual 能转为有效纹理，再讨论 shader。任一路径不能通过就保持未实现状态，不替换正式材质。
4. **满足条件后才接 Tauri。** GPU 背景层与透明 WebView2 前景分离；不改变整个 HWND 或 WebView 的整体 opacity。WebView2 官方支持背景 alpha 为 0 或 255，半透明值不受此接口支持；材质密度放在 GPU / CSS 材质层处理。[WebView2 背景接口](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2controller2)
5. **验收普通捕获和动态边界。** 在窗口刚出现、拖动、缩放、跨 100% / 150% / 200% DPI、部分移出屏幕、不同 Z 顺序和恢复显卡设备时检查。分别记录系统截图、WGC 整屏 / 单窗口、Desktop Duplication 录屏；不能只验一种工具。检查任务结束后没有采集线程、共享表面、隐藏辅助窗或临时文件残留。HDR 场景需要浮点采集和一致色彩处理，不能直接拿 SDR BGRA 结果宣称全场景一致。[Microsoft HDR 采集说明](https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture)

本项目可以先验收材质可读性和光学样板，但应明确分别报告它们。高级模式初始化失败时，以可读的材质层结束该次初始化；不制造无限自反馈，不让前景消失，不启用截图保护，也不把降级后的模糊描述为折射成功。

## API 稳定性与许可证

- **公开稳定 API**：D3D11、DXGI composition swap chain、WebView2 透明背景、WGC 指定窗口采集。公开支持不等于任意平台效果相同，需按当前 Windows、GPU、HDR 与多屏情况验收。Desktop Duplication 还具有适配器匹配、会话切换及并发数量限制。[DuplicateOutput](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutput1-duplicateoutput)
- **不作为稳定生产依赖**：已弃用的 Magnification 像素回调，以及 ordinal 获取的私有 DWM shared visual / `WCA_EXCLUDED_FROM_DDA`。后者在原始示例中已有签名随版本改变的证据，必须与公开的 `DwmRegisterThumbnail` 区分。
- **FloatNote** 为 MIT；它借鉴的 Liquid DOM 光学内容有单独第三方声明，移植实质代码时保留双方适用声明。[LICENSE](https://raw.githubusercontent.com/Fryingpan-Jason/FloatNote-Liquid-Glass/main/LICENSE)、[第三方声明](https://github.com/Fryingpan-Jason/FloatNote-Liquid-Glass/blob/main/THIRD_PARTY_NOTICES.md)
- **electron-liquid-glass、Win32-Acrylic-Effect、PyGlass** 的仓库许可均为 MIT。可借鉴实现，不直接继承其不符合要求的采集回退，也不将源码许可误当作 API 稳定性保证。[Electron 许可](https://raw.githubusercontent.com/hicccc77/electron-liquid-glass/main/LICENSE)、[Acrylic 许可](https://raw.githubusercontent.com/selastingeorge/Win32-Acrylic-Effect/master/LICENSE)、[PyGlass 许可](https://github.com/neomosh8/pyglass/blob/c12d91f1526badbe18f619ff893d2cdbbd86dac5/LICENSE)
- **ADeltaX gist** 的本次查看内容没有明确许可文本。只作为 API 可行性线索，不直接复制其实现到产品。[原始 gist](https://gist.github.com/ADeltaX/aea6aac248604d0cb7d423a61b06e247)
- **Microsoft ScreenCaptureforHWND、uWindowCapture** 都是 MIT，可用于公开 WGC 探针的实现参考，保留许可声明。[Microsoft 示例许可](https://raw.githubusercontent.com/microsoft/Windows.UI.Composition-Win32-Samples/master/LICENSE)、[uWindowCapture 许可](https://raw.githubusercontent.com/hecomi/uWindowCapture/master/LICENSE.md)

研究结论的边界：未找到一个可以直接移植、已经证明满足本项目全部条件的 Windows 桌面液态玻璃方案。这不等于数学或平台上绝对不可能；它意味着背景来源、跨捕获可见性与视觉一致性仍需实验，不应提前声称方案已经完整敲定。

后续进展：单自有来源已在本机验证遮挡下持续取帧、普通窗口截图可见、affinity 为零、真实折射开关和 1:1 坐标跟随。双来源探针只扩展到两个自有、不透明、无装饰矩形来源，方案与边界见 `src-tauri/examples/glass_multi_capture_probe.md`。微软采集样例维护者明确指出，读取窗口后方纹理的 Acrylic / window backdrop 效果不能作为普通独立窗口捕获内容保留；所以单窗或双窗成功不能外推为任意真实桌面的无损重建。[背景依赖限制](https://github.com/robmikh/Win32CaptureSample/issues/41#issuecomment-1024991777)

2026-09-14 的进一步实测已经证明私有 shared visual → 自有 helper → WGC → 可编程 D3D11 纹理这条桥在本机受控场景可用。内部 helper 必须按原始示例设置 live-preview 属性才越过 `E_INVALIDARG`；最终 output 不设置此属性，普通截图可以看到输出。避免每帧重复重建相同场景后，两分钟 2,537 帧均通过来源标记检查，F3 镜像明确作用于纹理。证据及全部失败轮次保存在 `artifacts/material-redesign/dwm-scene-probe`，说明在 `src-tauri/examples/dwm_scene_probe.md`。

这项证据解决了“传统 shared visual 能否进入可采样纹理”的局部可行性疑问。随后使用 Composition BackdropBrush + GaussianBlur 源样板，已经在同一冻结背景的 blur 12／0 两种状态下完成整个 320×360 源画面与输出的屏幕 PNG 对照，115,200 像素的 RGBA 差异均为 0。普通 GDI 与 Desktop Duplication 录屏也已实际得到包含七控件光学输出的约 4 秒视频。对应数据见 `artifacts/material-redesign/qa/native-composition-blur-screen-analysis.json`、`native-recording-verification.json`。

这些结果仍不证明任意桌面重建、私有 API 的版本稳定性、HDR、混合 DPI、无可见辅助窗的生命周期或最终应用集成。不能将受控原型直接命名为已完成的桌面液态玻璃后端，也不能将静态场景的像素一致性外推为 Apple 外观与动态一致性。

2026-09-15 的重叠实验进一步缩小了可行性结论：同位置 helper／cover／output 组在旁侧 C 可用，但 F6 把整组移到实际源区域 A 后，候选帧的四色标记出现与 F3 输出一致的镜像，尽管三个 HWND 仍在私有排除名单内。检测器拒绝这些帧并停止组。此时屏幕上仍正确的镜像来自最后一次有效缓存，不能据此判定排除成功。日志为 `artifacts/material-redesign/dwm-scene-probe/fourteenth-overlap-failure-evidence.jsonl`。需要定位私有场景分组或采集层的自我回采；这条桥尚不满足实际玻璃覆盖场景。

`IncludeSecondaryWindows` 的官方默认值是 false，且定义依赖弹出／工具窗样式、相交和 Z 顺序，不要求相同 owner。因此“同 owner 自动包含”目前不能作为该缺陷的确定解释。[Microsoft 属性说明](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.includesecondarywindows?view=winrt-26100)

同日的归属关系 A/B 得到了可重复区别。第十五轮仅将 output 的 owner 从 base 改为 null，错误输入由镜像变为 cover 的洋红色；第十六轮继续仅将 helper 的 owner 从 base 改为 null，cover 仍归 helper，整个源区域重叠期间及移回旁侧后均持续通过四角新帧检查，最终 2,253／2,253 帧有效。光学和 WGC 属性没有同时改动，最终 output 仍允许普通捕获。这支持窗口归属关系影响场景排除的判断，不能单凭结果断言私有函数内部如何匹配 HWND。后续仍需重叠后的完整像素保真、圆角、最小化／激活跟随及实际 WebView 集成；已通过四角标记并不覆盖源端模糊区域内的每个像素。

第十八轮已补齐上述完整像素保真项：在冻结背景、镜像、重叠条件下更改源模糊并恢复，新的 WGC 帧通过校验，完整屏幕结果与冻结参考镜像的 115,200 个像素 RGBA 差异均为 0。第十七轮屏幕对照包含验收工具的鼠标光圈，移至固定 ROI 外后重测解决，没有通过裁掉真实材质区域来取得一致。对应报告为 `artifacts/material-redesign/qa/native-isolated-clean-pixel-analysis.json`。这仍只证明当前受控源和窗口关系下的像素来源，不证明 Apple 材质或系统级生命周期。

后续已将透明 WebView2 放在原生 shader 前方，实际输入、按钮和焦点中的材质快捷键通过，普通录屏包含两层。三窗采用一致的 24px region 后，发现系统采集指示框在圆角外露出。采取的最小路径只查询已有 `graphicsCaptureWithoutBorder` 许可，Allowed 时设置当前 helper 会话的 `IsBorderRequired(false)`；没有调用授权请求，也未更改系统设置。官方明确保证 CheckAccess 不弹窗，同时边框属性的设置本身不能代替许可或证明视觉结果。[查询契约](https://learn.microsoft.com/en-us/uwp/api/windows.security.authorization.appcapabilityaccess.appcapabilityaccessstatus?view=winrt-26100)、[边框属性](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.isborderrequired?view=winrt-26100)

本机该查询返回 Allowed，PNG 和 Desktop Duplication 证明角落指示框消失，最终输出仍可捕获。另修正 GDI region 的右下边界后，64,880 个可见背景／边缘像素与镜像及原始背景预期相同，第二十六轮录像覆盖 C→A→停止。此结果不能外推未打包程序的全部许可映射；无 Allowed 时保留默认行为，生产尚需决定其可用性与回退。完整证据与已知边界在 `src-tauri/examples/dwm_scene_probe.md`，不以这些样板结果宣称 Apple 的视觉与动态已经一致。
