// B2: watch the series-approximation validity test evolve along one orbit.
#include "bigfloat.hpp"
#include "floatexp.hpp"
#include "reference.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

static const char* CRE =
    "-1.0069743306013604554903205072737466724498341569620852222340047322401250216077881286791860894912078";
static const char* CIM =
    "-0.30929306234822977280937688333252666569592449443536899596506161915291164038903410366922816112723778";

int main(int argc, char** argv) {
    const int ze = argc > 1 ? atoi(argv[1]) : 38;
    const int maxiter = argc > 2 ? atoi(argv[2]) : 3000;
    const int order = argc > 3 ? atoi(argv[3]) : 8;
    const int W = 800, H = 600;

    PrecisionPlan plan = plan_precision(1.0, ze, 0, 1200000);
    BigFloat::precision_limbs = plan.limbs;
    BigFloat cr = BigFloat::from_string(CRE);
    BigFloat ci = BigFloat::from_string(CIM);
    Fexp scale = (Fexp(3.0) / Fexp(1.0, ze)) / Fexp((double)W);
    Fexp dmax = scale * Fexp(std::sqrt((double)W * W + (double)H * H) * 0.5 + 1.0);
    printf("digits=%d scale=%.4e dmax=%.4e logd=%.3f order=%d\n", plan.digits, scale.to_double(),
           dmax.to_double(), dmax.log10_abs(), order);

    Orbit orb;
    orb.digits = plan.digits;
    std::string err;
    compute_orbit(orb, cr, ci, maxiter, false, order, dmax, &err);
    printf("len=%d escaped=%d skip=%d usable=%d order=%d\n", (int)orb.z.size(),
           (int)orb.escaped, orb.sa.skip, (int)orb.sa.usable, orb.sa.order);
    printf("A_k at skip (log10|A_k|, term = log10|A_k| + (k-1)logd):\n");
    for (int k = 1; k <= std::min(order, 12); ++k) {
        if (k >= (int)orb.sa.coef.size()) break;
        const double l = orb.sa.coef[(size_t)k].re.log10_abs();
        printf("  A_%-2d log10=%-10.3f term=%-10.3f\n", k, l,
               l + (double)(k - 1) * dmax.log10_abs());
    }
    return 0;
}
