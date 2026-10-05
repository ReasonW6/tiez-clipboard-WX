# Liquid Glass 研究基线与验收方案

研究日期：2026-09-14。本文记录一手资料及针对本项目提出的工程方案；不表示效果已经实现或通过原生窗口验收。本轮读取了 Apple 官方演示页面、逐字稿、文档和开源仓库页面，未播放或逐帧检验官方视频，未运行项目 UI 或获取新的窗口截图。

项目验证状态于 2026-09-15 根据现存代码、测试和 QA 证据更新；本次文档修订未重跑测试或操作 UI。

2026-09-15 后续实际浏览器验收已取得可见参考像素。Apple 中国官方发布图完整解码为 980×551；[官方动态细节视频](https://www.apple.com.cn/newsroom/videos/2025/autoplay/06/apple-introduces-a-delightful-and-elegant-new-software-design/apple-wwdc25-liquid-glass-details/large_2x.mp4)实际播放并查看了 6.791494s 的媒体控制圆盘与 20.316667s 片尾。用户允许其他来源后，[Ars Technica 正式发布期评测](https://arstechnica.com/gadgets/2025/09/ios-26-review-a-practical-yet-playful-update/)的控制中心图完整解码为 1206×2622，[9to5Mac Beta 1/Beta 2 对照](https://9to5mac.com/2025/06/23/apple-tweaks-control-center-visibility-in-ios-26-beta-2/)为 1600×800，均在 `tests/ui/apple-reference.html` 直接显示并查看。Ars 图片涉及下拉伸展状态，没有逐图构建号；测试版差异图仅作演化对照。以上更新替代“始终未看到任何官方或实机像素”的旧边界，但不表示 TieZ 已与 iOS 一致。

## 研究结论

本项目是文字密集的剪贴板工具。液态玻璃应以 Regular 的可读性与环境适应为基线，把折射用于有实际背景输入的控制层。把底色变薄、叠加白色内阴影，不能代替该系统。最高 100% 清透度仍然必须保留可读性约束。

Apple 的参考目标包含形状、背景、前景、光照和运动共同变化。当前资料没有给出可移植至 Windows 的完整渲染参数、光学实现或像素基准。因此可以逐项对照实现并验证，不能仅根据 CSS 或开源项目的名称宣称已经与 iOS 完全一致。

## Apple 的材质边界

| 官方事实 | 对本项目的含义 |
| --- | --- |
| Regular 会模糊背景并调整其明度，适合可能影响阅读的背景及文字较多的界面。Clear 更透明，主要用于照片、视频等富媒体；亮背景下可需要约 35% 的深色衬底。 | 主界面选 Regular 的行为模型；滑块最高档不能自动进入无保护的 Clear。 |
| Liquid Glass 主要承担控制、导航的浮动层，内容层适用标准材质。 | 列表正文维持稳定的阅读承托；工具栏、搜索、选中指示器按功能组织。 |

依据：[Apple HIG Materials](https://developer.apple.com/design/human-interface-guidelines/materials)。这些是公开设计规则，不是本项目当前效果的验收证明。

另一个容易混淆的名称是 iPhone 系统外观设置中的 Clear / Tinted。Apple 支持文档将它们描述为更透明 / 更高不透明度和对比度的外观选择；不能据此把 SwiftUI 的 `Glass.clear` 与本项目的 100% 端点直接画等号。[Apple iPhone User Guide](https://support.apple.com/en-gu/guide/iphone/iphd6804774e/ios)

## 官方演示对应的行为

Apple 在 WWDC25 将 lensing、形变、环境适应作为整体解释：背景经过弯曲；光照响应轮廓；材料尺寸改变时，阴影和散射也变化；前景可读性随着背景保持。其示例还明确避免玻璃叠玻璃，顶层控件可用填充与透明度表达，而非重复施加同一材质。[Meet Liquid Glass](https://developer.apple.com/videos/play/wwdc2025/219/)

官方回看入口分别为 [1:29 Dynamics](https://developer.apple.com/videos/play/wwdc2025/219/?time=89)、[6:00 Adaptivity](https://developer.apple.com/videos/play/wwdc2025/219/?time=360)、[10:31 Principles](https://developer.apple.com/videos/play/wwdc2025/219/?time=631)。后续视觉比较必须记录选取的实际系统版本、组件、背景、交互状态和参考帧，不能只写“参考苹果”。

Apple 的自定义视图 API 默认是 Regular 与胶囊形状；可使用其它形状并启用交互。`GlassEffectContainer` 组织多个效果的合并与形变，同时限制不必要的独立渲染。[Applying Liquid Glass to custom views](https://developer.apple.com/documentation/SwiftUI/Applying-Liquid-Glass-to-custom-views)

圆角也并非越大越好。Apple 的设计系统强调相邻曲线的同心关系，并区分触控胶囊与桌面密集布局中的圆角矩形。[Get to know the new design system](https://developer.apple.com/videos/play/wwdc2025/356/)

## Web 方案能证明什么

| 一手实现 | 读取后确认的技术边界 | 可借鉴之处 |
| --- | --- | --- |
| [Aave Building Glass for the Web](https://aave.com/design/building-glass-for-the-web) | 普通 DOM 路径把位移滤镜施加到自己可控的已渲染内容；文章明确没有从玻璃下方另行采样。滑块与分段选择器提供专门的轨道或高亮内容作为折射输入；视频与 Canvas 场景换用 WebGL。 | 同一形状位移场可用于不同渲染器；前景标签单独保持清晰；移动时不重建形状图。 |
| [samasante/liquid-glass](https://github.com/samasante/liquid-glass) | README 区分 DOM 自身滤镜、Chromium 的 backdrop 滤镜、显式背景副本，以及视频/Canvas 的 WebGL 路径。跨浏览器的“折射背景”不能忽略输入来自哪里。 | 接口明确区分清晰 children 和 refract 输入，避免把活文字送进光学滤镜。其像素效果和兼容性尚未在本项目验证。 |
| [ybouane/liquidglass](https://github.com/ybouane/liquidglass) | README 描述先获得场景输入，再裁切、模糊、着色的路径；这仍是网页场景输入。 | 可用来检查 WebGL 的输入、裁切和特效职责是否分离，不可据此宣称已经折射桌面。 |
| [rukkiecodes/liquid-glass](https://github.com/rukkiecodes/liquid-glass) | README 描述圆角矩形距离场、表面法线、Snell 位移、镜面反射，再输出至 SVG 或 Skia。 | 光学模型应从几何连续推导。该仓库是技术参考，不作为成熟度或与 Apple 一致的证据。 |

CSS `backdrop-filter` 的规范草案将输入描述为所属文档中的 Backdrop Root Image，过滤、变换、裁切后合成；它没有定义跨出 WebView、读取其它 Windows 窗口的能力。该草案的 Backdrop Root 定义尚未取得工作组共识，具体 WebView2 行为仍应测试。[CSS Filter Effects Level 2](https://drafts.csswg.org/filter-effects-2/)

因此，Tauri 中的原生桌面材质与网页内局部光学必须分别记录输入和输出。真实桌面折射需要独立证明有效桌面纹理如何进入渲染链路，以及截图、录屏、移动、多显示器时的行为。CSS 白边、渐变或位移图本身都不是这个证明。

### 实际源码核对

以下约束来自实际源码阅读与数学推导，尚未运行这些开源效果。

- `rukkiecodes` 的 [geometry.ts](https://raw.githubusercontent.com/rukkiecodes/liquid-glass/main/core/geometry.ts) 使用圆角矩形距离场、中央差分法线并将圆角限制在短边一半。[rasterize.ts](https://raw.githubusercontent.com/rukkiecodes/liquid-glass/main/core/rasterize.ts) 将轮廓外与内部平台设为无位移；[surface.ts](https://raw.githubusercontent.com/rukkiecodes/liquid-glass/main/core/surface.ts) 对边缘斜率限幅，[refraction.ts](https://raw.githubusercontent.com/rukkiecodes/liquid-glass/main/core/refraction.ts) 抑制平坦中心高光。这支持“同一几何、有限边缘带、明确平坦中心”的样板路线，不证明当前星形的根因。
- 轮廓外的位移图使用完整 alpha 的中性灰，不能留透明黑。`samasante` 的 [displacement.ts](https://raw.githubusercontent.com/samasante/liquid-glass/main/src/displacement.ts) 同样如此；其 [Glass.tsx](https://raw.githubusercontent.com/samasante/liquid-glass/main/src/Glass.tsx) 使用单独的形状 alpha 约束最终结果。源纹理采样出血与结果裁切必须分别处理。[W3C feDisplacementMap](https://www.w3.org/TR/filter-effects-1/#feDisplacementMapElement)
- `samasante` 的 RGB 矩阵保留各路完整 alpha，再以 arithmetic 累加。若三路都采到均匀 alpha 0.2，则合成 alpha 可成为 0.6。项目修复前的 `GlassLens.tsx` 存在同样结构，后续真实 Edge 栅格测试复现了半透明输入 alpha 被累加的问题；现已改为合并直通 RGB 后恢复单一 alpha，并加入回归。开源实现本身的行为仍是源码推导，不能混同为本轮实测。[Glass.tsx](https://raw.githubusercontent.com/samasante/liquid-glass/main/src/Glass.tsx)、[W3C feComposite arithmetic](https://www.w3.org/TR/filter-effects-1/#feCompositeElement)

## 本项目拟采用的方案

以下条目是根据上述资料与用户约束提出的工程决策，不是 Apple 公布的参数。

用户已明确选择以 iOS 控制中心为方向，让控件具有更明显的体积感。后续样板据此校准曲面、边缘折射、高光和阴影，仍须保持内容可读，并按固定 Apple 参考逐项验收形状、光照、模糊、折射、色散、亮度适应与动态；这一方向选择不表示视觉一致性已经达成。

1. **统一底色与安全范围。** 保留一个清透度滑块和现有持久化字段。窗口底色只有一个连续来源；标题栏、主体、底栏不能各自增加不同大块底色。清透度映射必须带可读性下界，前景不随全局 alpha 淡出。滑块端点不关闭保护、不切换材质、不产生跳变。
2. **先校准可读性，再校准外观。** 明暗主题分别确定最不利背景下的前景/底色组合。对于固定前景与固定色罩，按实际混合公式验证黑白极端及高饱和色；背景可动态读取时，再验证亮度调整的时间稳定性。纯色背景不会因为增大模糊而改变亮度，不能用模糊代替承托。
3. **一个部件只保留一套轮廓表达。** 标题栏按钮组先移除重叠的边框、镜面内阴影、整窗内高光，再由同一形状与统一光照生成边缘。不能在固定角落放星芒或通过相交圆弧充当焦散。真实焦散如要加入，必须能解释输入、形状和运动关系，并通过独立样板。
4. **光学背景与内容分离。** 列表文字、搜索文本、图标和焦点环处在未折射的前景层。位移必须只作用于明确的背景输入；面板中心不承担无功能的持续变形。色散强度需要参考帧校准，不能把彩边强度当作液态感评分。
5. **几何一致。** 胶囊半径来自实际高度；圆角矩形半径受短边约束。实际 CSS 几何、位移图尺寸和裁切形状保持一致；改变宽高、缩放、DPI 和圆角时同步更新。滤镜取样预留出血区域，输出严格裁切回最终外形。
6. **交互有起因。** 按压、选中切换和菜单展开可以触发柔和形变与光照变化；静止窗口不循环闪亮、呼吸或播放装饰动画。减少动态效果时保持位置、状态和焦点反馈，停止非必要形变。
7. **分开判断三种材质。** 液态玻璃检验折射与几何；毛玻璃检验散射、噪声和阅读稳定；云母检验平静、稳定的背景层级。三种模式共用可读性门槛，不能只改模糊半径和透明度便宣布达成不同材质。

## 验收矩阵

测试中的背景不得采用用户剪贴板私有内容；固定虚构文本和可重复图案。以下是完整验收要求；局部已有证据与其边界见下节，不能据此将整张矩阵标为通过。

| 维度 | 样本与操作 | 通过条件 |
| --- | --- | --- |
| 清透度 | 0、1、25、50、75、99、100；附连续拖动 | 无端点突变，任何档位文字与操作状态都保持可辨。 |
| 背景 | 黑、白、中灰、黑白交界、细字、高饱和色块、棋盘格、移动图案 | 普通模式直接通过；不得先开启“减少透明度”再替代普通模式结果。 |
| 文字 | 标题、正文、元信息、占位符、搜索命中、标签、按钮 | 小字与真实合成背景至少 4.5:1；不要拿抗锯齿边缘像素计算字体颜色。 |
| 操作信息 | 图标、选中态、键盘焦点、必要控件轮廓 | 必需的非文本视觉信息与相邻色至少 3:1；不能仅以微弱高光区别状态。 |
| 几何 | 最小窗口、常用尺寸；100%、125%、150%、200% DPI；按钮组、搜索、滑块 | 四角无星形、尖瓣、外溢圆弧、双边或裁切缺口；中心没有固定伪影。 |
| 真正折射 | 标准网格置于可声明的输入层下；移动背景、移动部件、改变部件宽高 | 轮廓附近出现与法线一致且连续的偏折；前景文字保持原形；没有重复旧帧、错误像素源或自身反馈。 |
| 动态 | 按压、释放、切换、滚动、展开、失焦 | 没有闪烁、突然换色、残影、滤镜滞后、静止时无原因发光；减少动态效果单独通过。 |
| 原生可用性 | 截图、录屏、显示器切换、窗口移动与恢复 | 窗口与正文在捕获中正常出现；不得依靠把用户窗口从捕获中隐藏来满足特效。 |

文字和非文本门槛来自 [WCAG 2.2 Contrast Minimum](https://www.w3.org/WAI/WCAG22/Understanding/contrast-minimum.html) 与 [Non-text Contrast](https://www.w3.org/WAI/WCAG22/Understanding/non-text-contrast.html)。这些可读性门槛不能单独证明视觉上与 Apple 一致；最终还需要在固定参考条件下比较轮廓、折射幅度、高光宽度、阴影与动效。

## 已有证据与尚未覆盖的边界

- **右上角伪影已有受控定位。** 实际 WebView2 测试窗口与固定灰底 Edge 页面中观察到的右侧双弧，在关闭 reflection 后仍存在；保留 refraction、移除父层底色、边框和内阴影后消失，已证实自身装饰进入折射输入。定位过程见[重建记录](MATERIAL-REBUILD-PLAN.md)，搜索框和标题按钮组的双弧回归见 [glass-optics.spec.ts](../tests/ui/glass-optics.spec.ts)。这项证据对应已复现的双弧，不能代替全部控件、动态尺寸与 DPI 的几何验收。
- **RGB 单一 alpha 修复已有回归证据。** [GlassLens.tsx](../src/shared/components/GlassLens.tsx) 已改为合并直通 RGB 后恢复绿色位移通道的单一 alpha，再独立应用覆盖率。真实 Edge 栅格回归已通过；其中颜色通道测试使用 alpha 为 0.25、0.5、0.8、1 的均匀色块，在独立覆盖率合成前检查中心 RGBA。修复前后的运行状态保存在 [optics-before](../artifacts/material-redesign/optics-before/.last-run.json) 和 [optics-after](../artifacts/material-redesign/optics-after/.last-run.json)。这些受控结果不能外推为动态非均匀透明输入、所有边缘及 Windows 最终合成均已通过。
- **跨 WebView 的输入已接入实际 TieZ，尚未完成验收。** 早期[重叠失败证据](../artifacts/material-redesign/qa/native-overlap-failure-analysis.json)暴露了窗口 owner 分组导致的自身反馈；后续探针通过解除 helper/output 的不当归属并排除输出组，在受控重叠布局取得持续新帧与像素保真证据。实际前端现在通过 C ABI 使用该原生管线，QA 只采集项目自有背景；原生准备好后才接管 CSS 底色。滑块轨道现已接入正确层级并通过 GPU/前端回归，region 查询恢复与移动/缩放动态仍在验证；纯移动 crop 更新已被实际像素否决，新 scene 替换仅为受控候选。详见[实际接入记录](MATERIAL-REBUILD-PLAN.md)。跨屏、HDR 与完整移动缩放仍未验收。
- **动态亮度适应尚未验证。** 仍需确认亮度估计能否读取最终材料输入、是否存在延迟与频繁变色；无法可靠获得时需要稳定承托，不能猜测前景颜色。
- **捕获可见性已有局部原生证据。** 受控探针的截图以及 GDI、Desktop Duplication 固定区域录屏已有记录，两种录像各解码出 120 帧，见[录屏验证数据](../artifacts/material-redesign/qa/native-recording-verification.json)和[重建记录](MATERIAL-REBUILD-PLAN.md)。这不覆盖所有录屏工具、生产透明圆角窗口、HDR 或混合 DPI；输出仍须保持普通截图与录屏可见。
- **Apple 视觉与动态一致性尚未验收。** 控制中心方向和已有曲面样板均不能替代实际参考比较。还需固定系统版本、组件、背景与交互状态，逐项检查轮廓、折射幅度、高光宽度、阴影、亮度适应及按压、切换、展开等动态。

研究完成后只允许将已有证据支持的路线推进到样板；样板通过再接入实际界面。没有原生视觉与捕获证据时，记录为未验证，不能将“规范已参考”“构建通过”替代材质效果完成。
