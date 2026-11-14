/* Is a replayed series identical to one integrated inside the orbit loop?
 *
 * sa_replay() must reproduce the coefficients bit for bit, otherwise every
 * frame that uses the series changes.  This compares A_k element by element
 * on both backends (GMP and BigFloat) and reports the first divergence.
 */
#include "gmp_bridge.hpp"
#include "reference.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

static int failures = 0;

static double log10abs(const CFexp& a) {
    const Fexp n = cfexp_abs2(a);
    if (n.m == 0.0) return -std::numeric_limits<double>::infinity();
    return 0.5 * n.log10_abs();
}

static void check(int backend, int digits, int steps, int order) {
    const std::string re = "-0.7436438870371513922864907972623513243389378523240987532214053477854301663";
    const std::string im = "0.1318259042053300000000000000000000000000000000000000000000000000000000000";
    const BigFloat cr = BigFloat::from_string(re), ci = BigFloat::from_string(im);
    const Fexp dm(1e-3);
    std::string err;

    orbit_set_backend(backend);
    Orbit a, b;
    a.digits = digits; b.digits = digits;
    compute_orbit(a, cr, ci, steps, false, order, dm, &err);
    compute_orbit(b, cr, ci, steps, false, 0, dm, &err);
    sa_replay(b, order, dm);
    orbit_set_backend(-1);

    const bool len_ok = a.z.size() == b.z.size();
    double worst = 0.0; int at = -1;
    const size_t n = std::min(a.sa.coef.size(), b.sa.coef.size());
    for (size_t k = 1; k < n; ++k) {
        const double x = log10abs(a.sa.coef[k]);
        const double y = log10abs(b.sa.coef[k]);
        const double d = (x == y) ? 0.0 : std::fabs(x - y);
        if (d > worst) { worst = d; at = (int)k; }
    }
    const bool ok = len_ok && a.sa.skip == b.sa.skip && a.sa.usable == b.sa.usable &&
                    a.sa.coef.size() == b.sa.coef.size() && worst == 0.0;
    std::printf("backend=%d digits=%-4d steps=%-5d order=%-2d len=%zu/%zu skip=%d/%d "
                "coef=%zu/%zu worst=%.3e @%d  %s\n",
                backend, digits, steps, order, a.z.size(), b.z.size(), a.sa.skip, b.sa.skip,
                a.sa.coef.size(), b.sa.coef.size(), worst, at, ok ? "identical" : "DIFFERENT");
    if (!ok) ++failures;
}

int main(int argc, char** argv) {
    if (!gmpb::available()) { std::printf("no gmp\n"); return 1; }
    (void)argc; (void)argv;
    for (int backend : {0, 1}) {
        for (int order : {2, 8, 16}) {
            check(backend, 120, 400, order);
            check(backend, 400, 1200, order);
        }
    }
    std::printf("%s\n", failures ? "FAIL" : "all identical");
    return failures ? 1 : 0;
}
