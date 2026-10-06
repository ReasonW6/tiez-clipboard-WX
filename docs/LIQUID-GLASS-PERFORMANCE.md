# 液态玻璃性能与呈现时序核对

核对日期：2026-10-05。范围为 Windows Graphics Capture、传统 DirectComposition、D3D11，以及本轮修复开始时的 `src-tauri/native/glass_renderer.cpp`、`.hlsl` 和配套头文件。以下接口事实来自 Microsoft 文档；项目结论来自静态代码，实际卡顿占比与闪烁根因仍需测量。本文没有运行窗口或修改产品代码。

## 已证实的接口约束

| 接口 | 已证实的行为 | 对本项目的约束 |
| --- | --- | --- |
| `CreateFreeThreaded` | `FrameArrived` 在 frame pool 内部工作线程触发，不依赖 `DispatcherQueue`。[Microsoft API](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool.createfreethreaded?view=winrt-26100) | 使用它不代表后续渲染自动离开窗口线程。 |
| WGC 帧生命周期 | `TryGetNextFrame` 取出帧；帧归还后不得继续保存其底层 surface。`ContentSize` 之外的像素可能未定义；重建 pool 应使用 `Recreate`。[Microsoft 指南](https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture) | 应把合格帧复制到拥有的纹理，及时归还帧；尺寸变化不能读取未定义边缘。 |
| D3D11 多线程 | device 可多线程调用；同一 immediate context 同时只能有一个调用线程，DXGI 操作尤其 `Present` 应与 context 使用同一线程。[Microsoft 指南](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-intro) | 不能仅把 WGC 回调里的 Draw 移到工作线程，同时让 UI 线程继续 Copy/Draw/Present。 |
| DXGI 呈现 | `Present` 可能阻塞；flip model 的 `SyncInterval=0` 允许丢弃过时的排队帧。`DO_NOT_WAIT` 遇到阻塞返回 `DXGI_ERROR_WAS_STILL_DRAWING`。[Present](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present)、[呈现标志](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present) | 改为 0 不等于保证非阻塞；暂时忙必须保留旧输出并重试，不能作为 device failure。 |
| DXGI 排队上限 | `SetMaximumFrameLatency` 只适用于创建时设置 `FRAME_LATENCY_WAITABLE_OBJECT` 的 swap chain；官方建议每次渲染前等待该句柄，包括第一帧，并最终关闭句柄。[排队上限](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-setmaximumframelatency)、[等待句柄](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject) | 当前创建参数没有此标志，不能只追加 `SetMaximumFrameLatency(1)`。UI 线程不能进行阻塞式逐帧等待。 |
| DirectComposition 批次 | 同一 device 的修改由 `Commit` 原子提交；引擎异步运行。同一 device 的另一线程调用 `Commit` 也会提交全部待处理修改。[Commit](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-idcompositiondevice2-commit)、[架构](https://learn.microsoft.com/en-us/windows/win32/directcomp/architecture-and-components) | 需要一个串行批次所有者；不能把每个函数理解成独立事务，也不能把跨 device 或 WebView 更新当成一个原子批次。 |
| 合成等待 | `WaitForCommitCompletion` 等待引擎处理前一次 `Commit`；`DwmFlush` 会阻塞调用者，等待调用进程的已排队 DirectX 更新显示。[提交等待](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-idcompositiondevice-waitforcommitcompletion)、[DwmFlush](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmflush) | 提交处理完成与屏幕显示完成须分开记录；正常背景帧不应靠多个同步等待串行推进。 |
| 大半径模糊 | Direct2D Gaussian blur 使用预缩放优化；速度模式使用线性过滤，平衡/质量模式使用三线性过滤，质量模式在更大半径才采用近似。[Microsoft Gaussian blur](https://learn.microsoft.com/en-us/windows/win32/direct2d/gaussian-blur) | 先充分低通再降采样、在小纹理模糊、升采样是有第一方先例的方案；简单拉开取样距离会混叠。 |
| 资源绑定 | SRV 与已绑定输出重叠时，`PSSetShaderResources` 会把对应输入槽设为 NULL；`GenerateMips` 递归生成 SRV 指定的 mip 范围，需要正确的 bind/misc flags 与格式支持。[SRV](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-pssetshaderresources)、[mip](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-generatemips) | ping-pong 的输入/输出必须互异，Copy/GenerateMips 前要结束使用；不能用返回值检查发现静默失败。 |
| 窗口区域 | `SetWindowRgn` 坐标以整个窗口为原点，会产生 `WM_WINDOWPOSCHANGING/CHANGED`；系统只显示区域内的像素。DComp root 则位于客户区原点，并受目标窗口可见区域裁剪。[窗口区域](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowrgn)、[DComp 坐标及裁剪](https://learn.microsoft.com/en-us/windows/win32/directcomp/basic-concepts) | native mask、DComp viewport、WebView body 必须采用同一客户区尺寸和 DPI；设置区域本身会带来重入消息。 |

## 当前代码中可定位的开销

1. **渲染仍占用窗口线程。** `FrameArrived` 仅调用 `queue`，再由 helper 的窗口过程调用 `frame_arrived`；后者复制 WGC 帧、生成 mip、模糊、逐控件合成并 `Present(1, 0)`。`glass_renderer.h` 明确所有调用与回调在 host UI 线程。微软也明确建议避免在 UI 线程处理重型 WGC 工作。[WGC 指南](https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture)

2. **每个新背景都进入一次同步 surface 发布。** `frame_arrived` 设置 `baseDirty`；`render` 重算背景后调用 `publish_background_surface`。`prepare_background_surface` 经 `make_background_surface` 新建 DComp surface、全量复制；`mount_output` 修改 visual、`Commit`、`WaitForCommitCompletion`。因此动态背景会反复分配、复制、等待。这是代码事实；每步耗时与最大瓶颈尚未测量。

3. **移动时有连续同步等待。** crop 超出已有 reserve 时，`translate_capture` 排空旧帧并调用 `replace_shared_scene`；后者新建私有 DWM scene，执行 `Commit → WaitForCommitCompletion → DwmFlush`。之后可能再重投影、模糊、呈现和发布背景。`prepare_resize` 另有 `render(true) → DwmFlush`。这些同步点足以构成窗口线程停顿风险，不能仅归因于色散 shader。[DwmFlush 阻塞语义](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmflush)

4. **逐控件处理整个 viewport。** `render_prefix` 为每个 control/track 调用 `draw_prefix_node`，每次写入两张完整目标、再生成一整条 color mip 链。shader 在形状外仍复制前一层 color/coverage。背景初始 pass 已缓存，但改变控件或背景仍重做所有节点。按代码计数，N 个控件与 T 条 track 的一次完整重建含 `N+T` 个逐节点 pass，初始 prefix 失效时再加 1 个。此计数不等于实际 GPU 毫秒数。

5. **大半径模糊的采样量随 DPI 增加。** `BlurPS` 在 sigma > 4 时做成对 Gaussian 采样；每方向纹理采样次数为 `1 + 2 * ceil(ceil(3*sigma)/2)`。sigma=16 时为 49 次，sigma=32 时为 97 次。`render_capture_background` 在带 halo 的完整 capture 分辨率上做两方向 pass。当前成对双线性取样已有价值，应优化处理分辨率和权重计算，避免退回稀疏大间隔取样。

6. **首次有效帧会同步读回 GPU。** `frame_arrived` 从五个点 Copy 到 staging texture 后立即 `Map(READ, 0)`；这发生在首次进入材质或 source 失效后。`D3D11_MAP_FLAG_DO_NOT_WAIT` 可在资源未就绪时返回 `DXGI_ERROR_WAS_STILL_DRAWING`。[Map 标志](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_map_flag) 若采用它，需把启动检测拆成排队读回与后续消费，保留安全 fallback；直接每次重新 Copy 再 Map 会延后检测。

7. **已有正确性保护应保留。** 当前消息队列已合并 frame 通知、按 `SystemRelativeTime` 取最新可接受帧、过滤 retarget 之前的帧；`draw_pass` / `draw_prefix_node` 已清除 SRV/RTV 绑定，prefix 使用 ping-pong。不能假设这些尚未实现。epoch、scope、geometry revision 和 resize lease 保护了旧位置图像与 DOM 几何；优化时须保留这些边界。

## 优先实施与验证建议

- 先分段测量 Copy/mip、背景模糊、prefix、Present、surface prepare/Commit wait；记录帧龄、丢弃帧、控件数、分配次数。当前 fixture QPC 日志只覆盖背景复制与 mount，不能证明 GPU 时间或真实屏幕帧率。离屏像素验收与硬件性能验收分别报告。
- 优先把背景的稳定资源和正常帧更新分开，减少逐帧 surface 分配/重挂载/同步等待。尺寸、scope 与有效 source 未变化时复用；交接前保留旧对象和完整候选，取消时不允许待提交修改在后续 helper `Commit` 中意外发布。`Commit` 的原子边界属于整个 device。[架构约束](https://learn.microsoft.com/en-us/windows/win32/directcomp/architecture-and-components)
- 呈现调度只消费最新状态，最多保留一个待执行更新。若尝试非阻塞呈现，暂时忙时保持已验证 surface 和 pending geometry，延后重试；仅成功的合格帧确认 ready。不要在消息处理器内无限重试、sleep 或等待 waitable handle。真正拆出渲染线程时，应一次性迁移 immediate context、DXGI 与渲染批次所有权，UI 只发布不可变的窗口/几何快照。[D3D11 线程约束](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-intro)
- 大 sigma 先用受控离屏 fixture 比较预滤波降采样方案，保持物理 sigma、捕获 halo 与 premultiplied coverage。只降低模糊工作分辨率，保留用于折射/色散的原始 mip 源。棋盘、文字、细线、窗口圆角与透明 coverage 应验证无混叠、黑边、彩色星形或亮度跳变。[预缩放第一方依据](https://learn.microsoft.com/en-us/windows/win32/direct2d/gaussian-blur)
- prefix 优先减少不相互取样的节点重复；合并或局部更新前证明前后层依赖。仅加 scissor 会留下 ping-pong 目标形状外的旧数据，不能直接当作等价优化；必须保留完整 prior color/coverage 或明确复制/覆盖受影响区域。
- 透明背景边先测量 host outer/client insets、window region、DComp texture/clip 和 WebView 根元素圆角。native 当前以 16 CSS px 构造圆角且已换算 client→window 坐标；截图不足以证实是哪层错位。不能直接删除 region、扩大 capture 到非真实像素或用更厚边框遮盖。[坐标语义](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowrgn)
- 闪烁按切换、移动、resize、失焦、source scope 变化、暂时忙分别复现，检查 pending/ready 循环、旧几何是否被确认、是否空挂载、是否跨 device 交接。`WaitForCommitCompletion` 已通过与单张离屏图片正常，均不足以证明此时没有可见闪烁。[提交等待语义](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-idcompositiondevice-waitforcommitcompletion)

## 仍需实测的假设和边界

同步等待、逐节点全窗口处理、首次 Map 都已存在于代码；它们分别占多少时间、哪一项引发用户截图中的闪烁尚无性能追踪证明。透明边可能来自外框/客户区/CSS 圆角不一致，也可能只是截屏时 native/fallback 的不同步，均应通过尺寸与状态记录判定。

`createScene` / `updateScene` 通过 DWM ordinal 163/164 调用私有接口，Microsoft 公开文档不能证明其 retarget、缓存、版本或重入行为。已有私有接口实验的结果不能升级为公开 API 保证；对它的更改应保持本项目受控探针与失败 fallback。

本次优化保持普通截图、录屏可见：不引入 `WDA_EXCLUDEFROMCAPTURE`、`WDA_MONITOR` 或 `DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY`。微软说明 `DISPLAY_ONLY` 会阻止部分公共采集方式访问内容，因此它不属于性能修复方案。[swap-chain 标志](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ne-dxgi-dxgi_swap_chain_flag)

不要用每帧 `ID3D11DeviceContext::Flush` 替代等待设计；微软明确它有显著开销，且本身异步，不是 GPU 完成栅栏。[Flush](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-flush)

## 实施与验收

以上是修复前的调研记录。最终实现、源码范围、离屏测量和验证边界见
[本轮验收记录](../artifacts/liquid-glass-performance-20261005/verification.md)。

## 2026-10-05 原生移动与按压复查

后续实际使用反馈仍有拖动卡顿、玻璃变为纯色和按压闪烁，说明上面的离屏结果不足以覆盖窗口消息路径。用户允许短时运行自有虚构窗口，且禁止占用鼠标键盘；新增回归只用 `SetWindowPos`、窗口消息和独立 WGC 会话进行驱动及观测。

修复前，同尺寸移动只要没有 `SWP_NOZORDER` 就进入捕获撤销/重建，平均 `SetWindowPos` 耗时约 14 毫秒。微软文档说明该标志保留顺序，也说明窗口移动、尺寸和顺序属于同一个 API 的参数；缺少标志本身不能证明实际顺序已改变。[SetWindowPos](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos)、[WM_WINDOWPOSCHANGED](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-windowposchanged)

本机探针还观察到：已有 topmost 窗口收到 `TOP` 请求后，其实际邻居和 band 可以维持不变。这里以实际结果为依据，不把这一观察解释成私有 DWM 或所有 Windows 版本的保证。实现保存已验证场景的顺序，移动后比较邻居和 band；纯顺序事务在检查期间暂停消费捕获帧，再决定保留还是撤销。真实顺序、尺寸、DPI、可见性或采样范围变化仍接受原有检查。

按压另有前后端交接问题：`pressure` 没有改变玻璃几何，后端却为每次更新分配新的几何回执。帧事件早于 IPC 返回时，前端会短暂撤销所有确认。现在同一 viewport、控件顺序、矩形、材质和 track 复用回执；只变 pressure 时仍正常渲染。位置、尺寸、材质、track 或绘制顺序改变必须取得新确认。原生可用时也不同时缩放对应 DOM 玻璃轮廓。

修复前失败记录、修复后浅色/深色实际呈现、动态背景更新、隐藏恢复及测量范围见 [原生复查验收](../artifacts/liquid-glass-runtime-20261005/verification.md)。离屏像素、窗口消息处理耗时和实际呈现分别记录，不能互相替代，也不能把测试循环等待当作实际应用 FPS。

## 2026-10-06 拖动、激活与边框刷新复查

10-05 的修复之后，实际使用中仍有拖动卡顿、拖动时材质消失、点击控件时整窗闪烁。原 runtime 回归里的宿主一开始就在同层最前，所以“移动同时改变实际 z 序”这条路径一直没有被覆盖到。

根因有三处，都会让 `suspend()` 拆掉整个捕获，前端随即退回 CSS 材质：

1. 模态拖动循环每一步都请求 `HWND_TOP`，且不带 `SWP_NOZORDER`。参照 ReactOS 的 `DefWndDoSizeMove` 实现；这里没有在 Windows 上逐条观测消息，以下回归使用相同的参数。只要实际邻居变化（宿主被提到同层最前，或别的置顶窗口出现、消失），`position_change_can_retarget` 就会拒绝平移。重建期间 `validTexture=false`，此后每一步又被拒绝一次，所以整个拖动过程中首帧都不可能被接受。
2. Tao 的 `set_focusable` 用 `SWP_FRAMECHANGED|SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER` 刷新边框，渲染器对 `SWP_FRAMECHANGED` 一律撤回。触发点包括输入框的 `activate_window_focus`，以及固定状态下点击窗口外部。
3. `update_scene` 一旦发现排除列表变化，就撤回输出并报告 `0`。

现在的策略：移动、实际重排和不改变 DPI 与边框样式的刷新，都保留当前捕获和已挂载图像；排除列表原地更新，用 `earliestRetargetFrame` 加 DwmFlush 栅栏拒绝旧场景的帧。辅助窗口被置顶层级变化挤到宿主上方时，把它们移回宿主正下方，不再重建。首帧到达之前的移动只平移 crop，并丢弃旧 crop 的首帧 alpha 读回。同一拖动步骤产生的多个 `WM_WINDOWPOSCHANGED` 只排队一次 reconcile。尺寸、DPI、边框样式和可见性的变化仍然走原有的撤回与重建路径。

`tests/native/glass_runtime_regression.h` 新增 `frame_refresh`、`reorder_drag`、`move_during_startup` 三个场景。同一台机器（144 DPI）上的结果如下，时间为单条窗口消息的最长处理耗时：

| 场景 | 修复前 | 修复后（连续 3 次） |
| --- | --- | --- |
| frame_refresh | 撤回 1 次，捕获重建，69 ms | 不撤回，同一捕获，1.3–1.7 ms |
| reorder_drag | 撤回 3 次，捕获重建，92 ms | 不撤回，同一捕获，3.0–5.2 ms |
| move_during_startup | 捕获重建，81 ms | 同一捕获，2.9–5.8 ms |

原有的“不带 NOZORDER 的移动”在修复前也会间歇性撤回（一次 123 ms），修复后不再出现。

仍需注意的边界：

- 跨捕获预留区时，`replace_shared_scene` 仍在 UI 线程同步执行 `WaitForCommitCompletion`，单次 8–20 ms。微软文档没有说明它能否跨线程调用，因此没有移到工作线程。
- 调整大小的模态循环同样不带 `SWP_NOZORDER`，`prepare_resize` 对这类请求仍会撤回。本轮没有修改调整大小的路径。
- QA fixture 模式下，第三方的全屏置顶窗口（本机观测到 `Qt51513QWindowToolSaveBits`）会触发 `fixture_obstructed`，这属于测试环境因素。生产模式不做遮挡检查。
- 回归测试由 `SetWindowPos` 驱动，没有使用真实鼠标拖动，也不代表实际显示帧率。
