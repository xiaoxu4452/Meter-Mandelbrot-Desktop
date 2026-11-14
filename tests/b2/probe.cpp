// B2 probe: show, at one deep view, how the two glitch criteria behave along a
// pixel's perturbation, and where the reference orbit actually dies.
#include "bigfloat.hpp"
#include "floatexp.hpp"
#include "reference.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const char* CRE =
    "-1.0069743306013604554903205072737466724498341569620852222340047322401250216077881286791860894912078";
static const char* CIM =
    "-0.30929306234822977280937688333252666569592449443536899596506161915291164038903410366922816112723778";

int main(int argc, char** argv) {
    double zoom_m = 1.0;
    int zoom_e = argc > 1 ? atoi(argv[1]) : 38;
    int maxiter = argc > 2 ? atoi(argv[2]) : 3000;
    int W = 300, H = 200;

    PrecisionPlan plan = plan_precision(zoom_m, zoom_e, 0, 1200000);
    BigFloat::precision_limbs = plan.limbs;
    BigFloat cr = BigFloat::from_string(CRE);
    BigFloat ci = BigFloat::from_string(CIM);

    Fexp span = Fexp(3.0) / Fexp(zoom_m, zoom_e);
    Fexp scale = span / Fexp((double)W);

    Orbit orb;
    orb.digits = plan.digits;
    Fexp dmax = scale * Fexp(std::sqrt((double)W * W + (double)H * H) * 0.5 + 1.0);
    std::string err;
    bool ok = compute_orbit(orb, cr, ci, maxiter, false, 0, dmax, &err);
    int len = (int)orb.z.size();
    printf("zoom=1e%d digits=%d limbs=%d maxiter=%d refLen=%d escaped=%d escape_iter=%d ok=%d\n",
           zoom_e, plan.digits, plan.limbs, maxiter, len, (int)orb.escaped, orb.escape_iter, (int)ok);
    printf("scale=%.6e  dmax=%.3e\n", scale.to_double(), dmax.to_double());

    // reference escape iteration (|Z| > 2)
    int ref_esc = -1;
    for (int n = 0; n < len; ++n) {
        if (fexp_cmp(cfexp_abs2(orb.z[n]), Fexp(4.0)) > 0) { ref_esc = n; break; }
    }
    printf("ref escapes |Z|>2 at n=%d (len=%d)\n\n", ref_esc, len);

    // sample pixels
    struct P { int x, y; };
    P pts[] = {{150, 100}, {0, 0}, {299, 199}, {290, 100}, {10, 100}};
    for (P p : pts) {
        double dx = ((double)p.x + 0.5) - W * 0.5;
        double dy = H * 0.5 - ((double)p.y + 0.5);
        CFexp d0(scale * Fexp(dx), scale * Fexp(dy));
        CFexp d = d0;
        int n_big = -1, n_cancel = -1, n_esc = -1;
        printf("pixel (%d,%d) d0=%.4e\n", p.x, p.y, d0.re.to_double());
        for (int n = 0; n < len; ++n) {
            CFexp z = orb.z[n] + d;
            Fexp z2 = cfexp_abs2(z);
            Fexp Zabs = cfexp_abs(orb.z[n]);
            Fexp dabs = cfexp_abs(d);
            if (n_big < 0 && fexp_cmp(Zabs, dabs) < 0) n_big = n;
            if (n_cancel < 0 && fexp_cmp(cfexp_abs(z), dabs) < 0) n_cancel = n;
            if (n_esc < 0 && fexp_cmp(z2, Fexp(4.0)) > 0) n_esc = n;
            if (n >= len - 8 || (n % 400 == 0)) {
                printf("  n=%5d  log10|Z|=%9.3f log10|d|=%9.3f log10|z|=%9.3f  %s\n",
                       n, Zabs.log10_abs(), dabs.log10_abs(), cfexp_abs(z).log10_abs(),
                       (fexp_cmp(Zabs, dabs) < 0) ? "<- |Z|<|d|" : "");
            }
            if (n + 1 >= len) break;
            // d_{n+1} = 2 Z_n d_n + d_n^2 + d_0  (unnormalised perturbation)
            d = orb.z[n] * d * Fexp(2.0) + d * d + d0;
        }
        printf("  => |Z|<|d| at n=%d, |z|<|d| at n=%d, escaped at n=%d\n\n", n_big, n_cancel, n_esc);
    }
    return 0;
}
