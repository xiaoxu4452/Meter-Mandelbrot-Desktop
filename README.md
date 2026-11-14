# Meter Mandelbrot — Desktop Edition

[中文说明 / Chinese README](README.zh.md)

A scientific deep-zoom Mandelbrot / Julia engine with a Win32 + WebView2
desktop application and a portable C++20 command-line renderer. Version 0.9
(2026-11-14).

The engine reaches **10^60000** magnification in well under a second on an
analytically periodic centre, and its perturbation output is **byte-identical
to an exact BigFloat oracle** at 10^20 and 10^38. Read
[docs/ALGORITHM.md](docs/ALGORITHM.md) for the mathematics and
[docs/BUILD.md](docs/BUILD.md) for every build path.

---

## 1. Build on Linux (the short version)

```bash
# Debian / Ubuntu
sudo apt-get install -y build-essential libgmp-dev   # runtime GMP, optional
git clone https://github.com/xiaoxu4452/Meter-Mandelbrot-Desktop.git
cd Meter-Mandelbrot-Desktop

make                 # -> build-linux/mset
./build-linux/mset info
make check           # 1e38 pixel-for-pixel oracle comparison
```

Other equivalent entry points:

```bash
./build-linux.sh                         # no make required, same flags
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release && cmake --build build-linux -j"$(nproc)"
make AVX2=0                              # CPUs without AVX2
make CXX=clang++                         # Clang
```

`gmp_bridge.cpp` loads `libgmp.so.10` with `dlopen`; the binary still builds
and runs when GMP is absent, falling back to the built-in base-10^9 BigFloat
(`MB_GMP=0` forces that path). The GUI is Windows-only; Linux builds the CLI,
which is the scriptable, verifiable, batch-capable renderer.

## 2. What is in this repository

| Path | Contents |
| --- | --- |
| `src/` | The renderer: `deep.cpp` (perturbation + AVX2 quad kernel), `reference.cpp` (orbit + series approximation), `bigfloat.cpp`, `floatexp.hpp`, `gmp_bridge.cpp`, `dive.cpp` (Newton nucleus / Misiurewicz), `render.cpp`, `main.cpp`, headers. `src/recovered*` are historical disassembly reconstructions, **not compiled**. |
| `tests/` | `b2/` verification programs: `cmp`, `bench`, `shot`, `trace`, `sa`, `sareplay`, `gmporbit`, `gmpbench`, `parsewidth`, … |
| `desktop/` | Windows application: `shell/` (Win32 + WebView2 host, 4-channel field bridge, D3D11 canvas), `ui/` (embedded HTML/CSS/JS workbench), `core/`, `gpu/`, `tools/` (UI resource generator), `third_party/` (vendored SDK headers and import libraries), `build.bat`. |
| `runtime/libgmp-10.dll` | The Windows GMP runtime shipped next to the exe (GMP 6.3.0). |
| `docs/` | Build guide, algorithm notes (EN + ZH), original development plan, shipped product README. |
| `buildall.bat`, `b2.bat`, `buildapp.bat`, `env-msvc.bat` | Windows build scripts for the CLI and the full application. |
| `CMakeLists.txt`, `Makefile`, `build-linux.sh` | Linux / macOS build. |

`desktop/shell/ui_resources.cpp` is a **generated** byte table (~19 MB) and is
not committed. Regenerate it before the Windows build:

```bat
node desktop\tools\gen-ui-resources.mjs
```

Likewise `desktop/third_party/boost` (159 MB of headers) is not vendored; see
[THIRD_PARTY.md](THIRD_PARTY.md) and `docs/BUILD.md` for how to provide it.

## 3. Build on Windows

Requires Visual Studio 2022 (MSVC 14.5x), Windows SDK 10.0.26100.0, Node.js
and the vendored WebView2 / Khronos / lcms2 / musl pieces under
`desktop/third_party`. `env-msvc.bat` sets `INCLUDE`/`LIB` explicitly because
`vcvars64.bat` shells out to `reg.exe`, which is blocked in some sandboxes;
edit its three path constants if your toolchain differs.

```bat
buildall.bat                    :: build\mset.exe      (CLI engine)
b2.bat cmp 38 3000 5           :: build + run one verification program
node desktop\tools\gen-ui-resources.mjs
buildapp.bat                    :: dist\Mandelbrot View.exe + mset.exe + libgmp-10.dll + README.txt
```

`buildapp.bat` points `MB_CORE` at this tree's `src\` and the shell at
`desktop\`, so the whole product is reproducible from a single checkout.

## 4. Using the command line

```bat
mset info
mset render --center-re -0.743643887037151 --center-im 0.13182590420533 ^
            --zoom 1e12 --width 1200 --height 800 --maxiter 4000 --out deep.png
mset nucleus      --center-re X --center-im Y --digits 300
mset misiurewicz  --m 2 --p 2 --digits 300
mset cmp 38 3000 5
mset --help
```

Long coordinates (> ~4000 digits) exceed the Windows command-line limit; pass
`--config view.json` instead. `--zoom` is `3/(m·2^e)` where `e` is a **binary**
exponent.

A/B switches (performance only, never change the picture):

| Variable | Effect |
| --- | --- |
| `MB_GMP=0` | force the built-in BigFloat reference orbit |
| `MB_PERIOD=0` | disable exact-period orbit reuse |
| `MB_QUASI=1` | allow near-period detection (off by default, see limits) |
| `MB_CENTER_REF=0` | anchor on the centre pixel instead of the exact centre |
| `B2_SIMD=0` | force the scalar pixel kernel |
| `B2_SA_REPLAY=0` | rebuild an orbit that carries series coefficients |
| `B2_REFCAP=N` | fix the reference-orbit step ceiling |
| `B2_TIME=1` | print per-pass / per-orbit timings and glitch distribution |

## 5. How the renderer works

Reference orbit + perturbation + series approximation. The reference is built
in GMP `mpf` at `depth digits + 50 guard bits`; per-pixel arithmetic uses a
float-exp pair (double mantissa + integer exponent) and switches to plain
double above 1e-250. Nine rules were established by measurement and are
enforced throughout `src/deep.cpp` and `src/reference.cpp`:

1. The reference must outlive every pixel; the frame keeps promoting candidates.
2. `ref_cap = maxiter` in one shot — a 4096/8192 staircase turns whole frames
   into "interior".
3. Eight parallel candidate orbits, keep the longest-lived.
4. Glitch = `|Z+d| << max(|Z|,|d|)` (20 lost bits).
5. Series approximation is accepted only when the truncation term is < 1e-12
   of the *first* term: `log10|A_N| - log10|A_1| + (N-1)·log10(dmax) < -12`,
   and integration closes at `|Z| > 2`. Coefficients are replayed over the
   stored `z_n` afterwards, so one frame needs one high-precision orbit.
6. Exact eventual-period detection in the first 64 GMP steps tiles the orbit
   analytically to `maxiter` with zero further full-precision squarings.
7. Julia perturbation has no constant term: `d_{n+1} = 2 Z_n d_n + d_n²`;
   only Mandelbrot carries `+d0`.
8. Every high-precision coordinate is parsed at the *current frame's* width.
9. The iteration budget is observational: a starved frame (zero escapes with
   a reference that reached the cap) raises the ceiling and repaints once.

See [docs/ALGORITHM.md](docs/ALGORITHM.md) for the full derivation, the SA
criterion, periodic centres and the Newton solvers.

## 6. Verified results

| Check | Result |
| --- | --- |
| vs exact BigFloat oracle (`cmp 38 3000 5`) | 100% identical, 0 unresolved |
| 1e20 / 1e6 | 100% / 98.71% (chaotic boundary ±1 iteration) |
| Julia 1e6 / 1e12 / 1e20 / 1e38 vs `--exact` | 0 differing pixels |
| 1e38, 426×240 | 44 ms |
| 1e73 (Example 14), 640×400 | 475 ms |
| 1e6000 at `c=i`, 320×200 | 84 ms |
| 1e60000 at `c=i`, 160×100, maxiter 200000 | 0.65 s (was > 20 min, no frame) |
| AVX2 four-wide `iterate_quad_d` | 33 → 8 ms main loop, byte-identical to scalar |
| GMP step cost (6000…120000 bits) | 22 / 43 / 106 / 463 / 980 µs, no FFT threshold |

`mset nucleus` solves Example 14 to period 11196 (200 digits 0.2 s, 6100
digits 21 s); `mset misiurewicz --m 2 --p 2` returns `0+1i`, `(3,1)` returns
`-2`.

## 7. Known limits

- Exact period reuse is safe only for analytically periodic centres (`c=i`,
  `c=-2`). `MB_QUASI=1` relaxes the test but is off by default: a merely
  near-periodic centre has a divergent true orbit.
- Zooming into a nucleus is entirely interior — mathematically correct.
- Recursive Newton diving from 1e73 to 1e6000 is not implemented.
- Near the shallow chaotic boundary, double precision can differ from the
  oracle by ±1 iteration.
- The desktop GUI requires Windows; the WebView2 runtime must be installed.

## 8. Licence

Project code: MIT (see [LICENSE](LICENSE)). Vendored third-party components
keep their own licences — see [THIRD_PARTY.md](THIRD_PARTY.md). This is an
independent reconstruction of *Retinamandelbrot for Windows Beta*; it is not
Kalles Fraktaler (`kf-2.15.5.2`).
