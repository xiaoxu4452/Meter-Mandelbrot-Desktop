# Meter Mandelbrot — 桌面版

[English README (default)](README.md)

深视点 Mandelbrot / Julia 科学渲染引擎，包含 Win32 + WebView2 桌面程序与可移植的
C++20 命令行渲染器。版本 0.9（2026-11-14）。

在解析精确周期中心上可放大到 **10^60000** 且单帧不足一秒；扰动层输出在 10^20
与 10^38 处与精确 BigFloat oracle **逐像素一致**。数学原理见
[docs/ALGORITHM.zh.md](docs/ALGORITHM.zh.md)，完整构建方式见
[docs/BUILD.zh.md](docs/BUILD.zh.md)。

---

## 1. Linux 构建（最简）

```bash
# Debian / Ubuntu
sudo apt-get install -y build-essential libgmp-dev   # 运行期 GMP，可选
git clone https://github.com/xiaoxu4452/Meter-Mandelbrot-Desktop.git
cd Meter-Mandelbrot-Desktop

make                 # -> build-linux/mset
./build-linux/mset info
make check           # 1e38 逐像素 oracle 对拍
```

等价入口：

```bash
./build-linux.sh                         # 不依赖 make，参数相同
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release && cmake --build build-linux -j"$(nproc)"
make AVX2=0                              # CPU 不支持 AVX2
make CXX=clang++                         # 使用 Clang
```

`gmp_bridge.cpp` 用 `dlopen` 加载 `libgmp.so.10`；未安装 GMP 也能编译运行，自动
回退到自研 10^9 进制 BigFloat（`MB_GMP=0` 可强制）。图形界面仅 Windows；Linux
构建的是可脚本化、可验证、可批处理的命令行引擎。

## 2. 仓库内容

| 路径 | 内容 |
| --- | --- |
| `src/` | 渲染核心：`deep.cpp`（扰动 + AVX2 四宽核）、`reference.cpp`（轨道 + 级数近似）、`bigfloat.cpp`、`floatexp.hpp`、`gmp_bridge.cpp`、`dive.cpp`（Newton 核 / Misiurewicz 求解）、`render.cpp`、`main.cpp` 及全部头文件。仅上述列出的翻译单元参与构建。 |
| `tests/` | `b2/` 验证程序：`cmp`、`bench`、`shot`、`trace`、`sa`、`sareplay`、`gmporbit`、`gmpbench`、`parsewidth` 等。 |
| `desktop/` | Windows 程序：`shell/`（Win32 + WebView2 宿主、4 通道场桥、D3D11 画布）、`ui/`（内嵌 HTML/CSS/JS 工作台）、`core/`、`gpu/`、`tools/`（UI 资源生成器）、`third_party/`（随仓库附带的 SDK 头文件与导入库）、`build.bat`。 |
| `runtime/libgmp-10.dll` | 与 exe 同目录发布的 Windows GMP 运行库（GMP 6.3.0）。 |
| `docs/` | 构建指南、算法说明（中英）、发布版产品说明。 |
| `buildall.bat`、`b2.bat`、`buildapp.bat`、`env-msvc.bat` | Windows 下 CLI 与完整程序的构建脚本。 |
| `CMakeLists.txt`、`Makefile`、`build-linux.sh` | Linux / macOS 构建。 |

`desktop/shell/ui_resources.cpp` 是**生成物**（约 19 MB 字节表），未提交。Windows
构建前先执行：

```bat
node desktop\tools\gen-ui-resources.mjs
```

同理 `desktop/third_party/boost`（159 MB 头文件）未随仓库附带，获取方式见
[THIRD_PARTY.md](THIRD_PARTY.md) 与 [docs/BUILD.zh.md](docs/BUILD.zh.md)。

## 3. Windows 构建

需要 Visual Studio 2022（MSVC 14.5x）、Windows SDK 10.0.26100.0、Node.js，
以及 `desktop/third_party` 下的 WebView2 / Khronos / lcms2 / musl 依赖。
`env-msvc.bat` 直接写死 `INCLUDE`/`LIB`（因为部分沙箱里 `vcvars64.bat` 调用的
`reg.exe` 被拦截）；工具链版本不同时改其中三个路径常量即可。

```bat
buildall.bat                    :: build\mset.exe      （命令行引擎）
b2.bat cmp 38 3000 5           :: 编译并运行单个验证程序
node desktop\tools\gen-ui-resources.mjs
buildapp.bat                    :: dist\Mandelbrot View.exe + mset.exe + libgmp-10.dll + README.txt
```

`buildapp.bat` 把 `MB_CORE` 指向本仓库 `src\`、外壳指向 `desktop\`，因此一次
检出即可复现整个产品。

## 4. 命令行用法

```bat
mset info
mset render --center-re -0.743643887037151 --center-im 0.13182590420533 ^
            --zoom 1e12 --width 1200 --height 800 --maxiter 4000 --out deep.png
mset nucleus      --center-re X --center-im Y --digits 300
mset misiurewicz  --m 2 --p 2 --digits 300
mset cmp 38 3000 5
mset --help
```

超长坐标（> 约 4000 位）会超过 Windows 命令行长度上限，改用 `--config view.json`。
`--zoom` 取 `3/(m·2^e)`，其中 `e` 是**二进制**指数。

A/B 开关（只影响性能，不改画面）：

| 变量 | 作用 |
| --- | --- |
| `MB_GMP=0` | 强制使用自研 BigFloat 参考轨道 |
| `MB_PERIOD=0` | 关闭精确周期轨道复用 |
| `MB_QUASI=1` | 允许准周期检测（默认关，见限制） |
| `MB_CENTER_REF=0` | 锚点改用中心像素而非精确中心 |
| `B2_SIMD=0` | 强制标量像素核 |
| `B2_SA_REPLAY=0` | 恢复"重建带级数系数的轨道" |
| `B2_REFCAP=N` | 固定参考轨道步数上限 |
| `B2_TIME=1` | 打印每趟 / 每条轨道耗时与 glitch 分布 |

## 5. 应用数学库（`src/mathx`）

引擎不只会画 `z^2 + c`。`src/mathx` 是一套自带的应用数学库：

* `bigmath.cpp` — 在深度渲染同一套 base-10^9 BigFloat 上实现任意精度除法、
  开方、`exp`、`log`、`sin`/`cos`、`atan`、`pow` 与常数 pi / ln10。所有函数都是
  全定义的（除以 0 得 0，`log(0)` 得 0），单个发散像素不会中断整帧。
* `expr.cpp` — 词法分析 + Pratt 语法分析 + 后缀虚拟机：变量 `z, c, n, z1`，
  常数 `i, pi, e`，单字母参数，隐式乘法（`2z`、`3(z+1)`），函数 `sin cos tan
  asin acos atan sinh cosh tanh exp log ln log10 sqrt abs abs2 arg conj re im
  pow min max atan2 floor ceil round sign if` 以及比较运算；语法错误带字节位置。
* `complex.hpp` — BigFloat 上的复数层。
* `families.cpp` — 折叠族与自定义公式的深层渲染。

折叠族（三叉戟、燃烧船、凯尔特、垂直、凤凰）使用**精确**的 δ 递推：折叠符号
取自参考轨道，因此没有 Taylor 截断，10^120 与 10^2 一样精确。用户公式使用二阶
Taylor 扰动，系数由任意精度参考轨道做中心差分得到。

```bat
mset family --kind tricorn --center-re <220 位> --center-im 0 ^
            --zoom 1e120 --width 800 --height 500 --out tricorn.png
mset family --kind formula --expr "sin(z)^2 + c" --zoom 1e30 --out fx.png
mset family-point --kind tricorn --preperiod 3 --period 1 --digits 220 ^
                  --guess-re -1.5437
```

`family-point` 用 2×2 Newton（中心差分雅可比）解 `z_{m+p}(c) = z_m(c)`：
`--preperiod 0` 求原子域中心，`--preperiod > 0` 求 Misiurewicz 点。220 位的边界
坐标就是这样来的——只有边界在 10^120 处仍有结构。

本机实测：1e120 三叉戟 360×220、2500 次迭代、4 条参考轨道——81 ms，参考轨道
2501 点，逃逸 534..583。桌面应用对族视图和 1e13 以深的公式使用同一引擎；浅层
公式仍走公式面板自带的 JS 求值器（它支持求和、连乘、分段等更丰富的语法）。

单元检查：`tests\bigmath.bat`（初等函数对照已知小数展开）与
`tests\family.bat`（表达式解析、五个族的 δ 递推对 `F(Z+d)-F(Z)` 全精度比对、
1e60/1e120 帧逐像素对照 BigFloat 直接迭代）。

## 6. 渲染原理

参考轨道 + 扰动 + 级数近似。参考轨道用 GMP `mpf` 按「深度位数 + 50 位保护位」
构建；逐像素用 float-exp（double 尾数 + 整数指数），1e-250 以上自动切纯 double
快路径。实测确立并贯穿 `src/deep.cpp` / `src/reference.cpp` 的九条规则：

1. 参考必须比所有像素活得久，帧内持续提升候选。
2. `ref_cap = maxiter` 一步给满；4096/8192 阶梯会让整帧误判 interior。
3. 并行 8 条候选轨道，留最长寿者。
4. glitch 判据 `|Z+d| << max(|Z|,|d|)`（丢 20 bit）。
5. 级数近似只有当截断项小于**首项**的 1e-12 才成立：
   `log10|A_N| - log10|A_1| + (N-1)·log10(dmax) < -12`，且 `|Z| > 2` 封口。
   系数在已存 `z_n` 上事后重放，一帧只需一条高精度轨道。
6. 头 64 步精确周期检测，命中后按周期平铺到 `maxiter`，零次额外全精度平方。
7. Julia 扰动无常数项 `d_{n+1}=2Z_n d_n+d_n²`；只有 Mandelbrot 才有 `+d0`。
8. 高精度坐标一律按**本帧**位宽解析。
9. 迭代预算来自观测：饿死帧（零逃逸且参考跑到上限）提预算并自动重渲一趟。

完整推导见 [docs/ALGORITHM.zh.md](docs/ALGORITHM.zh.md)。

## 7. 实测数据

| 项目 | 结果 |
| --- | --- |
| 精确 BigFloat oracle（`cmp 38 3000 5`） | 100% 一致，0 unresolved |
| 1e20 / 1e6 | 100% / 98.71%（混沌边界 ±1 迭代） |
| Julia 1e6 / 1e12 / 1e20 / 1e38 对 `--exact` | 0 像素差 |
| 1e38，426×240 | 44 ms |
| 1e73（Example 14），640×400 | 475 ms |
| 1e6000 `c=i`，320×200 | 84 ms |
| 1e60000 `c=i`，160×100 / maxiter 200000 | 0.65 s（此前 > 20 分钟不出帧） |
| AVX2 四宽 `iterate_quad_d` | 主趟 33 → 8 ms，与标量逐字节相同 |
| GMP 每步（6000…120000 位） | 22 / 43 / 106 / 463 / 980 µs，无 FFT 阈值跳变 |

`mset nucleus` 对 Example 14 解出周期 11196（200 位 0.2 s，6100 位 21 s）；
`mset misiurewicz --m 2 --p 2` 得 `0+1i`，`(3,1)` 得 `-2`。

## 8. 已知限制

- 精确周期复用只对解析精确的周期中心（`c=i`、`c=-2`）安全。`MB_QUASI=1`
  放宽判据但默认关闭：只是"看似周期"的中心真实轨道会发散。
- 以核为中心放大必然全 interior，数学正确。
- 未实现从 1e73 递归 Newton dive 到 1e6000。
- 浅视点混沌边界处 double 与 oracle 可能差 ±1 迭代。
- 桌面 GUI 仅 Windows，需安装 WebView2 Runtime。

## 9. 许可

本项目代码 MIT（见 [LICENSE](LICENSE)）；随仓库附带的第三方组件保留各自许可，
见 [THIRD_PARTY.md](THIRD_PARTY.md)。渲染核心为参考公开算法文献独立实现的
C++20 代码。
