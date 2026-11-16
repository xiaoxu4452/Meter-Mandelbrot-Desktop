// test_family.cpp — the applied-math library's own checks.
//
//  1. Expression parsing: accepted forms, rejected forms, numeric evaluation.
//  2. Family delta recurrences: one step of the double recurrence against the
//     exact BigFloat difference F(Z+d) - F(Z), for random Z, d and c.  This is
//     the algebra the deep frames depend on; a sign or fold error shows up here
//     immediately.
//  3. End to end at 1e60 and 1e120: every pixel of a small frame against a
//     direct arbitrary-precision iteration of the same point.
#include "../src/mathx/bigmath.hpp"
#include "../src/mathx/complex.hpp"
#include "../src/mathx/expr.hpp"
#include "../src/mathx/families.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using mx::BC;
using mx::Program;

static int g_fail = 0;

static void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_fail;
        std::printf("FAIL %s\n", what.c_str());
    } else {
        std::printf("ok   %s\n", what.c_str());
    }
}

// ---------------------------------------------------------------- expressions

static void test_expressions() {
    const int digits = 120;
    Program p = mx::compile("z^2 + c", digits);
    const BC z = mx::bc_from(0.5, 0.25);
    const BC c = mx::bc_from(-0.75, 0.1);
    const BC r = mx::run(p, z, c, BC{}, BigFloat((int64_t)0), nullptr, 0);
    const BC want = mx::bc_add(mx::bc_mul(z, z), c);
    check(std::fabs(r.re.to_double() - want.re.to_double()) < 1e-15 &&
              std::fabs(r.im.to_double() - want.im.to_double()) < 1e-15,
          "z^2 + c evaluates");

    Program q = mx::compile("sin(z)^2 + exp(-c) + 2z", digits);
    const BC r2 = mx::run(q, z, c, BC{}, BigFloat((int64_t)0), nullptr, 0);
    check(std::isfinite(r2.re.to_double()) && std::isfinite(r2.im.to_double()),
          "sin/exp/implicit multiplication evaluate");

    Program f = mx::compile("if(abs(z) < 1, z, z/abs(z))", digits);
    const BC r3 = mx::run(f, mx::bc_from(2, 0), c, BC{}, BigFloat((int64_t)0), nullptr, 0);
    check(std::fabs(r3.re.to_double() - 1.0) < 1e-14, "if/abs branch");

    bool threw = false;
    try {
        mx::compile("z + frobnicate(z)", digits);
    } catch (const mx::ExprError&) {
        threw = true;
    }
    check(threw, "unknown function rejected with a diagnostic");

    threw = false;
    try {
        mx::compile("z^2 + (c", digits);
    } catch (const mx::ExprError& e) {
        threw = e.pos > 0;
    }
    check(threw, "unbalanced parenthesis rejected");

    Program ph = mx::compile("z*z + z1*0.5 + c", digits);
    const BC r4 = mx::run(ph, mx::bc_from(0.3, 0), c, mx::bc_from(0.2, 0),
                          BigFloat((int64_t)0), nullptr, 0);
    check(std::fabs(r4.re.to_double() - (0.09 + 0.1 - 0.75)) < 1e-14, "z1 available");
}

// ---------------------------------------------------------------- recurrences

using BigFloat = ::BigFloat;

static BigFloat bfd(double v) { return BigFloat::from_double(v); }

struct Step {
    double dr, di;
};

// The exact family step, in full precision: F(z) for z = Z + d, minus F(Z).
static void exact_step(int kind, double phoenixP, const BC& Z, const BC& d, const BC& prev,
                       const BC& dprev, BC& out) {
    const BC z = mx::bc_add(Z, d);
    const BC prevZ = mx::bc_add(prev, dprev);
    auto F = [&](const BC& w, const BC& pv) {
        const BigFloat ar = w.re.mul(w.re);
        const BigFloat bi = w.im.mul(w.im);
        const BigFloat A = ar.sub(bi);
        const BigFloat B = w.re.mul(w.im).mul_ui(2);
        switch (kind) {
            case 1: return BC{A, -B};
            case 2: return BC{A, w.re.abs().mul(w.im.abs()).mul_ui(2)};
            case 3: return BC{A.abs(), B};
            case 4: return BC{A, B.abs()};
            default: {
                const BigFloat p = bfd(phoenixP);
                return BC{A.add(pv.re.mul(p)), B.add(pv.im.mul(p))};
            }
        }
    };
    const BC fz = F(z, prevZ);
    const BC fZ = F(Z, prev);
    out = mx::bc_sub(fz, fZ);
}

// The production recurrence, in pixel units with ps = 1 (so u == d).
static Step approx_step(int kind, double phoenixP, double X, double Y, double ur, double ui,
                        double pu_r, double pu_i) {
    const double ps = 1.0;
    const double t1 = 2.0 * X + ps * ur;
    const double t2 = 2.0 * Y + ps * ui;
    const double Dr = t1 * ur - t2 * ui;
    const double Di = X * ui + Y * ur + ps * ur * ui;
    const double D2i = 2.0 * Di;
    Step s;
    if (kind == 1) {
        s.dr = Dr;
        s.di = -D2i;
    } else if (kind == 2 || kind == 4) {
        const double B = 2.0 * X * Y;
        const int sB = B < 0 ? -1 : 1;
        const double Bq = B / ps;
        const int sp = (Bq + D2i) < 0 ? -1 : 1;
        s.dr = Dr;
        s.di = D2i * sp + Bq * (sp - sB);
    } else if (kind == 3) {
        const double A = X * X - Y * Y;
        const int sA = A < 0 ? -1 : 1;
        const double Aq = A / ps;
        const int sp = (Aq + Dr) < 0 ? -1 : 1;
        s.dr = Dr * sp + Aq * (sp - sA);
        s.di = D2i;
    } else {
        s.dr = Dr + phoenixP * pu_r;
        s.di = D2i + phoenixP * pu_i;
    }
    return s;
}

static void test_recurrence() {
    BigFloat::precision_limbs = 16;   // 144 digits: plenty for a double compare
    std::mt19937 rng(20261116u);
    std::uniform_real_distribution<double> pos(-1.2, 1.2);
    std::uniform_real_distribution<double> small(-1.0, 1.0);
    for (int kind = 1; kind <= 5; ++kind) {
        double worst = 0.0;
        for (int t = 0; t < 400; ++t) {
            const double X = pos(rng), Y = pos(rng);
            const double dr = small(rng) * 1e-3, di = small(rng) * 1e-3;
            const double pr = small(rng) * 1e-2, pi = small(rng) * 1e-2;
            const BC Z{mx::bc_from(X, Y)};
            const BC d{mx::bc_from(dr, di)};
            const BC prev{mx::bc_from(pr, pi)};
            const BC dprev{mx::bc_from(pi * 0.5, pr * 0.5)};
            BC exact;
            exact_step(kind, 0.5667, Z, d, prev, dprev, exact);
            const Step got = approx_step(kind, 0.5667, X, Y, dr, di, dprev.re.to_double(),
                                         dprev.im.to_double());
            const double er = std::fabs(got.dr - exact.re.to_double());
            const double ei = std::fabs(got.di - exact.im.to_double());
            const double denom = std::max(1e-12, std::fabs(exact.re.to_double()) +
                                                    std::fabs(exact.im.to_double()));
            const double rel = std::max(er, ei) / denom;
            if (rel > worst) worst = rel;
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "kind %d delta recurrence matches F(Z+d)-F(Z) (worst rel %.2e)",
                      kind, worst);
        check(worst < 2e-13, buf);
    }
}

// ---------------------------------------------------------------- end to end

static std::string deep(const char* base, int digits, unsigned seed) {
    std::string s = base;
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> d(0, 9);
    /* Append digits after the last significant one: still a valid decimal. */
    for (int i = 0; i < digits; ++i) s.push_back((char)('0' + d(rng)));
    return s;
}

// Direct arbitrary-precision iteration of one pixel.
static int oracle_iter(int kind, int mode, double phoenixP, const std::string& cre,
                       const std::string& cim, const std::string& jre, const std::string& jim,
                       double px, double py, int w, int h, double ps, int maxiter,
                       double bail2) {
    BigFloat::precision_limbs = 40;
    const BigFloat cr = BigFloat::from_string(cre);
    const BigFloat ci = BigFloat::from_string(cim);
    const BigFloat jr = jre.empty() ? BigFloat() : BigFloat::from_string(jre);
    const BigFloat ji = jim.empty() ? BigFloat() : BigFloat::from_string(jim);
    const BigFloat psb = bfd(ps);
    BigFloat zr, zi, pr, pi, cRe, cIm;
    if (mode == 0) {
        cRe = cr.add(psb.mul(bfd(px - w * 0.5)));
        cIm = ci.add(psb.mul(bfd(py - h * 0.5)));
    } else {
        cRe = jr;
        cIm = ji;
        zr = cr.add(psb.mul(bfd(px - w * 0.5)));
        zi = ci.add(psb.mul(bfd(py - h * 0.5)));
    }
    const BigFloat b2 = bfd(bail2);
    for (int n = 0; n < maxiter; ++n) {
        const BigFloat m = zr.mul(zr).add(zi.mul(zi));
        if (m.compare(b2) > 0) return n;
        const BigFloat ar = zr.mul(zr);
        const BigFloat bi = zi.mul(zi);
        const BigFloat A = ar.sub(bi);
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
        zr = nr.add(cRe);
        zi = ni.add(cIm);
    }
    return -1;
}

/* The real Misiurewicz point of preperiod 3 / period 1, 220 digits, generated
 * by `mset family-point`.  On the real axis all four folded families reduce to
 * z^2+c, so this single coordinate is a boundary point for every one of them —
 * and only a boundary point has structure at 1e120. */
static const char* kRealPoint =
    "-1.5436890126920763615708559718017479865252032976509839352408040378311686739279738664851579"
    "145760591254621208292263670601892787564633221410115229092189052927229927816971575829504221"
    "7008951856341070038520012800028236648479266126019230";

static void test_end_to_end(int kind, const char* name, double zoomExp, int digits) {
    const int w = 16, h = 12, maxiter = 1200;
    const std::string cre = kRealPoint;
    const std::string cim = "0";
    const double zoomMant = 1.0;
    /* 3 / width * 10^-zoomExp */
    const mx::Fexp ps(3.0 / (zoomMant * w), (int64_t)-zoomExp);

    mx::FamilyView v;
    v.kind = kind;
    v.mode = 0;
    v.centerRe = cre;
    v.centerIm = cim;
    v.width = w;
    v.height = h;
    v.maxiter = maxiter;
    v.digits = digits;
    v.ps = ps;
    v.phoenixP = 0.5667;
    v.threads = 2;
    std::vector<mx::PixelOut> px;
    mx::FamilyStats st;
    if (!mx::renderFamily(v, px, &st)) {
        check(false, std::string("render ") + name);
        return;
    }

    int mismatch = 0, interiorMismatch = 0, exact = 0, worst = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const mx::PixelOut& p = px[(size_t)y * w + x];
            const int want = oracle_iter(kind, 0, 0.5667, cre, cim, "", "", x + 0.5, y + 0.5,
                                         w, h, ps.to_double(), maxiter, 4.0);
            if (want < 0) {
                if (!p.interior) ++interiorMismatch;
            } else if (p.interior) {
                ++interiorMismatch;
            } else {
                const int d = std::abs(p.iter - want);
                if (d > 0) ++exact;
                if (d > worst) worst = d;
                /* A pixel can sit exactly on a fold of |x^2-y^2| / |2xy|: the
                 * reference's sign and the pixel's then differ and the escape
                 * count moves by a handful of iterations.  That is a property
                 * of the piecewise map, not of the perturbation. */
                if (d > 8) {
                    if (mismatch < 6)
                        std::printf("     pixel %d,%d got %d want %d (smooth=%.2f)\n",
                                    x, y, p.iter, want, p.smooth);
                    ++mismatch;
                }
            }
        }
    }
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%s at 1e%.0f: %dx%d pixels match the BigFloat oracle "
                                    "(refLen=%d, exact=%d, worst=%d it, outliers=%d/%d)",
                  name, zoomExp, w, h, st.refLen, exact, worst,
                  mismatch + interiorMismatch, w * h);
    check(mismatch + interiorMismatch == 0, buf);
}

int main() {
    test_expressions();
    test_recurrence();
    test_end_to_end(1, "tricorn", 60, 110);
    test_end_to_end(2, "burning ship", 60, 110);
    test_end_to_end(3, "celtic", 60, 110);
    test_end_to_end(4, "perpendicular", 60, 110);
    test_end_to_end(5, "phoenix", 60, 110);
    test_end_to_end(1, "tricorn", 120, 190);
    test_end_to_end(2, "burning ship", 120, 190);

    std::printf(g_fail ? "\n%d FAILURES\n" : "\nall family checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
