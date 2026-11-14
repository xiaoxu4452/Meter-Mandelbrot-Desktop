// B2: F2 arithmetic vs std::complex<double> on the real orbit of one pixel.
// Includes deep.cpp so it can use the kernel types directly.
#include "bigfloat.hpp"
#include "floatexp.hpp"
#include "reference.hpp"

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../../src/deep.cpp"

static const char* CRE =
    "-1.0069743306013604554903205072737466724498341569620852222340047322401250216077881286791860894912078";
static const char* CIM =
    "-0.30929306234822977280937688333252666569592449443536899596506161915291164038903410366922816112723778";

static double f2_value(const F2& a) {
    // only used for values inside double range here
    return std::ldexp(a.m, a.e);
}

int main(int argc, char** argv) {
    const int ze = argc > 1 ? atoi(argv[1]) : 6;
    const int maxiter = argc > 2 ? atoi(argv[2]) : 200;
    const int px = argc > 3 ? atoi(argv[3]) : 4;
    const int py = argc > 4 ? atoi(argv[4]) : 4;
    const int W = 300, H = 200;

    PrecisionPlan plan = plan_precision(1.0, ze, 0, 1200000);
    BigFloat::precision_limbs = plan.limbs;
    BigFloat cr = BigFloat::from_string(CRE);
    BigFloat ci = BigFloat::from_string(CIM);
    Fexp scale = (Fexp(3.0) / Fexp(1.0, ze)) / Fexp((double)W);

    const int rdx = W / 2, rdy = H / 2;
    Fexp oxr = scale * Fexp((double)rdx + 0.5 - W * 0.5);
    Fexp oyi = scale * Fexp(H * 0.5 - (double)rdy - 0.5);
    BigFloat rre = cr + BigFloat::from_floatexp(oxr.m, oxr.e);
    BigFloat rim = ci + BigFloat::from_floatexp(oyi.m, oyi.e);
    Orbit orb;
    orb.digits = plan.digits;
    Fexp dmax = scale * Fexp(std::sqrt((double)W * W + (double)H * H) * 0.5 + 1.0);
    std::string err;
    compute_orbit(orb, rre, rim, maxiter, false, 0, dmax, &err);

    const int kx = px - rdx, ky = rdy - py;
    Fexp sx = scale * Fexp((double)kx);
    Fexp sy = scale * Fexp((double)ky);

    F2 fr = f2_from_pow10(sx.m, sx.e);
    F2 fi = f2_from_pow10(sy.m, sy.e);
    printf("1e%d pixel(%d,%d) d0 decimal=(%.12e, %.12e)\n", ze, px, py, sx.to_double(),
           sy.to_double());
    printf("   F2 -> (%.12e, %.12e)  relerr re %.3e\n", f2_value(fr), f2_value(fi),
           (f2_value(fr) - sx.to_double()) / sx.to_double());

    std::complex<double> d0d(sx.to_double(), sy.to_double());
    std::complex<double> dd = d0d;
    std::complex<double> d(d0d);
    const int len = (int)orb.z.size();

    C2 d0f{fr, fi};
    C2 df = d0f;

    int it_d = -1, it_f = -1;
    printf("  n      |z| double        |z| F2            |d| double      |d| F2\n");
    for (int n = 0; n < len; ++n) {
        std::complex<double> Z(orb.z[(size_t)n].re.to_double(), orb.z[(size_t)n].im.to_double());
        const std::complex<double> z = Z + d;

        const C2 Zf{f2_from_pow10(orb.z[(size_t)n].re.m, orb.z[(size_t)n].re.e),
                    f2_from_pow10(orb.z[(size_t)n].im.m, orb.z[(size_t)n].im.e)};
        const C2 zf = c2_add(Zf, df);

        if (n < 3 || n > 124) {
            printf("  %3d  %16.10e  %16.10e  %14.6e  %14.6e\n", n, std::abs(z),
                   std::exp(0.5 * f2_log_abs(c2_abs2(zf))), std::abs(d),
                   std::exp(0.5 * f2_log_abs(c2_abs2(df))));
        }
        if (n >= 129 && n <= 134) {
            const F2 z2f = c2_abs2(zf);
            const F2 b4 = f2_from(4.0);
            printf("     [cmp] |z|^2=(%.6f,2^%d) bail=(%.6f,2^%d) cmp=%d  double_norm=%.6f\n",
                   z2f.m, z2f.e, b4.m, b4.e, f2_cmp_abs(z2f, b4), std::norm(z));
        }
        if (std::norm(z) > 4.0 && it_d < 0) it_d = n;
        if (f2_cmp_abs(c2_abs2(zf), f2_from(4.0)) > 0 && it_f < 0) it_f = n;
        if (it_d >= 0 && it_f >= 0) break;
        if (n + 1 >= len) break;
        d = 2.0 * Z * d + d * d + d0d;
        df = c2_add(c2_add(c2_scale2(c2_mul(Zf, df)), c2_mul(df, df)), d0f);
    }
    printf("iter double=%d  iter F2=%d\n", it_d, it_f);
    (void)dd;
    return 0;
}
