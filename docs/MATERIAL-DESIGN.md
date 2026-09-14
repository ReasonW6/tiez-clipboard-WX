# 材质设计与性能记录

## 参考与采用的规则

2026-09-14 查阅以下原始资料，采用其中适合剪贴板工具的层级和交互，不复制其渲染代码，也不增加运行时依赖。

| 参考 | 本项目采用的设计 |
| --- | --- |
| [Apple Materials](https://developer.apple.com/design/human-interface-guidelines/materials) | 以 regular material 的可读性为基础，玻璃主要用于导航和控件；长文本使用安静、清晰的内容底色。 |
| [Aave — Building Glass for the Web](https://aave.com/design/building-glass-for-the-web) | 采用移动选中胶囊、滑块和轻量按压响应；把装饰与前景标签分开，限制特效面积。没有移植 Aave 的 Canvas 折射引擎。 |
| [Microsoft Mica](https://learn.microsoft.com/en-us/windows/apps/design/style/mica) / [Acrylic](https://learn.microsoft.com/en-us/windows/apps/design/style/acrylic) | 保持稳定的原生材质背景，以底色控制层级，避免多层大面积模糊叠加。 |

液态玻璃采用中性冷色、圆润控件、低强度阴影和移动反馈。移除金属状的强白边、指针高光、光波、SVG 噪声/位移及正文阴影。窗口不对活文字或整页 DOM 应用光学滤镜；用户自选背景的模糊仅发生在背景图层。

## 连续范围与可读性

仍只显示一个液态玻璃透明度滑块。保存格式 `app.surface_opacity` 不变，液态玻璃显示值为 `100 - surface_opacity`；其他材质仍显示不透明度。所有值沿同一条线性曲线变化，端点不调用原生材质切换。

| 图层 | 最通透时 alpha | 最厚实时 alpha |
| --- | ---: | ---: |
| 窗口底色 | 0.22 | 0.96 |
| 内容面板 | 0.90 | 0.98 |
| 搜索与控件 | 0.76 | 0.96 |
| 顶栏 | 0.78 | 0.96 |

窗口底色每一档 alpha 增量均为 0.0074；99→100 与其他相邻档位采用相同变化量。高透明度保留文字底色，避免黑色桌面透出后浅色文字主题失去对比。自选背景图模糊为 8–24px 连续变化。Windows 原生桌面磨砂半径由系统决定；滑块表示材质通透程度，不会在端点关闭桌面背景，也不等同于 Apple 原生液态玻璃渲染器。

Windows 11 的窗口边角仍由 DWM 统一裁切。CSS 不再增加一套内缩圆角。Windows 10 保留已有实色降级；减少透明度/强制颜色使用实色，减少动态效果关闭按压缩放。

## 启动工作量与实测

前端入口只加载当前窗口；设置、标签、表情页面按需加载。默认应用发现改为打开相关设置时执行并缓存，取消启动两秒后创建隐藏预览 WebView。首次悬停仍按需创建预览窗口。

[Microsoft SHGetFileInfoW](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shgetfileinfow) 要求后台查询及 COM 初始化。图标与默认应用查询改为后台阻塞任务，保留缓存与返回格式；扫描程序也不占用异步运行时工作线程。测试确认 Shell 任务与调用线程不同，并验证系统程序图标有效且缓存一致。

同一台机器，生产前端构建，各五次新浏览器上下文，4 倍 CPU 限速；顺序测量，不与构建或浏览器回归并行。旧版为 `1760c31`，新版为本次改动。Tauri 命令全部模拟，以下数值仅代表前端测试场景。

| 指标 | 旧版 | 新版 |
| --- | ---: | ---: |
| 首次历史列表可见，中位数 | 783.9 ms | 693.8 ms |
| 最大长任务，中位数 | 162 ms | 133 ms |
| 初始加载脚本，解码字节数 | 769,217 | 572,393 |
| 启动程序扫描 | 1 次 | 0 次 |
| 启动默认应用查询 | 6 次 | 0 次 |

原始五次样本位于 `artifacts/material-rework/performance-before.json` 与 `performance-after.json`。脚本量减少约 25.6%，该场景首屏中位数减少约 11.5%。这不是原生进程冷启动速度的测量；WebView2、数据库和 DWM 的实际时间未纳入。

## 检查

- 27 项前端测试、74 项 Rust 测试、56 项浏览器回归及 TypeScript/Vite 构建通过。
- 三种材质 × 明暗模式 × 七个档位 × 黑/灰/白桌面底色，检查标题、正文、元信息、筛选标签的合成对比度至少 4.5:1。
- 同一虚构数据、相同背景比较旧版/新版 99% 与 100%；另展示云母/毛玻璃 0% 与 1% 和完整范围。
- 预览 `artifacts/material-rework/index.html` 仅证明浏览器布局与颜色，不包含真实桌面合成。原生窗口、实际粘贴和重启验证尚未执行。
