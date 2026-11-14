# Third-party components

The renderer links against almost nothing: GMP is **loaded at runtime** and is
never a link-time dependency. The Windows shell is the only part that needs
vendored SDK headers and import libraries.

| Component | Where | What it is used for | Licence | How to obtain if missing |
| --- | --- | --- | --- | --- |
| GMP 6.3.0 | `runtime/libgmp-10.dll` (Windows binary); system `libgmp.so.10` on Linux | high-precision `mpf` reference orbit | LGPL v3+ / GPL v2+ dual | `apt install libgmp10 libgmp-dev` · `brew install gmp` · `pacman -S mingw-w64-x86_64-gmp` · https://gmplib.org |
| WebView2 SDK | `desktop/third_party/webview2/` | the desktop application's browser host | Microsoft WebView2 SDK licence | `nuget install Microsoft.Web.WebView2` or https://developer.microsoft.com/microsoft-edge/webview2 |
| Khronos OpenCL / GL headers + `OpenCL.lib` | `desktop/third_party/khronos/` | optional GPU path, delay-loaded | Khronos / MIT-style | https://github.com/KhronosGroup/OpenCL-Headers |
| musl libm subset | `desktop/third_party/musl/` | CRT-free `exp`/`pow` replacement (this MSVC's libm returns garbage for `exp2`/`pow`/`exp`) | MIT (see `COPYRIGHT`) | https://musl.libc.org |
| Little CMS 2 | `desktop/third_party/lcms2/` | colour management in the shell | MIT | https://www.littlecms.com |
| MPFR / MPIR headers | `desktop/third_party/mpfr/`, `mpir/` | legacy bridge / cross-check | LGPL / BSD-style | https://www.mpfr.org · http://mpir.org |
| Boost headers | **not vendored** (159 MB) | legacy shell utility code only | Boost Software License 1.0 | `vcpkg install boost:x64-windows` · `apt install libboost-all-dev` |

`desktop/third_party/THIRD_PARTY_VERSIONS.txt` records the exact versions used
for the shipped build.

The generated `desktop/shell/ui_resources.cpp` embeds the workbench under
`desktop/ui/` (HTML/CSS/JS, plus `js/vendor/gmp-wasm.esm.min.js` — MIT). It is
rebuilt with `node desktop/tools/gen-ui-resources.mjs` and is deliberately not
committed.

This project is an independent reconstruction of *Retinamandelbrot for Windows
Beta*. Kalles Fraktaler (`kf-2.15.5.2`) is a different program with different
formulas and is not included here.
