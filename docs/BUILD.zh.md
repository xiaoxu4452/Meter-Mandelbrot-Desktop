# 构建指南

[English](BUILD.md)

## A. Linux / macOS —— 命令行引擎

### 环境要求

| | |
| --- | --- |
| 编译器 | GCC 11+ 或 Clang 14+，支持 C++20 |
| 构建工具 | GNU Make 或 CMake 3.16+ |
| 运行期可选 | GMP（`libgmp.so.10` / `libgmp.10.dylib`） |
| CPU | 建议 AVX2；`AVX2=0` 可构建无 AVX2 的纯标量版本 |

```bash
# Debian / Ubuntu
sudo apt-get install -y build-essential cmake libgmp-dev
# Fedora / RHEL
sudo dnf install -y gcc-c++ make cmake gmp-devel
# macOS
brew install cmake gmp
```

### 三种等价命令

```bash
# 1. Makefile —— 无需 CMake
make
./build-linux/mset info
make check                       # 执行 mset cmp 38 3000 5

# 2. 一键脚本 —— 参数相同，结束后打印 mset info
./build-linux.sh

# 3. CMake
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j"$(nproc)"
ctest --test-dir build-linux --output-on-failure
```

变体：

```bash
make AVX2=0                      # 不加 -mavx2，兼容旧 CPU
make CXX=clang++                 # 使用 Clang
cmake -S . -B b -DMB_AVX2=OFF -DMB_BUILD_TESTS=ON
```

### 直接一条命令

`Makefile` / `build-linux.sh` 内部执行的就是下面这条，机器上没有 make / CMake
时可直接使用：

```bash
g++ -O3 -std=c++20 -mavx2 -mfma -Wall -Wextra -pthread -Isrc \
  src/bigfloat.cpp src/mathx/bigmath.cpp src/mathx/expr.cpp \
  src/mathx/families.cpp \
  src/gmp_bridge.cpp src/dive.cpp src/reference.cpp \
  src/deep.cpp src/render.cpp src/palette.cpp src/image.cpp \
  src/workspace.cpp src/port/registry.cpp src/main.cpp \
  -o build-linux/mset -ldl -lpthread
```

### GMP 查找顺序

`src/gmp_bridge.cpp` 用 `dlopen` 依次尝试：exe 同目录的 `libgmp.so.10`、
动态加载器路径上的 `libgmp.so.10`、各发行版常见路径。全部失败时回退到自研
10^9 进制 BigFloat（较慢但精确）；`MB_GMP=0` 可强制该路径用于 A/B 对比。
`mset info` 会显示当前实际后端。

### 常见问题

- `fatal error: immintrin.h: No such file or directory` → 在 x86-64 上使用
  GCC/Clang，或用 `AVX2=0` 构建。`deep.cpp` 的 AVX2 段由
  `#if defined(__AVX2__)` 保护，标量内核仍可编译。
- `undefined reference to dlopen` → 加 `-ldl`（Makefile / CMake 已包含）。
- 启动时 `Illegal instruction` → CPU 无 AVX2，用 `make AVX2=0` 或
  `-DMB_AVX2=OFF` 重编。
- `mset info` 显示 BigFloat 后端 → 未找到 `libgmp.so.10`，安装运行库或设置
  `LD_LIBRARY_PATH`。

## B. Windows —— CLI 与桌面程序

### 环境要求

- Visual Studio 2022（MSVC 14.5x）+ Windows 10 SDK（10.0.26100.0）。
- Node.js 18+（生成内嵌 UI 资源与预设数据）。
- `desktop/third_party` 下的 WebView2 SDK、Khronos 头文件、lcms2、musl libm。
- 把 `runtime/libgmp-10.dll` 放到 exe 同目录，或保证它在 PATH 上。

`env-msvc.bat` 显式设置 `PATH` / `INCLUDE` / `LIB`。本机 `vcvars64.bat` 调用的
`reg.exe` 被沙箱拦截，会"成功"但留下空环境；工具链版本不同时改文件开头三个
常量（`VSDIR`、`MSVCVER`、`SDKVER`）即可。

```bat
:: 1. 命令行引擎
buildall.bat                          :: -> build\mset.exe（MSVC /arch:AVX2 /GL）

:: 2. 单个验证程序
b2.bat cmp 38 3000 5
b2.bat bench 38 3000 5

:: 3. 桌面程序（先重新生成内嵌 UI）
node desktop\tools\gen-ui-resources.mjs
buildapp.bat                          :: -> dist\Mandelbrot View.exe、mset.exe、
                                      ::    libgmp-10.dll、README.txt
```

`buildapp.bat` 把 `MB_CORE` 指向本仓库 `src\`、外壳指向 `desktop\`，不再依赖
开发机的绝对路径。`desktop/build.bat` 编译 `host.cpp`、`mset_backend.cpp`、
D3D11 画布、核心文件以及生成的 `ui_resources.cpp`。

> 关键：`buildall.bat` **必须**保留 `/arch:AVX2`。缺了它，整段 SIMD
> （`#if defined(__AVX2__)`）会被编译掉，四宽内核无声消失。`gmp_bridge.cpp`
> 与 `dive.cpp` 必须同时出现在 `buildall.bat`、`b2.bat`、
> `desktop/build.bat` 三处源列表里。

### 为什么 Windows 侧不用 CMake

本开发机上 CMake 的编译器探测会调用被拦截的 `reg.exe`，所以 `buildall.bat`
直接用 `cl`，翻译单元列表与 `CMakeLists.txt` 完全一致。两条路径产出相同
引擎，按环境选择即可。

## C. 复现 README 中的数据

```bash
./build-linux/mset info
./build-linux/mset cmp 38 3000 5
./build-linux/mset misiurewicz --m 2 --p 2 --digits 300
./build-linux/mset nucleus --center-re -0.743643887037151 --center-im 0.13182590420533 --digits 200
```

Windows 下用 `b2.bat <名称> <参数>` 编译并运行 `tests/b2/` 里的单个验证程序。
