// families.cpp — see families.hpp.
#include "families.hpp"

#include "bigmath.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace mx {
namespace {

constexpr double kGlitchEps = 1e-8;
constexpr int kMaxRebases = 8;

struct Orbit {
    std::vector<double> zr, zi, fa, fb;
    std::vector<double> ar, ai, br, bi, cr, ci;   // formula derivatives
    int len = 0;
    double px = 0.0, py = 0.0;
};

struct Ctx {
    BigFloat centerRe, centerIm;
    BigFloat juliaRe, juliaIm;
    BigFloat psBig;
    double ps = 0.0;
    double rotCos = 1.0, rotSin = 0.0;
    int w = 0, h = 0;
    int digits = 0;
};

inline BigFloat bf(int64_t v) { return BigFloat(v); }
inline BigFloat bfd(double v) { return BigFloat::from_double(v); }

struct Plane {
    BigFloat x, y;
};

Plane planeAt(const Ctx& c, double px, double py) {
    const double dx = px - (double)c.w * 0.5;
    const double dy = py - (double)c.h * 0.5;
    const double ox = dx * c.rotCos - dy * c.rotSin;
    const double oy = dx * c.rotSin + dy * c.rotCos;
    Plane p;
    p.x = c.centerRe.add(c.psBig.mul(bfd(ox)));
    p.y = c.centerIm.add(c.psBig.mul(bfd(oy)));
    return p;
}

BC evalFormula(const Program& p, const BC& z, const BC& c, const BC& z1, int64_t n) {
    return run(p, z, c, z1, BigFloat(n), nullptr, 0);
}

/* One step of a folded family, in full precision.  A and B are the folded
 * reference quantities the delta recurrence needs; they are stored per step. */
void stepFamily(int kind, const BigFloat& cr, const BigFloat& ci, double phoenixP,
                BigFloat& zr, BigFloat& zi, BigFloat& pr, BigFloat& pi,
                BigFloat& A, BigFloat& B) {
    const BigFloat ar = zr.mul(zr);
    const BigFloat bi = zi.mul(zi);
    A = ar.sub(bi);
    B = zr.mul(zi).mul_ui(2);
    BigFloat nr, ni;
    switch (kind) {
        case 1:  // tricorn
            nr = A;
            ni = -B;
            break;
        case 2:  // burning ship
            nr = A;
            ni = zr.abs().mul(zi.abs()).mul_ui(2);
            break;
        case 3:  // celtic
            nr = A.abs();
            ni = B;
            break;
        case 4:  // perpendicular
            nr = A;
            ni = B.abs();
            break;
        default: {  // phoenix
            const BigFloat pv = bfd(phoenixP);
            nr = A.add(pr.mul(pv));
            ni = B.add(pi.mul(pv));
            pr = zr;
            pi = zi;
            break;
        }
    }
    zr = nr.add(cr);
    zi = ni.add(ci);
}

int limbsForDigits(int digits) {
    int limbs = digits / 9 + 3;
    if (limbs < 8) limbs = 8;
    return limbs;
}

/* Build one reference orbit and its per-step coefficients. */
bool buildOrbit(const FamilyView& v, const Ctx& c, double px, double py, Orbit& o,
                std::string& err) {
    const int stride = v.maxiter + 1;
    o.zr.assign((size_t)stride, 0.0);
    o.zi.assign((size_t)stride, 0.0);
    o.fa.assign((size_t)stride, 0.0);
    o.fb.assign((size_t)stride, 0.0);
    o.px = px;
    o.py = py;
    const bool isFormula = v.kind == 6;
    if (isFormula) {
        o.ar.assign((size_t)stride, 0.0);
        o.ai.assign((size_t)stride, 0.0);
        o.br.assign((size_t)stride, 0.0);
        o.bi.assign((size_t)stride, 0.0);
        o.cr.assign((size_t)stride, 0.0);
        o.ci.assign((size_t)stride, 0.0);
    }

    bm::ScopedPrecision prec(limbsForDigits(c.digits));
    const Plane p = planeAt(c, px, py);

    BigFloat zr, zi, pr, pi;
    BigFloat cr, ci;
    if (v.mode == 0) {
        cr = p.x;
        ci = p.y;
    } else {
        cr = c.juliaRe;
        ci = c.juliaIm;
        zr = p.x;
        zi = p.y;
    }

    /* 10^-(digits/3): the central difference for the second derivative needs
     * h^2 well above the truncation error, and h itself must not be so small
     * that F(z+h)-F(z-h) disappears.  std::pow is avoided on purpose (this
     * runtime's libm once returned 1.32 for pow(10,-3)). */
    double h = 1.0;
    for (int i = 0; i < c.digits / 3; ++i) h *= 0.1;
    const BigFloat hb = bfd(h);
    const BigFloat two_h = bfd(2.0 * h);
    const BigFloat h2 = bfd(h * h);

    int len = stride;
    const BigFloat bail2b = bfd(v.bail2);
    for (int n = 0; n < stride; ++n) {
        BigFloat A, B;
        if (isFormula) {
            const BC z{zr, zi};
            const BC cc{cr, ci};
            const BC z1{pr, pi};
            const BC F = evalFormula(*v.formula, z, cc, z1, n);
            const BC zp{zr.add(hb), zi};
            const BC zm{zr.sub(hb), zi};
            const BC Fp = evalFormula(*v.formula, zp, cc, z1, n);
            const BC Fm = evalFormula(*v.formula, zm, cc, z1, n);
            const BC A2{bm::div(Fp.re.sub(Fm.re), two_h),
                        bm::div(Fp.im.sub(Fm.im), two_h)};
            const BC C2{bm::div(Fp.re.sub(F.re.mul_ui(2)).add(Fm.re), h2),
                        bm::div(Fp.im.sub(F.im.mul_ui(2)).add(Fm.im), h2)};
            BC B2{};
            if (v.mode == 0) {
                const BC cp{cr.add(hb), ci};
                const BC cm{cr.sub(hb), ci};
                const BC Fcp = evalFormula(*v.formula, z, cp, z1, n);
                const BC Fcm = evalFormula(*v.formula, z, cm, z1, n);
                B2 = BC{bm::div(Fcp.re.sub(Fcm.re), two_h),
                        bm::div(Fcp.im.sub(Fcm.im), two_h)};
            }
            o.ar[(size_t)n] = A2.re.to_double();
            o.ai[(size_t)n] = A2.im.to_double();
            o.br[(size_t)n] = B2.re.to_double();
            o.bi[(size_t)n] = B2.im.to_double();
            o.cr[(size_t)n] = C2.re.to_double();
            o.ci[(size_t)n] = C2.im.to_double();
            o.zr[(size_t)n] = zr.to_double();
            o.zi[(size_t)n] = zi.to_double();
        } else {
            const BigFloat ar = zr.mul(zr);
            const BigFloat bi = zi.mul(zi);
            A = ar.sub(bi);
            B = zr.mul(zi).mul_ui(2);
            o.zr[(size_t)n] = zr.to_double();
            o.zi[(size_t)n] = zi.to_double();
            o.fa[(size_t)n] = A.to_double();
            o.fb[(size_t)n] = B.to_double();
        }

        const BigFloat mag = zr.mul(zr).add(zi.mul(zi));
        if (mag.compare(bail2b) > 0 && n + 1 < stride) {
            len = n + 1;
            break;
        }
        if (isFormula) {
            /* z1 is the value at n-1, so the step must be taken before prev is
             * overwritten.  Getting this backwards is the phoenix bug in
             * miniature: the formula then reads the future. */
            const BC next = evalFormula(*v.formula, BC{zr, zi}, BC{cr, ci},
                                        BC{pr, pi}, n);
            pr = zr;
            pi = zi;
            zr = next.re;
            zi = next.im;
        } else {
            stepFamily(v.kind, cr, ci, v.phoenixP, zr, zi, pr, pi, A, B);
        }
        if (!std::isfinite(zr.to_double()) || !std::isfinite(zi.to_double())) {
            len = n + 1;
            break;
        }
    }
    if (len < stride) {
        const size_t n0 = (size_t)(len > 0 ? len - 1 : 0);
        for (int n = len; n < stride; ++n) {
            o.zr[(size_t)n] = o.zr[n0];
            o.zi[(size_t)n] = o.zi[n0];
            o.fa[(size_t)n] = o.fa[n0];
            o.fb[(size_t)n] = o.fb[n0];
            if (isFormula) {
                o.ar[(size_t)n] = o.ar[n0];
                o.ai[(size_t)n] = o.ai[n0];
                o.br[(size_t)n] = o.br[n0];
                o.bi[(size_t)n] = o.bi[n0];
                o.cr[(size_t)n] = o.cr[n0];
                o.ci[(size_t)n] = o.ci[n0];
            }
        }
    }
    o.len = len;
    if (o.zr.empty()) { err = "empty orbit"; return false; }
    return true;
}

inline int sgn(double v) { return v < 0.0 ? -1 : 1; }

/* Pixel loop, families 1..5: exact delta recurrence in pixel units. */
void renderBandFamilies(const FamilyView& v, const Ctx& c, const std::vector<Orbit>& refs,
                        int y0, int y1, std::vector<PixelOut>& out, long long* glitches,
                        int* escMin, int* escMax, int* bounded, const std::atomic<bool>* cancel) {
    const double ps = c.ps;
    const double bail2 = v.bail2;
    const double p = v.phoenixP;
    for (int y = y0; y < y1; ++y) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return;
        for (int x = 0; x < c.w; ++x) {
            PixelOut o;
            int r = 0;
            double ur = (double)x + 0.5 - refs[0].px;
            double ui = (double)y + 0.5 - refs[0].py;
            double dcx = ur, dcy = ui;
            double pu_r = 0.0, pu_i = 0.0;
            int rebases = 0;
            bool interior = true;
            const int len = refs[0].len;
            for (int n = 0; n < len; ++n) {
                const Orbit& R = refs[(size_t)r];
                const double X = R.zr[(size_t)n], Y = R.zi[(size_t)n];
                const double zr = X + ps * ur;
                const double zi = Y + ps * ui;
                const double z2 = zr * zr + zi * zi;
                if (!(z2 <= bail2)) {
                    const double lz = 0.5 * std::log(std::max(z2, 1e-300));
                    const double lb = 0.5 * std::log(bail2);
                    const double ratio = lz / (lb > 0.0 ? lb : 1.0);
                    double sm = (double)n + 1.0 - std::log2(ratio > 0.0 ? ratio : 1.0);
                    if (!(sm >= 0.0) || !std::isfinite(sm)) sm = 0.0;
                    o.iter = n;
                    o.smooth = sm;
                    interior = false;
                    break;
                }
                /* Glitch: adding nearly opposite numbers threw the low bits
                 * away.  Re-anchor on the nearest reference at the same step. */
                const double aZ = std::hypot(X, Y);
                const double ad = std::hypot(ur, ui) * ps;
                const double az = std::sqrt(z2);
                if (az < kGlitchEps * std::max(aZ, ad) && refs.size() > 1) {
                    if (++rebases > kMaxRebases) break;
                    int best = -1;
                    double bestD = ad;
                    for (size_t q = 0; q < refs.size(); ++q) {
                        if ((int)q == r) continue;
                        const double d = std::hypot(refs[q].zr[(size_t)n] - zr,
                                                    refs[q].zi[(size_t)n] - zi);
                        if (d < bestD) { bestD = d; best = (int)q; }
                    }
                    if (best < 0) break;
                    const Orbit& Q = refs[(size_t)best];
                    ur += (R.zr[(size_t)n] - Q.zr[(size_t)n]) / ps;
                    ui += (R.zi[(size_t)n] - Q.zi[(size_t)n]) / ps;
                    dcx = (double)x + 0.5 - Q.px;
                    dcy = (double)y + 0.5 - Q.py;
                    r = best;
                    if (glitches) ++*glitches;
                    continue;
                }
                if (n + 1 >= len) break;

                const double t1 = 2.0 * X + ps * ur;
                const double t2 = 2.0 * Y + ps * ui;
                const double Dr = t1 * ur - t2 * ui;
                const double Di = X * ui + Y * ur + ps * ur * ui;
                const double D2i = 2.0 * Di;
                double ndr, ndi;
                if (v.kind == 1) {
                    ndr = Dr + dcx;
                    ndi = -D2i + dcy;
                } else if (v.kind == 2 || v.kind == 4) {
                    const double B = R.fb[(size_t)n];
                    const int sB = sgn(B);
                    const double Bq = B / ps;
                    const int sp = sgn(Bq + D2i);
                    ndr = Dr + dcx;
                    ndi = D2i * (double)sp + Bq * (double)(sp - sB) + dcy;
                } else if (v.kind == 3) {
                    const double A = R.fa[(size_t)n];
                    const int sA = sgn(A);
                    const double Aq = A / ps;
                    const int sp = sgn(Aq + Dr);
                    ndr = Dr * (double)sp + Aq * (double)(sp - sA) + dcx;
                    ndi = D2i + dcy;
                } else {
                    ndr = Dr + dcx + p * pu_r;
                    ndi = D2i + dcy + p * pu_i;
                }
                pu_r = ur;
                pu_i = ui;
                ur = ndr;
                ui = ndi;
            }
            if (interior) {
                o.iter = -1;
                o.interior = true;
                o.smooth = -1.0;
                if (bounded) ++*bounded;
            } else {
                if (escMin && (*escMin < 0 || o.iter < *escMin)) *escMin = o.iter;
                if (escMax && o.iter > *escMax) *escMax = o.iter;
            }
            out[(size_t)y * (size_t)c.w + (size_t)x] = o;
        }
    }
}

/* Pixel loop for a user formula: second-order Taylor in pixel units. */
void renderBandFormula(const FamilyView& v, const Ctx& c, const Orbit& R,
                       int y0, int y1, std::vector<PixelOut>& out, long long* glitches,
                       int* escMin, int* escMax, int* bounded, const std::atomic<bool>* cancel) {
    const double ps = c.ps;
    const double bail2 = v.bail2;
    for (int y = y0; y < y1; ++y) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return;
        for (int x = 0; x < c.w; ++x) {
            PixelOut o;
            double ur = (double)x + 0.5 - R.px;
            double ui = (double)y + 0.5 - R.py;
            const double dcx = ur, dcy = ui;
            bool interior = true;
            for (int n = 0; n < R.len; ++n) {
                const double X = R.zr[(size_t)n], Y = R.zi[(size_t)n];
                const double zr = X + ps * ur;
                const double zi = Y + ps * ui;
                const double z2 = zr * zr + zi * zi;
                if (!(z2 <= bail2)) {
                    const double lz = 0.5 * std::log(std::max(z2, 1e-300));
                    const double lb = 0.5 * std::log(bail2);
                    const double ratio = lz / (lb > 0.0 ? lb : 1.0);
                    double sm = (double)n + 1.0 - std::log2(ratio > 0.0 ? ratio : 1.0);
                    if (!(sm >= 0.0) || !std::isfinite(sm)) sm = 0.0;
                    o.iter = n;
                    o.smooth = sm;
                    interior = false;
                    break;
                }
                if (n + 1 >= R.len) break;
                const double ar = R.ar[(size_t)n], ai = R.ai[(size_t)n];
                const double br = R.br[(size_t)n], bi = R.bi[(size_t)n];
                const double cr = R.cr[(size_t)n], ci = R.ci[(size_t)n];
                /* A u */
                const double t1r = ar * ur - ai * ui;
                const double t1i = ar * ui + ai * ur;
                /* B dc  (Mandelbrot only) */
                const double t2r = v.mode == 0 ? br * dcx - bi * dcy : 0.0;
                const double t2i = v.mode == 0 ? br * dcy + bi * dcx : 0.0;
                /* 0.5 C ps u^2 */
                const double u2r = ur * ur - ui * ui;
                const double u2i = 2.0 * ur * ui;
                const double s = 0.5 * ps;
                const double t3r = s * (cr * u2r - ci * u2i);
                const double t3i = s * (cr * u2i + ci * u2r);
                ur = t1r + t2r + t3r;
                ui = t1i + t2i + t3i;
            }
            if (interior) {
                o.iter = -1;
                o.interior = true;
                o.smooth = -1.0;
                if (bounded) ++*bounded;
            } else {
                if (escMin && (*escMin < 0 || o.iter < *escMin)) *escMin = o.iter;
                if (escMax && o.iter > *escMax) *escMax = o.iter;
            }
            out[(size_t)y * (size_t)c.w + (size_t)x] = o;
        }
    }
}

}  // namespace

const char* familyName(int kind) {
    switch (kind) {
        case 0: return "mandelbrot";
        case 1: return "tricorn";
        case 2: return "burning ship";
        case 3: return "celtic";
        case 4: return "perpendicular";
        case 5: return "phoenix";
        case 6: return "formula";
        default: return "?";
    }
}


namespace {

/* One family step without the fold bookkeeping, for the Newton solver. */
void famStep(int kind, double phoenixP, const BigFloat& cr, const BigFloat& ci,
             BigFloat& zr, BigFloat& zi, BigFloat& pr, BigFloat& pi) {
    const BigFloat A = zr.mul(zr).sub(zi.mul(zi));
    const BigFloat B = zr.mul(zi).mul_ui(2);
    BigFloat nr, ni;
    switch (kind) {
        case 1: nr = A; ni = -B; break;
        case 2: nr = A; ni = zr.abs().mul(zi.abs()).mul_ui(2); break;
        case 3: nr = A.abs(); ni = B; break;
        case 4: nr = A; ni = B.abs(); break;
        default: {
            const BigFloat p = bfd(phoenixP);
            nr = A.add(pr.mul(p));
            ni = B.add(pi.mul(p));
            pr = zr;
            pi = zi;
            break;
        }
    }
    zr = nr.add(cr);
    zi = ni.add(ci);
}

/* z_m and z_n (n = m + p) for a given c, in one pass. */
void famRun(int kind, double phoenixP, const BigFloat& cr, const BigFloat& ci, int m, int n,
            BigFloat& zmr, BigFloat& zmi, BigFloat& znr, BigFloat& zni) {
    BigFloat zr, zi, pr, pi;
    for (int k = 0; k < n; ++k) {
        if (k == m) { zmr = zr; zmi = zi; }
        famStep(kind, phoenixP, cr, ci, zr, zi, pr, pi);
    }
    if (m == 0) { zmr = BigFloat(); zmi = BigFloat(); }
    znr = zr;
    zni = zi;
    if (n == 0) { zmr = BigFloat(); zmi = BigFloat(); }
}

/* F(c) = z_n(c) - z_m(c): the nucleus (m=0) or Misiurewicz equation. */
void evalF(int kind, double phoenixP, const BigFloat& cr, const BigFloat& ci, int m, int n,
           BigFloat& Fr, BigFloat& Fi) {
    BigFloat zmr, zmi, znr, zni;
    famRun(kind, phoenixP, cr, ci, m, n, zmr, zmi, znr, zni);
    Fr = znr.sub(zmr);
    Fi = zni.sub(zmi);
}

}  // namespace

PointResult findFamilyPoint(int kind, double phoenixP, int preperiod, int period,
                            const std::string& guessRe, const std::string& guessIm,
                            int digits, int max_newton) {
    PointResult out;
    if (kind < 1 || kind > 5) { out.error = "kind must be 1..5"; return out; }
    if (period < 1) { out.error = "period must be positive"; return out; }
    if (digits < 40) digits = 40;
    const int m = preperiod < 0 ? 0 : preperiod;
    const int n = m + period;
    bm::ScopedPrecision prec(limbsForDigits(digits));

    BigFloat::precision_limbs = limbsForDigits(digits);
    BigFloat cr = BigFloat::from_string(guessRe.empty() ? std::string("0") : guessRe);
    BigFloat ci = BigFloat::from_string(guessIm.empty() ? std::string("0") : guessIm);

    double h = 1.0;
    for (int i = 0; i < digits / 3; ++i) h *= 0.1;
    const BigFloat hb = bfd(h);
    const BigFloat target = BigFloat::from_string(
        ("1e-" + std::to_string(digits > 24 ? digits - 20 : 12)).c_str());

    for (int it = 0; it < max_newton; ++it) {
        BigFloat Fr, Fi;
        evalF(kind, phoenixP, cr, ci, m, n, Fr, Fi);
        const BigFloat res = bm::sqrt(Fr.mul(Fr).add(Fi.mul(Fi)));
        out.log10_residual = res.is_zero() ? -999.0 : (double)res.magnitude_exp10();
        out.newton_steps = it;
        if (std::getenv("MB_FP_DEBUG"))
            std::fprintf(stderr, "[point] it=%d log10|F|=%.2f\n", it, out.log10_residual);
        if (res.compare(target) < 0) { out.ok = true; break; }
        if (!std::isfinite(res.to_double())) { out.error = "orbit overflow"; return out; }

        /* 2x2 Jacobian by central differences of F (not of z_n: the z_m term
         * moves with c too, and dropping it made every step 1.5x too long). */
        BigFloat rpr, rpi, rnr, rni;    // c.re + h / c.re - h
        BigFloat ipr, ipi, inr, ini;    // c.im + h / c.im - h
        evalF(kind, phoenixP, cr.add(hb), ci, m, n, rpr, rpi);
        evalF(kind, phoenixP, cr.sub(hb), ci, m, n, rnr, rni);
        evalF(kind, phoenixP, cr, ci.add(hb), m, n, ipr, ipi);
        evalF(kind, phoenixP, cr, ci.sub(hb), m, n, inr, ini);
        const BigFloat two_h = bfd(2.0 * h);
        const BigFloat a = bm::div(rpr.sub(rnr), two_h);      // dRe/dcr
        const BigFloat b = bm::div(ipr.sub(inr), two_h);      // dRe/dci
        const BigFloat c2 = bm::div(rpi.sub(rni), two_h);     // dIm/dcr
        const BigFloat d = bm::div(ipi.sub(ini), two_h);      // dIm/dci
        const BigFloat det = a.mul(d).sub(b.mul(c2));
        if (std::getenv("MB_FP_DEBUG") && it < 3)
            std::fprintf(stderr, "[jac] a=%.6g b=%.6g c=%.6g d=%.6g det=%.6g F=(%.6g,%.6g) h=%.1e\n",
                         a.to_double(), b.to_double(), c2.to_double(), d.to_double(),
                         det.to_double(), Fr.to_double(), Fi.to_double(), h);
        if (det.is_zero()) { out.error = "singular Jacobian"; return out; }
        const BigFloat dr = bm::div(b.mul(Fi).sub(d.mul(Fr)), det);
        const BigFloat di = bm::div(c2.mul(Fr).sub(a.mul(Fi)), det);
        if (std::getenv("MB_FP_DEBUG") && it < 4)
            std::fprintf(stderr, "[step] dr=%.6g di=%.6g c=%.9f %.9f\n", dr.to_double(),
                         di.to_double(), cr.to_double(), ci.to_double());
        cr = cr.add(dr);
        ci = ci.add(di);
        if (dr.is_zero() && di.is_zero() && res.compare(target) >= 0) {
            out.error = "stalled";
            return out;
        }
    }
    if (!out.ok) {
        out.error = "did not converge";
        return out;
    }
    out.re = cr.to_scientific(60);
    out.im = ci.to_scientific(60);
    /* Full-length decimal output, not scientific: the renderer parses it. */
    out.re = cr.to_fixed(digits + 10);
    out.im = ci.to_fixed(digits + 10);
    return out;
}

bool renderFamily(const FamilyView& v, std::vector<PixelOut>& out, FamilyStats* st) {
    const auto t0 = std::chrono::steady_clock::now();
    FamilyStats local;
    FamilyStats* s = st ? st : &local;
    *s = FamilyStats{};
    if (v.width <= 0 || v.height <= 0 || v.maxiter <= 0) {
        s->error = "bad geometry";
        return false;
    }
    if (v.kind < 1 || v.kind > 6) {
        s->error = "not a family";
        return false;
    }
    if (v.kind == 6 && !v.formula) {
        s->error = "formula missing";
        return false;
    }
    const double psd = v.ps.to_double();
    if (!(psd > 0.0) || !std::isfinite(psd)) {
        s->error = "pixel scale is outside double range";
        return false;
    }

    Ctx c;
    c.w = v.width;
    c.h = v.height;
    c.ps = psd;
    c.rotCos = v.rotCos;
    c.rotSin = v.rotSin;
    int digits = v.digits;
    if (digits <= 0) {
        digits = (int)(-v.ps.log10_abs()) + 40;
        if (digits < 60) digits = 60;
        if (digits > 12000) {
            s->error = "precision beyond the engine limit";
            return false;
        }
    }
    c.digits = digits;
    s->digits = digits;
    {
        bm::ScopedPrecision prec(limbsForDigits(digits));
        c.centerRe = BigFloat::from_string(v.centerRe);
        c.centerIm = BigFloat::from_string(v.centerIm);
        c.juliaRe = v.juliaRe.empty() ? BigFloat() : BigFloat::from_string(v.juliaRe);
        c.juliaIm = v.juliaIm.empty() ? BigFloat() : BigFloat::from_string(v.juliaIm);
        c.psBig = BigFloat::from_floatexp(v.ps.m, v.ps.e);
    }

    /* Reference candidates: the centre first, then three interior points.  A
     * longer-lived orbit is preferred, and the rest serve as rebase targets. */
    std::vector<std::pair<double, double>> pts;
    pts.push_back({(double)v.width * 0.5, (double)v.height * 0.5});
    if (v.kind != 6) {
        pts.push_back({(double)v.width * 0.25, (double)v.height * 0.25});
        pts.push_back({(double)v.width * 0.75, (double)v.height * 0.25});
        pts.push_back({(double)v.width * 0.5, (double)v.height * 0.75});
    }

    std::vector<Orbit> refs;
    refs.reserve(pts.size());
    std::string err;
    for (const auto& pt : pts) {
        Orbit o;
        if (!buildOrbit(v, c, pt.first, pt.second, o, err)) {
            s->error = err.empty() ? "reference orbit failed" : err.c_str();
            return false;
        }
        refs.push_back(std::move(o));
    }
    std::stable_sort(refs.begin(), refs.end(),
                     [](const Orbit& a, const Orbit& b) { return a.len > b.len; });
    s->refs = (int)refs.size();
    s->refLen = refs[0].len;

    out.assign((size_t)v.width * (size_t)v.height, PixelOut{});

    int threads = v.threads;
    if (threads <= 0) threads = (int)std::thread::hardware_concurrency();
    if (threads < 1) threads = 1;
    if (threads > v.height) threads = v.height;

    struct Acc {
        long long glitches = 0;
        int escMin = -1, escMax = -1, bounded = 0;
    };
    std::vector<Acc> accs((size_t)threads);

    auto work = [&](int y0, int y1, Acc& a) {
        if (v.kind == 6) {
            renderBandFormula(v, c, refs[0], y0, y1, out, &a.glitches, &a.escMin,
                              &a.escMax, &a.bounded, v.cancel);
        } else {
            renderBandFamilies(v, c, refs, y0, y1, out, &a.glitches, &a.escMin,
                               &a.escMax, &a.bounded, v.cancel);
        }
    };

    if (threads <= 1) {
        work(0, v.height, accs[0]);
    } else {
        std::vector<std::thread> pool;
        pool.reserve((size_t)threads);
        const int per = (v.height + threads - 1) / threads;
        for (int t = 0; t < threads; ++t) {
            const int y0 = t * per;
            const int y1 = std::min(v.height, y0 + per);
            if (y0 >= y1) break;
            pool.emplace_back(work, y0, y1, std::ref(accs[(size_t)t]));
        }
        for (auto& th : pool) th.join();
    }

    long long glitches = 0;
    int escMin = -1, escMax = -1, bounded = 0;
    for (const Acc& a : accs) {
        glitches += a.glitches;
        bounded += a.bounded;
        if (a.escMin >= 0 && (escMin < 0 || a.escMin < escMin)) escMin = a.escMin;
        if (a.escMax > escMax) escMax = a.escMax;
    }

    s->ok = true;
    s->glitches = glitches;
    s->escMin = escMin < 0 ? 0 : escMin;
    s->escMax = escMax < 0 ? 0 : escMax;
    s->bounded = bounded;
    const auto t1 = std::chrono::steady_clock::now();
    s->ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return true;
}

}  // namespace mx
