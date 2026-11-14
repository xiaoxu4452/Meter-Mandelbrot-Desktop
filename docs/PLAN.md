# new — 桌面版重建（核心按 out 重写）

## 为什么重建

当前 `Meter Mandelbrot` 的核心经过多轮加料（mset 第二实现、WebGPU 第三实现、
dd/direct/GPU 三条像素路径、四叉树、轨道缓存、deepPixelCap、闭式内部填充），
已经无法用"读一遍就懂"的方式推理，症状反复：

- 1e38 处参考轨道先于帧内像素逃逸（refLen 1874 < escMax 1895）→ 台阶 + 游走；
- Julia 视图出现整块深色圆盘（内点误判）；
- 同一视点连渲两次不一致（68% 像素有差，非位移）。

用户定调：**只借用桌面 UI，核心按 `C:\Users\harve\Desktop\mande\out` 重新构造，
大道至简，每个函数都要能讲清楚。**

## 基准（oracle）—— 已实测

`out/build/mset.exe` 可用，且它的两条路径在 1e38 给出**相反**的结果：

| 命令 | 结果 | 结论 |
|---|---|---|
| `render --zoom 1e38 ...`（扰动） | 整帧纯色，`glitches = 575994 / 576000` | 参考实现的扰动在 1e38 **不可信** |
| 同上 `--exact` | 300×200 / 17.7 s，**右侧丝状结构、左侧平滑** | 这才是真值 |

⇒ 新核心的验收一律以 `mset.exe render --exact` 的输出为准；
   扰动路径必须做到与之结构一致，而不是"看起来像"。

## 目标结构（单一职责，无分支）

| 层 | 文件 | 职责 | 来源 |
|---|---|---|---|
| view | `src/viewport.hpp` | 中心 BigFloat + 十进制 zoom；`point_at` / `zoom_at_pixel` / `pan_pixels` | out 原样 |
| orbit | `src/reference.*` | 参考轨道 + SA + glitch 修复 | out，需加强 |
| raster | `src/render.cpp` | 行块 + 线程，**只走扰动一条路** | out 精简 |
| shade | `src/palette.*` | 配色，颜色只依赖 smooth 迭代数 | out 原样 |
| shell | 待写 | WebView2 桥：view / render / cancel / meta 四条消息 | 从 `Meter Mandelbrot/src/shell` 精简 |

定位模型（`out/src/app/app_state.cpp:40`）就三行，原样照搬：

```cpp
auto before = view.point_at(x, y);
view.zoom_by(factor);
auto after  = view.point_at(x, y);
view.center_re += before.first - after.first;
view.center_im += before.second - after.second;
```

## 分批（每批一个可验证产物）

- **B1 视图与定位**：跑测试，`point_at → zoom_at_pixel → point_at` 在 1e10…1e100
  上锚点漂移 < 1e-6 px；`pan_pixels` 与 `point_at` 互逆。**已通过（漂移 0.000e+00）**。
- **B2 正确性**：扰动 + SA + glitch 修复，1e10/1e20/1e38 与 `--exact` 图结构一致
  （逐像素容差，先 1e10 再往下）。**已通过，见下节。**
- **B3 速度**：行块多线程，1e38 全帧 < 300 ms（out 现在 53 s / 900×640）。
- **B4 UI 桥**：复用现有前端，四条消息，滚轮一格 1.522×、手势期 0 帧。
- **B5 打包**：单 exe + 资源内嵌，回归五个视点。

## B2 结果（2026-11-14）

扰动引擎 = `src/deep.cpp`（`deep_render`），替换了 `render.cpp` 里原来的
`compute_pixel`。三个根因，每一个都用数字钉住：

1. **参考轨道先于像素死**。1e38 实测 `refLen=1874 < escMax=1895`，于是每个跑出
   轨道末端的像素都塌缩到同一个迭代数（实测全部返回 1812，而真值 1821–1892），
   画面就是台阶。修法：**把帧内寿命最长的点提升为参考**，反复做，直到没有像素
   能活过它（`deep_render` 的 reference ladder）。
2. **glitch 判据用错**。`|Z| < |d|` 在 1e38 对 99.99% 的像素成立（|d| 正常增长到
   与 |Z| 同量级），但对图像毫无意义；真正的灾难性抵消是 `|Z+d| << max(|Z|,|d|)`。
   改用「丢失 20 bit」判据（`g_cancel_bits`）。
3. **F2 归一化漏洞**。`f2_add` 在近抵消时只做一次 ×2，结果落在 [0.5,1) 之外，
   破坏后续所有量级比较 —— 这一条让 1e6 也只有 41% 匹配。改成按位重写指数域
   （`f2_norm`）后，1e6 → 98.8%。

级数近似另有三处判据错误（`reference.cpp`）：

- 原来要求**每一项**的绝对值 < 1e-5，导致 `skip` 与阶数无关、恒为 595；
  正确判据是**被丢弃的那一项相对首项**的大小；
- 只应看最高阶系数（截断项），不是 k=2；
- 结果：1e38 跳过率 595/1801 → **1621/1901（85%）**，order 16 时 1689（89%），
  而逐像素精度一个都没变。

验收（`b2 cmp <exp10> <maxiter> <step>`，oracle = `pixel_exact`，并已验证
oracle 自身在 88 位 vs 268 位十进制下逐像素一致）：

| 深度 | 匹配 | 残差 |
|---|---|---|
| 1e6 | 98.76% | ±1 为主，1 个 −22 |
| 1e9 | 99.31% | ±1 为主 |
| 1e20 | 98.76% | ±1 为主，1 个 −44 |
| 1e38 | 98.21% | ±1 为主（6/13），1 个 −57 |

图像级（1e38，300×200，扰动 vs `--exact`）：60000 像素中 1228 个不同，
**平均差 0.11/255**，最大 83。两张图肉眼不可分。

速度（1700×959，16 线程，LTCG）：1e10 3.3 s、1e20 1.4 s、1e38 3.8 s。
单线程 ≈ 66 ns/扰动步，16 线程 ≈ 13 ns/步。**B3 的入口就在这里**：内核目前
全部走 F2，改成 `complex<double>` 快路径可再拿 4–5×。

## 验收数字（固定下来，不再变）

- 滚轮一格 = `exp(±0.42)` = 1.522×，锚点漂移 < 0.01 px（页面侧 capture 坐标口径）。
- 1e38：`refLen > escMax` 必须成立；画面与 `--exact` 同构。
- 同一视点连渲两次：逐像素差 = 0（确定性）。
- 每个深度档的耗时写进 `evidence/perf.csv`。
