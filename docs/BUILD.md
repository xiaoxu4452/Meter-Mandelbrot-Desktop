# Build guide

[中文版](BUILD.zh.md)

## A. Linux / macOS — the command-line engine

### Requirements

| | |
| --- | --- |
| Compiler | GCC 11+ or Clang 14+ with C++20 |
| Build | GNU Make, or CMake 3.16+ |
| Optional runtime | GMP (`libgmp.so.10` / `libgmp.10.dylib`) |
| CPU | AVX2 recommended; `AVX2=0` builds a portable scalar/FP path |

```bash
# Debian / Ubuntu
sudo apt-get install -y build-essential cmake libgmp-dev
# Fedora / RHEL
sudo dnf install -y gcc-c++ make cmake gmp-devel
# macOS
brew install cmake gmp
```

### Three equivalent commands

```bash
# 1. Makefile — no CMake needed
make
./build-linux/mset info
make check                       # mset cmp 38 3000 5

# 2. Shell script — same flags, prints `mset info` at the end
./build-linux.sh

# 3. CMake
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j"$(nproc)"
ctest --test-dir build-linux --output-on-failure
```

Variants:

```bash
make AVX2=0                      # no -mavx2, works on older CPUs
make CXX=clang++                 # Clang
cmake -S . -B b -DMB_AVX2=OFF -DMB_BUILD_TESTS=ON
```

### The direct one-liner

This is exactly what `Makefile` / `build-linux.sh` invoke — useful on a machine
without make or CMake:

```bash
g++ -O3 -std=c++20 -mavx2 -mfma -Wall -Wextra -pthread -Isrc \
  src/bigfloat.cpp src/gmp_bridge.cpp src/dive.cpp src/reference.cpp \
  src/deep.cpp src/render.cpp src/palette.cpp src/image.cpp \
  src/workspace.cpp src/port/registry.cpp src/main.cpp \
  -o build-linux/mset -ldl -lpthread
```

### GMP lookup

`src/gmp_bridge.cpp` calls `dlopen` on, in order: `libgmp.so.10` next to the
executable, `libgmp.so.10` on the loader path, then common distro paths. If
nothing resolves, the engine falls back to its own base-10^9 BigFloat (slower
but exact). `MB_GMP=0` forces that path for A/B measurements. `mset info`
reports which backend is active.

### Troubleshooting

- `fatal error: immintrin.h: No such file or directory` → use GCC/Clang on
  x86-64, or build with `AVX2=0`. The AVX2 block in `deep.cpp` is guarded by
  `#if defined(__AVX2__)`, so the scalar kernel still compiles without it.
- `undefined reference to dlopen` → add `-ldl` (already in the Makefile /
  CMake `CMAKE_DL_LIBS`).
- `Illegal instruction` at startup → the CPU has no AVX2; rebuild with
  `make AVX2=0` or `-DMB_AVX2=OFF`.
- `mset info` says BigFloat backend → `libgmp.so.10` was not found; install
  the runtime package or set `LD_LIBRARY_PATH`.

## B. Windows — CLI + desktop application

### Requirements

- Visual Studio 2022 with MSVC 14.5x and the Windows 10 SDK (10.0.26100.0).
- Node.js 18+ (generates the embedded UI resources and preset data).
- WebView2 SDK, Khronos headers, lcms2 and musl libm under `desktop/third_party`.
- `runtime/libgmp-10.dll` copied next to the exe (or already on PATH).

`env-msvc.bat` sets `PATH`, `INCLUDE` and `LIB` explicitly. `vcvars64.bat`
calls `reg.exe`, which is blocked in some sandboxes and silently leaves the
environment empty; edit the three constants at the top of `env-msvc.bat`
(`VSDIR`, `MSVCVER`, `SDKVER`) if your installation differs.

```bat
:: 1. CLI engine
buildall.bat                          :: -> build\mset.exe  (MSVC /arch:AVX2 /GL)

:: 2. One verification program
b2.bat cmp 38 3000 5
b2.bat bench 38 3000 5

:: 3. Desktop application (regenerates the embedded UI first)
node desktop\tools\gen-ui-resources.mjs
buildapp.bat                          :: -> dist\Mandelbrot View.exe, mset.exe,
                                      ::    libgmp-10.dll, README.txt
```

`buildapp.bat` sets `MB_CORE` to this checkout's `src\` and points the shell at
`desktop\`, so no absolute path from the original development machine is
needed. `desktop/build.bat` compiles `host.cpp`, `mset_backend.cpp`, the D3D11
canvases, the core files and the generated `ui_resources.cpp`.

> Important: `buildall.bat` **must** keep `/arch:AVX2`. Without it the entire
> SIMD block (`#if defined(__AVX2__)`) is compiled out and the four-wide kernel
> silently disappears. `gmp_bridge.cpp` and `dive.cpp` must appear in the
> source lists of `buildall.bat`, `b2.bat` and `desktop/build.bat`.

### Why CMake is not used on Windows here

CMake's compiler detection shells out to `reg.exe` on this development machine,
which the sandbox blocks, so `buildall.bat` calls `cl` directly with the same
translation-unit list that `CMakeLists.txt` declares. Both produce identical
engines; use whichever the environment allows.

## C. Reproducing the numbers in the README

```bash
./build-linux/mset info
./build-linux/mset cmp 38 3000 5
./build-linux/mset misiurewicz --m 2 --p 2 --digits 300
./build-linux/mset nucleus --center-re -0.743643887037151 --center-im 0.13182590420533 --digits 200
```

On Windows use `b2.bat <name> <args>` to build and run the individual
verification programs from `tests/b2/`.
