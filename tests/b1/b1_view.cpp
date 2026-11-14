// B1 — the positioning model, checked in isolation.
//
// The whole interaction contract is three lines (see out/src/app/app_state.cpp):
//
//     before = view.point_at(x, y)
//     view.zoom_by(factor)
//     after  = view.point_at(x, y)
//     centre += before - after
//
// If that is exact, the plane point under the cursor cannot move, whatever the
// depth.  This test measures the residual in pixels of the *current* frame for
// four depths and two cursor positions, then checks that pan and point_at are
// inverses.
//
// Build: tests\b1.bat   (plain cl.exe — cmake is blocked in the agent sandbox)

#include "viewport.hpp"

#include <cmath>
#include <cstdio>
#include <string>

static int g_fail = 0;

static void check(bool ok, const std::string& what, const std::string& detail) {
    if (!ok) {
        ++g_fail;
        std::printf("FAIL  %-46s %s\n", what.c_str(), detail.c_str());
    } else {
        std::printf("ok    %-46s %s\n", what.c_str(), detail.c_str());
    }
}

// Worst-case drift, in pixels, of the plane point under (x,y) after `notches`
// wheel steps of `factor`.
static double drift_px(int64_t zoom_exp10, int x, int y, int notches, double factor) {
    Viewport v;
    v.zoom_mantissa = 1.0;
    v.zoom_exp10 = zoom_exp10;
    v.width = 1600;
    v.height = 900;
    v.recalc();

    auto before = v.point_at(x, y);
    for (int i = 0; i < notches; ++i) {
        auto b = v.point_at(x, y);
        v.zoom_by(factor);
        auto a = v.point_at(x, y);
        v.center_re = v.center_re + (b.first - a.first);
        v.center_im = v.center_im + (b.second - a.second);
    }
    auto after = v.point_at(x, y);

    const Fexp ps = v.pixel_scale();
    const double px = ps.m * std::pow(10.0, (double)ps.e);
    const double dre = (before.first - after.first).to_double();
    const double dim = (before.second - after.second).to_double();
    return std::sqrt(dre * dre + dim * dim) / px;
}

int main() {
    const double notch = 1.522;   // exp(0.42), one wheel step

    struct Case { int64_t exp10; const char* label; };
    const Case depths[] = {
        {10, "1e10"}, {38, "1e38"}, {60, "1e60"}, {100, "1e100"},
    };
    const int cursors[][2] = {{1120, 340}, {800, 450}};

    for (const auto& d : depths) {
        for (const auto& c : cursors) {
            const double dr = drift_px(d.exp10, c[0], c[1], 40, notch);
            char detail[160];
            std::snprintf(detail, sizeof detail,
                          "%s cursor %d,%d  40 notches  drift %.3e px",
                          d.label, c[0], c[1], dr);
            check(dr < 1e-6, "zoom_at_pixel keeps the anchor", detail);
        }
    }

    // pan by n pixels and back: point_at must return to where it started.
    {
        Viewport v;
        v.zoom_mantissa = 1.0;
        v.zoom_exp10 = 38;
        v.width = 1600;
        v.height = 900;
        v.recalc();
        auto p0 = v.point_at(1200, 260);
        v.pan_pixels(400.0, -150.0);
        v.pan_pixels(-400.0, 150.0);
        auto p1 = v.point_at(1200, 260);
        const Fexp ps = v.pixel_scale();
        const double px = ps.m * std::pow(10.0, (double)ps.e);
        const double dre = (p0.first - p1.first).to_double();
        const double dim = (p0.second - p1.second).to_double();
        const double dr = std::sqrt(dre * dre + dim * dim) / px;
        char detail[160];
        std::snprintf(detail, sizeof detail, "1e38 pan 400,-150 and back  drift %.3e px", dr);
        check(dr < 1e-6, "pan_pixels is reversible", detail);
    }

    // The zoom ladder must not lose the exponent: 4000 notches from 1e0 lands
    // at 10^(4000*log10(1.522)) = 10^729.4, and point_at must still be finite.
    {
        Viewport v;
        v.width = 800;
        v.height = 600;
        v.recalc();
        for (int i = 0; i < 4000; ++i) v.zoom_by(notch);
        auto p = v.point_at(700, 120);
        const double z = v.log10_zoom();
        const bool finite = std::isfinite(p.first.to_double()) &&
                            std::isfinite(p.second.to_double());
        char detail[160];
        std::snprintf(detail, sizeof detail, "4000 notches -> zoom 1e%.1f  finite=%d", z, finite ? 1 : 0);
        check(z > 700.0 && z < 760.0 && finite, "zoom ladder keeps the exponent", detail);
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
