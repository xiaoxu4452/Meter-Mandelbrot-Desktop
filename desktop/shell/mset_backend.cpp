#include "mset_backend.h"

#include "bigfloat.hpp"
#include "floatexp.hpp"
#include "palette.hpp"
#include "render.hpp"
#include "viewport.hpp"

#include "mathx/expr.hpp"
#include "mathx/families.hpp"

#include <map>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <cstdio>
#include <cstdlib>

namespace {
/* Segment trace for the integration layer.  A GUI process has no console and
 * a crash inside the renderer would otherwise only leave a fault address. */
void trace(const char* fmt, ...) {
    if (!std::getenv("MB_MSET_TRACE")) return;
    char path[512];
    const char* tmp = std::getenv("TEMP");
    std::snprintf(path, sizeof(path), "%s\\msetb.log", tmp ? tmp : ".");
    FILE* f = std::fopen(path, "ab");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
}
}  // namespace

namespace {

/* span = mant * 2^exp, possibly 2^-10000.  A double cannot hold the value, so
 * only its decimal logarithm is computed and the mantissa/exponent pair is
 * recovered from that.  Relative error is ~1e-15, i.e. a millionth of a pixel
 * at any depth. */
void decimal_span(double mant, int exp2, double& out_m, int& out_e) {
    if (!(mant > 0.0)) { out_m = 1.0; out_e = 0; return; }
    const double l10 = std::log10(mant) + (double)exp2 * 0.30102999566398120;
    const double e = std::floor(l10);
    const double m = std::pow(10.0, l10 - e);
    out_m = m;
    out_e = (int)e;
}

void normalize(double& m, int64_t& e) {
    if (!(m > 0.0)) { m = 1.0; e = 0; return; }
    while (m >= 10.0) { m /= 10.0; ++e; }
    while (m < 1.0) { m *= 10.0; --e; }
}

const char* palette_name(int index) {
    static const char* kNames[] = {"rainbow", "rainbow2", "nebula", "fire", "ocean", "mono"};
    const int n = (int)(sizeof(kNames) / sizeof(kNames[0]));
    if (index < 0) index = 0;
    return kNames[index % n];
}

thread_local std::string g_error;

/* ---------------------------------------------------------------------------
 * Colour-coordinate field cache.
 *
 * The core maps pixels to plane coordinates through the frame centre, so a
 * rotated view only closes exactly when the whole frame is rendered at once.
 * The shell asks for tiles; when they are not the whole frame we render the
 * frame once here and let every tile copy out of it.  One entry is enough: a
 * frame's tiles arrive back to back, and a new view changes the key.
 * ------------------------------------------------------------------------ */
struct FieldEntry {
    std::string key;
    std::vector<float> data;
    int w = 0, h = 0;
};
std::mutex g_field_mutex;
FieldEntry g_field;

std::string make_key(const char* re, const char* im, double spanMant, int spanExp,
                     int viewW, int viewH, int maxIter, int mode, int colorMode,
                     double rotation, const char* jr, const char* ji) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s|%s|%.17g|%d|%d|%d|%d|%d|%d|%.17g|%s|%s",
                  re, im, spanMant, spanExp, viewW, viewH, maxIter, mode,
                  colorMode, rotation, jr ? jr : "", ji ? ji : "");
    return std::string(buf);
}

/* Palette coordinate -> RGB, from the page's own gradient. */
void sample_gradient(const msetb::Gradient& g, double t, RGB& out) {
    if (g.count <= 0) { out = {0, 0, 0}; return; }
    double u = t - std::floor(t);
    if (!(u >= 0.0) || u > 1.0) u = 0.0;
    int i = 0;
    while (i + 1 < g.count && g.pos[i + 1] <= (float)u) ++i;
    const int j = std::min(i + 1, g.count - 1);
    const double p0 = g.pos[i], p1 = g.pos[j];
    const double span = (j == i || p1 <= p0) ? 0.0 : (p1 - p0);
    const double k = span > 0.0 ? std::clamp((u - p0) / span, 0.0, 1.0) : 0.0;
    out.r = g.rgb[i][0] + (g.rgb[j][0] - g.rgb[i][0]) * k;
    out.g = g.rgb[i][1] + (g.rgb[j][1] - g.rgb[i][1]) * k;
    out.b = g.rgb[i][2] + (g.rgb[j][2] - g.rgb[i][2]) * k;
}

}  // namespace

namespace msetb {


namespace {

/* A parsed user formula, cached by source text and precision: a tile pass asks
 * once per tile, and re-parsing a 200-digit expression per tile is pure cost. */
struct FormulaCache {
    std::mutex mu;
    std::string src;
    int digits = 0;
    mx::Program prog;
    bool ready = false;
};
FormulaCache g_formula;

bool compile_formula(const std::string& src, int digits, mx::Program& out) {
    if (src.empty()) return false;
    std::lock_guard<std::mutex> lk(g_formula.mu);
    if (g_formula.ready && g_formula.src == src && g_formula.digits == digits) {
        out = g_formula.prog;
        return true;
    }
    try {
        mx::Program p = mx::compile(src, digits);
        g_formula.prog = p;
        g_formula.src = src;
        g_formula.digits = digits;
        g_formula.ready = true;
        out = p;
        return true;
    } catch (const std::exception&) {
        g_formula.ready = false;
        return false;
    }
}

/* "a=0.5;b=1" -> slider values, in the program's own parameter order. */
void bind_params(const mx::Program& prog, const char* params,
                 std::vector<double>& out) {
    out.assign(prog.params.size(), 0.0);
    if (!params || !*params) return;
    std::map<std::string, double> kv;
    const char* p = params;
    while (*p) {
        while (*p == ';' || *p == ',' || *p == ' ') ++p;
        const char* eq = std::strchr(p, '=');
        if (!eq) break;
        std::string name(p, (size_t)(eq - p));
        const double v = std::atof(eq + 1);
        kv[name] = v;
        const char* semi = std::strchr(eq, ';');
        if (!semi) semi = std::strchr(eq, ',');
        if (!semi) break;
        p = semi + 1;
    }
    for (size_t i = 0; i < prog.params.size(); ++i) {
        auto it = kv.find(prog.params[i]);
        if (it != kv.end()) out[i] = it->second;
    }
}

/* Shared body of the family field/tile paths.  `ps` is plane units per pixel,
 * `digits` the working precision the caller already planned. */
bool render_family(const std::string& cre, const std::string& cim,
                   const std::string& jre, const std::string& jim,
                   const mx::Fexp& ps, int viewW, int viewH, int maxIter,
                   int mode, int kind, const char* expr, const char* params,
                   double rotation, std::vector<PixelOut>& px, mx::FamilyStats& st,
                   const std::atomic<bool>* cancel) {
    mx::Program prog;
    if (kind == 6 && !compile_formula(expr ? expr : "", 220, prog)) return false;
    std::vector<double> pv;
    if (kind == 6) bind_params(prog, params, pv);

    mx::FamilyView v;
    v.kind = kind;
    v.mode = mode;
    v.formula = kind == 6 ? &prog : nullptr;
    v.centerRe = cre;
    v.centerIm = cim;
    v.juliaRe = jre;
    v.juliaIm = jim;
    v.width = viewW;
    v.height = viewH;
    v.maxiter = maxIter > 0 ? maxIter : 600;
    v.ps = ps;
    v.rotCos = std::cos(rotation);
    v.rotSin = std::sin(rotation);
    v.cancel = cancel;
    return mx::renderFamily(v, px, &st);
}

}  // namespace

bool Supports(int mode, int kind, int power) {
    (void)power;
    if (mode != 0 && mode != 1) return false;
    return kind >= 0 && kind <= 6;
}

bool RenderTile(const char* centerRe, const char* centerIm,
                double spanMant, int spanExp,
                int viewW, int viewH, int x, int y, int w, int h,
                int maxIter, int mode, int kind, int power,
                const char* juliaRe, const char* juliaIm,
                int paletteIndex, int colorMode, bool invert,
                double rotation, const Gradient* grad,
                uint8_t* rgba, int outPitch,
                const std::atomic<bool>* cancel, Result* out,
                const char* expr, const char* params) {
    Result local;
    Result* r = out ? out : &local;
    *r = Result();
    if (!rgba || w <= 0 || h <= 0 || viewW <= 0 || viewH <= 0) {
        r->error = "bad rectangle";
        return false;
    }
    if (!Supports(mode, kind, power)) {
        r->error = "family not bridged";
        return false;
    }
    if (x < 0 || y < 0 || x + w > viewW || y + h > viewH) {
        r->error = "rectangle outside the frame";
        return false;
    }

    /* Families and formulas: render the frame once, hand back the rectangle.
     * The page's default path is the field pass (the shader colours it); this
     * tile path exists for the native canvas and for headless checks. */
    if (kind != 0) {
        try {
            double full_m = 1.0;
            int full_e = 0;
            decimal_span(spanMant, spanExp, full_m, full_e);
            Viewport vp;
            vp.zoom_mantissa = 3.0 / full_m;
            vp.zoom_exp10 = -(int64_t)full_e;
            normalize(vp.zoom_mantissa, vp.zoom_exp10);
            vp.width = viewW;
            vp.height = viewH;
            const int digits = (int)std::clamp(vp.log10_zoom() + 80.0, 60.0, 1200000.0);
            BigFloat::precision_limbs = std::max(128, digits / 9 + 8);
            vp.center_re = BigFloat::from_string(centerRe);
            vp.center_im = BigFloat::from_string(centerIm);
            vp.recalc();
            std::vector<PixelOut> px;
            mx::FamilyStats fs;
            if (!render_family(vp.center_re.to_fixed(digits),
                               vp.center_im.to_fixed(digits),
                               (juliaRe && *juliaRe) ? juliaRe : "",
                               (juliaIm && *juliaIm) ? juliaIm : "",
                               vp.pixel_scale(), viewW, viewH, maxIter, mode, kind,
                               expr, params, rotation, px, fs, cancel)) {
                r->error = "family render failed";
                return false;
            }
            const Palette pal = Palette::by_name("rainbow");
            const double band = fs.escMax > fs.escMin ? (fs.escMax - fs.escMin) / 8.0 : 64.0;
            for (int row = 0; row < h; ++row) {
                for (int col = 0; col < w; ++col) {
                    const PixelOut& p = px[(size_t)(y + row) * viewW + (size_t)(x + col)];
                    uint8_t* dst = rgba + (size_t)row * (size_t)outPitch * 4 + (size_t)col * 4;
                    uint8_t cr = 0, cg = 0, cb = 0;
                    if (!p.interior) {
                        double t = p.smooth / band * 6.283185307179586;
                        RGB c = grad ? RGB{0, 0, 0} : pal.eval(t);
                        if (grad) {
                            double u = p.smooth / std::max(1.0, band);
                            u = u - std::floor(u);
                            for (int i = 1; i < grad->count; ++i) {
                                if (u <= grad->pos[i] || i == grad->count - 1) {
                                    const double p0 = grad->pos[i - 1], p1 = grad->pos[i];
                                    const double a = p1 > p0 ? std::clamp((u - p0) / (p1 - p0), 0.0, 1.0) : 0.0;
                                    c.r = grad->rgb[i - 1][0] + (grad->rgb[i][0] - grad->rgb[i - 1][0]) * a;
                                    c.g = grad->rgb[i - 1][1] + (grad->rgb[i][1] - grad->rgb[i - 1][1]) * a;
                                    c.b = grad->rgb[i - 1][2] + (grad->rgb[i][2] - grad->rgb[i - 1][2]) * a;
                                    break;
                                }
                            }
                        }
                        if (invert) { c.r = 1 - c.r; c.g = 1 - c.g; c.b = 1 - c.b; }
                        cr = (uint8_t)std::clamp(c.r * 255.0 + 0.5, 0.0, 255.0);
                        cg = (uint8_t)std::clamp(c.g * 255.0 + 0.5, 0.0, 255.0);
                        cb = (uint8_t)std::clamp(c.b * 255.0 + 0.5, 0.0, 255.0);
                    }
                    dst[0] = cb; dst[1] = cg; dst[2] = cr; dst[3] = 255;
                }
            }
            r->ok = true;
            r->ms = fs.ms;
            r->refLen = fs.refLen;
            r->digits = fs.digits;
            r->glitches = (int)fs.glitches;
            r->maxEscape = fs.escMax;
            r->minEscape = fs.escMin;
            r->bounded = fs.bounded;
            r->iterPerCycle = band;
            return true;
        } catch (const std::exception& e) {
            g_error = e.what();
            r->error = g_error.c_str();
            return false;
        }
    }

    try {
        /* The frame's full width as a decimal (mantissa, exponent) pair. */
        double full_m = 1.0;
        int full_e = 0;
        decimal_span(spanMant, spanExp, full_m, full_e);

        /* The rebuilt core renders a frame as one unit: its reference ladder
         * picks the longest-lived point of the *frame*, and its palette band is
         * anchored on the *frame's* escape span.  Both are properties of the
         * whole picture, so a tile cut from it is the only way every tile gets
         * the same reference and the same colour phase -- rendering tiles
         * independently put a visible seam at every tile edge (each tile
         * normalised its own band).  The first tile pays for the frame; the
         * rest copy out of the cache. */
        const bool rotated = (rotation != 0.0);
        const bool whole = (x == 0 && y == 0 && w == viewW && h == viewH);
        const bool via_cache = !whole;

        int rw = via_cache ? viewW : w;
        int rh = via_cache ? viewH : h;
        /* The colour field is addressed from its own origin in both cases: a
         * cached frame holds the whole view, so this tile starts at (x,y); a
         * direct tile render covers exactly this rectangle at (0,0).  Carrying
         * x/y into a direct render read 11 MB past its end. */
        const int ox = via_cache ? x : 0;
        const int oy = via_cache ? y : 0;

        /* mset models the view as span/zoom.  Keeping span at 3 (the classic
         * Mandelbrot width) puts the whole depth into the zoom exponent, which
         * BigFloat can represent exactly. */
        Viewport vp;
        vp.zoom_mantissa = 3.0 / full_m;
        vp.zoom_exp10 = -(int64_t)full_e;
        normalize(vp.zoom_mantissa, vp.zoom_exp10);
        vp.width = viewW;
        vp.height = viewH;
        /* Same parse-width trap as RenderField: from_string truncates to the
         * previous frame's thread-local limb count.  The centre must survive
         * intact before the tile offsets are added at 1e-3000. */
        const int parse_digits =
            (int)std::clamp(vp.log10_zoom() + 80.0, 60.0, 1200000.0);
        BigFloat::precision_limbs = std::max(128, parse_digits / 9 + 8);
        vp.center_re = BigFloat::from_string(centerRe);
        vp.center_im = BigFloat::from_string(centerIm);
        vp.recalc();

        /* Tile centre in the plane, computed in BigFloat: at 1e-3000 a double
         * centre plus offset would collapse onto the double grid.
         *
         * The offset is the tile's *continuous* centre (pixel i occupies
         * [i, i+1)), i.e. x + w/2, not the centre of pixel x + w/2.  Asking
         * Viewport::point_at for pixel (x + w/2) built the tile half a pixel to
         * the right and half a pixel up, so every tile disagreed with its
         * neighbour by one pixel -- a hairline at every tile edge. */
        auto plane_at = [&](double cx, double cy) {
            const double dx = cx - (double)vp.width * 0.5;
            const double dy = (double)vp.height * 0.5 - cy;
            const Fexp offx = vp.pixel_scale() * Fexp(dx);
            const Fexp offy = vp.pixel_scale() * Fexp(dy);
            return std::make_pair(
                vp.center_re + BigFloat::from_floatexp(offx.m, offx.e),
                vp.center_im + BigFloat::from_floatexp(offy.m, offy.e));
        };
        auto centre = via_cache
                          ? std::make_pair(vp.center_re, vp.center_im)
                          : plane_at((double)x + (double)w * 0.5,
                                     (double)y + (double)h * 0.5);

        Viewport tv;
        tv.center_re = centre.first;
        tv.center_im = centre.second;
        tv.span = 3.0;
        tv.zoom_mantissa = vp.zoom_mantissa * (double)viewW / (double)rw;
        tv.zoom_exp10 = vp.zoom_exp10;
        normalize(tv.zoom_mantissa, tv.zoom_exp10);
        tv.width = rw;
        tv.height = rh;
        tv.recalc();

        /* One decimal digit per decade plus guard digits, or to_fixed would
         * round the centre and the frame would collapse onto a point. */
        const int digits = (int)std::clamp(tv.log10_zoom() + 80.0, 60.0, 1200000.0);

        RenderRequest req;
        req.center_re = tv.center_re.to_fixed(digits);
        req.center_im = tv.center_im.to_fixed(digits);
        req.zoom_mantissa = tv.zoom_mantissa;
        req.zoom_exp10 = tv.zoom_exp10;
        req.width = rw;
        req.height = rh;
        req.maxiter = maxIter > 0 ? maxIter : 600;
        req.span = 3.0;
        req.palette = palette_name(paletteIndex);
        req.color_mode = colorMode == 1 ? ColorMode::Iteration
                         : colorMode >= 2 ? ColorMode::Distance
                                          : ColorMode::Smooth;
        req.use_distance = false;  // the field carries the smooth coordinate
        req.invert = invert;
        req.interior_palette = false;
        req.julia = mode == 1;
        req.julia_re = (juliaRe && *juliaRe) ? juliaRe : "0";
        req.julia_im = (juliaIm && *juliaIm) ? juliaIm : "0";
        req.use_sa = true;
        req.sa_order = 16;
        req.tile_rebase = true;
        req.tile_size = 64;
        req.cancel = cancel;
        if (rotated) {
            req.rot_cos = std::cos(rotation);
            req.rot_sin = std::sin(rotation);
        }

        /* Always ask for the colour coordinate, never for a palette: the shell
         * owns the gradient, and this single path serves both the page's stops
         * and the built-in ramps. */
        const std::string key = make_key(centerRe, centerIm, spanMant, spanExp,
                                        viewW, viewH, maxIter, mode, colorMode,
                                        rotation, req.julia_re.c_str(),
                                        req.julia_im.c_str());

        const Palette pal = Palette::by_name(req.palette);
        const float* field_data = nullptr;
        std::vector<float> owned;
        double elapsed = 0.0;

        if (via_cache) {
            std::lock_guard<std::mutex> lock(g_field_mutex);
            if (g_field.key != key || (int)g_field.data.size() != viewW * viewH) {
                std::vector<float> buf((size_t)viewW * viewH, 0.0f);
                req.field = buf.data();
                Image img;
                RenderStats stats;
                std::string err;
                trace("cache render %dx%d", viewW, viewH);
                if (!render_image(req, img, stats, &err)) {
                    g_error = err.empty() ? "render failed" : err;
                    r->error = g_error.c_str();
                    return false;
                }
                if (stats.cancelled) { r->error = "cancelled"; return false; }
                r->ms = stats.seconds * 1000.0;
                r->refLen = stats.reference_length;
                r->digits = stats.precision_digits;
                r->glitches = (int)stats.glitches;
                r->maxEscape = stats.max_escape_iter;
                g_field.key = key;
                g_field.data = std::move(buf);
                g_field.w = viewW;
                g_field.h = viewH;
                elapsed = r->ms;
            } else {
                elapsed = 0.0;   // this tile paid nothing; the first one paid it all
            }
            field_data = g_field.data.data();
        } else {
            owned.assign((size_t)rw * (size_t)rh, 0.0f);
            trace("T x=%d y=%d w=%d h=%d vp=%dx%d z=%.6g/%.6g ps=%.10g centre=%.10g,%.10g tvz=%.6g",
                  x, y, w, h, vp.width, vp.height, vp.zoom_mantissa,
                  (double)vp.zoom_exp10, vp.pixel_scale().to_double(),
                  tv.center_re.to_double(), tv.center_im.to_double(),
                  tv.zoom_mantissa);
            trace("  req centre=%s,%s zoom=%.6g e10=%lld span=%.6g rot=%.3f maxiter=%d dims=%dx%d",
                  req.center_re.c_str(), req.center_im.c_str(), req.zoom_mantissa,
                  (long long)req.zoom_exp10, req.span, req.rot_cos, req.maxiter,
                  req.width, req.height);
            {
                auto p0 = tv.point_at(0, 0);
                auto p1 = tv.point_at(tv.width - 1, tv.height - 1);
                trace("  tv.w=%d tv.h=%d p00=%.10g,%.10g p11=%.10g,%.10g", tv.width,
                      tv.height, p0.first.to_double(), p0.second.to_double(),
                      p1.first.to_double(), p1.second.to_double());
            }
            req.field = owned.data();
            Image img;
            RenderStats stats;
            std::string err;
            trace("direct render %dx%d x=%d,y=%d", rw, rh, x, y);
            if (!render_image(req, img, stats, &err)) {
                g_error = err.empty() ? "render failed" : err;
                r->error = g_error.c_str();
                return false;
            }
            if (stats.cancelled) { r->error = "cancelled"; return false; }
            r->ms = stats.seconds * 1000.0;
            r->refLen = stats.reference_length;
            r->digits = stats.precision_digits;
            r->glitches = (int)stats.glitches;
            r->maxEscape = stats.max_escape_iter;
            field_data = owned.data();
            elapsed = r->ms;
        }
        r->ok = true;
        r->ms = elapsed;
        trace("rendered %dx%d field=%p digits=%d ms=%.1f", rw, rh, (void*)field_data,
              r->digits, r->ms);

        if (std::getenv("MB_MSET_TRACE")) {
            double lo = 1e300, hi = -1e300, sum = 0.0;
            int n = 0, nan = 0;
            for (int i = 0; i < rw * rh; ++i) {
                const float v = field_data[i];
                if (v != v) { ++nan; continue; }
                lo = std::min(lo, (double)v); hi = std::max(hi, (double)v);
                sum += v; ++n;
            }
            trace("field %dx%d lo=%.6g hi=%.6g mean=%.6g nan=%d sample=%.6g %.6g %.6g",
                  rw, rh, lo, hi, n ? sum / n : 0.0, nan,
                  (double)field_data[0], (double)field_data[rw / 2],
                  (double)field_data[(size_t)(rh / 2) * rw + rw / 2]);
        }
        const int pitch = outPitch > 0 ? outPitch : viewW;
        trace("shade rect=%d,%d %dx%d pitch=%d rw=%d rh=%d ox=%d oy=%d rgba=%p",
              x, y, w, h, pitch, rw, rh, ox, oy, (void*)rgba);
        /* `rgba` already points at this tile's first pixel (the shell offsets a
         * whole-pass surface), so the row stride is the only geometry needed
         * here.  Adding x/y again -- as this loop used to -- wrote the tile at
         * (x + x, y + y) of a frame-sized buffer: a 512x512 tile of a 2724-wide
         * pass landed 11 MB past the end of it, which is the 0xC0000005 that
         * killed the window on the first native pass. */
        for (int j = 0; j < h; ++j) {
            const float* src = field_data + (size_t)(oy + j) * (size_t)rw + (size_t)ox;
            uint8_t* d = rgba + (size_t)j * (size_t)pitch * 4;
            for (int i = 0; i < w; ++i) {
                const float t = src[i];
                RGB c;
                if (t != t) {                       // interior
                    c = {1.0, 1.0, 1.0};
                    if (!invert) c = {0.0, 0.0, 0.0};
                } else if (grad && grad->count > 0) {
                    sample_gradient(*grad, (double)t, c);
                } else {
                    c = pal.eval((double)t);
                }
                if (invert && t == t) { c.r = 1.0 - c.r; c.g = 1.0 - c.g; c.b = 1.0 - c.b; }
                d[0] = (uint8_t)std::lround(std::clamp(c.b, 0.0, 1.0) * 255.0);
                d[1] = (uint8_t)std::lround(std::clamp(c.g, 0.0, 1.0) * 255.0);
                d[2] = (uint8_t)std::lround(std::clamp(c.r, 0.0, 1.0) * 255.0);
                d[3] = 255;
                d += 4;
            }
        }
        trace("shade done %dx%d", w, h);
        return true;
    } catch (const std::exception& e) {
        g_error = e.what();
        r->error = g_error.c_str();
        return false;
    }
}

bool RenderField(const char* centerRe, const char* centerIm,
                 double spanMant, int spanExp, int viewW, int viewH,
                 int maxIter, int mode, int kind, int power,
                 const char* juliaRe, const char* juliaIm,
                 int colorMode, double rotation, int channels,
                 float* field, const std::atomic<bool>* cancel, Result* out,
                 const char* expr, const char* params) {
    Result local;
    Result* r = out ? out : &local;
    *r = Result();
    if (!field || viewW <= 0 || viewH <= 0) {
        r->error = "bad field request";
        return false;
    }
    if (!Supports(mode, kind, power)) {
        r->error = "family not bridged";
        return false;
    }

    /* Folded families and user expressions: their own engine (src/mathx), the
     * same view arithmetic.  The field layout is identical, so the page's
     * shader cannot tell which producer filled it. */
    if (kind != 0) {
        try {
            double full_m = 1.0;
            int full_e = 0;
            decimal_span(spanMant, spanExp, full_m, full_e);
            Viewport vp;
            vp.zoom_mantissa = 3.0 / full_m;
            vp.zoom_exp10 = -(int64_t)full_e;
            normalize(vp.zoom_mantissa, vp.zoom_exp10);
            vp.width = viewW;
            vp.height = viewH;
            const int digits = (int)std::clamp(vp.log10_zoom() + 80.0, 60.0, 1200000.0);
            BigFloat::precision_limbs = std::max(128, digits / 9 + 8);
            vp.center_re = BigFloat::from_string(centerRe);
            vp.center_im = BigFloat::from_string(centerIm);
            vp.recalc();
            std::vector<PixelOut> px;
            mx::FamilyStats fs;
            if (!render_family(vp.center_re.to_fixed(digits),
                               vp.center_im.to_fixed(digits),
                               (juliaRe && *juliaRe) ? juliaRe : "",
                               (juliaIm && *juliaIm) ? juliaIm : "",
                               vp.pixel_scale(), viewW, viewH, maxIter, mode, kind,
                               expr, params, rotation, px, fs, cancel)) {
                r->error = "family render failed";
                return false;
            }
            const size_t n = (size_t)viewW * (size_t)viewH;
            for (size_t i = 0; i < n; ++i) {
                const PixelOut& p = px[i];
                field[i * (size_t)channels] = p.interior ? -1.0f : (float)p.smooth;
                if (channels > 1) field[i * (size_t)channels + 1] = 0.0f;
                if (channels > 2) field[i * (size_t)channels + 2] = (float)p.iter;
                if (channels > 3) field[i * (size_t)channels + 3] = 0.0f;
            }
            if (channels > 4)
                std::memset(field + n * 4, 0, n * (size_t)(channels - 4) * sizeof(float));
            r->ok = true;
            r->ms = fs.ms;
            r->refLen = fs.refLen;
            r->digits = fs.digits;
            r->glitches = (int)fs.glitches;
            r->maxEscape = fs.escMax;
            r->minEscape = fs.escMin;
            r->bounded = fs.bounded;
            r->iterPerCycle = fs.escMax > fs.escMin ? (fs.escMax - fs.escMin) / 8.0 : 0.0;
            return true;
        } catch (const std::exception& e) {
            g_error = e.what();
            r->error = g_error.c_str();
            return false;
        }
    }

    try {
        /* The shell models the view as span/zoom with span fixed at 3; the
         * depth lives in the zoom exponent, which BigFloat holds exactly. */
        double full_m = 1.0;
        int full_e = 0;
        decimal_span(spanMant, spanExp, full_m, full_e);

        Viewport vp;
        vp.zoom_mantissa = 3.0 / full_m;
        vp.zoom_exp10 = -(int64_t)full_e;
        normalize(vp.zoom_mantissa, vp.zoom_exp10);
        vp.width = viewW;
        vp.height = viewH;

        /* One decimal digit per decade plus guard digits: to_fixed would
         * otherwise round the centre and the frame would collapse to a point. */
        const int digits = (int)std::clamp(vp.log10_zoom() + 80.0, 60.0, 1200000.0);

        /* BigFloat::from_string() truncates to the *thread-local* working width,
         * which here is whatever the PREVIOUS frame left behind.  A shallow
         * frame leaves ~10 limbs (90 digits); parsing a 1e100 centre at that
         * width drops exactly the low digits the frame is about to resolve, so
         * the picture drifts a little depending on which view came before it.
         * Parse at this frame's own width, then let recalc() install the
         * narrower arithmetic width used by the iteration itself. */
        BigFloat::precision_limbs = std::max(128, digits / 9 + 8);
        vp.center_re = BigFloat::from_string(centerRe);
        vp.center_im = BigFloat::from_string(centerIm);
        vp.recalc();

        RenderRequest req;
        req.center_re = vp.center_re.to_fixed(digits);
        req.center_im = vp.center_im.to_fixed(digits);
        req.zoom_mantissa = vp.zoom_mantissa;
        req.zoom_exp10 = vp.zoom_exp10;
        req.width = viewW;
        req.height = viewH;
        req.maxiter = maxIter > 0 ? maxIter : 600;
        req.span = 3.0;
        req.color_mode = colorMode == 1 ? ColorMode::Iteration
                         : colorMode >= 2 ? ColorMode::Distance
                                          : ColorMode::Smooth;
        req.use_distance = colorMode >= 2;
        req.field4 = field;
        req.field_distance = colorMode >= 2;
        req.julia = mode == 1;
        req.julia_re = (juliaRe && *juliaRe) ? juliaRe : "0";
        req.julia_im = (juliaIm && *juliaIm) ? juliaIm : "0";
        req.use_sa = true;
        req.sa_order = 16;
        req.tile_rebase = true;
        req.tile_size = 64;
        req.cancel = cancel;
        if (rotation != 0.0) {
            req.rot_cos = std::cos(rotation);
            req.rot_sin = std::sin(rotation);
        }

        if (std::getenv("MB_MSET_TRACE")) {
            trace("field %dx%d span=%.10g*2^%d -> dec %.10g e%d  zoom=%.10g e%lld "
                  "ps=%.10g digits=%d maxiter=%d centre=%s",
                  viewW, viewH, spanMant, spanExp, full_m, full_e,
                  req.zoom_mantissa, (long long)req.zoom_exp10,
                  vp.pixel_scale().log10_abs(), digits, req.maxiter,
                  req.center_re.c_str());
        }

        Image img;
        RenderStats stats;
        std::string err;
        if (!render_image(req, img, stats, &err)) {
            g_error = err.empty() ? "render failed" : err;
            r->error = g_error.c_str();
            return false;
        }
        if (stats.cancelled) {
            r->error = "cancelled";
            return false;
        }
        /* The value plane (z at the escape point) is not transcribed yet; zero
         * it so the shader cannot read the previous frame's orbit. */
        if (channels > 4) {
            std::memset(field + (size_t)viewW * viewH * 4, 0,
                        (size_t)viewW * viewH * (size_t)(channels - 4) * sizeof(float));
        }
        r->ok = true;
        r->ms = stats.seconds * 1000.0;
        r->refLen = stats.reference_length;
        r->digits = stats.precision_digits;
        r->glitches = (int)stats.glitches;
        r->maxEscape = stats.max_escape_iter;
        r->minEscape = stats.min_escape_iter;
        r->bounded = stats.bounded;
        r->iterPerCycle = stats.band_iters;
        return true;
    } catch (const std::exception& e) {
        g_error = e.what();
        r->error = g_error.c_str();
        return false;
    }
}

}  // namespace msetb
