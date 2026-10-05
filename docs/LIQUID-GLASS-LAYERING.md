# 控件独立取样与分层材质

2026-09-23，进行中的材质迭代。用户最新要求是同一表面整体清透或整体柔化，撤回此前“清晰边缘、模糊中心”的方向。尚未证明与 iOS 完全一致。

## 本地问题与量化

旧管线把整个窗口先做 Gaussian blur，再混入74%–90%的底色，最后把同一张tintedView交给所有控件的折射。完整GPU链测试中，搜索框字形的色散峰值由锐利输入的241/255降为最清端13/255，中间档约2/255。旧69项数学回归不能代表实景强度。完整输入、负对照和369组数值见[诊断记录](../artifacts/material-redesign/qa/glass-full-pipeline-diagnostic.md)。

## 参考与判断

Apple明确将几何高光、内容相关阴影、透镜成像和散射共同用于层次；较大的玻璃会表现出更厚的材质。不能据此把所有嵌套控件都简单叠加相同模糊。[Apple WWDC25](https://developer.apple.com/videos/play/wwdc2025/219/)

作者实现提供了曲面轮廓、法线与透射位移的公开推导，但明确属于近似，没有逐像素复现Apple的承诺。[Kube原作者文章](https://kube.io/blog/liquid-glass-css-svg/)

另一个公开实现把同一背景的不同mip用于清晰边缘和散射中心，并让各个控件独立选择模糊级别。这是可检查的工程思路，不能把作者对Apple的描述当作Apple实现证明。[LiquidGlass源码作者说明](https://github.com/anuero/LiquidGlass)

## 当前设计：每块表面统一清晰度

交互控件使用 `clear`，剪贴板文字卡片使用 `regular`，设置正文的独立阅读底层使用 `reading`。`GlassSurfaceLod` 不接收像素位置、深度、半径或控件尺寸，因此每块表面只有一个过滤强度；曲率与体部投影改变折射、RGB位移、受光和轮廓覆盖。

在DPI=1时，clear对应清透度100/50/0的LOD为0/0.5849625/1。最清端整面读取已完成下层的mip0，不再额外模糊中心；如果没有下层，输入才是一次着色后的原始背景。regular使用整面一致的 `sqrt(12) * max(2*dpi, windowSigma)` 足迹，reading保留 `sqrt(12) * max(4*dpi, windowSigma)`；各自的边缘与内部一起柔化。2026-09-28的原生同背景对照支持降低regular清透端的额外散射，设置阅读面保留原有过滤。该调整不改变色散IOR间隔、固定tint或独立阴影，也不表示Apple像素完全一致；对照与调色顺序实验见[本轮记录](../artifacts/material-redesign/qa/tone-order-20260928/comparison.json)。

材质角色经显式可选字段传递，旧JSON默认clear。C/Rust control ABI为84字节；GPU使用原shape.z预留位，6208字节常量缓冲不变。设置阅读底层固定在滚动视口背后，各控件位于其前方。

2026-09-23后续已接入逐层光学前缀。原始预乘背景只加入一次全局tint，每个轨道在所属thumb前绘制一次；玻璃读取已完成下层的RGBA16F颜色和有效alpha，另存R16F联合覆盖率。最终用 `Cfinal + (1-Mfinal)*(windowBase-C0)` 替换未被控件覆盖的部分，避免再次乘圆角覆盖率。前景文字、图标和CSS阴影不写入光学纹理。前端依据实际 `.glass-lens` 的嵌套stacking context、z-index和flex/grid修正树序排序，不能使用React注册次序。方案推导与参考源码见[研究记录](../artifacts/material-redesign/qa/native-recovery-20260923/LAYER-REFERENCE.md)。

两层反射提高深色表面的亮度，液态玻璃专用muted、link、danger前景分别调整为`#e1e7f0`、`#e0eaff`、`#ffdde5`。两层阅读区域最低对比度实测4.534:1。新管线2460项完整GPU回归通过；五条旧的“相同轮廓仅保留最上层”断言已替换为真实叠层、轨道不重复和一次全局tint检查。折射、色散及4.5:1门槛未降低。实际窗口设置页序列被关闭指令中止，此后用户要求仅代码与离屏验证，因此新管线的完整实机视觉和动态验收仍未完成。

[先前统一材质验证](../artifacts/material-redesign/qa/uniform-glass-20260923/IMPLEMENTATION.md)记录当时2443项完整GPU、105项隔离、4201项RGB投影、7项Rust、36项UI与构建结果。144组完整位图和常量过滤对照逐字节一致，旧分区过滤负例均被检出。原有中心必须加深的测试目标已按用户新要求替换，旧结果保留为历史证据。

## 2026-09-23 体部投影接入

此前平中心的法线为正Z、入射恒为-Z，因而整片中心没有折射。新实现把有限厚度平板在虚拟透视视线下的横向位移，平滑接到现有肩部模型：比较体内光线与无折射光线在同厚度接收面的坐标。体部方向由连续的 `(pixel-center)` 给出，不使用会切换轴的SDF法线；其过渡权重在中轴处已为常量。肩部与体部仍是UI近似，并非Apple实现或完整体积追迹。

相机距离为 `max(6*restingHeight, .3*max(width,height))`，全部按物理长度同尺度变化。采用静止厚度确定相机位置，防止相机随按压一起移动、抵消体部的厚度响应。单点中心保持不动，其周围取样坐标会改变，因此产生内部成像，而非只增强边缘彩色。

接入前的8组实际GPU/网页对照见[候选筛选](../artifacts/material-redesign/qa/dispersion-study-20260923/EXPERIMENT.md)，主源码、DPI、按压、几何及60组实际布局的检查见[集成验证](../artifacts/material-redesign/qa/body-optics-validation-20260923/INTEGRATION.md)。原无折返、source范围、alpha、色散/对比度门槛继续保留；历史的“中心坐标必须不变”断言已由独立平板成像参考代替。真实窗口动态与iOS一致性仍未通过，不能从这些离屏检查推导完成。

## 已撤回的2026-09-22分区散射设计

以下保留当时的推导和实验记录，不代表当前清晰度策略。

窗口继续使用原可读基底。控件另读保留原始预乘颜色的scene mip链，用曲面深度和控件短边决定采样层级，清晰肩部向散射中心过渡。小标签、搜索框和较大卡片由尺寸得到不同的中心散射量。R/G/B保留原色散系数，改用恒定接收平面的Snell投影距离，避免局部高度在曲率最强的外沿归零、同时压掉位移。

D3D11资源显式要求mip生成所需的格式支持和绑定标志，新捕获帧仅复制LOD0并生成下级mip；几何和按压改变不重复生成。两张SRV绘制后一起解绑。依据[GenerateMips契约](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-generatemips)和[SampleLevel契约](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-samplelevel)。

控件外阴影仍由一层持久CSS绘制；卡片/文本区与小控件有不同承托范围。前景文字不进入光学输入，普通截图录屏可见，已有来源校验和圆角处理不变。

## 已拒绝的第一轮候选

初次独立取样候选让肩部的tint低于中心（.36对.74），在纯灰背景产生显著深色厚边。实际普通截图和视频验证其观感偏向凸起塑料，因此已拒绝该参数；它不能算作成功复刻。证据为layer-comparison-gray-candidate.png，与layer-comparison-gray-before.png是同一个灰底、同一位置的解码帧。

当前改为肩部和中心使用相同可读tint，仅让局部成像/散射不同。常见半径下压力厚度被几何上限截断的问题已修复；固定上限负对照及松手恢复由GPU回归覆盖，不能据此代替实机按压验收。窗口大sigma的稀疏Gaussian taps混叠另作独立对照，见下文。

## 2026-09-22 曲面选择

同一布局、源图、tint、LOD和阴影下，比较旧cubic/local、有界squircle/local、cubic/constant三个GPU原型，并分别关闭高光查看成像。选择cubic/constant：保留原平滑法线和平台零支撑，增加实际背景位移；有界squircle主要集中增强外侧窄条和高光，完整管线有三个可见面积项未过，不作为最终选择。

新的投影为 `height/(1+.5*max(baseIOR-1.52,0))`，高IOR收窄接收距离以防折叠。生产IOR1.52、20px肩宽的静止最大R/B坐标间距约2.860px，全按压约3.396px。原色散系数没有增加。纯灰输入不产生彩虹，平坦中心不继续叠加色散。完整推导、稠密数值扫描及其限制见[曲面参考](LIQUID-GLASS-LENS-PROFILE.md)与[恒定投影审计](../artifacts/material-redesign/qa/glass-constant-projection-audit-20260922.md)。

实际GPU的[完整管线845项](../artifacts/material-redesign/qa/glass-constant-projection-integrated-20260922.json)和[隔离69项](../artifacts/material-redesign/qa/glass-constant-projection-isolated-final-20260922.json)通过。旧背景路径的负对照保留新光学模型，不能再称为冻结的旧效果；配对检查要求原始输入额外贡献至少3个百分点可见肩部面积。原绝对可见性、对比度、错误纹理、深色厚边和按压负对照仍保留。隔离色散上限改为20px肩宽的20%（4px），测得等效R/B边缘宽度2.894px；这是该夹具预算，并非任意图案和相位的全域上限。

这些结果证实了取样变化和受测性质，尚不代表与iOS的视觉或动态一致。实机录像仍需逐帧核对放大后的轮廓、色边、前景可读性和缩放过渡。

## 基础环境反射与前景约束

实际深色录像仍显示控件本体与窗口过于接近。离屏对照拒绝了“整块控件降低tint”：虽然浅色谱峰增加，灰底控件却整体暗了15/255，muted、链接和危险操作文字都低于4.5:1。详见[受控对比](../artifacts/material-redesign/qa/local-tint-experiment-20260922/evaluation.md)。

当前保留原tint，补入IOR1.52对应的法向Fresnel基础反射 `f0=((1.52-1)/(1.52+1))²≈.04258`。用 `1-(1-f0)*(1-edgeHighlight)` 合成整面弱环境反射与原方向高光，最后仅应用一次轮廓coverage。均匀灰源的深色控件整体亮约9/255，曲面位移、内部散射和外阴影保持独立。它是本项目环境光近似，不代表Apple内部实现，也不会让纯灰源凭空出现色散。

新反射使原深色危险文字不足4.5，因此调亮为 `#ffd4dc`；10px快捷粘贴提示从品牌accent改用现有可读链接色。完整管线增加muted、link/快捷键、danger三类真实前景检查，[899项全部通过](../artifacts/material-redesign/qa/glass-environment-integrated-20260922.json)，最小安全区域对比4.599:1。[隔离69项](../artifacts/material-redesign/qa/glass-environment-isolated-final-20260922.json)保留原取样、色偏、轨道和alpha门槛；平面中心的参考图只增加均匀反射，仍要求背景与轨道位置不变。

设置页正常已保存快捷键曾漏挂GlassLens，导致边框仍为闭合细线；已修复该分支，并验证原生提交、透明宿主边框及fallback恢复。相同几何的CSS备用位移图采用16项精确键缓存，52组输出字节一致，避免重复卡片逐次生成相同PNG。缓存不是新的光学公式，也不等于证明实机布局延迟已消除。

## 已撤回的更深内部散射候选

原先约8px的中心mip足迹仍保留大号背景字形。隔离比较原值、2倍和sqrt(12)倍bodySpread后，选择较强候选进入实机验收：56px字形在卡片内域的灰度标准差均值由11.82降为3.91，body=0的外肩部、控件外和均匀背景逐字节不变。它只改变背景散射，不改前景文字、tint或曲面RGB坐标。完整比较与代价见[实验报告](../artifacts/material-redesign/qa/body-scatter-experiment-20260922/evaluation.md)。新增的大字相位中，整个肩部的色散面积会下降，不能用不变的外肩部宣称所有光学性质不变。

## 当前高强度模糊的采样修正

sigma大于4时，原25个tap的间距超过1px，会让细条纹在更强模糊下重新出现。水平pass直接使用fractional mip的候选虽改善sigma16，却引入相位位移和部分周期残留回升，因此未采用。

当前实现在sigma>4时使用半径ceil(3*sigma)的离散Gaussian，把相邻两项权重合并成一次bilinear采样，仍从LOD0读取并携带预乘alpha；sigma<=4保留原路径。与独立dense参考比较，不透明图样误差最多1/255，低sigma与旧版逐字节一致。透明纹理极低coverage处归一化会放大8位误差，实际tint后最坏3/255，不能称所有像素完全等价。硬件384×384两pass的GPU timestamp中，sigma16中位约0.071ms、sigma32约0.135ms；不包括捕获、Optics、提交和显示，也不代表软件后端成本。实验保留在[paired Gaussian记录](../artifacts/material-redesign/qa/blur-paired-gaussian-experiment-20260922/evaluation.md)。该防混叠修正继续保留，不属于已撤回的分区散射策略，仍不能由离屏结果推导iOS一致。
