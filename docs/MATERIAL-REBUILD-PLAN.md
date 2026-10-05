# 材质重建与验收

2026-09-14。目标是以 Apple 官方 iOS Liquid Glass 的视觉与动态为对照，同时修复三种材质在普通模式下的可读性。截图与录屏必须保留。完整视觉一致性尚未验证；以下修复与原型不能单独作为完成证明。

## 背景自适应调色候选（2026-09-27）

参考侧取得了可用于更直接比较的原生 SwiftUI 样例：作者公开了 `.regular` 代码、日落 JPEG 与 Xcode iPhone 预览 PNG。仅以裸露背景拟合居中 fill 的位置/缩放，独立右侧验证区域的平均绝对 RGB 误差约 1.43/255。这建立了该图的背景对应关系，还没有完成玻璃参数比较，也不代表已锁定控制中心或系统 build。当前页脚间距调整后的 8 个原生离屏场景同时复验通过。详见[最新原生布局与配准参考](../artifacts/material-redesign/qa/native-footer-and-ios-reference-20260927/VERIFICATION.md)。

交接检查另确认了一处现有差异：原生控件保留白色法线反射，CSS 备用控件再次叠加主题 tint，导致深色时提亮与压暗相反。现在统一了这层反射，保留宿主交互颜色。108 个平坦点的最大差由 14 降到 2 个色阶，72 点与既有实际 HLSL 输出交叉核对；这只证明中心色调，不证明空间采样或真实交接时序相同。完整备用界面复验并修复页脚内边距后，828 个文字片段通过。详见[色调交接验证](../artifacts/material-redesign/qa/fallback-surface-tone-20260927/VERIFICATION.md)。

后续在透明输入中发现模糊中间纹理的精度问题：8 位预乘 RGB 与 alpha 分别取整，归一化后会在低覆盖尾部产生色偏。当前仅将原生窗口及完整捕获区域的横向模糊中间纹理改为 RGBA16F，输出仍为 BGRA8，未采用新的自适应调色曲线。直接调用生产渲染方法的同色半平面回归在修改前 36 项中 35 项失败，修改后全部通过；详见[精度修复与离屏复验](../artifacts/material-redesign/qa/blur-coverage-precision-20260927/VERIFICATION.md)。

重新核对 iOS 26 官方控制中心插图与 WWDC 自适应说明后，离屏比较固定 tint、局部最小 tint、共享预处理三种处理。各 60 场景、1520 个文字片段通过可读性检查，但局部最小 tint 压平灰阶；直接在 prefix/BlurPS 各自应用还产生了顺序差异。真实 WARP 棋盘反例的额外明暗差为 33–39 个色阶，共享预处理模型降至 1 个色阶内，但它不能修复调色曲线自身的平台。

本轮未接入生产。后续方案须同时保证灰阶连续、两条采样路径处理一致，以及原生/CSS 回退外观一致。固定布局、原始像素、官方参考及证据边界见 [实验结论](../artifacts/material-redesign/qa/control-optics-reference-20260927/DECISION.md)。这不是同条件 Apple 像素校准，严格一致性目标保持未完成。

## 捕获来源研究更新（2026-09-27）

当前帧时间戳下限不能证明私有场景的 crop／排除集合已经生效；把下限后移或重建帧池，也缺少来源保证，并可能丢掉唯一的新静态帧。本轮未将这些猜测接入生产。

一手文档检索与本机无窗口探针发现了值得优先验证的公开接口方向：Windows 25H2（26200.8246）的元数据暴露 `ConfigurationIteration` 和 `IDisplayGraphicsCaptureSession` 的窗口排除方法。只查询普通 `GraphicsCaptureSession` 会漏掉后者。接口存在不代表已证明排除后能取得正确的下层背景，或编号与捕获帧有可依赖的对应关系。

后续未启动会话检查进一步发现：本机实际显示捕获会话查询排除接口返回 `E_NOINTERFACE`；配置编号接口虽然可查询，getter 返回 `E_NOTIMPL`。因此暂不采用这条公开路径替换私有场景桥。探针没有启动捕获或读取帧，结果仅限本机与所测创建条件，详见 [实际接口检查](../artifacts/material-redesign/qa/capture-session-api-20260927/RESULT.md)。

候选方案仍须以本项目虚构背景的真实来源像素验证。用户当前要求仅做代码和离屏验证，没有打开测试窗口或启动捕获；固定 ROI 复用、普通截图策略和生产渲染保持原状。来源分析、候选与验收条件见 [研究记录](../artifacts/material-redesign/qa/capture-scene-generation-20260927/REFERENCE.md)，元数据初查见 [能力元数据](../artifacts/material-redesign/qa/capture-scene-generation-20260927/capabilities.json)。

## 已核对的依据

- [Apple HIG Materials](https://developer.apple.com/design/human-interface-guidelines/materials) 与 [Meet Liquid Glass](https://developer.apple.com/videos/play/wwdc2025/219/)：文字密集界面采用 Regular 的可读性与环境适应，避免玻璃叠玻璃；控件与内容分层。
- [Aave](https://aave.com/design/building-glass-for-the-web)：几何位移可以用于受控网页内容，不提供跨 WebView 的桌面纹理。
- Windows 背景来源与 API 限制见 [Windows 研究](WINDOWS-GLASS-RESEARCH.md)，视觉标准见 [对照研究](LIQUID-GLASS-REFERENCE.md)。

## 已复现的问题

1. 当前代码清透端窗口 alpha 为 0.06–0.10，普通模式浅色标题压黑底的静态对比度约 1.09–1.18:1。原有跨背景测试先强制减少透明度，未守住正常使用场景。
2. 在实际 WebView2 测试窗口与固定灰底 Edge 页面中均观察到右侧双弧。关闭 reflection 后仍存在；保留 refraction、移除父层底色、边框和内阴影后消失。这证实折射输入包含了自身装饰。
3. 原 RGB 合成累加了三路 alpha。真实 Edge 栅格回归以半透明色块验证这一缺陷，不能仅检查 CSS 声明。
4. `#root::before` 的高光声明被 `display:none` 覆盖，当前不实际绘制。排查不得把未生效的声明当作画面来源。

## 实施顺序

1. 恢复普通模式的三材质、明暗模式、全滑块范围可读性检查。基于实际前景色求窗口统一底色下界；保留轻量同色子面板，避免复原多段厚实底色。保存键和滑块方向保持兼容。
2. 重新分离背景采样与前景装饰，只保留一套轮廓。保持单一输出 alpha，中心合成不改变原背景，边缘映射不重复采样同一轮廓。分别验证透明页面与不透明页面。
3. 先做公开 API 原生探针：仅采集本项目自有背景 HWND，WGC 帧进入 D3D11 shader，前方输出窗口保持 WDA_NONE。验证遮挡下的持续更新和输出捕获；不采集用户其他应用，不改变系统隐私设置。
4. 单来源探针通过后，继续验证与 Tauri / WebView2 的合成与实际背景选择。尚未验证的多来源、跨屏、HDR、遮挡与重建不能包装成全桌面支持。高级管线未通过前不替换生产背景。

## 验收门槛

- 普通模式直接检查透明度 0/1/25/50/75/99/100，黑、白、灰以及含文字/细线的复杂背景；正文、标题、元信息与可操作控件可辨认。
- 半透明 RGBA 输入在平坦中心保持原值；边缘细线发生真实位移，均匀背景不产生第二条内部弧线。
- 真实 WebView2 复查标题按钮组、搜索框、选择胶囊、滑块，覆盖明暗、窄窗口、动态缩放与不同 DPI。
- 捕获探针记录来源、输出窗口 display affinity、有效纹理尺寸与帧更新；通过外部窗口截图检验输出确实可见。单窗口截图不能代替整屏录屏验证。
- 与 Apple 原始视觉示例逐项比较形状、光照、模糊、折射、色散、亮度适应与动画。编译成功、API 成功或测试数量都不能替代这个门槛。

本轮生产改动前的文件副本保留在 `artifacts/material-redesign/baseline`，用于保护已有未提交改动与回滚。临时诊断文件在任务结束后清理，保留有用的回归测试和验证证据。

## 控制中心方向的当前进展

用户明确选择较明显的控件体积感。早期原生单来源和双来源探针接入同一份原创 `glass_optics.hlsli`，以圆形、圆角方形、横向胶囊和竖向滑块验证连续轮廓、折射与单一几何高光。以下按时间记录探针证据；实际 TieZ 的接入和遗留问题见末节。

2026-09-14 的双来源 GPU 集成检查固定同一份背景纹理，对比开关折射后的 BGRA 渲染结果。七个控件各自的 3×3 中心区域差异均为 0，输出内 22,278 个像素的变化超过 10 级。证据为 `artifacts/material-redesign/probe/controls-gpu-evidence.jsonl`。这验证了原生管线中的中心稳定与实际位移，不等于已经完成 iOS 视觉、动画或全桌面取样。

Computer Use 返回的原生截图实际为 JPEG，已改用对应扩展名；这类画面用于视觉对照，不能把压缩后逐像素差异当作 shader 的无损结果。控件截图保存在 `artifacts/material-redesign/qa/native-controls-refraction-*.jpg`。

完整 DWM 场景取样已取得受控原型证据：在仅包含本项目窗口的固定裁切区域，私有 shared visual 经被遮住的 helper 和公开 WGC 进入可采样 GPU 纹理；120 秒内 2,537 帧全部通过来源标记检查，普通截图可见输出，镜像 shader 生效。探针只对内部 helper 设置 live-preview 属性，最终输出保持普通捕获。原生 Acrylic 样板仍为灰色实底，真实背景模糊保留、材质属性更新、跨屏与生产集成尚未验证。详见 `src-tauri/examples/dwm_scene_probe.md`，不可据此宣称完整桌面或 iOS 一致性已完成。

后续已在同一个场景桥中实际显示七种曲面，完成取样与光学的组合验证。legacy native blur 样板曾呈黑底；改用项目现有的 Composition BackdropBrush + GaussianBlur，并把前景标签放在独立 surface 后，源端模糊及其采样保留已在受控范围内通过：模糊 12／0 两种状态下，完整 320×360 场景的屏幕 PNG 对照均为 RGBA 差异 0。数值与图像在 `artifacts/material-redesign/qa/native-composition-blur-screen-analysis.json` 及对应 PNG。

GDI 和 Desktop Duplication 各完成约 4 秒的固定测试区域录屏，均解码出 120 帧，已查看视频解码帧确认七控件曲面与动态背景可见。文件为 `qa/native-controls-gdi-recording.mp4`、`qa/native-controls-dda-recording.mp4`。编码帧数不代表材质原生刷新率，也不证明全部录屏工具／HDR／混合 DPI 兼容。

2026-09-15 已补齐同位置辅助组的启动和停止录像，输出 alpha 读回全为 255，未检测到洋红色诊断遮盖层泄漏。启动／控件切换与停止分别由两段录像覆盖，具体帧数、日志和限制见探针说明。

随后 F6 重叠取景测试失败：输出由 C 移至固定源区域 A 后，三个新候选帧中的四色标记成为 F3 输出的左右镜像，说明链路采回了自身。候选帧被拒绝，辅助组停止；正确显示的缓存截图不能证明持续新帧有效。证据为 `dwm-scene-probe/fourteenth-overlap-failure-evidence.jsonl` 与 `qa/native-overlap-failure-analysis.json`。此前在旁侧 C 的取景保真证据仍成立，但不能外推为可覆盖源区域的生产后端。

第十五／十六轮逐次只解除 output、helper 与源 base 的 owner 关系：只解除 output 后，错误输入变成 cover 的洋红色；两者均解除后，F6 重叠、镜像、七曲面和移回 C 均持续取得有效新帧，最终 2,253／2,253 帧通过。20 秒 Desktop Duplication 录像解码为 1,187 帧，完整测试区域未检测到洋红遮盖层。该对照证明本机此链路受窗口归属关系影响，没有证明私有 DWM 内部所有分组语义。证据为 `dwm-scene-probe/sixteenth-isolated-overlap-evidence.jsonl`、`qa/native-isolated-overlap-video-analysis.json`。

第十八轮补齐重叠后的完整像素保真：冻结背景后开启镜像并移入 A，再将源 GaussianBlur 从 12 改成 0、恢复 12，以新输入验证材质更新。移走验收鼠标光圈后，完整 320×360 屏幕画面与原始参考的镜像逐像素一致，115,200 个像素的 RGBA 差异全为 0。日志证明两次材质变更均取得新帧；静止场景不要求连续重复产帧。证据为 `qa/native-isolated-clean-pixel-analysis.json` 与 `dwm-scene-probe/eighteenth-clean-pixel-evidence.jsonl`。

第十九轮仅给 helper／cover 增加 `WS_EX_TOOLWINDOW`，运行时检查到实际样式位。重叠镜像与七曲面保持 1,477／1,477 帧有效，GDI 20 秒录像解码 599 帧，未检测到洋红遮盖层。第 506 帧从镜像标记回到原始标记，证明录像覆盖了 F1 停止；未操作系统任务切换界面。该样式的任务入口行为依据 Windows API 契约，不冒充完整 Shell 交互验收。

第二十一／二十二轮已接入独立透明 WebView2 测试前景，原生 output target 使用 children 后方层。实际输入 `sample 42`、点击计数 1、焦点内 F1/F3/F5/F6 及 Escape 均已操作验证；背景镜像／折射时，前景文本保持方向及输入状态。第二十一轮 3,359／3,359 新帧有效，20 秒 Desktop Duplication 录像包含清晰前景和动态曲面。第二十二轮的启动录像覆盖 F1 前后，1,769 帧没有超过 90% 输出区域呈白色的帧，也无洋红泄漏；这使用已有隔离 profile，不外推为全部设备或帧间瞬态。详细证据在 `qa/native-webview-foreground-verification.json`。

圆角阶段随后修复两项实际缺陷：内部 WGC 指示框露出，以及 GDI region 少保留最右／下边界导致镜像黑列。仅在只读许可查询返回 Allowed 时设置内部会话的边框偏好，没有请求新许可或改变系统设置；实际 PNG 证明角落恢复背景色。正确保留完整区域后，64,880 个可见背景像素（含 564 个圆角外像素）逐像素一致；刻意不镜像的前景包围区域单独验证。第二十六轮 30 秒录像明确覆盖 C→A 移动与 F1 停止，1,773 帧没有洋红泄漏或顶角黄色指示框，运行 1,214／1,214 帧有效。各项证据与限制见 `qa/native-rounded-geometry-pixel-analysis.json` 和 `qa/native-rounded-motion-video-analysis.json`。

上述独立样板提供来源与合成证据，不作为控制中心材质完成版。

## 实际 TieZ 接入（2026-09-15）

已把场景桥与 GPU 曲面渲染提取为 `native/glass_renderer`，通过静态 C ABI 接入实际 `window_material.rs`。原 WUC 背景与原生 DComp target 按顺序释放、接管；云母和毛玻璃继续使用可读基线。前端 `GlassLens` 提交实际控件几何与按压状态，只在原生首帧完成后关闭重复底色和光学层。失效、强制颜色及减少透明度时恢复可读样式；被祖先裁剪或正在淡出的控件暂留 CSS 路径，尚不算完整原生动态验收。

构建阶段已通过生产 bin 与隔离 example 的编译和实际链接、4 条相关 Rust 测试、34 条前端单元测试，以及 13 条协议／SVG 光学回归。这些结果证明接线和局部行为，不能替代真实窗口、截图和 iOS 视觉验收。

`material_probe.rs` 现在运行真实前端及五条虚构剪贴板记录，背景来自同进程的全屏不透明 fixture。原生 QA 参数只允许该窗口覆盖范围内的取景，异常时停止；没有运行真实数据库、剪贴板监听或启动项。fixture 提供背景、隐显、移动和窄宽切换。当前 Windows 将 1420 列为保留端口，因此本机验收使用测试参数 `TIEZ_MATERIAL_QA_PORT=5173`，没有更改系统端口保留配置。

原生首版沿用可读基线的 tint 范围；它不是 Apple 参数，也尚未解决宽清透范围与动态亮度适应的最终观感。移动目前停止并重建内部捕获，仍须检验帧间底色、延迟、圆角与前景保持。没有任何路径给最终用户窗口设置捕获排除。

Apple 对照版本固定为 [iOS 26 控制中心](https://support.apple.com/guide/iphone/use-and-customize-control-center-iph59095ec58/26/ios/26)，避免无版本说明页切换至其他版本。早期官方图片未成功显示；后续经 Apple 中国访问路径及用户授权的第三方实机报道，项目参考页已实际显示官方图、播放官方细节视频，并显示 Ars Technica 正式发布期控制中心图和 9to5Mac 版本对照。具体尺寸、时间点和版本限制已记入参考文档；TieZ 的逐项视觉和动态一致性仍未验收。

实际接入随后暴露了独立样板没有覆盖的问题，当前状态仍为未完成：

- 无装饰主窗口的 outer/client 相差左、右、下各 8 像素及顶部 1 像素。初版直接复用 client 区域失败，下一版虽能显示但普通截图出现白色侧边。现在 host 的 region 使用平移到 outer 坐标的圆角 client 矩形，内部 helper/cover 再还原为 client 坐标。失败截图 `qa/tiez-native-first-ready-screen.png` 保留用于追溯，不能充当修复后证据。
- WebView2 的项目内“设置”提示浮窗曾被 QA 来源检查误判。现只允许经过主进程直属子进程、映像、创建时间、存活句柄和特定非激活样式验证的项目 tooltip；它仍从采样场景排除。修复后实际悬停、打开设置和滚动时可保持原生 ready。
- 原生与备用 WUC 两条退出路径已实际验证退出码 0。退出时先退休材质资源、关闭 Compositor，再等待拥有的当前线程 DispatcherQueue 关闭完成；异步关闭期间保留消息泵。强制重启路径尚未验证。
- 圆角外透明像素参与模糊曾造成黑边；两次 Gaussian pass 现在携带预乘颜色与覆盖率，纵向 pass 后才归一化。实际 WARP 运行覆盖 24 组均匀、彩色与透明输入，对比旧实现能暴露缺陷。均匀灰最大误差 0，彩色至多 1/255。它证明边界取样修复，不证明 iOS 光学一致性。
- 标题收缩动画曾把圆形按钮拉宽并令关闭按钮越出窗口；现在父组与子按钮协调位置动画。250/352/425 像素、正常/减少动态和连续快速切换回归通过，实际设置入口也已复看。
- `qa/tiez-native-track-screen-run.txt` 的后续分诊证明，临时 HRGN 有效时，主窗口查询仍可能瞬时失败或持续不可读。现保留拥有的完整 mask，一次 fresh HRGN 重读后仍须全区域相等；持续缺失则丢弃当前帧、停止捕获后有界重建。实际例程两次故障注入已分别取得恢复 ready、再次同形状丢失后稳定 failed 的证据，尚未证明上游由哪个调用触发。
- 滑块轨道已和拇指同批提交到原生渲染，放在 blur/tint 之后、折射之前；DOM 保留数值、文字及单一键盘焦点。真实 Edge 协议与实际 WARP 光学回归通过，实际窗口完成 0/50/100 操作。只移动几何时复用背景模糊结果；完整动态延迟仍待测。

上述新日志均位于 `artifacts/material-redesign/qa/`。公共窗口缩放权限与边缘命中区已接入并通过协议测试，真实鼠标缩放、最新普通录屏、完整明暗与清透端验收仍须继续。

轨道接入、三材质标签控件对比度、普通截图/录像及当前失败证据见[本轮实际验收记录](../artifacts/material-redesign/qa/tiez-track-integration-20260915.md)。最小化状态分类修复后可恢复 ready，但真实录屏继续发现辅助 cover 在恢复动画露边。离屏辅助候选的后续 90 秒、5,342 帧复验没有诊断色线或大黑块，且恢复前后背景持续更新，详见[离屏恢复报告](../artifacts/material-redesign/qa/offscreen-restore-audit.md)。生产默认离屏及显示布局变更的安全重建已实现，实际修复版仍需复验；移动时旧背景/回退过渡尚未通过。云母与毛玻璃的明暗两端已有实际系统截图。Apple 像素与动态一致性仍未完成。
