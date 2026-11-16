// ---------------------------------------------------------------------------
// mset_backend.h - bridge from the shell's tile jobs to the reconstructed core.
//
// The shell keeps its framework: WebView2 owns the panels, the page decides
// which tiles to render, the D3D11 child window presents them.  Only the
// pixel producer changes: this module renders with the reconstructed
// Retinamandelbrot core (native/mset) instead of the old stitched engine.
//
// It exposes plain types on purpose.  The core has its own Image / Palette /
// RGB and a global precision setting; none of that leaks into host.cpp.
//
// Colour sets and rotation are NOT reasons to fall back any more:
//   * the page's gradient (`stops`) is applied here, from a colour-coordinate
//     field the core hands over.  The core never needs to know the gradient,
//     and re-shading later costs no iterations.
//   * view rotation is a rotation of the core's pixel->plane map, so the
//     picture is rotated in the renderer rather than by the presenter.
// Only the folded families (which the core does not transcribe yet) and
// orbit traps still fall back.
// ---------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <cstdint>

namespace msetb {

struct Result {
    bool ok = false;
    const char* error = "";
    double ms = 0.0;
    int refLen = 0;       // reference orbit length in steps
    int digits = 0;       // MPFR/BigFloat precision actually used
    int glitches = 0;
    int maxEscape = 0;    // largest escape count among escaped pixels
    int minEscape = 0;
    int bounded = 0;      // interior pixel count
    /* Iterations per palette cycle the core fitted to this frame.  A shader
     * that shades the field itself has to use the same number, or a deep frame
     * (whose whole escape range is a hundred iterations) paints one flat band. */
    double iterPerCycle = 0.0;
};

/* A gradient, exactly as the page sends it: [pos, r, g, b] per stop, with pos
 * in 0..1 ascending and channels in 0..1. */
struct Gradient {
    const float* stops = nullptr;
    int count = 0;        // number of stops (not floats)
    float pos[32] = {};
    float rgb[32][3] = {};
};

// Render the rectangle (x,y,w,h) of a viewW x viewH frame whose centre is
// `centerRe`/`centerIm` and whose full width is spanMant * 2^spanExp.
// `mode` is 0 = Mandelbrot, 1 = Julia; `kind` selects the family (0 = z^2+c,
// 1 tricorn, 2 burning ship, 3 celtic, 4 perpendicular, 5 phoenix,
// 6 a user expression in `expr`).  Kind 6 expressions are parsed by
// src/mathx/expr.cpp; `params` carries "a=0.5;b=1" for their sliders.
// `rotation` is the view angle in radians (0 = upright).  `grad` may be null,
// in which case the core's built-in sine palette is used.
// Pixels are written as BGRA into `rgba`, which must point at the tile's own
// first pixel, with `outPitch` pixels per row (so a tile of a bigger pass can
// be written straight into that pass surface: rgba + pitch * row).
bool RenderTile(const char* centerRe, const char* centerIm,
                double spanMant, int spanExp,
                int viewW, int viewH, int x, int y, int w, int h,
                int maxIter, int mode, int kind, int power,
                const char* juliaRe, const char* juliaIm,
                int paletteIndex, int colorMode, bool invert,
                double rotation, const Gradient* grad,
                uint8_t* rgba, int outPitch,
                const std::atomic<bool>* cancel, Result* out,
                const char* expr = nullptr, const char* params = nullptr,
                const char* phoenixP = nullptr);

// True when the bridge can describe this request at all (used to decide
// fallback before allocating work).  Every family is supported at every
// depth: the folded ones go through src/mathx/families.cpp.
bool Supports(int mode, int kind, int power);

// Render the colour field for a whole frame, in the shell's native layout:
// `channels` floats per texel, [0] smooth iteration (negative inside the set),
// [1] distance in pixels, [2] integer iteration, [3] linear distance.
// Channels 4 and 5 (z at the escape point) are zeroed when asked for: the
// rebuilt core does not transcribe the 20-step continuation yet.
// The caller owns `field`; it must hold viewW * viewH * channels floats.
bool RenderField(const char* centerRe, const char* centerIm,
                 double spanMant, int spanExp, int viewW, int viewH,
                 int maxIter, int mode, int kind, int power,
                 const char* juliaRe, const char* juliaIm,
                 int colorMode, double rotation, int channels,
                 float* field, const std::atomic<bool>* cancel, Result* out,
                 const char* expr = nullptr, const char* params = nullptr,
                 const char* phoenixP = nullptr);

}  // namespace msetb
