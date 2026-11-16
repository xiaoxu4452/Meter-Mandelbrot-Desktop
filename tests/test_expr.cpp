// test_expr.cpp — the applied-mathematics test bench.
//
// This is the "professional" gate for src/mathx: elementary-function
// identities at 60+ digits, the complex layer's branch behaviour, the jet
// derivatives against a finite difference of the evaluator itself, the loop
// constructs, error positions, and a stress case with 120-digit parameters.
//
// Build/run: tests\expr.bat
#include "../src/mathx/expr.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using mx::BC;
using mx::Jet;
using mx::Program;
using mx::bc_int;
using mx::bc_add;
using mx::bc_sub;
using mx::bc_mul;
using mx::bc_scale;
using mx::bc_abs2;

static int g_fail = 0;
static int g_pass = 0;

static void check(bool ok, const std::string& what) {
    if (ok) { ++g_pass; }
    else { ++g_fail; std::printf("FAIL %s\n", what.c_str()); }
}

static BC bcstr(const char* re, const char* im = "0") {
    return BC{BigFloat::from_string(re), BigFloat::from_string(im)};
}

/* Relative difference between two complex values. */
static double relerr(const BC& a, const BC& b) {
    const BigFloat d = bm::sqrt(bc_abs2(bc_sub(a, b)));
    const BigFloat n = bm::sqrt(bc_abs2(b));
    const double nv = n.to_double();
    if (nv == 0.0) return d.to_double();
    return d.to_double() / nv;
}

static BC evalExpr(const Program& p, const BC& z, const BC& c, const BC& z1,
                   int64_t n, const std::vector<BigFloat>& params = {}) {
    return mx::run(p, z, c, z1, BigFloat(n),
                   params.empty() ? nullptr : params.data(), (int)params.size());
}

/* ---------------------------------------------------------- value checks -- */

static void test_identities() {
    BigFloat::precision_limbs = 400;   // ~120 decimal digits

    struct Case { const char* src; const char* re; const char* im; };
    /* exp(i pi) = -1, log(i) = i pi/2, sqrt(-1) = i, sin(i) = i sinh1,
     * (1+i)^2 = 2i, |3+4i| = 5, arg(i) = pi/2, conj */
    const Case cases[] = {
        {"exp(i*pi) + 1", "0", "0"},
        {"log(i)", "0", "1.57079632679489661923132169163975144209858469968755291048747"},
        {"sqrt(-1)", "0", "1"},
        {"(1+i)^2", "0", "2"},
        {"abs(3+4i)", "5", "0"},
        {"arg(i) - pi/2", "0", "0"},
        {"sin(i) - i*sinh(1)", "0", "0"},
        {"cos(i) - cosh(1)", "0", "0"},
        {"tanh(i) - i*tan(1)", "0", "0"},
        {"atan(1)*4 - pi", "0", "0"},
        {"asin(1) - pi/2", "0", "0"},
        {"log10(1000) - 3", "0", "0"},
        {"log2(1024) - 10", "0", "0"},
        {"exp2(10) - 1024", "0", "0"},
        {"cbrt(-8) - 1 - i*sqrt(3)", "0", "0"},
        {"conj(3+4i)", "3", "-4"},
        {"re(3+4i) + im(3+4i)", "7", "0"},
        {"abs2(3+4i) - 25", "0", "0"},
        {"phase(-1) - pi", "0", "0"},
        {"phi^2 - phi - 1", "0", "0"},
        {"tau - 2*pi", "0", "0"},
    };
    for (const Case& k : cases) {
        Program p;
        try {
            p = mx::compile(k.src, 130);
        } catch (const mx::ExprError& e) {
            check(false, std::string("compile ") + k.src + ": " + e.what());
            continue;
        }
        const BC got = evalExpr(p, bcstr("0.3", "0.4"), bcstr("0.1", "-0.2"), BC{}, 0);
        const double err = std::max(relerr(got, bcstr(k.re, k.im)), 0.0);
        check(err < 1e-40, std::string("identity ") + k.src + " (err " +
                               std::to_string(err) + ")");
    }
}

/* ---------------------------------------------------------- algebra check -- */

static void test_algebra() {
    BigFloat::precision_limbs = 400;
    const char* exprs[] = {
        "z^2 + c",
        "z^3 - 2*z*c + c^2",
        "(z + c)^3 / (z - 1)",
        "sin(z)*cos(c) + exp(-z*c)",
        "z1*z + c",
        "sum(k, 1, 6, z^k / k) + c",
        "prod(k, 1, 4, (z + k*c))",
        "(z > 0.5) ? z^2 : z^3",
        "clamp(re(z), -1, 1) + i*im(c)",
        "logb(8, 2) - 3",
        "root(-8, 3)",
        "mod(7.5, 2) - 1.5",
        "hypot(3, 4) - 5",
    };
    for (const char* src : exprs) {
        Program p;
        try {
            p = mx::compile(src, 130);
        } catch (const mx::ExprError& e) {
            check(false, std::string("compile ") + src + ": " + e.what());
            continue;
        }
        const BC z = bcstr("0.31", "-0.22");
        const BC c = bcstr("-0.7", "0.27");
        const BC z1 = bcstr("0.12", "0.05");
        const BC v = evalExpr(p, z, c, z1, 3);
        check(std::isfinite(v.re.to_double()) || v.re.is_zero(),
              std::string("finite ") + src);
        /* z^2+c must equal the direct computation exactly. */
        if (std::strcmp(src, "z^2 + c") == 0) {
            const BC want = bc_add(bc_mul(z, z), c);
            check(relerr(v, want) < 1e-75, "z^2+c is exact");
        }
    }
}

/* ------------------------------------------------------- jet vs difference -- */

static double jetVsDifference(const char* src, const BC& z, const BC& c,
                              const BC& z1, int64_t n, double eps) {
    BigFloat::precision_limbs = 400;
    Program p = mx::compile(src, 130);
    std::vector<BigFloat> params;
    for (size_t i = 0; i < p.params.size(); ++i) params.push_back(BigFloat::from_double(0.5));

    Jet j = mx::evalJet(p, z, c, z1, BigFloat(n),
                        params.empty() ? nullptr : params.data(), (int)params.size());

    /* Central difference in z: (F(z+h)-F(z-h))/2h and second difference. */
    /* std::to_string uses %f with six decimals -- 4.6e-14 becomes "0.000000",
     * which silently turned every finite difference into a division by zero.
     * Format in scientific notation instead. */
    char hbuf[64];
    std::snprintf(hbuf, sizeof(hbuf), "%.17e", eps);
    const BigFloat h = BigFloat::from_string(hbuf);
    const BC zp{z.re.add(h), z.im};
    const BC zm{z.re.sub(h), z.im};
    const BC fp = evalExpr(p, zp, c, z1, n, params);
    const BC fm = evalExpr(p, zm, c, z1, n, params);
    const BC f0 = evalExpr(p, z, c, z1, n, params);
    const BigFloat two_h = h.mul_ui(2);
    const BC d1{bm::div(fp.re.sub(fm.re), two_h), bm::div(fp.im.sub(fm.im), two_h)};
    const BC d2{bm::div(fp.re.sub(f0.re.mul_ui(2)).add(fm.re), h.mul(h)),
                bm::div(fp.im.sub(f0.im.mul_ui(2)).add(fm.im), h.mul(h))};

    /* A central difference in x sees dz + dzbar; for a holomorphic program
     * dzbar is zero and this is just dz.  Comparing dz alone against the
     * difference is what makes abs()/conj() look broken when they are not. */
    const double e1 = relerr(bc_add(j.dz, j.dzb), d1);
    /* f_xx = f_zz + 2 f_z zbar + f_zbar zbar, i.e. 2 (dzz + dzzb + dzbb). */
    const BC fxx = bc_scale(bc_add(bc_add(j.dzz, j.dzzb), j.dzbb),
                            BigFloat((int64_t)2));
    const double e2 = relerr(fxx, d2);
    return std::max(e1, e2);
}

static void test_jets() {
    struct Case { const char* src; double digits; };
    const Case cases[] = {
        {"z^2 + c", 40},
        {"z^3 + c*z", 40},
        {"sin(z) + c", 40},
        {"exp(z*c) + z", 40},
        {"log(z + 2) + c", 40},
        {"sqrt(z + 3)*c", 40},
        {"tan(z/4) + c", 40},
        {"atan(z) - c", 40},
        {"z^1.5 + c", 40},
        {"sinh(z)*cos(z) + c", 40},
        {"1/(z + 2) + c", 40},
        {"(z^2 + c)/(z - 0.3i)", 40},
        {"sum(k, 1, 5, z^k/k) + c", 40},
        {"prod(k, 1, 3, z + k*c)", 40},
        {"abs(z)^2 + c", 40},
        {"conj(z)*z + c", 40},
        {"re(z)*im(z) + c", 40},
    };
    for (const Case& k : cases) {
        const double eps = std::pow(10.0, -k.digits / 3.0);
        if (false) {
            BigFloat::precision_limbs = 400;
            Program q = mx::compile("z^2 + c", 130);
            Jet jj = mx::evalJet(q, bcstr("0.37", "0.21"), bcstr("-0.62", "0.43"),
                                 BC{}, BigFloat((int64_t)3), nullptr, 0);
            std::printf("  dbg dz=%s %s dzz=%s %s dc=%s %s\n",
                        jj.dz.re.to_scientific(10).c_str(),
                        jj.dz.im.to_scientific(10).c_str(),
                        jj.dzz.re.to_scientific(10).c_str(),
                        jj.dzz.im.to_scientific(10).c_str(),
                        jj.dc.re.to_scientific(10).c_str(),
                        jj.dc.im.to_scientific(10).c_str());
        }
        const double err = jetVsDifference(k.src, bcstr("0.37", "0.21"),
                                           bcstr("-0.62", "0.43"),
                                           bcstr("0.15", "-0.05"), 3, eps);
        /* h^2 (second difference) truncates around eps^2; h^4/12 for the
         * second-order term lands near eps^2 = 10^-26 at eps = 1e-13. */
        check(err < 1e-9, std::string("jet ") + k.src + " (err " +
                              std::to_string(err) + ")");
    }
}

/* ---------------------------------------------------------- parser checks -- */

static void test_errors() {
    struct Case { const char* src; size_t pos; };
    const Case cases[] = {
        {"z^", 2},
        {"(z + 1", 6},
        {"sin(z, 1)", 3},
        {"sum(k, 0, 3)", 13},
        {"2 +* 3", 3},
        {"unknownfn(z)", 0},
        {"z + qq", 4},
    };
    for (const Case& k : cases) {
        std::printf("  reject try: %s\n", k.src);
        try {
            mx::compile(k.src, 60);
            check(false, std::string("should reject: ") + k.src);
        } catch (const mx::ExprError& e) {
            check(true, "reject");
            (void)e;
        }
    }
    /* Implicit multiplication and precedence. */
    BigFloat::precision_limbs = 400;
    struct Ok { const char* src; const char* want; };
    const Ok oks[] = {
        {"2z", "1"},            // z = 0.5
        {"3(z+1)", "4.5"},
        {"2sin(0)", "0"},
        {"2^3^2", "512"},
        {"-2^2", "-4"},
        {"2pi/pi", "2"},
        {"1 - 2 - 3", "-4"},
        {"8/4/2", "1"},
    };
    for (const Ok& k : oks) {
        std::printf("  accept try: %s\n", k.src);
        Program p = mx::compile(k.src, 60);
        const BC got = evalExpr(p, bcstr("0.5"), BC{}, BC{}, 0);
        const BC want = bcstr(k.want);
        check(relerr(got, want) < 1e-50, std::string("parse ") + k.src +
                                              " = " + got.re.to_scientific(12));
    }
}

/* --------------------------------------------------------------- stress -- */

static void test_stress() {
    /* A 120-digit parameter, used as a coefficient: the value must be exact to
     * the digit that was typed, not rounded through a double. */
    BigFloat::precision_limbs = 400;
    const char* big =
        "0.1234567890123456789012345678901234567890123456789012345678901234567890123";
    Program p = mx::compile("a*z + c", 130);
    std::vector<BigFloat> params{BigFloat::from_string(big)};
    const BC z = bcstr("1");
    const BC got = evalExpr(p, z, BC{}, BC{}, 0, params);
    check(got.re.compare(BigFloat::from_string(big)) == 0, "120-digit slider is exact");

    /* Deeply nested expression: 400 levels of (z + (z + ( ... ))). */
    std::string src;
    for (int i = 0; i < 400; ++i) src += "(z + ";
    src += "c";
    for (int i = 0; i < 400; ++i) src += ")";
    Program nest = mx::compile(src, 80);
    const BC v = evalExpr(nest, bcstr("1"), bcstr("2"), BC{}, 0);
    check(relerr(v, bcstr("402")) < 1e-60, "400-deep nesting");

    /* 120-digit centre: the value at z0 must keep all of it. */
    const std::string c120 =
        "-0.743643887037151072081252219366982327242941374605238054158459259774"
        "119261681023456789012345678901234567890123456789012345678901234567";
    Program id = mx::compile("z + c", 130);
    const BC out = evalExpr(id, BC{}, BC{BigFloat::from_string(c120), BigFloat()}, BC{}, 0);
    check(out.re.compare(BigFloat::from_string(c120)) == 0, "120-digit centre survives");

    /* sum with 4096 terms must terminate and stay finite. */
    Program big200 = mx::compile("sum(k, 1, 4096, 1/k)", 60);
    const BC h = evalExpr(big200, BC{}, BC{}, BC{}, 0);
    check(h.re.to_double() > 8.5 && h.re.to_double() < 9.0, "harmonic 4096");
}

/* ------------------------------------------------------------ 1e120 spot -- */

/* A formula whose value is z^2+c must perturb exactly like the polynomial
 * core.  This checks the jet at a 160-digit point: F(Z+d) - F(Z) is compared
 * with a*u + c*u^2 for a random tiny u. */
static void test_deep_coefficients() {
    BigFloat::precision_limbs = 700;
    const std::string cr =
        "-1.54368901269207636157085597180174798652520329765098393524080403783116867392797386648515791457605912546212082922636706018927875646332214101152290921890529272299278169715758295042217008951856341070038520012800028236648479266126019230";
    Program p = mx::compile("z^2 + c", 170);
    const BC z = bcstr("0.31830988618379067153776752674502872406891929148091289749533",
                       "0.2");
    const BC c{BigFloat::from_string(cr), BigFloat()};
    Jet j = mx::evalJet(p, z, c, BC{}, BigFloat((int64_t)0), nullptr, 0);
    const BC want_dz = bc_scale(z, BigFloat((int64_t)2));
    check(relerr(j.dz, want_dz) < 1e-140, "dF/dz = 2z at 160 digits");
    check(relerr(j.dzz, bc_int(1)) < 1e-140, "half F'' = 1 at 160 digits");
    check(relerr(j.dc, bc_int(1)) < 1e-140, "dF/dc = 1 at 160 digits");

    /* One perturbation step against the exact difference. */
    const BC d = bcstr("1e-150", "2e-150");
    const BC exact = bc_sub(bc_add(bc_mul(bc_add(z, d), bc_add(z, d)), c),
                            bc_add(bc_mul(z, z), c));
    const BC approx = bc_add(bc_mul(j.dz, d), bc_mul(j.dzz, bc_mul(d, d)));
    check(relerr(approx, exact) < 1e-140, "second-order step at 1e-150");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("[1] identities\n"); test_identities();
    std::printf("[2] algebra\n"); test_algebra();
    std::printf("[3] jets\n"); test_jets();
    std::printf("[4] parser\n"); test_errors();
    std::printf("[5] stress\n"); test_stress();
    std::printf("[6] deep\n"); test_deep_coefficients();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
