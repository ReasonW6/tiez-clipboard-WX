# 玻璃肩部曲面与投影距离

2026-09-22。只读源码与已有 CPU 数值研究，未运行外部实现、GPU 候选或 UI。结论是优先小范围比较半像素有界 squircle/local 与 cubic/constant；不能把更大位移、负 Jacobian 或更亮边缘单独作为视觉成败判据。

## 基线和一手来源

本地依据为 [glass_optics.hlsli](../src-tauri/native/glass_optics.hlsli) 与 [72 组曲面对比及 6 组高 IOR 检查](../artifacts/material-redesign/qa/glass-squircle-profile-analysis.json)。读取时两者记录的 HLSLI SHA-256 均为 `2acf1e525ab7cfbbd691dadbf60d051f47ec0a51f10f2c0ded333bbe64ccb345`。以下数值是抗锯齿、mip、着色和反射之前的双精度模型结果。

[Kube 原文](https://kube.io/blog/liquid-glass-css-svg/) 给出归一化高度 `g(t)=(1-(1-t)^4)^(1/4)`，通过高度导数求法线，并将折射与方向性边缘高光组合；文章明确是近似。正文没有给出足以核对其线上实现投影距离的完整代码，不能据此断言 Kube 也采用下述恒定投影。其 Apple 曲面与高光描述是作者观察，不是 Apple 实现证据。

[anuero 的 Metal 源码](https://raw.githubusercontent.com/anuero/LiquidGlass/main/Sources/LiquidGlass/Shaders.metal) 可直接核对：`h=max(1-(1-t)^4,1e-4)^(1/4)`，导数上限为 40；法线斜率为 `dhdx*thickness/bevel`。折射方向投影时乘整个 `thickness`，没有乘局部 `h`。RGB 则缩放同一位移，红色乘 `1+chroma*(1-t)`，蓝色乘 `1-chroma*(1-t)`；这与 TieZ 的逐波长 Snell 不同。[仓库 Sources](https://github.com/anuero/LiquidGlass#sources) 明确引用 Kube，因此两者不构成两份独立的物理或 Apple 一致性验证。

该 Metal 实现的光照窗口为 `exp(-((t-.16)/.19)^2)`，主镜面项取半角向量余弦的 `shininess` 次幂，反侧副瓣强度为一半；另加 `(1-Nz)^6` Fresnel 项，整体随背景亮度减弱，背光侧乘内阴影。[同一 shader](https://raw.githubusercontent.com/anuero/LiquidGlass/main/Sources/LiquidGlass/Shaders.metal) 的默认系数来自 [Configuration](https://raw.githubusercontent.com/anuero/LiquidGlass/main/Sources/LiquidGlass/LiquidGlassConfiguration.swift)：镜面 `.6`、指数 `90`、Fresnel `.22`、内阴影 `.22`；默认 bevel/thickness 为 `14/11` 点。这些是该作者参数，不是 Apple 参数。

## 高度相同，投影仍可不同

令肩宽为 `w`、最大厚度为 `H`、`a=H/w`、`t=depth/w`。表面法线为 `normalize(a*h'(t)*outward, 1)`。折射射线的横向斜率绝对值记为 `K`，则 local 位移量为 `H*h(t)*K`，constant 为 `H*K`。后者的 `H` 是艺术化的固定投影距离，不能同时解释成每一点到同一平底的真实局部高度。

| 组合 | 高度/边界性质 | 位移差别 |
| --- | --- | --- |
| 现有 cubic/local | `h=1-(1-t)^3`；外沿斜率有限，平台处斜率归零 | 外沿局部厚度为零，位移也归零，峰值位于肩部内部。 |
| 原始 squircle/local | `h=g(t)`；外沿导数发散，平台连接平缓 | 位移仍连续归零，但在极窄边带迅速升高，采样相位敏感。 |
| 半像素有界 squircle/local | `e=.5/w`，`h=(g(e+(1-e)*t)-g(e))/(1-g(e))` | 保留 `h(0)=0,h(1)=1`，外沿导数有限，增强量分布到可采样的肩部。 |
| anuero 式 squircle/constant | squircle 斜率封顶 40，固定投影 `H` | 外沿有很大非零位移；遮罩外仍透明，不能与 local 的零位移外沿等同。 |

半像素对应源纹理像素，不是固定 CSS 像素。当前 JSON 中 `w=radius`；肩宽小于半径的其它构型没有逐项计算。生产候选应使用肩宽确定正则尺度，并验证最小尺寸，不能直接把这张表外推到任意尺寸。

## 已有数值说明什么

以下统一为 `radius=w=20 px, a=.75, n=1.52`，均直接摘取现有 JSON。

| 模型 | 最大位移 px | 距外沿深度 px | 最小 Jn | 最小 Jt | 最小 det J |
| --- | ---: | ---: | ---: | ---: | ---: |
| cubic/local | 3.3225 | 5.9033 | .6260 | .7456 | .4824 |
| squircle/local | 6.2266 | .3488 | −.4367 | .6828 | −.3110 |
| 有界 squircle/local | 4.4882 | 2.1573 | .4204 | .7453 | .3270 |
| squircle/constant 参考模型 | 16.0738 | 0 | −20.5718 | .1948 | −4.0083 |

最后一行隔离了恒定投影及导数封顶，仍使用 TieZ 波长 IOR、现有圆角法线；不是 anuero 完整渲染的数值复现，也不是下文 cubic/constant 候选。各列极小值通常不在同一点，不能用两列极小值相乘重建 `min det J`。

生产 [调用点](../src-tauri/native/glass_renderer.hlsl) 的基础 IOR 为 `1.52`，现有色散得 R/G/B 有效 IOR `1.3744/1.52/1.6656`。在 `radius=11/16/20`、厚宽比 `.75/.84`、这三个有效 IOR 的已算范围内，有界 squircle 最差 `Jn=.205672, det J=.142275`，出现在 `r20,a.84,n1.6656`；cubic 对应最差为 `.462133,.310099`，都没有检测到折叠。原始 squircle 最差 `Jn=−.736614, det J=−.453045`，已有折叠。恒定投影参考模型的全表最差 `Jn=−25.002967`；最差 determinant 为 `−5.640068`，发生在另一组参数。

这不覆盖 helper 的全部合法参数。基础 IOR 上限 `2.5` 加蓝色偏移 `.16` 得有效 `2.66`；有界 squircle 在这 6 组高端检查中都有负 `Jn`。`r11,a.75` 仅为 `−.004776`，最坏 `r20,a.84` 为 `Jn=−.267015,det J=−.137945`。不能把当前生产 `1.52` 的正值结果说成全范围无折叠，也不能用高端 fold 直接否决当前生产候选。

## Fold 与角落星形不是同一个判据

此处是逆向采样映射。若位移量为 `w*F(t)`，当前圆弧模型有 `Jn=1+F'(t)`、`Jt=1-F(t)/(1-t)`；直边的切向因子为 1。`Jn` 穿过零表示邻近位置的源坐标顺序反转，负 determinant 证实局部定向反转。它可以产生重复细线或镜像边带，但 shader 仍能为每个输出像素采样；数学 fold 本身不是未定义行为，更不自动证明角落星形。

应优先排查会制造固定尖瓣的方向或支撑不连续：中轴法线跳变处仍有非零位移/高光、直边与圆角使用不同公式且接缝不匹配、各 RGB 的几何/裁切不一致、或分段项有硬切。现有 `shoulder<=radius` 且平台斜率/位移归零，让中轴法线分支失去影响；有界 squircle 也应保留这一点。径向对称的连续曲面不会仅因 `Jn<0` 自发获得固定角向尖瓣。不过，平滑映射对结构化背景也可能形成尖锐像，装饰反馈也能形成双弧，所以可见星形本身不能反证数学不连续，须检查无装饰位移场与灰底光照的分离对照。

## 最小 GPU 试验

只与现有基线比较两个候选，不先叠加更多色散、厚度或灯光参数。

1. **半像素有界 squircle/local。** 保留 SDF、肩宽、厚宽比、逐波长 Snell 和取样层级，只替换配套的高度与导数。核对 `h(0)=0`、平台零斜率、有限法线以及半像素 AA 区。
2. **cubic/constant。** 保留 cubic 高度与法线，仅把折射投影距离由 `H*h(t)` 换为 `H`。散射继续读原局部厚度，避免一次改动混入多个变量。当前 JSON 没有计算此候选，须独立测位移与 Jacobian，不可套用 squircle/constant 行。

先用圆角矩形与胶囊、`r11/r20`、未按压/完全按压、生产 IOR 做基线加两候选；选出有价值者后补 `.25/.5/.75 px` 相位及高 IOR 边界。使用无装饰细网格/细字检查折射、RGB 分离和重复边；用均匀灰底及高光开/关检查体积，完整管线再看阴影承托、暗亮背景和最终边缘。保持同一源纹理、tint、LOD 与外阴影。

均匀源满足 `C(p+offset)=C`，单纯加大位移不会改变纯灰像素。灰底体积只能由光照、遮蔽/阴影等合成体现。尤其沿光方向、外沿 `t=0,a=.75,n=1.52`，把现有 Fresnel 高光公式代入 JSON 法线后，cubic 未预乘高光为 `.339893`；有界 squircle 的 `r11/16/20` 都饱和到 `1`。因此即使不改灯光系数，也必须单独看高光是否过曝、是否重新形成白色塑料边；更亮不等于体积更可信。
