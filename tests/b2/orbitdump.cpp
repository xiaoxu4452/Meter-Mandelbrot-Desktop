/* Dump the first N reference-orbit values from both backends, so a divergence
 * can be pinned to the exact step instead of guessed at.
 *   orbitdump [digits] [steps]
 */
#include "gmp_bridge.hpp"
#include "reference.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    const int digits = argc > 1 ? std::atoi(argv[1]) : 200;
    const int steps = argc > 2 ? std::atoi(argv[2]) : 12;
    const BigFloat cr = BigFloat::from_string(
        "-0.7436438870371513922864907972623513243389378523240987532214053477854301663"
        "6451274010401263725688590132069065385463185718437892690451223146557660701431");
    const BigFloat ci = BigFloat::from_string("0.13182590420533");
    const Fexp dm(1e-3);

    Orbit g, b;
    g.digits = digits;
    b.digits = digits;
    std::string err;
    orbit_set_backend(1);
    compute_orbit_from(g, BigFloat(0.0), BigFloat(0.0), cr, ci, steps, false, 0, dm, &err);
    orbit_set_backend(0);
    compute_orbit_from(b, BigFloat(0.0), BigFloat(0.0), cr, ci, steps, false, 0, dm, &err);
    orbit_set_backend(-1);

    const size_t n = std::min(g.z.size(), b.z.size());
    std::printf("digits=%d  gmp_len=%zu bigfloat_len=%zu\n", digits, g.z.size(), b.z.size());
    for (size_t i = 0; i < n; ++i) {
        const double dr = std::fabs(g.z[i].re.log10_abs() - b.z[i].re.log10_abs());
        const double di = std::fabs(g.z[i].im.log10_abs() - b.z[i].im.log10_abs());
        std::printf("%4zu  re gmp %+.17e e%-4lld bigf %+.17e e%-4lld  d=%.2e\n", i, g.z[i].re.m,
                    (long long)g.z[i].re.e, b.z[i].re.m, (long long)b.z[i].re.e, dr);
        if (dr > 1e-14 || di > 1e-14) {
            std::printf("      im gmp %+.17e e%-4lld bigf %+.17e e%-4lld  d=%.2e   <-- first split\n",
                        g.z[i].im.m, (long long)g.z[i].im.e, b.z[i].im.m, (long long)b.z[i].im.e, di);
            break;
        }
    }
    return 0;
}
