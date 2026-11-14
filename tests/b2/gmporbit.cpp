/* Does the GMP orbit match the BigFloat one?
 *
 * The reference orbit is the only thing GMP replaces, so this compares the two
 * element by element: same escape iteration, same length, and every z_n equal
 * to within the precision both paths round to (a few units in the last place
 * of a double mantissa).  If they agree here, any render difference must come
 * from somewhere else.
 *
 *   gmporbit [digits] [steps]
 */
#include "gmp_bridge.hpp"
#include "reference.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

static int failures = 0;

static void compare(const char* label, int digits, int steps) {
    const std::string re =
        "-0.7436438870371513922864907972623513243389378523240987532214053477854301663"
        "6451274010401263725688590132069065385463185718437892690451223146557660701431";
    const std::string im =
        "0.1318259042053300000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000000000000";
    const BigFloat cr = BigFloat::from_string(re);
    const BigFloat ci = BigFloat::from_string(im);

    Orbit a, b;
    a.digits = digits;
    b.digits = digits;
    std::string err;
    const Fexp dm(1e-3);

    orbit_set_backend(1);
    compute_orbit_from(a, BigFloat(0.0), BigFloat(0.0), cr, ci, steps, false, 0, dm, &err);
    /* Force the slow path for the second orbit. */
    orbit_set_backend(0);
    compute_orbit_from(b, BigFloat(0.0), BigFloat(0.0), cr, ci, steps, false, 0, dm, &err);
    orbit_set_backend(-1);

    if (a.z.size() != b.z.size()) {
        std::printf("%-8s digits=%-5d length %zu vs %zu  MISMATCH\n", label, digits, a.z.size(),
                    b.z.size());
        ++failures;
        return;
    }
    double worst = 0.0;
    int worst_at = -1;
    Fexp wx, wy;
    for (size_t i = 0; i < a.z.size(); ++i) {
        for (int c = 0; c < 2; ++c) {
            const Fexp& x = c ? a.z[i].im : a.z[i].re;
            const Fexp& y = c ? b.z[i].im : b.z[i].re;
            if (x.m == 0.0 && y.m == 0.0) continue;
            /* Compare as relative difference in log10 space, which is what a
             * different rounding of the last mantissa bit looks like. */
            const double dx = std::fabs(x.log10_abs() - y.log10_abs());
            if (dx > worst) { worst = dx; worst_at = (int)i; wx = x; wy = y; }
        }
    }
    const bool same_escape = a.escaped == b.escaped && a.escape_iter == b.escape_iter;
    const bool gmp_used = a.gmp && !b.gmp;
    std::printf("%-8s digits=%-5d len=%-6zu esc=%-6d worst dlog10=%.3e @%d  %s\n", label, digits,
                a.z.size(), a.escape_iter, worst, worst_at,
                (gmp_used && same_escape && worst < 1e-10) ? "ok" : "MISMATCH");
    if (worst >= 1e-10) {
        std::printf("      gmp  %.17e e%lld\n      bigf %.17e e%lld\n", wx.m, (long long)wx.e,
                    wy.m, (long long)wy.e);
    }
    if (!gmp_used || !same_escape || worst >= 1e-10) ++failures;
}

int main(int argc, char** argv) {
    if (!gmpb::available()) {
        std::printf("gmp unavailable: %s\n", gmpb::error());
        return 1;
    }
    std::printf("gmp %s\n", gmpb::version());
    const int steps = argc > 1 ? std::atoi(argv[1]) : 600;
    if (argc > 2) {
        compare("orbit", std::atoi(argv[2]), steps);
    } else {
        for (int d : {80, 120, 250, 500, 900}) compare("orbit", d, steps);
    }
    std::printf("%s\n", failures ? "FAIL" : "all matched");
    return failures ? 1 : 0;
}
